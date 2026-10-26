import QtQuick
import Scancore

// Run inside the app with SCANCORE_PROFILE set (tools/frame-times.sh; on
// Android debug builds through files/start.json): frame times while Retro's
// pages turn and while the welcome moves between its steps, against a
// budget. Prints one FRAMES line per scenario and exits 1 when one is over.
//
// Budget (SCANCORE_START "budget": {render, dropped, worst}): 95 % of frames
// take no longer than `render` ms to draw (16, one frame at 60 Hz), no more than
// `dropped` frames per change come later than 1.5 frames after the last,
// and none comes later than `worst` ms (50, three frames: a stall).
Item {
    id: run
    readonly property var budget: Object.assign({ render: 16, dropped: 1, worst: 50, refresh: 60 }, App.startAction.budget || {})
    readonly property int repeats: App.startAction.repeats || 6
    property var results: []
    property var queue: []

    function later(ms, fn) {
        const t = Qt.createQmlObject("import QtQuick; Timer {}", run)
        t.interval = ms
        t.triggered.connect(() => { t.destroy(); fn() })
        t.start()
    }
    function then(fn) { queue.push(fn) }
    function next() {
        if (queue.length === 0) { report(); return }
        queue.shift()(() => next())
    }
    function until(test, done, waited) {
        waited = waited || 0
        if (test() || waited > 8000) { done(); return }
        later(20, () => until(test, done, waited + 20))
    }

    // One change measured: `start` begins it, `moving` is true while
    // it runs; the frames go to `name`.
    function measure(name, start, moving, done, detail) {
        App.startFrameTimes()
        start()
        later(60, () => until(() => !moving(), () => later(80, () => {
            const times = App.stopFrameTimes()
            let entry = results.find(r => r.name === name)
            if (!entry) { entry = { name: name, intervals: [], renders: [], changes: 0, worstDropped: 0, stalls: [] }; results.push(entry) }
            const intervals = Array.from(times.intervals)
            const late = 1.5 * 1000 / budget.refresh
            entry.intervals = entry.intervals.concat(intervals)
            entry.renders = entry.renders.concat(Array.from(times.renders))
            entry.changes += 1
            entry.worstDropped = Math.max(entry.worstDropped, intervals.filter(v => v > late).length)
            const stall = Math.max(0, ...intervals)
            if (stall > budget.worst) entry.stalls.push((detail || "") + " " + stall.toFixed(0) + " ms")
            done()
        })))
    }

    function stack() { return Router.shell.stackFor(Router.shell.section) }
    function welcome() {
        for (const child of run.Window.window.contentItem.children)
            for (const inner of child.children)
                if (inner.stepKeys !== undefined) return inner
        return null
    }

    function retroTurns() {
        then(done => { App.setSetting("uiMode", "retro"); Router.go("settings"); later(2500, done) })
        for (let i = 0; i < repeats; ++i) {
            then(done => {
                const page = stack().currentItem
                const list = Array.from(page.sections)
                const target = list[i % list.length]
                measure("retro turn forward", () => page.open(target), () => stack().busy, done, "to " + (target.key || target.title))
            })
            then(done => later(400, done))
            then(done => measure("retro turn back", () => Router.back(), () => stack().busy, done))
            then(done => later(400, done))
        }
    }
    function welcomeSwipes(look) {
        then(done => {
            App.setSetting("uiMode", look)
            Router.go("home")
            App.setSetting("onboarded", false)
            // The opening plays first.
            later(4000, done)
        })
        for (let round = 0; round < Math.max(1, Math.round(repeats / 3)); ++round) {
            for (let s = 0; s < 5; ++s)
                then(done => {
                    const w = welcome()
                    const t0 = Date.now()
                    measure(look + " welcome next", () => w.go(w.step + 1), () => Date.now() - t0 < Motion.page + 40, () => later(300, done), w.stepKeys[w.step] + " to " + w.stepKeys[w.step + 1])
                })
            for (let s = 5; s > 0; --s)
                then(done => {
                    const w = welcome()
                    const t0 = Date.now()
                    measure(look + " welcome back", () => w.go(w.step - 1), () => Date.now() - t0 < Motion.page + 40, () => later(300, done), w.stepKeys[w.step] + " to " + w.stepKeys[w.step - 1])
                })
        }
        then(done => { App.setSetting("onboarded", true); later(1500, done) })
    }

    function percentile(list, p) {
        if (!list.length) return 0
        const sorted = list.slice().sort((a, b) => a - b)
        return sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))]
    }
    function report() {
        let over = 0
        for (const r of results) {
            const worstRender = Math.max(0, ...r.renders)
            const fails = []
            if (percentile(r.renders, 0.95) > budget.render) fails.push("frames took " + percentile(r.renders, 0.95).toFixed(1) + " ms to draw (p95)")
            if (r.worstDropped > budget.dropped) fails.push(r.worstDropped + " late frames in one change")
            if (r.stalls.length) fails.push("stalls:" + r.stalls.join(","))
            if (fails.length) over += 1
            console.warn("FRAMES", r.name + ":", r.changes, "changes,", r.intervals.length, "frames;",
                         "interval median", percentile(r.intervals, 0.5).toFixed(1), "p95", percentile(r.intervals, 0.95).toFixed(1),
                         "worst", Math.max(0, ...r.intervals).toFixed(1), "ms; draw p95", percentile(r.renders, 0.95).toFixed(1),
                         "worst", worstRender.toFixed(1), "ms; late frames per change at most", r.worstDropped,
                         fails.length ? "OVER BUDGET: " + fails.join(", ") : "within budget")
        }
        Qt.exit(over ? 1 : 0)
    }

    Component.onCompleted: {
        if (!App.profiling) {
            console.warn("FRAMES needs SCANCORE_PROFILE=1")
            Qt.exit(2)
            return
        }
        App.setSetting("onboarded", true)
        App.setSetting("reducedMotion", false)
        const only = App.startAction.scenarios || ["retro", "welcome"]
        if (only.indexOf("retro") >= 0) retroTurns()
        if (only.indexOf("welcome") >= 0) { welcomeSwipes("standard"); welcomeSwipes("retro") }
        later(2000, () => next())
    }
}

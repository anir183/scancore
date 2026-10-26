import QtQuick
import Scancore

// The release packages, started once on each system (release.yml "smoke",
// and by hand: SCANCORE_START {"probe": this file, "fixtures": folder of
// tests/fixtures, "out": folder for the pictures}): a comic opens in the
// reader with its pages, a book in the novel reader with its text, and a
// video plays in the player (muted). A picture of each goes to `out`.
// Prints SMOKE lines and exits 0 when all three worked ("noPlayer": true
// leaves the player out).
Item {
    id: run
    readonly property string fixtures: App.startAction.fixtures || ""
    readonly property string out: App.startAction.out || ""
    property var results: []
    property var held: []

    function later(ms, fn) {
        const t = Qt.createQmlObject("import QtQuick; Timer {}", run)
        t.interval = ms
        t.triggered.connect(() => { t.destroy(); fn() })
        t.start()
    }
    function until(test, done, waited) {
        waited = waited || 0
        if (test() || waited > 30000) { done(test()); return }
        later(100, () => until(test, done, waited + 100))
    }
    function immersive() {
        for (const child of run.Window.window.contentItem.parent.children)
            if (child.edgeToEdge !== undefined && child.depth !== undefined) return child
        return null
    }
    function current() { const s = immersive(); return s ? s.currentItem : null }
    function file(name) { return "file://" + (fixtures.startsWith("/") ? "" : "/") + fixtures + "/" + name }
    function picture(name) {
        if (out) App.saveWindowImage(run.Window.window, out + "/" + name + ".png")
    }
    function report(name, ok, detail) {
        results.push({ name: name, ok: ok })
        console.warn("SMOKE", name, ok ? "ok" : "FAILED", detail || "")
    }
    // A task kept alive until it answers (Tasks are JavaScript-owned).
    function hold(task, done) {
        held.push(task)
        task.finished.connect(r => done(r, ""))
        task.failed.connect(e => done(null, e))
    }

    function comic(next) {
        const url = file("smoke/smoke-comic.cbz")
        hold(Comics.series(url, { title: "Smoke Comic", source: "local", sourceUrl: url, media: "comic" }), (series, error) => {
            if (!series || !series.chapters || series.chapters.length === 0) { report("comic", false, error || "no chapters"); next(); return }
            Router.openComicReader(series, series.chapters, 0)
            until(() => current() && current().panels && current().panels.length > 0, (ok) => {
                later(2500, () => {
                    picture("comic")
                    report("comic", ok, ok ? current().panels.length + " pages" : "the reader showed no pages")
                    Router.back()
                    later(1000, next)
                })
            })
        })
    }
    function novel(next) {
        const url = file("smoke/smoke-novel.epub")
        hold(Novels.book(url, { title: "Smoke Novel", source: "local", sourceUrl: url, media: "novel" }), (book, error) => {
            if (!book || !book.chapters || book.chapters.length === 0) { report("novel", false, error || "no chapters"); next(); return }
            Router.openNovelReader(book, book.chapters, 0)
            until(() => current() && current().blocks && current().blocks.length > 0, (ok) => {
                later(1500, () => {
                    picture("novel")
                    report("novel", ok, ok ? current().blocks.length + " blocks" : "the reader showed no text")
                    Router.back()
                    later(1000, next)
                })
            })
        })
    }
    Component {
        id: playerComponent
        Rectangle {
            anchors.fill: parent
            color: "black"
            z: 100
            property alias np: np
            NativePlayer { id: np; anchors.fill: parent }
        }
    }
    function player(next) {
        // ctest's run is off screen, where the player is off.
        if (App.startAction.noPlayer) { next(); return }
        if (!Anime.nativePlayerAvailable) { report("player", false, "no native player here (OpenGL)"); next(); return }
        const holder = playerComponent.createObject(run.Window.window.contentItem.parent)
        holder.np.player.muted = true
        holder.np.player.volume = 0
        holder.np.url = file("video/a.mp4")
        until(() => holder.np.player.loaded && (holder.np.player.position > 0.2 || holder.np.player.ended), (ok) => {
            picture("player")
            report("player", ok, ok ? "played to " + holder.np.player.position.toFixed(1) + " s" : "the video did not play")
            holder.destroy()
            later(500, next)
        })
    }

    Component.onCompleted: {
        App.setSetting("onboarded", true)
        App.setSetting("reducedMotion", true)
        later(2000, () => comic(() => novel(() => player(() => {
            const failed = results.filter(r => !r.ok).length
            console.warn("SMOKE", results.length - failed, "of", results.length, "worked")
            Qt.exit(failed ? 1 : 0)
        }))))
    }
}

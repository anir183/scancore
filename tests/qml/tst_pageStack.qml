import QtQuick
import QtQuick.Controls.Basic
import QtTest
import Scancore

// Pages are never left hidden by a page change: after every push, pop and
// replace (and when one cuts the last short) the current page is shown,
// flat, in place and drawn, in the Standard and the Retro look, with motion
// on. 2.12.1 left every section's first page and the first reader at
// opacity 0 (an Animator that never ran); 2.12.3's Retro turn could leave a
// page turned away.
Item {
    id: root
    width: 640; height: 480

    // Plain pages in colours the test can find on screen.
    Component {
        id: plain
        Rectangle { property string name; color: "#000000" }
    }
    Component { id: sectionStack; PageStack { anchors.fill: parent } }
    // The test module keeps every component in one folder (run.cmake).
    Component { id: firstPageStack; PageStack { anchors.fill: parent; initial: "Scancore/ComicsPage.qml" } }
    Component { id: readerStack; ImmersiveStack { anchors.fill: parent } }

    TestCase {
        name: "PagesShown"
        when: windowShown
        property var stack
        property int made: 0

        function page(colour) {
            made += 1
            return plain.createObject(null, { name: "page " + made, color: colour })
        }
        function settle() {
            tryVerify(() => !stack.busy, 4000, "the page change ends")
            // StackView hands out the last values a frame later.
            wait(40)
        }
        function flat(item) {
            return item.transform === undefined || item.transform.length === 0
        }
        function shown(what, colour) {
            settle()
            const item = stack.currentItem
            verify(item !== null, what + ": a page is current")
            verify(item.visible, what + ": the page is visible")
            fuzzyCompare(item.opacity, 1, 0.001, what + ": the page is opaque")
            compare(item.x, 0, what + ": the page is in place")
            compare(item.scale, 1, what + ": the page is at its size")
            verify(flat(item), what + ": the page is not turned")
            for (let i = 0; i < stack.depth; ++i) {
                const under = stack.get(i, StackView.DontLoad)
                if (under) verify(flat(under), what + ": page " + i + " is not left turned")
            }
            if (colour) {
                // Within a step or two of rounding: a page hidden, or another
                // one over it, is far off.
                const drawn = grabImage(root).pixel(root.width / 2, root.height / 2)
                const wanted = Qt.color(colour)
                const off = Math.max(Math.abs(drawn.r - wanted.r), Math.abs(drawn.g - wanted.g), Math.abs(drawn.b - wanted.b))
                verify(off < 4 / 255, what + ": the page is what is drawn (" + drawn + ", not " + wanted + ")")
            }
        }

        function looks() {
            return [
                { tag: "standard sections", look: "standard", stack: sectionStack },
                { tag: "retro sections", look: "retro", stack: sectionStack },
                { tag: "standard readers", look: "standard", stack: readerStack },
                { tag: "retro readers", look: "retro", stack: readerStack },
            ]
        }
        function init() {
            App.settings = { comicSource: "all", catalogComic: "comick", theme: "dark", reducedMotion: false, uiMode: "standard" }
        }
        function look(data) {
            App.setSetting("uiMode", data.look)
            compare(Theme.retro, data.look === "retro")
            compare(Motion.reduced, false)
            stack = createTemporaryObject(data.stack, root)
            verify(stack !== null)
        }

        function test_changes_data() { return looks() }
        function test_changes(data) {
            look(data)
            // The first page as a section's stack gets it (Immediate), and
            // one pushed onto the empty stack with a transition (a reader).
            stack.push(page("#204080"), {}, StackView.Immediate)
            shown("first page, immediate", "#204080")
            stack.clear()
            stack.push(page("#208040"))
            shown("first page, animated", "#208040")

            stack.push(page("#802040"))
            shown("push", "#802040")
            stack.pop()
            shown("pop", "#208040")
            stack.replace(page("#806020"))
            shown("replace", "#806020")
            stack.push(page("#406080"))
            stack.push(page("#604080"))
            shown("push twice", "#604080")
            stack.pop(null)
            shown("pop to the first", "#806020")
        }

        // A change while the last one still runs: the turn or slide it cut
        // short must not leave its page hidden or turned.
        function test_cutShort_data() { return looks() }
        function test_cutShort(data) {
            look(data)
            stack.push(page("#204080"), {}, StackView.Immediate)
            shown("first page", "#204080")
            stack.push(page("#802040"))
            wait(30)
            verify(stack.busy, "the push is still moving")
            stack.pop()
            shown("pop during a push", "#204080")
            stack.push(page("#208040"))
            wait(30)
            stack.push(page("#806020"))
            wait(30)
            stack.push(page("#406080"))
            shown("three quick pushes", "#406080")
            stack.pop()
            wait(30)
            stack.pop()
            shown("two quick pops", "#208040")
            stack.push(page("#604080"))
            wait(30)
            stack.replace(page("#604020"))
            shown("replace during a push", "#604020")
        }

        // A section's own first page, a real one, as Main's section stacks
        // open it; then a page over it and back.
        function test_sectionFirstPage_data() {
            return [ { tag: "standard", look: "standard", stack: firstPageStack },
                     { tag: "retro", look: "retro", stack: firstPageStack } ]
        }
        function test_sectionFirstPage(data) {
            look(data)
            shown("the section's first page")
            const first = stack.currentItem
            stack.push(page("#802040"))
            shown("a page over it", "#802040")
            stack.pop()
            shown("back to the first page")
            compare(stack.currentItem, first)
        }

        // The look changed with pages open (Settings → Interface → Look).
        function test_lookChanged_data() { return looks() }
        function test_lookChanged(data) {
            look(data)
            stack.push(page("#204080"), {}, StackView.Immediate)
            stack.push(page("#802040"))
            shown("before", "#802040")
            App.setSetting("uiMode", data.look === "retro" ? "standard" : "retro")
            stack.pop()
            shown("pop in the other look", "#204080")
            stack.push(page("#208040"))
            shown("push in the other look", "#208040")
        }
    }
}

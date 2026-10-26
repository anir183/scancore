import QtQuick
import QtQuick.Controls.Basic
import Scancore

// The stack readers and the player open on (Main's immersive layer): a fade
// with a slight zoom in, a fade out under the next one.
StackView {
    id: stack
    // Once nothing moves, the current page is shown at its size, however
    // the change ended (a pop cut short by the next one left it zoomed).
    function settle() {
        if (!currentItem) return
        currentItem.opacity = 1
        currentItem.scale = 1
    }
    onBusyChanged: if (!busy) settle()
    onCurrentItemChanged: if (!busy) settle()

    // NumberAnimations, not Animators: the first reader pushed onto the
    // empty stack gets the starting opacity without the animation
    // running, and an Animator never puts the end value back, so it
    // opened invisible (2.12.1).
    pushEnter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Motion.slow; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 1.03; to: 1; duration: Motion.page; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.decelerate }
        }
    }
    pushExit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: Motion.fast } }
    popEnter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Motion.normal } }
    popExit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; to: 0; duration: Motion.normal }
            NumberAnimation { property: "scale"; to: 1.02; duration: Motion.normal }
        }
    }
    replaceEnter: pushEnter
    replaceExit: pushExit
}

import QtQuick
import Scancore

// A screenshot of the app as SCANCORE_START asks ("probe": this file):
// {"settings": {...} applied first, "section": name, "wait": ms, "out": png}.
// For checking layouts on a scratch profile (tools/shot.sh).
Item {
    id: shot
    Component.onCompleted: {
        const s = App.startAction.settings || {}
        App.setSetting("onboarded", true)
        for (const key in s) App.setSetting(key, s[key])
        if (App.startAction.go) Router.go(App.startAction.go)
        const t = Qt.createQmlObject("import QtQuick; Timer {}", shot)
        t.interval = App.startAction.wait || 4000
        t.triggered.connect(() => {
            App.saveWindowImage(shot.Window.window, App.startAction.out)
            Qt.exit(0)
        })
        t.start()
    }
}

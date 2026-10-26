pragma Singleton
import QtQuick
QtObject {
    // run.cmake sets it to SCANCORE_LOOK_AS (the phone layout).
    property string platform: "linux"
    property var settings: ({comicSource: "all", catalogComic: "comick", theme: "dark", reducedMotion: true})
    signal siteChecked(string url)
    function setSetting(key, value) { const copy = Object.assign({}, settings); copy[key] = value; settings = copy }
    function dependOn() {}
    function icon(name) { return "" }
    function relativeTime(value) { return "" }
}

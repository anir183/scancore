pragma Singleton
import QtQuick
import Scancore

// What plays and what comes next. Songs are Music's song maps ({id, title,
// artists, audio, cover, anime, ...}); AudioPlayer plays them. The queue
// lives as long as the app, so a song keeps going while pages change.
QtObject {
    id: queue
    property var songs: []
    property int index: -1
    // "off", "all" (the queue again) or "one" (this song again).
    property string repeat: "off"
    property bool shuffle: false
    // Indexes in play order while shuffling.
    property var order: []
    readonly property var current: index >= 0 && index < songs.length ? songs[index] : null
    readonly property bool active: current !== null
    readonly property bool hasNext: songs.length > 0 && (repeat !== "off" || position(index) < songs.length - 1)
    readonly property bool hasPrevious: songs.length > 0 && position(index) > 0
    // A music extension's song: where it plays is asked for first.
    property var streamTask: null
    readonly property bool resolving: !!streamTask && streamTask.loading
    // Something is on its way: the address, or the first sound.
    readonly property bool busy: resolving || AudioPlayer.loading

    function artistsOf(song) { return song ? (song.artists || []).join(", ") : "" }

    // Plays `list` from `start`.
    function playList(list, start) {
        songs = Array.from(list || []).filter(s => s && (s.audio || s.source))
        if (songs.length === 0) return
        shuffleOrder()
        go(Math.max(0, Math.min(songs.length - 1, start || 0)))
    }
    function playSong(song) { playList([song], 0) }
    // After the one playing.
    function playNext(song) {
        if (!active) return playSong(song)
        const list = songs.slice()
        list.splice(index + 1, 0, song)
        songs = list
        shuffleOrder()
    }
    function addToQueue(song) {
        if (!active) return playSong(song)
        songs = songs.concat([song])
        shuffleOrder()
    }
    function removeAt(i) {
        if (i === index || i < 0 || i >= songs.length) return
        const list = songs.slice()
        list.splice(i, 1)
        if (i < index) index -= 1
        songs = list
        shuffleOrder()
    }
    function next() {
        if (songs.length === 0) return
        const at = position(index)
        if (at < songs.length - 1) go(order[at + 1])
        else if (repeat !== "off") go(order[0])
    }
    function previous() {
        // A few seconds in: the start of this one, as players do.
        if (AudioPlayer.position > 3 || !hasPrevious) {
            AudioPlayer.seek(0)
            return
        }
        go(order[position(index) - 1])
    }
    function clear() {
        streamTask = null
        AudioPlayer.stop()
        AudioPlayer.showControls("", "", "")
        songs = []
        index = -1
    }

    // ------------------------------------------------------------ inside
    function position(i) { const p = order.indexOf(i); return p < 0 ? i : p }
    function shuffleOrder() {
        const all = songs.map((_, i) => i)
        if (!shuffle) { order = all; return }
        // The one playing first, the rest in a new order.
        const rest = all.filter(i => i !== index)
        for (let i = rest.length - 1; i > 0; --i) {
            const j = Math.floor(Math.random() * (i + 1))
            const t = rest[i]; rest[i] = rest[j]; rest[j] = t
        }
        order = index >= 0 ? [index].concat(rest) : rest
    }
    onShuffleChanged: shuffleOrder()
    function go(i) {
        index = i
        const song = current
        if (!song) return
        streamTask = null
        if (song.audio) {
            AudioPlayer.load(song.audio, 0)
        } else {
            AudioPlayer.stop()
            const task = Music.streamOf(song)
            streamTask = task
            task.finished.connect(r => { if (queue.streamTask === task && queue.current === song) AudioPlayer.load(r.url, 0, r.headers) })
            task.failed.connect(e => { if (queue.streamTask === task) Router.toast(e, "error") })
        }
        AudioPlayer.showControls(song.title, artistsOf(song) || song.anime || song.album || "", song.cover || "", song.album || song.anime || "")
    }

    // Time listened, for Wrapped (Activity.listened): written every 15 s
    // while a song plays, the rest when it stops or changes.
    property real listenedFrom: 0
    property var listenedSong: null
    function countListening() {
        if (listenedFrom > 0 && listenedSong) {
            const seconds = Math.round((Date.now() - listenedFrom) / 1000)
            if (seconds > 0) Activity.listened(listenedSong, seconds)
        }
        const on = AudioPlayer.playing && current !== null
        listenedFrom = on ? Date.now() : 0
        listenedSong = on ? current : null
    }
    onCurrentChanged: countListening()
    property Timer listening: Timer {
        interval: 15000
        repeat: true
        running: AudioPlayer.playing && queue.current !== null
        onRunningChanged: queue.countListening()
        onTriggered: queue.countListening()
    }

    property Connections player: Connections {
        target: AudioPlayer
        function onFinished() {
            if (queue.repeat === "one") { AudioPlayer.seek(0); AudioPlayer.play(); return }
            if (queue.hasNext) queue.next()
        }
        function onNextRequested() { queue.next() }
        function onPreviousRequested() { queue.previous() }
    }
}

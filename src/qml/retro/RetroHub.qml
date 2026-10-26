import QtQuick
import QtQuick.Controls.Basic
import Scancore

// Home in the Retro mode: the panorama hub. Explore is a packed grid of
// tiles (what was pinned, or the library's newest, with the accent's action
// tiles between them), Recent and Library are the history and the library,
// and Comics, Novels and Anime are a few big links over what is new there.
Item {
    id: hub

    property var historyTask: Library.continueItems("", 24)
    property var libraryTask: Library.entries("")
    property var history: []
    property var library: []
    readonly property var updates: library.filter(e => e.newUnits > 0)
    Connections {
        target: hub.historyTask
        function onFinished(result) { hub.history = Router.keep(hub.history, result, hub.historyTask.digest) }
    }
    Connections {
        target: hub.libraryTask
        function onFinished(result) { hub.library = Router.keep(hub.library, result, hub.libraryTask.digest) }
    }
    // As on Home: out of sight, library changes are only noted.
    property bool libraryStale: false
    onVisibleChanged: {
        Retro.onHub = visible
        if (visible && libraryStale) { libraryStale = false; refresh.restart() }
        // Back from a page: the tiles swing in again.
        if (visible && turn > 0) turnIn.restart()
    }
    Connections {
        target: Library
        function onChanged() { if (hub.visible) refresh.restart(); else hub.libraryStale = true }
    }
    Timer {
        id: refresh
        interval: 400
        onTriggered: {
            hub.historyTask = Library.continueItems("", 24)
            hub.libraryTask = Library.entries("")
        }
    }

    // One clock for every live tile.
    property int beat: 0
    Timer {
        interval: 3200
        repeat: true
        running: hub.visible && Retro.liveTiles && !Motion.reduced && Qt.application.state === Qt.ApplicationActive
        onTriggered: hub.beat++
    }

    // Turnstile: the tiles swing away before a page opens and back in when
    // it closes.
    property real turn: 0
    property var pending: null
    function launch(run) {
        if (Motion.reduced) { run(); return }
        pending = run
        turnOut.restart()
    }
    SequentialAnimation {
        id: turnOut
        NumberAnimation { target: hub; property: "turn"; from: 0; to: 1; duration: Motion.normal; easing.type: Easing.InCubic }
        ScriptAction {
            script: {
                const run = hub.pending
                hub.pending = null
                if (run) run()
                // Nothing covered the hub (a dialog, the palette): back in.
                Qt.callLater(() => { if (hub.visible) turnIn.restart() })
            }
        }
    }
    NumberAnimation {
        id: turnIn
        target: hub
        property: "turn"
        to: 0
        duration: Motion.slow
        easing.type: Easing.OutCubic
    }

    // -------------------------------------------------------------- tiles

    function referer(it) {
        if (!it || !it.sourceUrl) return ""
        return it.media === "comic" ? Comics.imageReferer(it.sourceUrl) : it.media === "novel" ? Novels.imageReferer(it.sourceUrl) : ""
    }
    function unitWord(it) { return it.media === "anime" ? qsTr("Episode %1") : qsTr("Chapter %1") }

    function coverTile(entry, size, key, pinned, phase) {
        const it = entry || {}
        const back = it.newUnits > 0
            ? { label: it.title, detail: (it.newUnits === 1 ? qsTr("1 new") : qsTr("%1 new").arg(it.newUnits)) + (it.latestTitle ? "\n" + it.latestTitle : "") }
            : it.unitTitle || it.unitNumber ? { label: it.title, detail: it.unitTitle || unitWord(it).arg(it.unitNumber) } : null
        return { key: key, size: size, kind: "cover", item: it, pinned: pinned, phase: phase, label: it.title || "",
                 count: it.newUnits > 0 ? "+" + it.newUnits : (it.knownUnits > 0 ? String(it.knownUnits) : ""), back: back }
    }

    readonly property var exploreTiles: {
        const byId = {}
        for (const e of library) byId[e.id] = e
        const pins = Retro.pinList
        const covers = pins.map((p, i) => coverTile(byId[p.id] || p.item, p.size, "pin:" + p.id, true, i))
        // Topped up to four with the library's newest, so pinning one does
        // not empty the hub.
        if (covers.length < 4) {
            const pinned = {}
            for (const p of pins) pinned[p.id] = true
            const pool = updates.concat(library.filter(e => !(e.newUnits > 0))).filter(e => !pinned[e.id]).slice(0, 4 - covers.length)
            pool.forEach((e, i) => covers.push(coverTile(e, covers.length === 0 ? "medium" : "small", "auto:" + e.id, false, i)))
        }
        const out = []
        const take = (n) => { for (let i = 0; i < n && covers.length; ++i) out.push(covers.shift()) }
        take(2)
        out.push({ key: "random-comic", size: "medium", kind: "action", label: qsTr("read a random comic") })
        const last = history[0]
        if (last)
            out.push({ key: "continue", size: "medium", kind: "icon", icon: "eye", label: qsTr("continue"), item: last, phase: 2,
                       back: { label: last.media === "anime" ? unitWord(last).arg(last.unitNumber || last.unitKey) : (last.unitTitle || ""),
                               detail: last.title } })
        take(2)
        out.push({ key: "random-genre", size: "medium", kind: "action", label: qsTr("explore a random genre") })
        while (covers.length) take(1)
        out.push({ key: "search", size: "small", kind: "icon", icon: "search", label: qsTr("search") })
        out.push({ key: "downloads", size: "small", kind: "icon", icon: "download", label: qsTr("downloads"), count: Downloads.pending })
        out.push({ key: "history", size: "small", kind: "icon", icon: "history", label: qsTr("history") })
        out.push({ key: "settings", size: "small", kind: "icon", icon: "settings", label: qsTr("settings") })
        const year = Router.wrappedYear()
        if (year > 0)
            out.push({ key: "wrapped", size: "wide", kind: "icon", icon: "sparkles", label: qsTr("your %1 wrapped").arg(year) })
        return out
    }

    function pick(list) { return list.length ? list[Math.floor(Math.random() * list.length)] : null }

    property var latestForRandom: null
    Connections {
        target: hub.latestForRandom
        function onFinished(result) {
            const it = hub.pick(result.updates || [])
            hub.latestForRandom = null
            if (it) hub.launch(() => Router.openSeries(it, null))
            else Router.toast(qsTr("Nothing to pick from yet"), "info")
        }
        function onFailed(error) { hub.latestForRandom = null; Router.toast(error, "error") }
    }
    function randomComic() {
        const own = library.filter(e => e.media === "comic" && e.category !== "completed" && e.category !== "dropped")
        const it = pick(own)
        if (it) launch(() => Router.openSeries(it, null))
        else latestForRandom = Comics.latest(30)
    }

    function openTile(t) {
        switch (t.key) {
        case "continue": launch(() => Router.resume(t.item)); return
        case "random-comic": randomComic(); return
        case "random-genre": {
            const g = pick(Array.from(Anime.genres()))
            launch(() => Router.openAnimeBrowse(g ? { genres: [g] } : {}))
            return
        }
        case "search": Router.shell.openPalette(); return
        case "wrapped": Router.openWrapped(Router.wrappedYear()); return
        case "downloads": case "history": case "settings": launch(() => Router.go(t.key)); return
        }
        if (t.item) launch(() => Router.openSeries(t.item, null))
    }

    ActionMenu { id: tileMenu }
    function holdTile(t, item) {
        if (t.kind !== "cover" || !t.item || !t.item.id) return
        const id = t.item.id
        const sizes = [["small", qsTr("Small")], ["medium", qsTr("Medium")], ["wide", qsTr("Wide")]]
        const list = []
        if (t.pinned) {
            for (const [value, label] of sizes)
                list.push({ text: label, icon: "layout-grid", checked: t.size === value, run: () => Retro.resize(id, value) })
            list.push(null)
            list.push({ text: qsTr("Move earlier"), icon: "chevron-left", run: () => Retro.move(id, -1) })
            list.push({ text: qsTr("Move later"), icon: "chevron-right", run: () => Retro.move(id, 1) })
            list.push({ text: qsTr("Unpin"), icon: "pin-off", danger: true, run: () => Retro.unpin(id) })
        } else {
            list.push({ text: qsTr("Pin to the hub"), icon: "pin", run: () => Retro.pin(t.item, t.size) })
        }
        tileMenu.openAt(item, item.width / 2, item.height / 2, list)
    }

    // -------------------------------------------------------------- hub

    readonly property var backgroundEntry: history.length ? history[0] : (library.length ? library[0] : null)

    Panorama {
        id: pano
        anchors.fill: parent
        focus: true
        title: Retro.title
        gradientTitle: Retro.gradientTitle
        upperCase: Retro.upperCase
        sections: Retro.sectionList
        background: Retro.background === "recent" && hub.backgroundEntry
            ? ((hub.backgroundEntry.extra && hub.backgroundEntry.extra.banner) || hub.backgroundEntry.cover || "") : ""
        backgroundReferer: hub.referer(hub.backgroundEntry)
        pages: ({ explore: explorePage, recent: recentPage, library: libraryPage,
                  comics: comicsPage, novels: novelsPage, anime: animePage, music: musicPage })
        onCurrentKeyChanged: {
            Retro.hubSection = currentKey
            rememberSection.restart()
        }
        onHeaderClicked: (key) => {
            if (key === "recent") hub.launch(() => Router.go("history"))
            else if (key !== "explore") hub.launch(() => Router.go(key))
        }
        Component.onCompleted: {
            const saved = App.settings.retroSection
            if (saved) Qt.callLater(() => pano.goKey(saved))
            Retro.hubSection = currentKey
        }
    }

    // Reopening the hub returns to the section left last (saved once the
    // panning stops).
    Timer {
        id: rememberSection
        interval: 1500
        onTriggered: App.rememberSetting("retroSection", pano.currentKey)
    }

    // Room under every section for the app bar's button.
    readonly property int bottomRoom: 24

    Component {
        id: explorePage
        Item {
            property string sectionKey
            property bool current
            SmoothFlickable {
                anchors.fill: parent
                contentHeight: tiles.height + hub.bottomRoom
                TileGrid {
                    id: tiles
                    width: parent.width
                    height: implicitHeight
                    columns: width >= 520 ? 6 : 4
                    gap: Theme.compact ? 8 : 10
                    tiles: hub.exploreTiles
                    beat: hub.beat
                    live: Retro.liveTiles
                    turn: hub.turn
                    onClicked: (t) => hub.openTile(t)
                    onHeld: (t, item) => hub.holdTile(t, item)
                }
            }
        }
    }

    // A two-line row: a square picture, the title, and a line in the accent.
    component RetroRow: AbstractButton {
        id: rowButton
        property var entry: ({})
        property string detail: ""
        width: ListView.view ? ListView.view.width : 300
        height: 76
        background: null
        scale: pressed && !Motion.reduced ? 0.98 : 1
        Behavior on scale { NumberAnimation { duration: Motion.instant } }
        contentItem: Item {
            Cover {
                id: thumb
                width: 64
                height: 64
                anchors.verticalCenter: parent.verticalCenter
                radius: 0
                source: rowButton.entry.cover || ""
                referer: hub.referer(rowButton.entry)
                title: rowButton.entry.title || ""
            }
            Column {
                anchors.left: thumb.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                Text {
                    width: parent.width
                    text: rowButton.entry.title || ""
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXl
                    font.weight: 350
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: rowButton.detail
                    color: Theme.tile
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                }
            }
        }
    }

    Component {
        id: recentPage
        Item {
            property string sectionKey
            property bool current
            ListView {
                anchors.fill: parent
                clip: true
                maximumFlickVelocity: Motion.flickVelocity
                flickDeceleration: Motion.flickDeceleration
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                bottomMargin: hub.bottomRoom
                model: hub.history
                WheelScroll {}
                delegate: RetroRow {
                    required property var modelData
                    entry: modelData
                    detail: modelData.media === "anime" ? hub.unitWord(modelData).arg(modelData.unitNumber || modelData.unitKey)
                        : (modelData.unitTitle || "")
                    onClicked: hub.launch(() => Router.resume(modelData))
                }
                Text {
                    visible: hub.history.length === 0 && hub.historyTask && !hub.historyTask.loading
                    width: parent.width
                    text: qsTr("What you read and watch shows up here.")
                    color: Theme.textMuted
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textLg
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    Component {
        id: libraryPage
        Item {
            property string sectionKey
            property bool current
            GridView {
                id: shelf
                anchors.fill: parent
                clip: true
                maximumFlickVelocity: Motion.flickVelocity
                flickDeceleration: Motion.flickDeceleration
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                readonly property int columns: Math.max(3, Math.floor(width / 150))
                cellWidth: Math.floor(width / columns)
                cellHeight: Math.round(cellWidth * 1.3)
                bottomMargin: hub.bottomRoom
                model: hub.library
                WheelScroll {}
                delegate: Item {
                    required property var modelData
                    required property int index
                    width: shelf.cellWidth
                    height: shelf.cellHeight
                    Tile {
                        id: libraryTile
                        anchors.fill: parent
                        anchors.rightMargin: 8
                        anchors.bottomMargin: 8
                        tile: hub.coverTile(parent.modelData, "medium", "lib:" + parent.modelData.id, false, parent.index % 3)
                        beat: hub.beat
                        live: false
                        onClicked: hub.launch(() => Router.openSeries(parent.modelData, null))
                        onHeld: hub.holdTile(tile, libraryTile)
                    }
                }
                Text {
                    visible: hub.library.length === 0 && hub.libraryTask && !hub.libraryTask.loading
                    width: parent.width
                    text: qsTr("Series you save show up here.")
                    color: Theme.textMuted
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textLg
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    // Comics, Novels, Anime: big links, then what is new there as small tiles.
    component MediaPage: Item {
        id: media
        property string sectionKey
        property bool current
        property var links: []
        property string shelfTitle: ""
        property var items: []
        property bool loading: false
        property string error: ""
        signal retry()

        SmoothFlickable {
            anchors.fill: parent
            contentHeight: mediaColumn.height + hub.bottomRoom
            Column {
                id: mediaColumn
                width: parent.width
                spacing: 4
                Repeater {
                    model: media.links
                    AbstractButton {
                        id: link
                        required property var modelData
                        width: mediaColumn.width
                        height: linkText.implicitHeight + 10
                        background: null
                        onClicked: hub.launch(modelData.run)
                        contentItem: Text {
                            id: linkText
                            text: link.modelData.label
                            color: link.pressed ? Theme.tile : Theme.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.compact ? 26 : 28
                            font.weight: Font.Light
                            elide: Text.ElideRight
                        }
                    }
                }
                Item { width: 1; height: 14 }
                Text {
                    visible: media.shelfTitle.length > 0
                    text: media.shelfTitle
                    color: Theme.textMuted
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                    font.capitalization: Retro.upperCase ? Font.AllUppercase : Font.AllLowercase
                }
                Item { width: 1; height: 8 }
                Spinner { visible: media.loading && media.items.length === 0 }
                Text {
                    visible: media.error.length > 0 && media.items.length === 0
                    width: parent.width
                    text: media.error
                    color: Theme.textMuted
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    wrapMode: Text.Wrap
                    TapHandler { onTapped: media.retry() }
                }
                Grid {
                    id: covers
                    columns: 3
                    spacing: 8
                    readonly property real cell: Math.floor((mediaColumn.width - 2 * spacing) / 3)
                    Repeater {
                        // A few, as small tiles: the page itself has the rest.
                        model: media.items.slice(0, 12)
                        Tile {
                            required property var modelData
                            required property int index
                            width: covers.cell
                            height: Math.round(covers.cell * 1.3)
                            tile: ({ key: "new:" + index, size: "medium", kind: "cover", item: modelData, label: "" })
                            live: false
                            onClicked: hub.launch(() => Router.openSeries(modelData, null))
                        }
                    }
                }
            }
        }
    }

    Component {
        id: comicsPage
        MediaPage {
            id: comics
            property var task: Comics.latest(30)
            links: [{ label: qsTr("all comics"), run: () => Router.go("comics") },
                    { label: qsTr("coming up"), run: () => Router.openUpcoming() },
                    { label: qsTr("extensions"), run: () => Router.openExtensions() }]
            shelfTitle: qsTr("Latest chapters")
            items: task && task.ok ? task.result.updates : []
            loading: task ? task.loading : false
            error: task && !task.loading ? task.error : ""
            onRetry: task = Comics.latest(30)
        }
    }
    Component {
        id: novelsPage
        MediaPage {
            property var task: Novels.discover()
            links: [{ label: qsTr("all novels"), run: () => Router.go("novels") },
                    { label: qsTr("downloads"), run: () => Router.go("downloads") }]
            shelfTitle: task && task.ok && task.result.shelves.length ? task.result.shelves[0].title : qsTr("Novels")
            items: task && task.ok && task.result.shelves.length ? task.result.shelves[0].books : []
            loading: task ? task.loading : false
            error: task && !task.loading ? task.error : ""
            onRetry: task = Novels.discover()
        }
    }
    Component {
        id: animePage
        MediaPage {
            property var task: Anime.home()
            links: [{ label: qsTr("all anime"), run: () => Router.go("anime") },
                    { label: qsTr("schedule"), run: () => Router.openAnimeSchedule() },
                    { label: qsTr("browse"), run: () => Router.openAnimeBrowse({}) }]
            shelfTitle: qsTr("Trending")
            items: task && task.ok ? task.result.trending : []
            loading: task ? task.loading : false
            error: task && !task.loading ? task.error : ""
            onRetry: task = Anime.home()
        }
    }
    // Music: what is playing, and this season's anime songs to start one.
    Component {
        id: musicPage
        Item {
            id: music
            property string sectionKey
            property bool current
            property var task: Music.animeSongsAvailable ? Music.seasonSongs() : null
            readonly property var songs: {
                if (!task || !task.ok) return []
                const out = []
                for (const a of task.result) for (const s of a.songs) out.push(s)
                return out
            }
            SmoothFlickable {
                anchors.fill: parent
                contentHeight: musicColumn.height + hub.bottomRoom
                Column {
                    id: musicColumn
                    width: parent.width
                    spacing: 4
                    Repeater {
                        model: [{ label: qsTr("all music"), run: () => Router.go("music") }].concat(
                            MusicQueue.active ? [{ label: qsTr("now playing"), run: () => Router.openNowPlaying() }] : [])
                        AbstractButton {
                            id: musicLink
                            required property var modelData
                            width: musicColumn.width
                            height: musicLinkText.implicitHeight + 10
                            background: null
                            onClicked: hub.launch(modelData.run)
                            contentItem: Text {
                                id: musicLinkText
                                text: musicLink.modelData.label
                                color: musicLink.pressed ? Theme.tile : Theme.text
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.compact ? 26 : 28
                                font.weight: Font.Light
                                elide: Text.ElideRight
                            }
                        }
                    }
                    Item { width: 1; height: 14 }
                    Text {
                        visible: music.songs.length > 0
                        text: qsTr("This season")
                        color: Theme.textMuted
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.DemiBold
                        font.capitalization: Retro.upperCase ? Font.AllUppercase : Font.AllLowercase
                    }
                    Spinner { visible: !!music.task && music.task.loading }
                    Repeater {
                        // A few: the Music page has the rest.
                        model: music.songs.slice(0, 10)
                        SongRow {
                            required property var modelData
                            required property int index
                            x: -8
                            width: musicColumn.width + 8
                            song: modelData
                            onPlayed: MusicQueue.playList(music.songs, index)
                        }
                    }
                }
            }
        }
    }
}

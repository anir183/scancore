import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Scancore

// SCANCORE Wrapped: the year as a story, one slide at a time across the whole
// window. Tap the right of the screen (or press →) for the next slide and the
// left for the previous one; holding pauses it. Slides with nothing to show
// are left out, and the last one is a card that can be saved as a picture.
// Customise (the sliders button) picks the style, the slides, whether titles
// and covers show, and what the card holds; it is kept in the `wrapped`
// setting. Only the slide on screen exists, and nothing is drawn with effects.
Item {
    id: page
    property int year: new Date().getFullYear()
    readonly property bool edgeToEdge: true
    readonly property real insetTop: SafeArea.margins.top
    readonly property real insetBottom: SafeArea.margins.bottom
    focus: true

    property var task: Activity.year(year)
    property var stats: null
    Connections {
        target: page.task
        function onFinished(result) { page.stats = result }
    }

    readonly property var monthNames: [qsTr("January"), qsTr("February"), qsTr("March"), qsTr("April"), qsTr("May", "month"), qsTr("June"), qsTr("July"), qsTr("August"),
                                       qsTr("September"), qsTr("October"), qsTr("November"), qsTr("December")]
    readonly property var dayNames: [qsTr("Mondays"), qsTr("Tuesdays"), qsTr("Wednesdays"), qsTr("Thursdays"), qsTr("Fridays"), qsTr("Saturdays"), qsTr("Sundays")]
    readonly property var dayLetters: [qsTr("M", "Monday"), qsTr("T", "Tuesday"), qsTr("W", "Wednesday"), qsTr("T", "Thursday"), qsTr("F", "Friday"), qsTr("S", "Saturday"), qsTr("S", "Sunday")]

    function number(n) { return Number(n || 0).toLocaleString(Qt.locale(), "f", 0) }
    function hours(seconds) { return Math.floor((seconds || 0) / 3600) }
    function timeText(seconds) {
        const h = hours(seconds)
        if (h >= 1) return number(h) + (h === 1 ? qsTr(" hour") : qsTr(" hours"))
        const m = Math.round((seconds || 0) / 60)
        return number(m) + (m === 1 ? qsTr(" minute") : qsTr(" minutes"))
    }
    function unitsText(units, media) {
        return number(units) + (media === "anime" ? (units === 1 ? qsTr(" episode") : qsTr(" episodes"))
                                                  : (units === 1 ? qsTr(" chapter") : qsTr(" chapters")))
    }
    function daysText(days) { return number(days) + (days === 1 ? qsTr(" day") : qsTr(" days")) }
    function indexOfMax(list, value) {
        let best = -1, bestValue = 0
        list.forEach((x, i) => { const v = value(x); if (v > bestValue) { bestValue = v; best = i } })
        return best
    }
    // A day and month in the app's language (Qt.formatDate follows the system's).
    function dayMonth(d) { return qsTr("%1 %2", "day, month").replace("%1", d.getDate()).replace("%2", monthNames[d.getMonth()]) }
    function dateText(iso) { return iso ? dayMonth(new Date(iso + "T12:00:00")) : "" }

    // ------------------------------------------------------------ choices
    // {style, hidden: [slide], private, card: {format, stats: [key], covers, name}}
    readonly property var prefs: App.settings.wrapped || ({})
    function pref(key, fallback) { return prefs[key] !== undefined ? prefs[key] : fallback }
    function setPref(key, value) {
        const next = Object.assign({}, App.plain(prefs))
        next[key] = value
        App.setSetting("wrapped", next)
    }
    function cardPref(key, fallback) { const c = prefs.card || {}; return c[key] !== undefined ? c[key] : fallback }
    function setCardPref(key, value) {
        const c = Object.assign({}, App.plain(prefs.card || {}))
        c[key] = value
        setPref("card", c)
    }
    readonly property string style: pref("style", "vivid")
    readonly property bool hideTitles: pref("private", false) === true
    readonly property var hiddenSlides: Array.from(pref("hidden", []))
    readonly property string cardFormat: cardPref("format", "story")
    readonly property var cardStats: Array.from(cardPref("stats", ["time", "chapters", "episodes", "streak", "genre", "month"]))
    readonly property bool cardCovers: cardPref("covers", true) === true
    readonly property bool cardName: cardPref("name", false) === true && Account.signedIn

    readonly property var mediaLabels: ({ comic: qsTr("A comic"), novel: qsTr("A novel"), anime: qsTr("An anime"), music: qsTr("A song") })
    function titleOf(item) { return !item ? "" : hideTitles ? (mediaLabels[item.media] || qsTr("A series")) : item.title }
    function coverOf(item) { return !item || hideTitles ? "" : item.cover || "" }

    readonly property bool empty: stats !== null && stats.total.seconds === 0 && stats.total.units === 0 && !(stats.music && stats.music.seconds > 0)
    readonly property int busiestMonth: stats ? indexOfMax(stats.months, m => m.seconds + m.units * 600) : -1
    readonly property int favouriteDay: stats ? indexOfMax(stats.weekdays, s => s) : -1
    readonly property var mediaUsed: stats ? ["comic", "novel", "anime"].filter(m => stats.topByMedia[m].length > 0) : []
    readonly property bool musicUsed: !!stats && !!stats.music && stats.music.seconds >= 600 && stats.music.top.length > 0

    // Every slide this year has something for, in order; `slides` leaves
    // out the ones hidden in Customise (never the opening or the card).
    readonly property var available: {
        if (!stats || empty) return []
        const out = ["intro"]
        if (stats.total.mediaSeconds > 0) out.push("time")
        if (stats.total.units > 0) out.push("counts")
        if (stats.top.length > 0) out.push("top")
        if (mediaUsed.length > 1) out.push("media")
        if (stats.genres.length > 0) out.push("genres")
        if (stats.author && stats.author.name) out.push("author")
        if (musicUsed) out.push("music")
        if (busiestMonth >= 0) out.push("rhythm")
        if (stats.activeDays > 0) out.push("streak")
        if (stats.first && stats.first.title) out.push("first")
        out.push("card")
        return out
    }
    readonly property var slides: available.filter(s => s === "intro" || s === "card" || hiddenSlides.indexOf(s) < 0)
    readonly property var slideNames: ({
        time: qsTr("Time spent"), counts: qsTr("Chapters and episodes"), top: qsTr("Top series"), media: qsTr("Number one of each"),
        genres: qsTr("Genres"), author: qsTr("Author"), music: qsTr("Music"), rhythm: qsTr("Busiest month"), streak: qsTr("Streak"),
        first: qsTr("Where it started")
    })
    property int current: 0
    readonly property string slide: slides[current] || ""
    readonly property bool last: current >= slides.length - 1
    property bool held: false
    // Customise open.
    property alias customising: sheet.visible
    onSlidesChanged: if (current >= slides.length) current = Math.max(0, slides.length - 1)

    // ------------------------------------------------------------ colours
    // Each style gives every slide {a, b} (top and bottom), the ink, a
    // softer ink and a highlight. Retro draws them flat.
    readonly property var vivid: ({
        intro: ["#4b1fd1", "#12082e"], time: ["#e2471f", "#3a0c05"], counts: ["#0d9488", "#032420"],
        top: ["#c0267a", "#2a0619"], media: ["#2563eb", "#0a1636"], genres: ["#d99a06", "#2e1e01"],
        author: ["#7c3aed", "#1a0a33"], music: ["#16a34a", "#04210f"], rhythm: ["#0891b2", "#03202a"],
        streak: ["#dc2626", "#2c0606"], first: ["#4f46e5", "#0d0b2b"], card: ["#27272f", "#0b0b0e"]
    })
    readonly property var vividHighlights: ({
        intro: "#ffd84d", time: "#ffe08a", counts: "#9bfff0", top: "#ffb3dc", media: "#a9c8ff", genres: "#fff2b0",
        author: "#e2ccff", music: "#a7f3c0", rhythm: "#a5f0ff", streak: "#ffd0a0", first: "#c9c6ff", card: "#ffd84d"
    })
    readonly property var order: ["intro", "time", "counts", "top", "media", "genres", "author", "music", "rhythm", "streak", "first", "card"]
    readonly property color accentBase: Theme.accentIsMono ? Qt.color("#7c5cff") : Qt.color(Theme.accent)
    function paletteFor(name, which) {
        const i = Math.max(0, order.indexOf(name))
        const accent = accentBase
        switch (which || style) {
        case "accent": {
            const h = accent.hslHue < 0 ? 0.7 : accent.hslHue
            const hue = (h + (i % 4 - 1.5) * 0.035 + 1) % 1
            return { a: Qt.hsla(hue, Math.max(0.45, accent.hslSaturation), 0.42, 1), b: Qt.hsla(hue, 0.5, 0.09, 1),
                     ink: "#ffffff", soft: Qt.rgba(1, 1, 1, 0.74), hi: Qt.hsla(hue, 1, 0.82, 1) }
        }
        case "midnight":
            return { a: "#17171f", b: "#060608", ink: "#ffffff", soft: Qt.rgba(1, 1, 1, 0.66), hi: Qt.lighter(accent, 1.3) }
        case "paper":
            return { a: "#f7f3ea", b: "#ebe3d3", ink: "#17130e", soft: Qt.rgba(0.09, 0.07, 0.05, 0.66),
                     hi: Theme.accentIsMono ? Qt.color("#c2410c") : Qt.darker(accent, 1.25) }
        default: {
            const pair = vivid[name] || vivid.card
            return { a: pair[0], b: pair[1], ink: "#ffffff", soft: Qt.rgba(1, 1, 1, 0.75), hi: vividHighlights[name] || "#ffd84d" }
        }
        }
    }
    readonly property var colours: paletteFor(slide || "intro")
    readonly property color ink: colours.ink
    readonly property color soft: colours.soft
    readonly property color hi: colours.hi
    readonly property bool flat: Theme.retro
    function tone(alpha) { return Qt.rgba(ink.r, ink.g, ink.b, alpha) }

    function next() {
        if (!last) current += 1
    }
    function previous() {
        if (current > 0) current -= 1
        else clock.restart()
    }
    onCurrentChanged: last ? clock.stop() : clock.restart()

    // How far through the slide on screen; it moves on by itself.
    property real progress: 0
    NumberAnimation {
        id: clock
        target: page
        property: "progress"
        from: 0
        to: 1
        duration: page.slide === "intro" ? 4500 : 7000
        running: page.slides.length > 0 && !page.last
        paused: running && (page.held || sheet.visible || Qt.application.state !== Qt.ApplicationActive)
        onFinished: page.next()
    }

    Keys.onPressed: (event) => {
        if (sheet.visible) {
            if (event.key === Qt.Key_Escape) { sheet.visible = false; event.accepted = true }
            return
        }
        if (event.key === Qt.Key_Right || event.key === Qt.Key_Space || event.key === Qt.Key_PageDown) { page.next(); event.accepted = true }
        else if (event.key === Qt.Key_Left || event.key === Qt.Key_PageUp) { page.previous(); event.accepted = true }
        else if (event.key === Qt.Key_Escape) { Router.back(); event.accepted = true }
    }

    // ------------------------------------------------------------ backdrop
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: page.colours.a; Behavior on color { ColorAnimation { duration: Motion.slow } } }
            GradientStop { position: 1; color: page.flat ? page.colours.a : page.colours.b; Behavior on color { ColorAnimation { duration: Motion.slow } } }
        }
    }
    // A ring and a disc drifting about two corners, in the slide's colours.
    Item {
        id: shapes
        anchors.fill: parent
        visible: !page.flat
        clip: true
        property real drift: 0
        NumberAnimation on drift {
            running: shapes.visible && !Motion.reduced && Qt.application.state === Qt.ApplicationActive
            from: 0; to: Math.PI * 2; duration: 40000; loops: Animation.Infinite
        }
        Rectangle {
            readonly property real size: Math.max(page.width, page.height) * 0.75
            width: size; height: size; radius: size / 2
            x: page.width - size * 0.55 + Math.sin(shapes.drift) * 30
            y: -size * 0.45 + Math.cos(shapes.drift) * 24
            color: "transparent"
            border.width: size * 0.12
            border.color: page.tone(0.06)
        }
        Rectangle {
            readonly property real size: Math.max(page.width, page.height) * 0.5
            width: size; height: size; radius: size / 2
            x: -size * 0.4 + Math.cos(shapes.drift) * 26
            y: page.height - size * 0.55 + Math.sin(shapes.drift) * 20
            color: Qt.rgba(page.hi.r, page.hi.g, page.hi.b, 0.1)
            Behavior on color { ColorAnimation { duration: Motion.slow } }
        }
    }
    // Retro: the slide's name across the top, bigger than the screen, as a
    // pivot's header.
    Text {
        visible: page.flat && page.slide !== ""
        x: 12
        y: page.insetTop + 52
        text: {
            const name = page.slide === "intro" ? qsTr("wrapped") : page.slide === "card" ? String(page.year) : (page.slideNames[page.slide] || "")
            return Retro.upperCase ? name.toUpperCase() : name.toLowerCase()
        }
        color: page.tone(0.16)
        font.family: Theme.fontDisplay
        font.pixelSize: Math.min(page.width / 4.2, 120)
        font.weight: Font.Light
        wrapMode: Text.NoWrap
    }

    // Taps: the left third goes back, the rest forward; holding pauses.
    TapHandler {
        enabled: page.slides.length > 0 && !sheet.visible
        onPressedChanged: page.held = pressed
        onTapped: (point) => {
            if (point.position.x < page.width / 3) page.previous()
            else page.next()
        }
    }

    // The bars along the top, one per slide, Customise and close.
    Item {
        id: top
        x: 16
        y: page.insetTop + 12
        width: page.width - 32
        height: 40
        z: 2
        Row {
            id: bars
            width: parent.width - close.width - customise.width - 16
            spacing: 4
            Repeater {
                model: page.slides.length
                Rectangle {
                    required property int index
                    width: (bars.width - (page.slides.length - 1) * 4) / Math.max(1, page.slides.length)
                    height: 3
                    radius: page.flat ? 0 : 1.5
                    color: page.tone(0.28)
                    Rectangle {
                        height: parent.height
                        radius: parent.radius
                        color: page.ink
                        width: parent.width * (index < page.current ? 1 : index > page.current ? 0 : page.last ? 1 : page.progress)
                    }
                }
            }
        }
        Text {
            anchors.top: bars.bottom
            anchors.topMargin: 10
            text: qsTr("SCANCORE Wrapped ") + page.year
            color: page.soft
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            font.weight: Font.DemiBold
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 1.2
        }
        IconButton {
            id: customise
            anchors.right: close.left
            anchors.rightMargin: 4
            y: -8
            visible: page.slides.length > 0
            iconName: "sliders-horizontal"
            iconColor: page.ink
            tip: qsTr("Customise")
            onClicked: sheet.visible = true
        }
        IconButton {
            id: close
            anchors.right: parent.right
            y: -8
            iconName: "x"
            iconColor: page.ink
            tip: qsTr("Close")
            onClicked: Router.back()
        }
    }

    // Loading, or a year with nothing in it.
    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 420)
        spacing: 12
        visible: page.slides.length === 0
        Spinner { anchors.horizontalCenter: parent.horizontalCenter; visible: page.stats === null }
        Text {
            width: parent.width
            visible: page.empty
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("Nothing yet in ") + page.year
            color: page.ink
            font.family: Theme.fontDisplay
            font.pixelSize: Theme.text2xl
            font.weight: Font.Bold
        }
        Text {
            width: parent.width
            visible: page.empty
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("Chapters you finish and time spent reading, watching and listening show up here.")
            color: page.soft
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
    }

    // The slide on screen.
    Loader {
        id: stage
        anchors.fill: parent
        // Retro: under the pivot's header.
        anchors.topMargin: page.insetTop + 64 + (page.flat ? Math.min(page.width / 4.2, 120) * 0.75 : 0)
        anchors.bottomMargin: page.insetBottom + 24
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        active: page.slide !== ""
        sourceComponent: ({
            intro: introSlide, time: timeSlide, counts: countsSlide, top: topSlide, media: mediaSlide, genres: genresSlide,
            author: authorSlide, music: musicSlide, rhythm: rhythmSlide, streak: streakSlide, first: firstSlide, card: cardSlide
        })[page.slide] || null
        onLoaded: entrance.restart()
        ParallelAnimation {
            id: entrance
            NumberAnimation { target: stage.item; property: "opacity"; from: 0; to: 1; duration: Motion.slow; easing.type: Easing.OutCubic }
            NumberAnimation { target: stage.item; property: "y"; from: 24; to: 0; duration: Motion.page; easing.type: Easing.OutCubic }
        }
    }

    // ------------------------------------------------------------ pieces
    component Kicker: Text {
        width: parent ? parent.width : implicitWidth
        wrapMode: Text.Wrap
        color: page.soft
        font.family: Theme.fontSans
        font.pixelSize: Theme.textLg
        font.weight: Font.Medium
        font.capitalization: page.flat && Retro.upperCase ? Font.AllUppercase : Font.MixedCase
    }
    component Big: Text {
        width: parent ? parent.width : implicitWidth
        wrapMode: Text.Wrap
        color: page.ink
        font.family: Theme.fontDisplay
        font.pixelSize: Math.min(page.width / 6, 88)
        font.weight: page.flat ? Font.Light : Font.Bold
        font.letterSpacing: page.flat ? -0.5 : -1.5
        lineHeight: 0.95
    }
    component Line: Text {
        width: parent ? parent.width : implicitWidth
        wrapMode: Text.Wrap
        color: page.ink
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXl
        font.weight: Font.DemiBold
    }
    // A number that counts up to `value` as its slide opens.
    component CountUp: Big {
        id: count
        property real value: 0
        property real shown: Motion.reduced ? value : 0
        text: page.number(Math.round(shown))
        color: page.hi
        NumberAnimation on shown { running: !Motion.reduced; from: 0; to: count.value; duration: 1100; easing.type: Easing.OutCubic }
    }
    // A bar that grows to `fill` (0..1) once its slide is up.
    component Bar: Rectangle {
        id: bar
        property real fill: 0
        property int delay: 0
        property color ink: page.ink
        property real grown: Motion.reduced ? 1 : 0
        height: 10
        radius: page.flat ? 0 : height / 2
        color: page.tone(0.16)
        SequentialAnimation on grown {
            running: !Motion.reduced
            PauseAnimation { duration: 200 + bar.delay }
            NumberAnimation { from: 0; to: 1; duration: 800; easing.type: Easing.OutCubic }
        }
        Rectangle {
            height: parent.height
            radius: parent.radius
            color: bar.ink
            width: Math.max(parent.height, parent.width * bar.fill * bar.grown)
        }
    }
    // A column of the slide's content, centred in the window and no wider than a phone held sideways.
    // Taller than the stage, it starts at the top instead of spilling over it.
    component Body: Column {
        y: parent ? Math.max(0, (parent.height - height) / 2) : 0
        anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
        width: Math.min(parent ? parent.width : 0, 560)
        spacing: 14
    }
    component Heading: Text {
        width: parent ? parent.width : implicitWidth
        color: Theme.textMuted
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1
        topPadding: 6
    }
    component OptionRow: Item {
        property alias label: switchLabel.text
        property alias detail: switchDetail.text
        property alias checked: switchToggle.checked
        signal toggled(bool on)
        width: parent ? parent.width : 0
        height: Math.max(switchText.height, switchToggle.height)
        Column {
            id: switchText
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - switchToggle.width - 12
            Text { id: switchLabel; width: parent.width; wrapMode: Text.Wrap; color: Theme.text; font.family: Theme.fontSans; font.pixelSize: Theme.textMd; font.weight: Font.DemiBold }
            Text { id: switchDetail; width: parent.width; visible: text !== ""; wrapMode: Text.Wrap; color: Theme.textMuted; font.family: Theme.fontSans; font.pixelSize: Theme.textSm }
        }
        Toggle {
            id: switchToggle
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            onToggled: parent.toggled(checked)
        }
    }
    component SmallCover: Cover {
        property var entry
        radius: page.flat ? 0 : Theme.radiusMd
        source: page.coverOf(entry)
        // Too small for a title to be read.
        title: width >= 80 ? page.titleOf(entry) : ""
        tint: page.tone(0.12)
    }

    // ------------------------------------------------------------ slides
    Component {
        id: introSlide
        Item {
            id: intro
            // The year's own covers, tilted and drifting, under a shade.
            readonly property var covers: {
                if (page.hideTitles) return []
                const out = [], seen = new Set()
                const add = (it) => { if (it && it.cover && !seen.has(it.cover)) { seen.add(it.cover); out.push(it.cover) } }
                page.stats.top.forEach(add)
                for (const m of ["comic", "novel", "anime"]) page.stats.topByMedia[m].forEach(add)
                if (page.stats.music) page.stats.music.top.forEach(add)
                return out
            }
            Item {
                id: mosaic
                visible: intro.covers.length >= 3
                x: -stage.anchors.leftMargin
                y: -stage.anchors.topMargin
                width: page.width
                height: page.height
                clip: true
                readonly property real cell: Math.max(92, Math.min(150, page.width / 4))
                readonly property int columns: Math.ceil(width / cell) + 2
                property real drift: 0
                NumberAnimation on drift { running: mosaic.visible && !Motion.reduced; from: 0; to: -mosaic.cell * 1.45 - 10; duration: 30000; loops: Animation.Infinite }
                Grid {
                    x: (mosaic.width - width) / 2
                    y: -mosaic.cell + mosaic.drift
                    columns: mosaic.columns
                    spacing: 10
                    rotation: -10
                    Repeater {
                        model: mosaic.visible ? mosaic.columns * (Math.ceil(mosaic.height / (mosaic.cell * 1.45)) + 4) : 0
                        Cover {
                            required property int index
                            width: mosaic.cell
                            height: mosaic.cell * 1.45
                            radius: page.flat ? 0 : Theme.radiusMd
                            source: intro.covers[(index * 7 + Math.floor(index / 5)) % intro.covers.length]
                            tint: page.tone(0.1)
                        }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop { position: 0; color: Qt.rgba(page.colours.b.r, page.colours.b.g, page.colours.b.b, 0.82) }
                        GradientStop { position: 0.3; color: Qt.rgba(page.colours.b.r, page.colours.b.g, page.colours.b.b, 0.6) }
                        GradientStop { position: 0.6; color: Qt.rgba(page.colours.b.r, page.colours.b.g, page.colours.b.b, 0.88) }
                        GradientStop { position: 1; color: page.colours.b }
                    }
                }
            }
            Body {
                spacing: 8
                // The mark is twice as wide as it is high.
                Image {
                    source: App.icon("scancore-mark", page.ink)
                    sourceSize: Qt.size(96 * Screen.devicePixelRatio, 48 * Screen.devicePixelRatio)
                    width: 96
                    height: 48
                    fillMode: Image.PreserveAspectFit
                }
                Item { width: 1; height: 14 }
                Text {
                    text: qsTr("Wrapped")
                    color: page.hi
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.Bold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 3
                }
                Big { text: String(page.year); font.pixelSize: Math.min(page.width / 3, 200); lineHeight: 0.85 }
                Kicker { text: qsTr("Your year in comics, novels, anime and music") }
                Item { width: 1; height: 20 }
                Rectangle {
                    width: beginLabel.implicitWidth + 28
                    height: 36
                    radius: page.flat ? 0 : 18
                    color: page.tone(0.14)
                    Text {
                        id: beginLabel
                        anchors.centerIn: parent
                        text: Theme.touch ? qsTr("Tap to begin") : qsTr("Click or press → to begin")
                        color: page.ink
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: Font.DemiBold
                    }
                }
            }
        }
    }

    Component {
        id: timeSlide
        Item {
            id: timeRoot
            readonly property var byMedia: page.stats.total.byMedia
            readonly property real maxSeconds: Math.max(1, byMedia.comic, byMedia.novel, byMedia.anime)
            readonly property real days: page.stats.total.mediaSeconds / 86400
            readonly property bool inHours: page.hours(page.stats.total.mediaSeconds) >= 1
            Body {
                Kicker { text: qsTr("This year you spent") }
                Row {
                    width: parent.width
                    spacing: 12
                    CountUp {
                        id: hoursCount
                        width: implicitWidth
                        wrapMode: Text.NoWrap
                        value: timeRoot.inHours ? page.hours(page.stats.total.mediaSeconds) : Math.round(page.stats.total.mediaSeconds / 60)
                        font.pixelSize: Math.min(page.width / 4.2, 150)
                    }
                    Text {
                        anchors.baseline: hoursCount.baseline
                        text: timeRoot.inHours ? qsTr("hours") : qsTr("minutes")
                        color: page.ink
                        font.family: Theme.fontDisplay
                        font.pixelSize: Theme.text2xl
                        font.weight: Font.Bold
                    }
                }
                Kicker { text: qsTr("reading and watching") }
                Kicker {
                    visible: timeRoot.days >= 1
                    text: qsTr("That's %1 days without a break.").arg(timeRoot.days.toLocaleString(Qt.locale(), "f", 1))
                    color: page.hi
                    font.weight: Font.DemiBold
                }
                Item { width: 1; height: 14 }
                Repeater {
                    model: [{ key: "comic", label: qsTr("Comics") }, { key: "novel", label: qsTr("Novels") }, { key: "anime", label: qsTr("Anime") }]
                    Column {
                        required property var modelData
                        required property int index
                        width: parent.width
                        spacing: 6
                        visible: timeRoot.byMedia[modelData.key] >= 30
                        Item {
                            width: parent.width
                            height: label.height
                            Text {
                                id: label
                                text: modelData.label
                                color: page.ink
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            Text {
                                anchors.right: parent.right
                                text: page.timeText(timeRoot.byMedia[modelData.key])
                                color: page.soft
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                            }
                        }
                        Bar {
                            width: parent.width
                            delay: index * 120
                            fill: timeRoot.byMedia[modelData.key] / timeRoot.maxSeconds
                        }
                    }
                }
            }
        }
    }

    Component {
        id: countsSlide
        Item {
            readonly property var t: page.stats.total
            Body {
                spacing: 12
                Kicker { text: qsTr("You finished") }
                Repeater {
                    model: [
                        { value: t.comic, label: qsTr("comic chapters"), icon: "book-open" },
                        { value: t.novel, label: qsTr("novel chapters"), icon: "book-text" },
                        { value: t.anime, label: qsTr("anime episodes"), icon: "tv" }
                    ].filter(x => x.value > 0)
                    Rectangle {
                        required property var modelData
                        width: parent.width
                        height: tileRow.height + 28
                        radius: page.flat ? 0 : Theme.radiusLg
                        color: page.tone(page.flat ? 0.14 : 0.1)
                        Row {
                            id: tileRow
                            x: 18
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 36
                            spacing: 16
                            Icon { anchors.verticalCenter: parent.verticalCenter; name: modelData.icon; size: 28; color: page.hi }
                            Column {
                                width: parent.width - 44
                                CountUp { value: modelData.value; font.pixelSize: Math.min(page.width / 8, 64) }
                                Line { text: modelData.label; color: page.soft; font.pixelSize: Theme.textMd }
                            }
                        }
                    }
                }
                Item { width: 1; height: 4 }
                Kicker {
                    visible: page.stats.series > 0
                    text: page.stats.series === 1 ? qsTr("in one series") : qsTr("across %1 series").arg(page.number(page.stats.series))
                }
            }
        }
    }

    Component {
        id: topSlide
        Item {
            readonly property var first: page.stats.top[0]
            readonly property var rest: page.stats.top.slice(1, 5)
            readonly property bool wide: width > height * 1.1
            Body {
                Kicker { text: qsTr("The series you came back to most") }
                Flow {
                    width: parent.width
                    spacing: 20
                    Item {
                        width: heroCover.width + 30
                        height: heroCover.height
                        SmallCover {
                            id: heroCover
                            x: 30
                            entry: first
                            width: wide ? Math.min(200, page.width * 0.3) : Math.min(page.width * 0.48, 200, (page.height - 460) / 1.45)
                            height: width * 1.45
                            radius: page.flat ? 0 : Theme.radiusLg
                        }
                        // The rank over the cover's corner.
                        Text {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: -heroCover.height * 0.08
                            text: "1"
                            color: page.hi
                            font.family: Theme.fontDisplay
                            font.pixelSize: heroCover.height * 0.5
                            font.weight: Font.Black
                        }
                    }
                    Column {
                        width: wide ? parent.width - heroCover.width - 50 : parent.width
                        spacing: 8
                        Line { text: page.titleOf(first); font.pixelSize: Theme.text2xl; font.family: Theme.fontDisplay; font.weight: Font.Bold; maximumLineCount: 3; elide: Text.ElideRight }
                        Kicker { text: page.unitsText(first.units, first.media); color: page.hi }
                        Item { width: 1; height: 6; visible: rest.length > 0 }
                        Repeater {
                            model: rest
                            Row {
                                required property var modelData
                                required property int index
                                width: parent.width
                                height: 52
                                spacing: 12
                                Text {
                                    width: 20
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: index + 2
                                    color: page.soft
                                    font.family: Theme.fontDisplay
                                    font.pixelSize: Theme.textXl
                                    font.weight: Font.Bold
                                }
                                SmallCover { entry: modelData; width: 34; height: 48; radius: page.flat ? 0 : 6 }
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 20 - 34 - 24
                                    Text {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        text: page.titleOf(modelData)
                                        color: page.ink
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textMd
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        text: page.unitsText(modelData.units, modelData.media)
                                        color: page.soft
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textSm
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: mediaSlide
        Item {
            id: mediaRoot
            readonly property var labels: ({ comic: qsTr("Comic"), novel: qsTr("Novel"), anime: qsTr("Anime") })
            Body {
                Kicker { text: qsTr("Your number one of each") }
                Row {
                    id: tops
                    width: parent.width
                    spacing: 14
                    readonly property real column: (width - (page.mediaUsed.length - 1) * spacing) / page.mediaUsed.length
                    Repeater {
                        model: page.mediaUsed
                        Column {
                            required property string modelData
                            required property int index
                            readonly property var item: page.stats.topByMedia[modelData][0]
                            width: tops.column
                            spacing: 8
                            Text {
                                text: mediaRoot.labels[modelData]
                                color: page.hi
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                                font.weight: Font.Bold
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 1.1
                            }
                            SmallCover {
                                entry: item
                                width: Math.min(parent.width, 180, (page.height - 360) / 1.45)
                                height: width * 1.45
                                rotation: Motion.reduced || page.flat ? 0 : (index - (page.mediaUsed.length - 1) / 2) * 3
                            }
                            Text {
                                width: parent.width
                                text: page.titleOf(item)
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                color: page.ink
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: page.unitsText(item.units, item.media)
                                color: page.soft
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: genresSlide
        Item {
            id: genresRoot
            readonly property real maxUnits: Math.max(1, page.stats.genres[0].units)
            Body {
                spacing: 14
                Kicker { text: qsTr("The genres you lived in") }
                Big { text: page.stats.genres[0].name; color: page.hi; font.pixelSize: Math.min(page.width / 7, 80) }
                Item { width: 1; height: 6 }
                Repeater {
                    model: page.stats.genres
                    Column {
                        required property var modelData
                        required property int index
                        width: parent.width
                        spacing: 6
                        Item {
                            width: parent.width
                            height: name.height
                            Text {
                                id: name
                                width: parent.width - count.width - 12
                                elide: Text.ElideRight
                                text: (index + 1) + ".  " + modelData.name
                                color: page.ink
                                font.family: Theme.fontSans
                                font.pixelSize: index === 0 ? Theme.textLg : Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            Text {
                                id: count
                                anchors.right: parent.right
                                text: page.number(modelData.units)
                                color: page.soft
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                            }
                        }
                        Bar {
                            width: parent.width
                            height: index === 0 ? 12 : 8
                            delay: index * 110
                            ink: index === 0 ? page.hi : page.ink
                            fill: modelData.units / genresRoot.maxUnits
                        }
                    }
                }
            }
        }
    }

    Component {
        id: authorSlide
        Item {
            Body {
                Text {
                    text: "“"
                    height: 64
                    color: page.hi
                    font.family: Theme.fontDisplay
                    font.pixelSize: 120
                    font.weight: Font.Black
                }
                Kicker { text: qsTr("The author you read most") }
                Big { text: page.stats.author.name; font.pixelSize: Math.min(page.width / 8, 72) }
                Kicker { text: page.unitsText(page.stats.author.units, "comic"); color: page.hi; font.weight: Font.DemiBold }
            }
        }
    }

    Component {
        id: musicSlide
        Item {
            id: musicRoot
            readonly property var music: page.stats.music
            Body {
                Kicker { text: qsTr("You listened for") }
                Big { text: page.timeText(musicRoot.music.seconds); color: page.hi; font.pixelSize: Math.min(page.width / 7, 80) }
                Kicker {
                    visible: musicRoot.music.songs > 1
                    text: qsTr("to %1 songs").arg(page.number(musicRoot.music.songs))
                }
                Item { width: 1; height: 10 }
                Repeater {
                    model: musicRoot.music.top
                    Row {
                        required property var modelData
                        required property int index
                        width: parent.width
                        height: 56
                        spacing: 12
                        Text {
                            width: 20
                            anchors.verticalCenter: parent.verticalCenter
                            text: index + 1
                            color: index === 0 ? page.hi : page.soft
                            font.family: Theme.fontDisplay
                            font.pixelSize: Theme.textXl
                            font.weight: Font.Bold
                        }
                        SmallCover {
                            entry: ({ title: modelData.title, cover: modelData.cover, media: "music" })
                            width: 48
                            height: 48
                            radius: page.flat ? 0 : 6
                        }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 20 - 48 - 24
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: page.hideTitles ? page.mediaLabels.music : modelData.title
                                color: page.ink
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                visible: !page.hideTitles
                                text: modelData.artist || ""
                                color: page.soft
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                    }
                }
                Kicker {
                    visible: !!musicRoot.music.artist && !page.hideTitles
                    text: musicRoot.music.artist ? qsTr("and the artist you played most: %1").arg(musicRoot.music.artist.name) : ""
                }
            }
        }
    }

    Component {
        id: rhythmSlide
        Item {
            id: rhythmRoot
            readonly property real maxMonth: Math.max(1, ...page.stats.months.map(m => m.seconds))
            readonly property real maxDay: Math.max(1, ...page.stats.weekdays)
            Body {
                Kicker { text: qsTr("Your busiest month was") }
                Big { text: page.monthNames[page.busiestMonth]; color: page.hi }
                Item {
                    id: chart
                    width: parent.width
                    height: Math.min(150, page.height * 0.2)
                    Row {
                        anchors.fill: parent
                        spacing: 5
                        Repeater {
                            model: page.stats.months
                            Item {
                                id: month
                                required property var modelData
                                required property int index
                                width: (chart.width - 11 * 5) / 12
                                height: chart.height
                                property real grown: Motion.reduced ? 1 : 0
                                SequentialAnimation on grown {
                                    running: !Motion.reduced
                                    PauseAnimation { duration: 150 + month.index * 45 }
                                    NumberAnimation { from: 0; to: 1; duration: 600; easing.type: Easing.OutBack }
                                }
                                Rectangle {
                                    anchors.bottom: initial.top
                                    anchors.bottomMargin: 6
                                    width: parent.width
                                    height: Math.max(3, (parent.height - 24) * modelData.seconds / rhythmRoot.maxMonth * month.grown)
                                    radius: page.flat ? 0 : 3
                                    color: month.index === page.busiestMonth ? page.hi : page.tone(0.32)
                                }
                                Text {
                                    id: initial
                                    anchors.bottom: parent.bottom
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: page.monthNames[month.index].slice(0, 1)
                                    color: page.soft
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textXs
                                }
                            }
                        }
                    }
                }
                Item { width: 1; height: 8 }
                Kicker {
                    visible: page.favouriteDay >= 0
                    text: page.favouriteDay >= 0 ? qsTr("and you read and watched most on ") + page.dayNames[page.favouriteDay] + "." : ""
                }
                // The week, each day's dot as big as its share.
                Row {
                    visible: page.favouriteDay >= 0
                    spacing: (parent.width - 7 * 40) / 6
                    Repeater {
                        model: page.stats.weekdays
                        Column {
                            required property var modelData
                            required property int index
                            width: 40
                            spacing: 4
                            Item {
                                width: 40
                                height: 40
                                Rectangle {
                                    anchors.centerIn: parent
                                    readonly property real size: 10 + 30 * modelData / rhythmRoot.maxDay
                                    width: size
                                    height: size
                                    radius: page.flat ? 0 : size / 2
                                    color: index === page.favouriteDay ? page.hi : page.tone(0.3)
                                }
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: page.dayLetters[index]
                                color: page.soft
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: streakSlide
        Item {
            id: streakRoot
            readonly property int streak: page.stats.longestStreak
            Body {
                Kicker { text: qsTr("Your longest streak") }
                Row {
                    width: parent.width
                    spacing: 12
                    CountUp { id: streakCount; width: implicitWidth; wrapMode: Text.NoWrap; value: streakRoot.streak; font.pixelSize: Math.min(page.width / 4, 150) }
                    Text {
                        anchors.baseline: streakCount.baseline
                        text: streakRoot.streak === 1 ? qsTr("day") : qsTr("days")
                        color: page.ink
                        font.family: Theme.fontDisplay
                        font.pixelSize: Theme.text2xl
                        font.weight: Font.Bold
                    }
                }
                Kicker { text: qsTr("in a row, of %1 days with something read or watched").arg(page.number(page.stats.activeDays)) }
                // The streak, a square a day (up to two months' worth).
                Flow {
                    width: parent.width
                    spacing: 4
                    Repeater {
                        model: Math.min(60, streakRoot.streak)
                        Rectangle {
                            id: dayMark
                            required property int index
                            width: 12
                            height: 12
                            radius: page.flat ? 0 : 3
                            color: page.hi
                            opacity: Motion.reduced ? 1 : 0
                            SequentialAnimation on opacity {
                                running: !Motion.reduced
                                PauseAnimation { duration: 200 + dayMark.index * 18 }
                                NumberAnimation { to: 1; duration: 200 }
                            }
                        }
                    }
                }
                Item { width: 1; height: 8; visible: page.stats.bestDay !== "" }
                Line {
                    visible: page.stats.bestDay !== "" && page.stats.bestDaySeconds > 0
                    text: qsTr("Your biggest day: ") + page.dateText(page.stats.bestDay) + ", " + page.timeText(page.stats.bestDaySeconds)
                }
            }
        }
    }

    Component {
        id: firstSlide
        Item {
            readonly property var first: page.stats.first
            Body {
                Kicker { text: qsTr("Where your year started") }
                SmallCover {
                    entry: first
                    width: Math.min(parent.width * 0.45, 180, (page.height - 380) / 1.45)
                    height: width * 1.45
                    radius: page.flat ? 0 : Theme.radiusLg
                    rotation: Motion.reduced || page.flat ? 0 : -3
                }
                Line { text: page.titleOf(first); font.pixelSize: Theme.text2xl; font.family: Theme.fontDisplay; font.weight: Font.Bold; maximumLineCount: 3; elide: Text.ElideRight }
                Kicker { text: qsTr("the first series you finished something in, on ") + page.dayMonth(new Date(first.firstAt * 1000)) }
            }
        }
    }

    // ------------------------------------------------------------ the card
    // Everything a card can show; Customise picks which (up to six).
    readonly property var cardChoices: [
        { key: "time", label: qsTr("Time") }, { key: "chapters", label: qsTr("Chapters") }, { key: "episodes", label: qsTr("Episodes") },
        { key: "series", label: qsTr("Series") }, { key: "streak", label: qsTr("Longest streak") }, { key: "days", label: qsTr("Active days") },
        { key: "genre", label: qsTr("Top genre") }, { key: "month", label: qsTr("Busiest month") }, { key: "author", label: qsTr("Top author") },
        { key: "music", label: qsTr("Music") }
    ]
    function cardValue(key) {
        const s = stats
        switch (key) {
        case "time": return timeText(s.total.mediaSeconds)
        case "chapters": return number(s.total.comic + s.total.novel)
        case "episodes": return number(s.total.anime)
        case "series": return number(s.series)
        case "streak": return daysText(s.longestStreak)
        case "days": return number(s.activeDays)
        case "genre": return s.genres.length ? s.genres[0].name : "—"
        case "month": return busiestMonth >= 0 ? monthNames[busiestMonth] : "—"
        case "author": return s.author && s.author.name ? s.author.name : "—"
        case "music": return s.music && s.music.seconds > 0 ? timeText(s.music.seconds) : "—"
        }
        return ""
    }
    readonly property var cardSizes: ({ story: Qt.size(360, 640), square: Qt.size(400, 400), wide: Qt.size(640, 360) })

    Component {
        id: cardSlide
        Item {
            id: cardStage
            readonly property size size: page.cardSizes[page.cardFormat] || page.cardSizes.story
            readonly property bool wideCard: page.cardFormat === "wide"
            // The card is drawn at its own size and scaled to fit; the
            // picture is taken from the card itself, at three times that.
            readonly property real fit: Math.min(1, width / size.width, (height - actions.height - 20) / size.height)
            readonly property var shownStats: page.cardStats.filter(k => page.cardChoices.some(c => c.key === k))
                                                           .slice(0, page.cardFormat === "square" ? 4 : 6)
            readonly property var topCovers: page.cardCovers && !page.hideTitles
                ? page.stats.top.filter(t => !!t.cover).slice(0, 3) : []
            readonly property var cardColours: page.paletteFor("intro")
            Item {
                id: holder
                anchors.horizontalCenter: parent.horizontalCenter
                width: card.width * cardStage.fit
                height: card.height * cardStage.fit
                Rectangle {
                    id: card
                    width: cardStage.size.width
                    height: cardStage.size.height
                    scale: cardStage.fit
                    transformOrigin: Item.TopLeft
                    radius: page.flat ? 0 : 26
                    clip: true
                    // Paper: the card on paper needs its edge.
                    border.width: page.style === "paper" ? 1 : 0
                    border.color: Qt.rgba(0, 0, 0, 0.12)
                    readonly property color ink: cardStage.cardColours.ink
                    readonly property color soft: cardStage.cardColours.soft
                    readonly property color hi: cardStage.cardColours.hi
                    gradient: Gradient {
                        GradientStop { position: 0; color: cardStage.cardColours.a }
                        GradientStop { position: 1; color: page.flat ? cardStage.cardColours.a : cardStage.cardColours.b }
                    }
                    // The big year in the corner.
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: -10
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: -card.height * 0.06
                        text: String(page.year)
                        color: Qt.rgba(card.ink.r, card.ink.g, card.ink.b, 0.08)
                        font.family: Theme.fontDisplay
                        font.pixelSize: Math.min(card.width, card.height) * 0.42
                        font.weight: Font.Black
                    }
                    Item {
                        id: cardInner
                        x: 28
                        y: 26
                        width: card.width - 56
                        height: card.height - 52
                        Row {
                            id: cardHead
                            spacing: 10
                            Image {
                                anchors.verticalCenter: parent.verticalCenter
                                source: App.icon("scancore-mark", card.ink)
                                // Three times over: the picture is saved at 3x.
                                sourceSize: Qt.size(144, 72)
                                width: 48
                                height: 24
                                fillMode: Image.PreserveAspectFit
                            }
                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                Text {
                                    text: qsTr("Wrapped %1").arg(page.year)
                                    color: card.ink
                                    font.family: Theme.fontDisplay
                                    font.pixelSize: 20
                                    font.weight: Font.Bold
                                }
                                Text {
                                    visible: page.cardName
                                    text: "@" + (Account.user.username || "")
                                    color: card.soft
                                    font.family: Theme.fontSans
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                        // Covers: across the top (story, square) or down the
                        // left (wide), and the stats beside or under them.
                        Row {
                            id: coverRow
                            anchors.top: cardHead.bottom
                            anchors.topMargin: 18
                            visible: cardStage.topCovers.length > 0
                            spacing: page.cardFormat === "story" ? -18 : 10
                            x: page.cardFormat === "story" ? (cardInner.width - width) / 2 : 0
                            readonly property real coverWidth: cardStage.wideCard ? 84 : page.cardFormat === "story" ? 118
                                : (cardInner.width - (cardStage.topCovers.length - 1) * 10) / Math.max(1, cardStage.topCovers.length)
                            Repeater {
                                model: cardStage.topCovers
                                Cover {
                                    required property var modelData
                                    required property int index
                                    // A story fans them out, the first on top.
                                    readonly property bool fan: page.cardFormat === "story" && cardStage.topCovers.length === 3 && !page.flat
                                    z: fan && index === 1 ? 0 : fan ? (index === 0 ? 2 : 1) : 0
                                    rotation: fan ? (index - 1) * 7 : 0
                                    y: fan && index !== 1 ? 14 : 0
                                    width: coverRow.coverWidth
                                    height: Math.min(width * 1.45, page.cardFormat === "square" ? 112 : 190)
                                    radius: page.flat ? 0 : 10
                                    source: modelData.cover || ""
                                    title: modelData.title
                                    tint: Qt.rgba(card.ink.r, card.ink.g, card.ink.b, 0.12)
                                }
                            }
                        }
                        // A story keeps its stats low, over the year.
                        Column {
                            id: cardStats
                            readonly property bool low: page.cardFormat === "story"
                            // Wide: beside the covers.
                            readonly property bool beside: cardStage.wideCard && coverRow.visible
                            anchors.top: low ? undefined : coverRow.visible && !cardStage.wideCard ? coverRow.bottom : cardHead.bottom
                            anchors.topMargin: beside ? 18 : 16
                            anchors.bottom: low ? footer.top : undefined
                            anchors.bottomMargin: 28
                            x: beside ? coverRow.width + 24 : 0
                            width: cardInner.width - x
                            spacing: page.cardFormat === "story" ? 18 : 12
                            Text {
                                width: parent.width
                                visible: page.stats.top.length > 0
                                text: qsTr("Top series: ") + page.titleOf(page.stats.top[0])
                                elide: Text.ElideRight
                                color: card.ink
                                font.family: cardStats.low ? Theme.fontDisplay : Theme.fontSans
                                font.pixelSize: cardStats.low ? 17 : 14
                                font.weight: cardStats.low ? Font.Bold : Font.DemiBold
                            }
                            Grid {
                                columns: cardStage.wideCard && !cardStats.beside ? 3 : 2
                                columnSpacing: 16
                                rowSpacing: page.cardFormat === "story" ? 18 : 10
                                Repeater {
                                    model: cardStage.shownStats
                                    Column {
                                        required property string modelData
                                        readonly property int columns: cardStage.wideCard && !cardStats.beside ? 3 : 2
                                        width: (cardStats.width - (columns - 1) * 16) / columns
                                        spacing: 2
                                        Text {
                                            text: (page.cardChoices.find(c => c.key === modelData) || {}).label || ""
                                            color: card.soft
                                            font.family: Theme.fontSans
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            font.capitalization: Font.AllUppercase
                                            font.letterSpacing: 1
                                        }
                                        Text {
                                            width: parent.width
                                            elide: Text.ElideRight
                                            text: page.cardValue(modelData)
                                            color: card.hi
                                            font.family: Theme.fontDisplay
                                            font.pixelSize: page.cardFormat === "story" ? 24 : 20
                                            font.weight: Font.Bold
                                        }
                                    }
                                }
                            }
                        }
                        Text {
                            id: footer
                            anchors.bottom: parent.bottom
                            text: "scancore.onl"
                            color: card.soft
                            font.family: Theme.fontSans
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
            Row {
                id: actions
                anchors.top: holder.bottom
                anchors.topMargin: 20
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                ScButton {
                    text: qsTr("Save picture")
                    iconName: "download"
                    variant: "primary"
                    onClicked: saveDialog.open()
                }
                ScButton {
                    text: qsTr("Customise")
                    iconName: "sliders-horizontal"
                    onClicked: sheet.visible = true
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "rotate-ccw"
                    iconColor: page.ink
                    tip: qsTr("Watch again")
                    onClicked: page.current = 0
                }
            }
            FileDialog {
                id: saveDialog
                fileMode: FileDialog.SaveFile
                defaultSuffix: "png"
                nameFilters: [qsTr("PNG picture (*.png)")]
                selectedFile: "scancore-wrapped-" + page.year + ".png"
                onAccepted: {
                    const file = selectedFile
                    card.grabToImage((result) => {
                        const saved = Activity.saveImage(result.image, file)
                        Router.toast(saved ? qsTr("Picture saved") : qsTr("Could not save the picture"), saved ? "success" : "error")
                    }, Qt.size(card.width * 3, card.height * 3))
                }
            }
        }
    }

    // ------------------------------------------------------------ customise
    Rectangle {
        id: sheet
        visible: false
        anchors.fill: parent
        z: 5
        color: Qt.rgba(0, 0, 0, 0.5)
        onVisibleChanged: if (visible) sheetBody.forceActiveFocus()
        TapHandler { onTapped: sheet.visible = false }
        Rectangle {
            id: sheetBody
            readonly property bool side: page.width >= 720
            width: side ? 420 : page.width
            // On phones the card stays in sight above it.
            height: side ? page.height : Math.min(page.height * 0.62, options.height + 32 + page.insetBottom)
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            color: Theme.surface
            radius: side || page.flat ? 0 : Theme.radiusLg
            focus: true
            Keys.onEscapePressed: sheet.visible = false
            // Taps inside stay inside.
            TapHandler {}
            Flickable {
                id: sheetFlick
                anchors.fill: parent
                anchors.topMargin: (sheetBody.side ? page.insetTop : 0) + 16
                anchors.bottomMargin: page.insetBottom + 16
                contentHeight: options.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: options
                    x: 20
                    width: sheetFlick.width - 40
                    spacing: 14
                    Item {
                        width: parent.width
                        height: 40
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 48
                            elide: Text.ElideRight
                            text: qsTr("Customise Wrapped")
                            color: Theme.text
                            font.family: Theme.fontDisplay
                            font.pixelSize: Theme.textXl
                            font.weight: Font.Bold
                        }
                        IconButton {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            iconName: "x"
                            tip: qsTr("Close")
                            onClicked: sheet.visible = false
                        }
                    }
                    Heading { text: qsTr("Style") }
                    Flow {
                        width: parent.width
                        spacing: 8
                        Repeater {
                            model: [{ value: "vivid", label: qsTr("Vivid") }, { value: "accent", label: qsTr("Your accent") },
                                    { value: "midnight", label: qsTr("Midnight") }, { value: "paper", label: qsTr("Paper") }]
                            Chip {
                                required property var modelData
                                text: modelData.label
                                selected: page.style === modelData.value
                                dot: page.paletteFor("intro", modelData.value).a
                                onClicked: page.setPref("style", modelData.value)
                            }
                        }
                    }
                    OptionRow {
                        label: qsTr("Hide titles and covers")
                        detail: qsTr("For sharing: series show as “A comic”, “An anime”.")
                        checked: page.hideTitles
                        onToggled: (on) => page.setPref("private", on)
                    }
                    Heading { text: qsTr("Slides") }
                    Flow {
                        width: parent.width
                        spacing: 8
                        Repeater {
                            model: page.available.filter(s => s !== "intro" && s !== "card")
                            Chip {
                                required property string modelData
                                text: page.slideNames[modelData] || modelData
                                selected: page.hiddenSlides.indexOf(modelData) < 0
                                iconName: selected ? "check" : ""
                                onClicked: {
                                    const hidden = page.hiddenSlides.slice()
                                    const at = hidden.indexOf(modelData)
                                    if (at >= 0) hidden.splice(at, 1)
                                    else hidden.push(modelData)
                                    page.setPref("hidden", hidden)
                                }
                            }
                        }
                    }
                    Heading { text: qsTr("Card") }
                    Segmented {
                        width: parent.width
                        fill: true
                        options: [{ value: "story", label: qsTr("Story") }, { value: "square", label: qsTr("Square") }, { value: "wide", label: qsTr("Wide") }]
                        value: page.cardFormat
                        onPicked: (v) => page.setCardPref("format", v)
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: page.cardFormat === "square" ? qsTr("What it shows: the first four you pick.") : qsTr("What it shows: up to six.")
                        color: Theme.textMuted
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Flow {
                        width: parent.width
                        spacing: 8
                        Repeater {
                            model: page.cardChoices
                            Chip {
                                required property var modelData
                                readonly property int at: page.cardStats.indexOf(modelData.key)
                                text: modelData.label
                                selected: at >= 0
                                detail: at >= 0 ? String(at + 1) : ""
                                onClicked: {
                                    const list = page.cardStats.slice()
                                    if (at >= 0) list.splice(at, 1)
                                    else {
                                        if (list.length >= 6) list.shift()
                                        list.push(modelData.key)
                                    }
                                    page.setCardPref("stats", list)
                                }
                            }
                        }
                    }
                    OptionRow {
                        label: qsTr("Covers of the top series")
                        checked: page.cardCovers
                        onToggled: (on) => page.setCardPref("covers", on)
                    }
                    OptionRow {
                        visible: Account.signedIn
                        label: qsTr("My username")
                        checked: page.cardName
                        onToggled: (on) => page.setCardPref("name", on)
                    }
                    ScButton {
                        text: qsTr("See the card")
                        variant: "primary"
                        visible: !page.last
                        onClicked: { sheet.visible = false; page.current = page.slides.length - 1 }
                    }
                }
            }
        }
    }
}

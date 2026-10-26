import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Scancore

// A year in SCANCORE: time spent, chapters and episodes, the longest streak,
// the busiest month and the series read most, on one card that can be saved
// as a picture to share.
Item {
    id: page
    property int year: new Date().getFullYear()
    property var task: Activity.year(year)
    property var stats: null
    onYearChanged: task = Activity.year(year)
    Connections {
        target: page.task
        function onFinished(result) { page.stats = result }
    }

    readonly property var monthNames: [qsTr("January"), qsTr("February"), qsTr("March"), qsTr("April"), qsTr("May", "month"), qsTr("June"), qsTr("July"), qsTr("August"),
                                       qsTr("September"), qsTr("October"), qsTr("November"), qsTr("December")]
    readonly property var dayNames: [qsTr("Mondays"), qsTr("Tuesdays"), qsTr("Wednesdays"), qsTr("Thursdays"), qsTr("Fridays"), qsTr("Saturdays"), qsTr("Sundays")]

    function duration(seconds) {
        seconds = Math.max(0, Math.round(seconds || 0))
        const h = Math.floor(seconds / 3600)
        const m = Math.floor((seconds % 3600) / 60)
        return h > 0 ? h + " h " + m + " m" : m + " m"
    }
    function indexOfMax(list, value) {
        let best = -1, bestValue = 0
        list.forEach((x, i) => { const v = value(x); if (v > bestValue) { bestValue = v; best = i } })
        return best
    }
    readonly property int busiestMonth: stats ? indexOfMax(stats.months, m => m.seconds + m.units * 600) : -1
    readonly property int favouriteDay: stats ? indexOfMax(stats.weekdays, s => s) : -1
    readonly property string topMedia: {
        if (!stats) return ""
        const t = stats.total.byMedia
        const best = ["comic", "novel", "anime"].sort((a, b) => t[b] - t[a])[0]
        return t[best] > 0 ? ({ comic: "comics", novel: "novels", anime: "anime" })[best] : ""
    }
    readonly property bool empty: stats !== null && stats.total.seconds === 0 && stats.total.units === 0

    SmoothFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: body.height + 48

        Column {
            id: body
            width: flick.width
            spacing: 20

            PageHeader {
                title: qsTr("Your year")
                subtitle: qsTr("Time, chapters and episodes, on every device you sync")
                showBack: true
                Row {
                    spacing: 4
                    IconButton { iconName: "chevron-left"; tip: qsTr("Previous year"); onClicked: page.year -= 1 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.year
                        color: Theme.text
                        font.family: Theme.fontDisplay
                        font.pixelSize: Theme.textLg
                        font.weight: Font.DemiBold
                        leftPadding: 4
                        rightPadding: 4
                    }
                    IconButton {
                        iconName: "chevron-right"
                        tip: qsTr("Next year")
                        enabled: page.year < new Date().getFullYear()
                        onClicked: page.year += 1
                    }
                }
                ScButton {
                    text: qsTr("Wrapped")
                    iconName: "sparkles"
                    variant: "primary"
                    enabled: page.stats !== null && !page.empty
                    onClicked: Router.openWrapped(page.year)
                }
                ScButton {
                    text: qsTr("Save picture")
                    iconName: "download"
                    enabled: page.stats !== null && !page.empty
                    onClicked: saveDialog.open()
                }
            }

            StateView {
                width: parent.width
                height: visible ? 320 : 0
                visible: page.stats === null || page.empty
                task: page.stats === null ? page.task : null
                empty: page.empty
                emptyTitle: qsTr("Nothing yet in ") + page.year
                emptyText: qsTr("Chapters you finish and time spent reading and watching show up here.")
                emptyIcon: "gift"
            }

            // The card that becomes the picture.
            Rectangle {
                id: card
                visible: page.stats !== null && !page.empty
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width - 2 * Theme.gutter, 760)
                height: cardBody.height + 64
                radius: Theme.radiusXl
                color: Theme.surface
                border.width: 1
                border.color: Theme.border
                clip: true
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.mix(Theme.surface, Theme.accent, 0.16) }
                    GradientStop { position: 0.55; color: Theme.surface }
                    GradientStop { position: 1; color: Theme.mix(Theme.surface, Theme.accent, 0.08) }
                }

                Column {
                    id: cardBody
                    x: 32
                    y: 32
                    width: parent.width - 64
                    spacing: 26

                    Row {
                        spacing: 12
                        Image {
                            anchors.verticalCenter: parent.verticalCenter
                            source: App.icon("scancore-mark", Theme.text)
                            sourceSize: Qt.size(60 * Screen.devicePixelRatio, 30 * Screen.devicePixelRatio)
                            width: 60
                            height: 30
                            fillMode: Image.PreserveAspectFit
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: page.year + qsTr(" in SCANCORE")
                            color: Theme.text
                            font.family: Theme.fontDisplay
                            font.pixelSize: Theme.text2xl
                            font.weight: Font.Bold
                        }
                    }

                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: page.stats !== null
                        text: {
                            if (!page.stats) return ""
                            const t = page.stats.total
                            let line = qsTr("You spent ") + page.duration(t.seconds) + qsTr(" here")
                            if (page.topMedia) line += qsTr(", most of it on ") + page.topMedia
                            return line + "."
                        }
                        color: Theme.textMuted
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textLg
                    }

                    // The big numbers.
                    Grid {
                        width: parent.width
                        columns: width > 520 ? 3 : 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Repeater {
                            model: page.stats ? [
                                { label: qsTr("Chapters"), value: page.stats.total.comic + page.stats.total.novel },
                                { label: qsTr("Episodes"), value: page.stats.total.anime },
                                { label: qsTr("Time"), value: page.duration(page.stats.total.seconds) },
                                { label: qsTr("Days active"), value: page.stats.activeDays },
                                { label: qsTr("Longest streak"), value: page.stats.longestStreak + (page.stats.longestStreak === 1 ? " day" : " days") },
                                { label: qsTr("Busiest month"), value: page.busiestMonth >= 0 ? page.monthNames[page.busiestMonth] : "—" }
                            ] : []
                            Column {
                                required property var modelData
                                width: (cardBody.width - 32) / (cardBody.width > 520 ? 3 : 2)
                                spacing: 2
                                Text {
                                    text: modelData.value
                                    color: Theme.text
                                    font.family: Theme.fontDisplay
                                    font.pixelSize: Theme.text3xl
                                    font.weight: Font.Bold
                                    width: parent.width
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: modelData.label
                                    color: Theme.textMuted
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                }
                            }
                        }
                    }

                    // Month by month.
                    Item {
                        id: monthChart
                        width: parent.width
                        height: 110
                        visible: page.stats !== null
                        readonly property real maxMonth: page.stats ? Math.max(1, ...page.stats.months.map(m => m.seconds)) : 1
                        Row {
                            anchors.fill: parent
                            spacing: 6
                            Repeater {
                                model: page.stats ? page.stats.months : []
                                Item {
                                    required property var modelData
                                    required property int index
                                    width: (parent.width - 11 * 6) / 12
                                    height: parent.height
                                    Rectangle {
                                        anchors.bottom: monthLabel.top
                                        anchors.bottomMargin: 6
                                        width: parent.width
                                        height: Math.max(3, (parent.height - 26) * modelData.seconds / monthChart.maxMonth)
                                        radius: 4
                                        color: index === page.busiestMonth ? Theme.accent : Theme.alpha(Theme.text, 0.22)
                                    }
                                    Text {
                                        id: monthLabel
                                        anchors.bottom: parent.bottom
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: page.monthNames[index].slice(0, 1)
                                        color: Theme.textFaint
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textXs
                                    }
                                }
                            }
                        }
                    }

                    // Read and watched most.
                    Column {
                        width: parent.width
                        spacing: 12
                        visible: page.stats !== null && page.stats.top.length > 0
                        Text {
                            text: qsTr("Read and watched most")
                            color: Theme.textMuted
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                            font.weight: Font.DemiBold
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: 1.1
                        }
                        Row {
                            spacing: 12
                            Repeater {
                                model: page.stats ? page.stats.top : []
                                Column {
                                    required property var modelData
                                    required property int index
                                    readonly property real coverWidth: Math.min(104, (cardBody.width - 5 * 12) / 6)
                                    width: coverWidth
                                    spacing: 6
                                    Cover {
                                        width: parent.coverWidth
                                        height: parent.coverWidth * 1.45
                                        radius: Theme.radiusMd
                                        source: modelData.cover || ""
                                    }
                                    Text {
                                        width: parent.width
                                        text: (index + 1) + ". " + modelData.title
                                        color: Theme.text
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textXs
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: modelData.units + (modelData.media === "anime" ? (modelData.units === 1 ? qsTr(" episode") : qsTr(" episodes"))
                                                                                             : (modelData.units === 1 ? qsTr(" chapter") : qsTr(" chapters")))
                                        color: Theme.textFaint
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textXs
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: page.favouriteDay >= 0
                        text: page.favouriteDay >= 0 ? qsTr("You read and watched most on ") + page.dayNames[page.favouriteDay] + "." : ""
                        color: Theme.textMuted
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                    }
                }
            }
        }
    }

    FileDialog {
        id: saveDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: [qsTr("PNG picture (*.png)")]
        selectedFile: "scancore-" + page.year + ".png"
        onAccepted: {
            const file = selectedFile
            // Twice the size on screen, so it stays sharp when shared.
            card.grabToImage((result) => {
                const saved = Activity.saveImage(result.image, file)
                Router.toast(saved ? qsTr("Picture saved") : qsTr("Could not save the picture"), saved ? "success" : "error")
            }, Qt.size(card.width * 2, card.height * 2))
        }
    }
}

import QtQuick
import QtQuick.Controls.Basic
import Scancore

// One release radar for everything you follow: anime episodes as AniList
// schedules them, and the next comic and novel chapters as each series'
// rhythm suggests, day by day for the next two weeks.
Item {
    id: page
    property string media: "all"
    property var task: Upkeep.radar(14)
    // A big library has dozens due some days: eight, then all (by day).
    readonly property int firstItems: 8
    property var opened: ({})
    readonly property var days: {
        const all = task && task.ok ? task.result : []
        if (media === "all") return all
        return all.map(d => ({ day: d.day, items: d.items.filter(i => i.media === media) })).filter(d => d.items.length > 0)
    }

    function dayLabel(iso) {
        const d = new Date(iso + "T12:00:00")
        const today = new Date()
        today.setHours(12, 0, 0, 0)
        const diff = Math.round((d - today) / 86400000)
        if (diff === 0) return qsTr("Today")
        if (diff === 1) return qsTr("Tomorrow")
        return Qt.formatDate(d, "dddd d MMMM")
    }
    function unitLine(it) {
        const unit = it.media === "anime" ? (it.unit > 0 ? qsTr("Episode %1").arg(it.unit) : qsTr("Next episode"))
                                          : (it.unit > 0 ? qsTr("Chapter %1").arg(it.unit) : qsTr("Next chapter"))
        if (!it.guess) return unit + " · " + Qt.formatTime(new Date(it.at * 1000), "hh:mm")
        if (it.late) return unit + " · " + qsTr("due, a little late")
        return unit + " · " + (it.every === 7 ? qsTr("usually weekly") : it.every === 1 ? qsTr("usually daily")
                                                : qsTr("usually every %1 days").arg(it.every))
    }
    function open(it, cover) {
        if (it.media === "anime") Router.openSeries({ media: "anime", anilistId: it.anilistId, title: it.title, cover: it.cover }, cover)
        else Router.openSeries(Library.seriesRow(it.seriesId), cover)
    }

    SmoothFlickable {
        anchors.fill: parent
        contentHeight: column.height + 60

        Column {
            id: column
            width: parent.width
            spacing: 22

            PageHeader {
                title: qsTr("Upcoming", "release radar")
                subtitle: qsTr("What comes out next in your library")
                showBack: true
            }

            Segmented {
                x: Theme.gutter
                width: Theme.compact ? parent.width - 2 * Theme.gutter : implicitWidth
                fill: Theme.compact
                options: [{ value: "all", label: qsTr("Everything") }, { value: "comic", label: qsTr("Comics") },
                          { value: "novel", label: qsTr("Novels") }, { value: "anime", label: qsTr("Anime") }]
                value: page.media
                onPicked: (v) => page.media = v
            }

            Repeater {
                model: page.days
                Column {
                    id: dayColumn
                    required property var modelData
                    x: Theme.gutter
                    width: column.width - 2 * Theme.gutter
                    spacing: 6
                    Text {
                        text: page.dayLabel(dayColumn.modelData.day)
                        color: Theme.text
                        font.family: Theme.fontDisplay
                        font.pixelSize: Theme.textLg
                        font.weight: Font.DemiBold
                        bottomPadding: 4
                    }
                    Repeater {
                        model: page.opened[dayColumn.modelData.day] ? dayColumn.modelData.items : dayColumn.modelData.items.slice(0, page.firstItems)
                        delegate: AbstractButton {
                            id: row
                            required property var modelData
                            width: dayColumn.width
                            height: 78
                            hoverEnabled: true
                            onClicked: page.open(modelData, cover)
                            background: Rectangle {
                                radius: Theme.radiusLg
                                color: row.hovered ? Theme.surface2 : "transparent"
                                Behavior on color { ColorAnimation { duration: Motion.fast } }
                            }
                            contentItem: Row {
                                spacing: 14
                                leftPadding: 8
                                Cover {
                                    id: cover
                                    width: 42
                                    height: 60
                                    anchors.verticalCenter: parent.verticalCenter
                                    radius: 8
                                    source: row.modelData.cover || ""
                                }
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: row.width - 8 - 42 - 14 - 8
                                    spacing: 4
                                    Text {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        text: row.modelData.title
                                        color: Theme.text
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textMd
                                        font.weight: Font.DemiBold
                                    }
                                    Row {
                                        spacing: 6
                                        Icon {
                                            anchors.verticalCenter: parent.verticalCenter
                                            name: row.modelData.media === "anime" ? "tv" : row.modelData.media === "novel" ? "book-text" : "book-open"
                                            size: 13
                                            color: Theme.textFaint
                                        }
                                        Text {
                                            width: row.width - 8 - 42 - 14 - 8 - 19
                                            elide: Text.ElideRight
                                            text: page.unitLine(row.modelData)
                                            color: row.modelData.late ? Theme.warning : Theme.textMuted
                                            font.family: Theme.fontSans
                                            font.pixelSize: Theme.textSm
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ScButton {
                        visible: dayColumn.modelData.items.length > page.firstItems && !page.opened[dayColumn.modelData.day]
                        variant: "ghost"
                        text: qsTr("Show all %1").arg(dayColumn.modelData.items.length)
                        onClicked: page.opened = Object.assign({}, page.opened, { [dayColumn.modelData.day]: true })
                    }
                }
            }

            Text {
                visible: page.task && page.task.ok && page.days.length > 0
                x: Theme.gutter
                width: column.width - 2 * Theme.gutter
                wrapMode: Text.Wrap
                text: qsTr("Comic and novel dates are guesses from when their last chapters came out.")
                color: Theme.textFaint
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            StateView {
                width: parent.width
                task: page.task
                empty: page.task && page.task.ok && page.days.length === 0
                emptyTitle: qsTr("Nothing expected soon")
                emptyText: qsTr("Save series to your library and check them for updates; their next chapters show here.")
                emptyIcon: "calendar"
                onRetry: page.task = Upkeep.radar(14)
            }
        }
    }
}

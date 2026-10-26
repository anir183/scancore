import QtQuick
import QtQuick.Controls.Basic
import Scancore

// What the library needs: sources that stopped answering or updating, with
// a move to another source one tap away, and series saved twice, to merge.
Item {
    id: page
    property var task: Upkeep.report()
    readonly property var report: task && task.ok ? task.result : ({ sources: [], failing: [], stalled: [], duplicates: [] })
    // Long lists show their first rows until asked: a big library can have
    // hundreds of series on a source that stopped.
    readonly property int firstRows: 20
    property var expanded: ({})
    function capped(list, key) { return expanded[key] ? list : list.slice(0, firstRows) }
    component ShowAll: ScButton {
        property string listKey
        property int total: 0
        visible: total > page.firstRows
        x: Theme.gutter
        variant: "ghost"
        text: page.expanded[listKey] ? qsTr("Show fewer") : qsTr("Show all %1").arg(total)
        onClicked: page.expanded = Object.assign({}, page.expanded, { [listKey]: !page.expanded[listKey] })
    }
    readonly property bool empty: report.sources.length + report.failing.length + report.stalled.length + report.duplicates.length === 0
    // The series to keep in each duplicate group, by group index.
    property var keep: ({})
    property var mergeTask: null

    function reload() { task = Upkeep.report() }
    // Library changes come every couple of seconds while reading (progress):
    // a page out of sight (another section, under a reader or the player)
    // only notes them and reloads once it shows again.
    property bool libraryStale: false
    onVisibleChanged: if (visible && libraryStale) { libraryStale = false; reloadTimer.restart() }
    Connections {
        target: Library
        function onChanged() { if (page.visible) reloadTimer.restart(); else page.libraryStale = true }
    }
    Timer { id: reloadTimer; interval: 600; onTriggered: page.reload() }

    function sourceLabel(row) {
        return row.media === "novel" ? ((Novels.sources().find(x => x.key === row.source) || {}).label || row.source)
                                     : Comics.sourceLabel(row.source)
    }
    function entry(row) {
        return { id: row.id, media: row.media, title: row.title, cover: row.cover || "", source: row.source,
                 sourceLabel: sourceLabel(row), sourceUrl: row.sourceUrl, extra: row.extra || {} }
    }
    function move(rows) { migrate.start(rows.map(r => entry(r))) }
    // By default the copy read furthest stays.
    function keptIn(group, index) {
        if (keep[index]) return keep[index]
        return group.slice().sort((a, b) => b.read - a.read || b.units - a.units)[0].id
    }
    function merge(group, index) {
        const keepId = keptIn(group, index)
        mergeTask = Upkeep.merge(keepId, group.map(r => r.id).filter(id => id !== keepId))
        mergeTask.finished.connect(() => Router.toast(qsTr("Merged"), "success"))
        mergeTask.failed.connect((error) => Router.toast(error, "error"))
    }

    MigrateDialog {
        id: migrate
        onMigrated: page.reload()
    }

    SmoothFlickable {
        anchors.fill: parent
        contentHeight: column.height + 60

        Column {
            id: column
            width: parent.width
            spacing: 18

            PageHeader {
                title: qsTr("Library health")
                subtitle: qsTr("Stopped sources and duplicates")
                showBack: true
            }

            // ---------------------------------------------- whole sources
            Repeater {
                model: page.capped(page.report.sources, "sources")
                Rectangle {
                    id: sourceCard
                    required property var modelData
                    x: Theme.gutter
                    width: column.width - 2 * Theme.gutter
                    height: sourceRow.height + 28
                    radius: Theme.radiusLg
                    color: Theme.alpha(Theme.warning, 0.08)
                    border.width: 1
                    border.color: Theme.alpha(Theme.warning, 0.35)
                    Item {
                        id: sourceRow
                        x: 16
                        y: 14
                        width: parent.width - 32
                        height: Math.max(sourceTexts.height, moveAll.height)
                        Column {
                            id: sourceTexts
                            anchors.left: parent.left
                            anchors.right: moveAll.left
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text {
                                width: parent.width
                                wrapMode: Text.Wrap
                                text: qsTr("%1 has not answered for %2 days").arg(page.sourceLabel(sourceCard.modelData)).arg(sourceCard.modelData.days)
                                color: Theme.text
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            Text {
                                width: parent.width
                                wrapMode: Text.Wrap
                                text: sourceCard.modelData.count === 1 ? qsTr("1 series of yours is on it. Moving it keeps what you read.")
                                    : qsTr("%1 series of yours are on it. Moving them keeps what you read.").arg(sourceCard.modelData.count)
                                color: Theme.textMuted
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                        ScButton {
                            id: moveAll
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            variant: "primary"
                            compact: true
                            text: qsTr("Move them")
                            onClicked: page.move(sourceCard.modelData.series)
                        }
                    }
                }
            }
            ShowAll { listKey: "sources"; total: page.report.sources.length }

            // ---------------------------------------------- single series
            Component {
                id: seriesRow
                Item {
                    id: rowItem
                    property var row: ({})
                    property string line: ""
                    property string action: qsTr("Move")
                    readonly property var alternatives: Upkeep.alternatives(row.id || "")
                    width: column.width - 2 * Theme.gutter
                    height: 76
                    Cover {
                        id: rowCover
                        anchors.verticalCenter: parent.verticalCenter
                        width: 42
                        height: 60
                        radius: 8
                        source: rowItem.row.cover || ""
                    }
                    Column {
                        anchors.left: rowCover.right
                        anchors.leftMargin: 14
                        anchors.right: rowActions.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: rowItem.row.title || ""
                            color: Theme.text
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: rowItem.line
                            color: Theme.textMuted
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                    Row {
                        id: rowActions
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        // A source it was merged from: straight back to it.
                        ScButton {
                            visible: rowItem.alternatives.length > 0 && !Theme.compact
                            compact: true
                            variant: "ghost"
                            text: qsTr("Back to %1").arg(page.sourceLabel(Object.assign({ media: rowItem.row.media }, rowItem.alternatives[0] || {})))
                            onClicked: {
                                const t = Library.migrate(rowItem.row.id, rowItem.alternatives[0].id, {})
                                t.finished.connect(() => Router.toast(qsTr("Moved"), "success"))
                                t.failed.connect((error) => Router.toast(error, "error"))
                            }
                        }
                        ScButton {
                            compact: true
                            text: rowItem.action
                            onClicked: page.move([rowItem.row])
                        }
                    }
                }
            }

            Column {
                visible: page.report.failing.length > 0
                width: column.width
                Column {
                    width: parent.width
                    spacing: 4
                    Text {
                        x: Theme.gutter
                        text: qsTr("Not answering")
                        color: Theme.text
                        font.family: Theme.fontDisplay
                        font.pixelSize: Theme.textXl
                        font.weight: Font.DemiBold
                    }
                    ShowAll { listKey: "failing"; total: page.report.failing.length }
                    Repeater {
                        model: page.capped(page.report.failing, "failing")
                        Loader {
                            required property var modelData
                            x: Theme.gutter
                            sourceComponent: seriesRow
                            onLoaded: {
                                item.row = modelData
                                item.line = [page.sourceLabel(modelData), qsTr("failing for %1 days").arg(modelData.days)].join(" · ")
                            }
                        }
                    }
                }
            }

            Column {
                visible: page.report.stalled.length > 0
                width: column.width
                Column {
                    width: parent.width
                    spacing: 4
                    Text {
                        x: Theme.gutter
                        text: qsTr("Stopped updating")
                        color: Theme.text
                        font.family: Theme.fontDisplay
                        font.pixelSize: Theme.textXl
                        font.weight: Font.DemiBold
                    }
                    Text {
                        x: Theme.gutter
                        width: column.width - 2 * Theme.gutter
                        wrapMode: Text.Wrap
                        text: qsTr("Their sources answer but have had no new chapter for months. Another source may be further ahead.")
                        color: Theme.textMuted
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    ShowAll { listKey: "stalled"; total: page.report.stalled.length }
                    Repeater {
                        model: page.capped(page.report.stalled, "stalled")
                        Loader {
                            required property var modelData
                            x: Theme.gutter
                            sourceComponent: seriesRow
                            onLoaded: {
                                item.row = modelData
                                item.action = Theme.compact ? qsTr("Move") : qsTr("Look elsewhere")
                                item.line = [page.sourceLabel(modelData), qsTr("nothing new for %1 days").arg(modelData.days)].join(" · ")
                            }
                        }
                    }
                }
            }

            // ---------------------------------------------- duplicates
            Column {
                visible: page.report.duplicates.length > 0
                width: column.width
                Column {
                    width: parent.width
                    spacing: 10
                    Text {
                        x: Theme.gutter
                        text: qsTr("Saved twice")
                        color: Theme.text
                        font.family: Theme.fontDisplay
                        font.pixelSize: Theme.textXl
                        font.weight: Font.DemiBold
                    }
                    Text {
                        x: Theme.gutter
                        width: column.width - 2 * Theme.gutter
                        wrapMode: Text.Wrap
                        text: qsTr("The same series on more than one source. Merging keeps one with everything you read in either, and remembers the other source to move back to.")
                        color: Theme.textMuted
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    ShowAll { listKey: "duplicates"; total: page.report.duplicates.length }
                    Repeater {
                        model: page.capped(page.report.duplicates, "duplicates")
                        Rectangle {
                            id: group
                            required property var modelData
                            required property int index
                            readonly property string kept: page.keptIn(modelData, index)
                            x: Theme.gutter
                            width: column.width - 2 * Theme.gutter
                            height: groupColumn.height + 24
                            radius: Theme.radiusLg
                            color: Theme.surface
                            border.width: 1
                            border.color: Theme.border
                            Column {
                                id: groupColumn
                                x: 12
                                y: 12
                                width: parent.width - 24
                                spacing: 4
                                Repeater {
                                    model: group.modelData
                                    AbstractButton {
                                        id: copy
                                        required property var modelData
                                        readonly property bool chosen: group.kept === modelData.id
                                        width: groupColumn.width
                                        height: 64
                                        hoverEnabled: true
                                        onClicked: {
                                            const next = Object.assign({}, page.keep)
                                            next[group.index] = modelData.id
                                            page.keep = next
                                        }
                                        background: Rectangle {
                                            radius: Theme.radiusMd
                                            color: copy.chosen ? Theme.alpha(Theme.accent, 0.10) : copy.hovered ? Theme.surface2 : "transparent"
                                            border.width: copy.chosen ? 1 : 0
                                            border.color: Theme.alpha(Theme.accent, 0.45)
                                        }
                                        contentItem: Row {
                                            leftPadding: 8
                                            spacing: 12
                                            Cover {
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: 34
                                                height: 48
                                                radius: 6
                                                source: copy.modelData.cover || ""
                                            }
                                            Column {
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: copy.width - 8 - 34 - 12 - 90
                                                spacing: 2
                                                Text {
                                                    width: parent.width
                                                    elide: Text.ElideRight
                                                    text: copy.modelData.title
                                                    color: Theme.text
                                                    font.family: Theme.fontSans
                                                    font.pixelSize: Theme.textMd
                                                    font.weight: Font.Medium
                                                }
                                                Text {
                                                    width: parent.width
                                                    elide: Text.ElideRight
                                                    text: [page.sourceLabel(copy.modelData),
                                                           qsTr("%1 of %2 read").arg(copy.modelData.read).arg(copy.modelData.units)].join(" · ")
                                                    color: Theme.textMuted
                                                    font.family: Theme.fontSans
                                                    font.pixelSize: Theme.textSm
                                                }
                                            }
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                visible: copy.chosen
                                                text: qsTr("Keep")
                                                color: Theme.accent
                                                font.family: Theme.fontSans
                                                font.pixelSize: Theme.textSm
                                                font.weight: Font.DemiBold
                                            }
                                        }
                                    }
                                }
                                Row {
                                    anchors.right: parent.right
                                    spacing: 8
                                    topPadding: 4
                                    ScButton {
                                        compact: true
                                        variant: "ghost"
                                        text: qsTr("Not the same")
                                        onClicked: { Upkeep.keepApart(group.modelData.map(r => r.id)); page.reload() }
                                    }
                                    ScButton {
                                        compact: true
                                        variant: "primary"
                                        text: qsTr("Merge")
                                        loading: page.mergeTask !== null && page.mergeTask.loading
                                        onClicked: page.merge(group.modelData, group.index)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            StateView {
                width: parent.width
                task: page.task
                empty: page.task && page.task.ok && page.empty
                emptyTitle: qsTr("All is well")
                emptyText: qsTr("Every source answered lately, and nothing is saved twice.")
                emptyIcon: "check"
                onRetry: page.reload()
            }
        }
    }
}

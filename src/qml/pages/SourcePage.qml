import QtQuick
import QtQuick.Controls.Basic
import Scancore

// One source by itself: its popular and latest lists, and searches with its
// own filters, a page at a time as the list scrolls. Comics' and anime's
// extensions and novels' plugins alike.
Item {
    id: page
    property string sourceKey
    readonly property bool novel: sourceKey.startsWith("lnreader:")
    readonly property bool anime: Router.extensionAnime(sourceKey)
    readonly property var service: anime ? Anime : novel ? Novels : Comics
    readonly property var info: { App.dependOn(App.manifestVersion); return page.service.sources().find(s => s.key === sourceKey) || ({ label: sourceKey }) }
    // "popular", "latest" or "search"
    property string listing: "popular"
    property string query: ""
    property var filterDefaults: []
    property var filterStates: []
    readonly property int filterCount: filterDefaults.length ? filterSheet.changes(filterStates, filterDefaults) : 0

    property var items: []
    property int pageNumber: 0
    property bool hasNext: true
    property var task: null
    property var filtersTask: page.service.filters(sourceKey)
    // Ignores pages of a listing the user has since left.
    property int generation: 0

    function reload() {
        generation += 1
        items = []
        pageNumber = 0
        hasNext = true
        task = null
        more()
        flick.scrollTo(0)
    }
    // Scrolling asks for the next page; after a failure only "Try again" does.
    function more(retry) {
        if ((task && task.loading) || !hasNext) return
        if (!retry && task && !task.ok) return
        const g = generation
        const next = pageNumber + 1
        task = page.service.browse(sourceKey, listing, next, query, listing === "search" ? filterStates : [])
        task.finished.connect((r) => {
            if (g !== page.generation) return
            page.pageNumber = next
            // Some sources repeat items across pages; one with nothing new
            // ends the list.
            const seen = {}
            for (const it of page.items) seen[it.id] = true
            const fresh = r.items.filter(it => !seen[it.id])
            page.hasNext = r.hasNextPage && fresh.length > 0
            if (fresh.length) page.items = page.items.concat(fresh)
        })
    }
    function search(text) {
        query = text.trim()
        listing = query.length || filterCount ? "search" : "popular"
        reload()
    }

    Component.onCompleted: reload()
    Connections {
        target: page.filtersTask
        function onFinished(list) {
            page.filterDefaults = list
            if (!page.filterStates.length) page.filterStates = list
        }
    }

    FilterSheet {
        id: filterSheet
        onFiltersApplied: (states) => {
            page.filterStates = states
            page.listing = "search"
            page.reload()
        }
    }
    SourceSettingsDialog { id: settingsDialog }

    SmoothFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height + 60
        // The next page loads before the end comes into view.
        onContentYChanged: if (contentY + height > contentHeight - 1200 && page.items.length) page.more()

        Column {
            id: column
            width: parent.width
            spacing: 20

            PageHeader {
                title: page.info.label || ""
                subtitle: [page.info.languageName, (page.info.base || "").replace(/^https?:\/\//, "")].filter(s => s).join(" · ")
                showBack: true
                Row {
                    spacing: 4
                    IconButton {
                        visible: page.info.extension === true
                        iconName: "sliders-horizontal"
                        tip: qsTr("Source settings")
                        onClicked: settingsDialog.start(page.sourceKey, page.info.label)
                    }
                    IconButton {
                        visible: !!page.info.base
                        iconName: "external-link"
                        tip: qsTr("Open site")
                        onClicked: App.openUrl(page.info.base)
                    }
                }
            }

            Flow {
                x: Theme.gutter
                width: parent.width - 2 * Theme.gutter
                spacing: 10
                Segmented {
                    options: page.info.browseLatest
                        ? [{ value: "popular", label: qsTr("Popular") }, { value: "latest", label: qsTr("Latest") }]
                        : [{ value: "popular", label: qsTr("Popular") }]
                    value: page.listing === "search" ? "" : page.listing
                    onPicked: (v) => {
                        field.text = ""
                        page.query = ""
                        page.listing = v
                        page.reload()
                    }
                }
                SearchField {
                    id: field
                    width: Math.min(420, parent.width - (Theme.compact ? 0 : 360))
                    placeholderText: qsTr("Search ") + (page.info.label || "")
                    onSubmitted: (text) => page.search(text)
                }
                ScButton {
                    visible: page.filterDefaults.length > 0
                    text: page.filterCount ? qsTr("Filters (") + page.filterCount + ")" : qsTr("Filters")
                    iconName: "funnel"
                    variant: page.filterCount ? "primary" : "secondary"
                    onClicked: filterSheet.start(page.filterDefaults, page.filterStates)
                }
            }

            CoverGrid {
                items: page.items
                onOpened: (it, cover) => Router.openSeries(it, cover)
            }

            StateView {
                width: parent.width
                visible: page.items.length === 0
                task: page.task
                empty: page.task !== null && page.task.ok && page.items.length === 0
                emptyTitle: qsTr("Nothing here")
                emptyText: page.listing === "search" ? qsTr("Try other words or fewer filters.") : qsTr("The source listed nothing.")
                emptyIcon: "search"
                onRetry: page.reload()
            }

            // The next page.
            Item {
                width: parent.width
                height: 60
                visible: page.items.length > 0 && (page.hasNext || (page.task && !page.task.ok && !page.task.loading))
                Spinner { anchors.centerIn: parent; size: 20; visible: page.task !== null && page.task.loading }
                ScButton {
                    anchors.centerIn: parent
                    visible: page.task !== null && !page.task.loading && !page.task.ok
                    text: qsTr("Try again")
                    onClicked: page.more(true)
                }
            }
        }
    }

    BackToTop { flick: flick }
}

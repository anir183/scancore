import QtQuick
import QtQuick.Controls.Basic
import Scancore

// Extensions → Diagnostics: for when an extension misbehaves. Whether the
// runtime is there and answers, a quick test of one source (its popular
// page, timed), the sites each extension contacted this session, and the
// end of the host's log, with Copy report for an issue.
Item {
    id: page

    property var task: Extensions.diagnostics()
    readonly property var report: task && task.ok ? task.result : ({})
    readonly property var installedList: { App.dependOn(Extensions.revision); return Extensions.installed("") }
    // Every extension source, for the test.
    readonly property var sourceOptions: {
        const out = []
        for (const ext of installedList)
            for (const s of (ext.sources || []))
                out.push({ value: s.key, label: s.name + (s.languageName ? " · " + s.languageName : ""), kind: ext.kind })
        return out
    }
    // Extensions by package, for names in the network list.
    readonly property var byPackage: {
        const out = {}
        for (const ext of installedList) out[ext.pkg] = ext
        return out
    }
    readonly property var networkRows: {
        const use = report.networkUse || {}
        return Object.keys(use).map(pkg => ({
            pkg: pkg,
            name: (byPackage[pkg] || {}).name || pkg,
            hosts: Array.from(use[pkg]).sort((a, b) => b.requests - a.requests)
        })).sort((a, b) => a.name.localeCompare(b.name))
    }

    // The source test.
    property string testKey: sourceOptions.length ? sourceOptions[0].value : ""
    property var testTask: null
    property double testStarted: 0
    property string testResult: ""
    function runTest() {
        const option = sourceOptions.find(o => o.value === testKey)
        if (!option) return
        const service = option.kind === "aniyomi" || option.kind === "cloudstream" ? Anime : option.kind === "lnreader" ? Novels : Comics
        testResult = ""
        testStarted = Date.now()
        testTask = service.browse(testKey, "popular", 1)
    }
    Connections {
        target: page.testTask
        function onFinished(result) {
            const n = (result.items || []).length
            page.testResult = qsTr("Answered in %1 s with %2 titles.").arg(((Date.now() - page.testStarted) / 1000).toFixed(1)).arg(n)
            page.task = Extensions.diagnostics()
        }
        function onFailed(error) {
            page.testResult = qsTr("Failed after %1 s: %2").arg(((Date.now() - page.testStarted) / 1000).toFixed(1)).arg(error)
            page.task = Extensions.diagnostics()
        }
    }

    function reportText() {
        const r = report
        const lines = ["SCANCORE " + App.version, (r.system || "") + " · Qt " + (r.qt || ""),
                       "Extensions runtime: " + (r.runtimeInstalled ? "installed" : r.runtimeAvailable ? "not installed" : "not available here")
                       + " · host " + (r.host ? r.host.version : "?") + " (app wants " + (r.hostVersion || "?") + ")"]
        if (r.hostError) lines.push("Host error: " + r.hostError)
        if (r.host) lines.push("Extension libraries: " + Array.from(r.host.libs || []).join(", ") + " · anime: " + Array.from(r.host.animeLibs || []).join(", "))
        lines.push("Installed: " + installedList.map(e => e.name + " " + (e.version || "")).join(", "))
        if (testResult) lines.push("Test of " + testKey + ": " + testResult)
        for (const row of networkRows) lines.push(row.name + ": " + row.hosts.map(h => h.host + " ×" + h.requests).join(", "))
        if (r.log) lines.push("", "Host log (end):", r.log)
        return lines.join("\n")
    }

    function age(ms) { return ms > 0 ? App.relativeTime(Math.round(ms / 1000)) : "" }

    SmoothFlickable {
        anchors.fill: parent
        contentHeight: column.height + 60
        Column {
            id: column
            width: parent.width
            spacing: Theme.sectionGap * 0.6

            PageHeader {
                title: qsTr("Diagnostics")
                subtitle: qsTr("For when an extension misbehaves")
                showBack: true
                ScButton {
                    text: qsTr("Copy report")
                    iconName: "copy"
                    enabled: page.task && page.task.done
                    onClicked: {
                        App.copy(page.reportText())
                        Router.toast(qsTr("The report is copied. Paste it into an issue or a message."), "success")
                    }
                }
                ScButton {
                    text: qsTr("Refresh")
                    iconName: "refresh-cw"
                    variant: "ghost"
                    onClicked: page.task = Extensions.diagnostics()
                }
            }

            SettingsGroup {
                x: Theme.gutter
                width: parent.width - 2 * Theme.gutter
                title: qsTr("Runtime")
                SettingRow {
                    iconName: "package"
                    title: page.report.runtimeInstalled ? qsTr("Extensions runtime installed")
                        : page.report.runtimeAvailable ? qsTr("Extensions runtime not installed") : qsTr("No runtime for this system")
                    description: page.task && page.task.loading ? qsTr("Asking the host…")
                        : page.report.hostError ? page.report.hostError
                        : page.report.host ? qsTr("Host %1 answered in %2 ms").arg(page.report.host.version).arg(page.report.hostAnswerMs)
                        : ""
                    Spinner { visible: page.task ? page.task.loading : false }
                }
                SettingRow {
                    visible: !!page.report.host
                    iconName: "layers"
                    title: qsTr("Extension libraries it takes")
                    description: page.report.host ? qsTr("Mihon %1 · Aniyomi %2").arg(Array.from(page.report.host.libs || []).join(", "))
                                                        .arg(Array.from(page.report.host.animeLibs || []).join(", ")) : ""
                }
                SettingRow {
                    iconName: "monitor"
                    title: qsTr("This device")
                    description: (page.report.system || "") + " · SCANCORE " + App.version
                }
            }

            SettingsGroup {
                x: Theme.gutter
                width: parent.width - 2 * Theme.gutter
                visible: page.sourceOptions.length > 0
                title: qsTr("Test a source")
                note: qsTr("Loads the source's popular titles once and times it.")
                SettingRow {
                    iconName: "gauge"
                    title: qsTr("Source")
                    description: page.testResult
                    Row {
                        spacing: 8
                        Select {
                            model: page.sourceOptions
                            value: page.testKey
                            onPicked: (v) => page.testKey = v
                        }
                        ScButton {
                            text: page.testTask && page.testTask.loading ? qsTr("Testing…") : qsTr("Test")
                            enabled: !(page.testTask && page.testTask.loading)
                            onClicked: page.runTest()
                        }
                    }
                }
            }

            SettingsGroup {
                x: Theme.gutter
                width: parent.width - 2 * Theme.gutter
                title: qsTr("Sites the extensions contacted")
                note: qsTr("This session only, counted in the extensions runtime. Nothing but the sites' names is kept.")
                SettingRow {
                    visible: page.networkRows.length === 0
                    iconName: "globe"
                    title: qsTr("Nothing yet")
                    description: qsTr("Browse or read with an extension and come back.")
                }
                Repeater {
                    // One row per extension that went online (a handful).
                    model: page.networkRows
                    SettingRow {
                        required property var modelData
                        iconName: "globe"
                        title: modelData.name
                        description: modelData.hosts.map(h => qsTr("%1 · %2 requests, %3").arg(h.host).arg(h.requests).arg(page.age(h.last))).join("\n")
                    }
                }
            }

            SettingsGroup {
                x: Theme.gutter
                width: parent.width - 2 * Theme.gutter
                visible: (page.report.log || "").length > 0
                title: qsTr("Host log")
                note: qsTr("The end of extensions/host.log.")
                Text {
                    x: 16
                    width: parent.width - 32
                    topPadding: 12
                    bottomPadding: 12
                    text: page.report.log || ""
                    color: Theme.textMuted
                    font.family: "monospace"
                    font.pixelSize: Theme.textXs
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                }
            }
        }
    }
}

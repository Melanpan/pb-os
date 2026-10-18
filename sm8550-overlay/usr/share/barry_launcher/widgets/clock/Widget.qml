// Clock: the time, big, and the date under it. An example widget pack
// (README.md): Barry draws the tile, this only what is in it.
//
// The time zone is the system's now: JavaScript's Date keeps the one Barry
// started with, so the offset comes from barry_launcher_statsd's /stats.
import QtQuick
import Barry.Input

Item {
    id: root
    property real s: 1
    property bool shown: true

    property int offsetMin: -new Date().getTimezoneOffset()
    property bool twelveHour: false
    property double now: Date.now()

    readonly property var local: new Date(now + offsetMin * 60000)
    readonly property var days: ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
    readonly property var months: ["January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December"]

    function pad(n) { return (n < 10 ? "0" : "") + n }
    function timeText() {
        const h = local.getUTCHours(), m = pad(local.getUTCMinutes())
        return twelveHour ? ((h + 11) % 12 + 1) + ":" + m : pad(h) + ":" + m
    }
    function zone() {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE || x.status !== 200)
                return
            try {
                const t = JSON.parse(x.responseText).time
                root.offsetMin = t.utcOffset
                root.twelveHour = /[AP]M/i.test(t.text)
            } catch (e) {}
        }
        x.open("GET", "http://127.0.0.1:47823/stats")
        x.send()
    }
    onShownChanged: if (shown) { now = Date.now(); zone() }
    Component.onCompleted: zone()
    Timer {
        interval: 1000
        repeat: true
        running: root.shown
        onTriggered: root.now = Date.now()
    }
    Timer {
        interval: 60000
        repeat: true
        running: root.shown
        onTriggered: root.zone()
    }

    Column {
        anchors.centerIn: parent
        spacing: 4 * root.s
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8 * root.s
            Text {
                id: clockText
                text: root.timeText()
                color: Skin.colors.text
                font { family: Skin.font || "Noto Sans"; pixelSize: 88 * root.s; weight: Font.Bold }
            }
            Text {
                visible: root.twelveHour
                anchors.baseline: clockText.baseline
                text: root.local.getUTCHours() < 12 ? "AM" : "PM"
                color: Skin.colors.accent
                font { family: Skin.font || "Noto Sans"; pixelSize: 30 * root.s; weight: Font.DemiBold }
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.days[root.local.getUTCDay()] + ", " + root.months[root.local.getUTCMonth()] + " " + root.local.getUTCDate()
            color: Skin.colors.text
            opacity: 0.75
            font { family: Skin.font || "Noto Sans"; pixelSize: 24 * root.s }
        }
    }
}

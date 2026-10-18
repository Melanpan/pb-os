// Calendar: this month, today lit; ‹ › look at the months around it, a tap
// on the month's name comes back to today. An example widget pack
// (README.md).
import QtQuick
import QtQuick.Layouts
import Barry.Input

Item {
    id: root
    property real s: 1
    property bool shown: true

    // Today in the system's time zone (statsd's offset; see the clock).
    property int offsetMin: -new Date().getTimezoneOffset()
    property double now: Date.now()
    readonly property var today: new Date(now + offsetMin * 60000)
    property int shift: 0  // months from this one
    readonly property int year: today.getUTCFullYear() + Math.floor((today.getUTCMonth() + shift) / 12)
    readonly property int month: ((today.getUTCMonth() + shift) % 12 + 12) % 12
    readonly property var months: ["January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December"]
    // Monday first.
    readonly property int lead: (new Date(Date.UTC(year, month, 1)).getUTCDay() + 6) % 7
    readonly property int length: new Date(Date.UTC(year, month + 1, 0)).getUTCDate()
    readonly property bool thisMonth: year === today.getUTCFullYear() && month === today.getUTCMonth()

    function zone() {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState === XMLHttpRequest.DONE && x.status === 200) {
                try { root.offsetMin = JSON.parse(x.responseText).time.utcOffset } catch (e) {}
            }
        }
        x.open("GET", "http://127.0.0.1:47823/stats")
        x.send()
    }
    onShownChanged: if (shown) { now = Date.now(); zone() }
    Component.onCompleted: zone()
    // Midnight comes: a new today.
    Timer {
        interval: 60000
        repeat: true
        running: root.shown
        onTriggered: root.now = Date.now()
    }

    component Arrow: Text {
        signal tapped()
        color: Skin.colors.text
        font { family: Skin.font || "Noto Sans"; pixelSize: 36 * root.s; weight: Font.Bold }
        Layout.preferredWidth: 50 * root.s
        horizontalAlignment: Text.AlignHCenter
        TapHandler { onTapped: parent.tapped() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6 * root.s
        RowLayout {
            Layout.fillWidth: true
            Arrow { text: "‹"; onTapped: root.shift-- }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: root.months[root.month] + " " + root.year
                color: Skin.colors.text
                font { family: Skin.font || "Noto Sans"; pixelSize: 26 * root.s; weight: Font.DemiBold }
                TapHandler { onTapped: root.shift = 0 }
            }
            Arrow { text: "›"; onTapped: root.shift++ }
        }
        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 7
            rowSpacing: 0
            columnSpacing: 0
            Repeater {
                model: ["M", "T", "W", "T", "F", "S", "S"]
                delegate: Text {
                    required property string modelData
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: modelData
                    color: Skin.colors.text
                    opacity: 0.55
                    font { family: Skin.font || "Noto Sans"; pixelSize: 19 * root.s; weight: Font.DemiBold }
                }
            }
            Repeater {
                model: 42
                delegate: Item {
                    id: day
                    required property int index
                    readonly property int date: index - root.lead + 1
                    readonly property bool inMonth: date >= 1 && date <= root.length
                    readonly property bool isToday: inMonth && root.thisMonth && date === root.today.getUTCDate()
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Rectangle {
                        anchors.centerIn: parent
                        width: Math.min(parent.width, parent.height) * 0.92
                        height: width
                        radius: width / 2
                        visible: day.isToday
                        color: Skin.colors.lit
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: day.inMonth
                        text: day.date
                        color: day.isToday ? Skin.colors.litText : Skin.colors.text
                        font { family: Skin.font || "Noto Sans"; pixelSize: 21 * root.s; weight: day.isToday ? Font.Bold : Font.Normal }
                    }
                }
            }
        }
    }
}

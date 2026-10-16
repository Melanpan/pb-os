pragma ComponentBehavior: Bound
// Pboard: the dashboard in the colors of a modern phone keyboard: round-
// cornered tiles in the key color, the current FPS on the accent color,
// in whichever of the skin's color variants is chosen (skin.json
// "variants", picked in the Barry Launcher plugin's Themes tab). The same layout as the
// built-in dashboard (status bar, current FPS, temperature, fan, gauges,
// quick controls); the colors are skin.json's (Barry.Input's Skin), which
// the keyboards, trackpads and home screen use too.
import QtQuick
import QtQuick.Layouts
import Barry.Input

Rectangle {
    id: root
    property var dashboard
    readonly property var st: dashboard ? dashboard.stats : ({})
    readonly property real s: Math.min(width / 1240, height / 1080)
    readonly property var c: Skin.colors

    color: root.c.background

    function rate(bps) {
        const units = ["B/s", "KB/s", "MB/s", "GB/s"]
        let i = 0
        while (bps >= 1000 && i < units.length - 1) { bps /= 1000; i++ }
        return i === 0 ? Math.round(bps) + units[i] : bps.toFixed(1) + units[i]
    }

    component Label: Text {
        color: root.c.text
        font.family: Skin.font
    }

    // A flat, round-cornered tile, as the keyboard's keys.
    component Cap: Rectangle {
        default property alias content: face.data
        radius: 24 * root.s
        color: root.c.key
        Item {
            id: face
            anchors.fill: parent
        }
    }

    component Tile: Cap {
        id: tile
        property string title
        property string value: "–"
        Layout.preferredWidth: 300 * root.s
        Layout.fillHeight: true
        Label {
            x: 24 * root.s; y: 18 * root.s
            text: tile.title
            color: root.c.keyText
            opacity: 0.7
            font { pixelSize: 22 * root.s; weight: Font.Bold; letterSpacing: 1 * root.s }
        }
        Label {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 14 * root.s
            text: tile.value
            color: root.c.keyText
            font { pixelSize: 84 * root.s; bold: true }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 26 * root.s
        spacing: 22 * root.s

        // Status bar: time | network speed, battery
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12 * root.s
            Layout.rightMargin: 12 * root.s
            spacing: 24 * root.s
            Label {
                text: root.st.time ? root.st.time.text : ""
                font { pixelSize: 26 * root.s; weight: Font.DemiBold }
            }
            Item { Layout.fillWidth: true }
            Label {
                text: root.st.net ? root.rate(root.st.net.bytesPerSec) : ""
                opacity: 0.7
                font { pixelSize: 26 * root.s; weight: Font.DemiBold }
            }
            Label {
                text: root.st.battery
                      ? (root.st.battery.status === "Charging" ? "⚡ " : "🔋 ") + root.st.battery.percent + "%"
                      : ""
                font { pixelSize: 26 * root.s; weight: Font.DemiBold }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 22 * root.s

            // Current FPS, on the teal plate.
            Rectangle {
                id: plate
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 32 * root.s
                color: root.c.accent

                Rectangle {
                    x: 24 * root.s
                    y: 20 * root.s
                    width: pill.implicitWidth + 36 * root.s
                    height: pill.implicitHeight + 12 * root.s
                    radius: height / 2
                    color: Qt.rgba(0, 0, 0, 0.22)
                    Label {
                        id: pill
                        anchors.centerIn: parent
                        text: "GAME MODE"
                        color: root.c.litText
                        font { pixelSize: 22 * root.s; bold: true; letterSpacing: 1 * root.s }
                    }
                }
                Row {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 40 * root.s
                    spacing: 8 * root.s
                    Label {
                        id: fpsNum
                        text: root.st.fps !== undefined && root.st.fps !== null ? Math.round(root.st.fps) : "–"
                        color: root.c.litText
                        font { pixelSize: 210 * root.s; weight: Font.Bold }
                    }
                    Label {
                        anchors.baseline: fpsNum.baseline
                        text: "FPS"
                        color: root.c.litText
                        opacity: 0.9
                        font { pixelSize: 40 * root.s; weight: Font.Medium }
                    }
                }
                Label {
                    anchors.right: parent.right
                    anchors.rightMargin: 26 * root.s
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 20 * root.s
                    text: "CURRENT FPS"
                    color: root.c.litText
                    opacity: 0.85
                    font { pixelSize: 22 * root.s; weight: Font.Bold; letterSpacing: 1 * root.s }
                }
            }

            ColumnLayout {
                Layout.fillHeight: true
                spacing: 22 * root.s
                Tile {
                    title: "TEMP"
                    value: root.st.tempC !== undefined ? root.st.tempC + "℃" : "–"
                }
                Tile {
                    title: "FAN"
                    value: root.st.fanPct !== undefined ? root.st.fanPct + "%" : "–"
                }
            }
        }

        // Gauges
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 206 * root.s
            color: root.c.borderQuiet
            radius: 24 * root.s
            RowLayout {
                anchors.fill: parent
                anchors.margins: 18 * root.s
                spacing: 10 * root.s
                Gauge {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    scale_: root.s
                    label: "CPU"; unit: "GHz"; color: root.c.accent
                    value: root.st.cpu ? root.st.cpu.ghz.toFixed(2) : "--"
                    fraction: root.st.cpu ? root.st.cpu.load / 100 : 0
                }
                Gauge {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    scale_: root.s
                    label: "GPU"; unit: "MHz"; color: root.c.accent
                    value: root.st.gpu ? String(root.st.gpu.mhz) : "--"
                    fraction: root.st.gpu && root.st.gpu.maxMhz ? root.st.gpu.mhz / root.st.gpu.maxMhz : 0
                }
                Gauge {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    scale_: root.s
                    label: "PWR"; unit: "W"; color: root.c.accent
                    value: root.st.powerW !== undefined ? root.st.powerW.toFixed(2) : "--"
                    fraction: root.st.powerW !== undefined ? Math.abs(root.st.powerW) / 15 : 0
                }
                Gauge {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    scale_: root.s
                    label: "RAM"; unit: "GB"; color: root.c.accent
                    value: root.st.memory ? root.st.memory.usedGb.toFixed(2) : "--"
                    fraction: root.st.memory && root.st.memory.totalGb ? root.st.memory.usedGb / root.st.memory.totalGb : 0
                }
            }
        }

        QuickControls {
            Layout.fillWidth: true
            Layout.preferredHeight: 250 * root.s
            dashboard: root.dashboard
            s: root.s
        }
    }
}

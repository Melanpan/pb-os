pragma ComponentBehavior: Bound
// Water: a dark skin after the sea at dusk: deep navy case, sea-blue
// keycaps with pale aqua letters, teal and bright aqua keys, and the
// current FPS seen through the water of a small tiled pool, which ripples
// as the dashboard opens and wherever it is tapped (WaterPad.qml; the
// keyboards' trackpads and the Deck pads are pools too). The same layout
// as the built-in dashboard (status bar, current FPS, temperature, fan,
// gauges, quick controls); the colors are skin.json's (Barry.Input's
// Skin), which the keyboards, trackpads and home screen use too.
import QtQuick
import QtQuick.Layouts
import Barry.Input

Rectangle {
    id: root
    property var dashboard
    readonly property var st: dashboard ? dashboard.stats : ({})
    readonly property real s: Math.min(width / 1240, height / 1080)
    readonly property var c: Skin.colors

    gradient: Gradient {
        GradientStop { position: 0; color: Qt.lighter(root.c.background, 1.04) }
        GradientStop { position: 1; color: Qt.darker(root.c.background, 1.04) }
    }

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

    // A keycap: a face over its darker lower edge.
    component Cap: Rectangle {
        default property alias content: face.data
        property color faceColor: root.c.key
        radius: 28 * root.s
        color: root.c.keyShadow
        Rectangle {
            id: face
            anchors { fill: parent; bottomMargin: 7 * root.s }
            radius: parent.radius
            color: parent.faceColor
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
            font { pixelSize: 22 * root.s; weight: Font.Bold; letterSpacing: 1 * root.s }
        }
        Label {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 14 * root.s
            text: tile.value
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

            // Current FPS, under the water.
            Rectangle {
                id: plateCase
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 32 * root.s
                color: root.c.panel
                border.color: root.c.borderQuiet
                border.width: 3 * root.s

                // Little bubbles in the corners.
                Repeater {
                    model: 4
                    delegate: Rectangle {
                        required property int index
                        width: 16 * root.s
                        height: width
                        radius: width / 2
                        color: Qt.rgba(0.6, 0.92, 1, 0.18)
                        border.color: Qt.rgba(0.75, 0.96, 1, 0.6)
                        border.width: 2 * root.s
                        x: index % 2 === 0 ? 10 * root.s : plateCase.width - width - 10 * root.s
                        y: index < 2 ? 10 * root.s : plateCase.height - height - 10 * root.s
                        Rectangle {
                            x: 3 * root.s; y: 3 * root.s
                            width: 4 * root.s; height: width; radius: width / 2
                            color: Qt.rgba(1, 1, 1, 0.8)
                        }
                    }
                }

                WaterPad {
                    id: plate
                    anchors { fill: parent; margins: 34 * root.s }
                    s: root.s
                    radius: 24 * root.s
                    Component.onCompleted: splash(width / 2, height / 2, 0.9)

                    inside: [
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 16 * root.s
                            width: pill.implicitWidth + 36 * root.s
                            height: pill.implicitHeight + 12 * root.s
                            radius: height / 2
                            color: Qt.rgba(0, 0.12, 0.28, 0.35)
                            Label {
                                id: pill
                                anchors.centerIn: parent
                                text: "GAME MODE"
                                color: "white"
                                font { pixelSize: 22 * root.s; bold: true; letterSpacing: 1 * root.s }
                            }
                        },
                        Row {
                            anchors.centerIn: parent
                            anchors.horizontalCenterOffset: 40 * root.s
                            spacing: 8 * root.s
                            Label {
                                id: fpsNum
                                text: root.st.fps !== undefined && root.st.fps !== null ? Math.round(root.st.fps) : "–"
                                color: "white"
                                font { pixelSize: 210 * root.s; weight: Font.ExtraBold }
                            }
                            Label {
                                anchors.baseline: fpsNum.baseline
                                text: "FPS"
                                color: "white"
                                opacity: 0.9
                                font { pixelSize: 40 * root.s; bold: true }
                            }
                        },
                        Label {
                            anchors.right: parent.right
                            anchors.rightMargin: 26 * root.s
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 20 * root.s
                            text: "CURRENT FPS"
                            color: "white"
                            opacity: 0.9
                            font { pixelSize: 26 * root.s; weight: Font.DemiBold; letterSpacing: 1 * root.s }
                        }
                    ]
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
            color: root.c.panel
            radius: 28 * root.s
            RowLayout {
                anchors.fill: parent
                anchors.margins: 18 * root.s
                spacing: 10 * root.s
                Gauge {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    scale_: root.s
                    label: "CPU"; unit: "GHz"; color: "#4fd6ff"
                    value: root.st.cpu ? root.st.cpu.ghz.toFixed(2) : "--"
                    fraction: root.st.cpu ? root.st.cpu.load / 100 : 0
                }
                Gauge {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    scale_: root.s
                    label: "GPU"; unit: "MHz"; color: "#2fe0bd"
                    value: root.st.gpu ? String(root.st.gpu.mhz) : "--"
                    fraction: root.st.gpu && root.st.gpu.maxMhz ? root.st.gpu.mhz / root.st.gpu.maxMhz : 0
                }
                Gauge {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    scale_: root.s
                    label: "PWR"; unit: "W"; color: "#ffc069"
                    value: root.st.powerW !== undefined ? root.st.powerW.toFixed(2) : "--"
                    fraction: root.st.powerW !== undefined ? Math.abs(root.st.powerW) / 15 : 0
                }
                Gauge {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    scale_: root.s
                    label: "RAM"; unit: "GB"; color: "#8fa8ff"
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

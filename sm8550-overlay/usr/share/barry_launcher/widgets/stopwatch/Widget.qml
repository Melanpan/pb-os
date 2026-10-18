// Stopwatch: Start/Stop, and Lap (while running) or Reset; the last lap
// under the time. It keeps counting while the page is away. An example
// widget pack (README.md).
import QtQuick
import QtQuick.Layouts
import Barry.Input

Item {
    id: root
    property real s: 1
    property bool shown: true

    property bool running: false
    property double startedAt: 0   // Date.now() when it last started
    property double banked: 0      // ms counted before that
    property double tick: 0
    property var laps: []
    readonly property double elapsed: banked + (running ? tick - startedAt : 0)

    function show(ms) {
        const t = Math.floor(ms / 100)
        const tenths = t % 10, secs = Math.floor(t / 10) % 60, mins = Math.floor(t / 600)
        return mins + ":" + (secs < 10 ? "0" : "") + secs + "." + tenths
    }
    Timer {
        interval: 50
        repeat: true
        running: root.running && root.shown
        triggeredOnStart: true
        onTriggered: root.tick = Date.now()
    }

    component Button: Rectangle {
        id: b
        property string label
        property bool lit: false
        signal tapped()
        Layout.fillWidth: true
        Layout.preferredHeight: 58 * root.s
        radius: height / 2
        color: tap.pressed ? Skin.colors.pressed : lit ? Skin.colors.lit : Skin.colors.raised
        Text {
            anchors.centerIn: parent
            text: b.label
            color: b.lit && !tap.pressed ? Skin.colors.litText : Skin.colors.text
            font { family: Skin.font || "Noto Sans"; pixelSize: 23 * root.s; weight: Font.DemiBold }
        }
        TapHandler {
            id: tap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: b.tapped()
        }
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8 * root.s
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: root.show(root.elapsed)
            color: Skin.colors.text
            font { family: Skin.font || "Noto Sans"; pixelSize: 64 * root.s; weight: Font.Bold; features: { "tnum": 1 } }
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: root.laps.length ? "Lap " + root.laps.length + "  " + root.show(root.laps[root.laps.length - 1]) : " "
            color: Skin.colors.text
            opacity: 0.6
            font { family: Skin.font || "Noto Sans"; pixelSize: 20 * root.s }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 12 * root.s
            Button {
                label: root.running ? "Lap" : "Reset"
                onTapped: {
                    if (root.running) {
                        const now = Date.now()
                        root.laps = root.laps.concat([root.banked + now - root.startedAt
                                                      - root.laps.reduce((a, b) => a + b, 0)])
                    } else {
                        root.banked = 0
                        root.laps = []
                    }
                }
            }
            Button {
                label: root.running ? "Stop" : "Start"
                lit: !root.running
                onTapped: {
                    const now = Date.now()
                    if (root.running) {
                        root.banked += now - root.startedAt
                        root.tick = now
                    } else {
                        root.startedAt = now
                        root.tick = now
                    }
                    root.running = !root.running
                }
            }
        }
    }
}

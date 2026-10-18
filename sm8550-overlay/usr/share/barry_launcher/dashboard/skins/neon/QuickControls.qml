pragma ComponentBehavior: Bound
// Quick controls for the Neon skin (the built-in one's, in the skin's
// colors): fan profile and the games' refresh rate (the stick lights are on
// Barry's quick settings page). Reads dashboard.controls and changes it with
// dashboard.setControls(); a control the device lacks is left out.
import QtQuick
import QtQuick.Layouts
import Barry.Input

Rectangle {
    id: qc
    property var dashboard
    property real s: 1
    readonly property var c: dashboard ? dashboard.controls : ({})

    color: Skin.colors.panel
    radius: 28 * s

    readonly property var fanNames: ({ eco: "Quiet", balanced: "Balanced", performance: "Performance", max: "Max" })

    component Label: Text {
        color: Skin.colors.text
        font { family: Skin.font; pixelSize: 22 * qc.s; weight: Font.DemiBold }
        opacity: 0.8
        Layout.preferredWidth: 130 * qc.s
    }

    component Choice: Rectangle {
        id: choice
        property string label
        property bool active: false
        signal chosen()
        Layout.fillWidth: true
        Layout.fillHeight: true
        radius: 18 * qc.s
        color: tap.pressed ? Skin.colors.pressed : active ? Skin.colors.lit : Skin.colors.key
        Text {
            anchors.centerIn: parent
            text: choice.label
            color: choice.active ? Skin.colors.litText : Skin.colors.keyText
            font { family: Skin.font; pixelSize: 24 * qc.s; weight: Font.DemiBold }
        }
        TapHandler {
            id: tap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: choice.chosen()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18 * qc.s
        spacing: 12 * qc.s

        // Fan profile
        RowLayout {
            visible: !!qc.c.fan
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12 * qc.s
            Label { text: "FAN" }
            Repeater {
                model: qc.c.fan ? qc.c.fan.profiles : []
                delegate: Choice {
                    required property string modelData
                    label: qc.fanNames[modelData] || modelData
                    active: qc.c.fan.profile === modelData
                    onChosen: qc.dashboard.setControls({ fanProfile: modelData })
                }
            }
        }

        // Refresh rate
        RowLayout {
            visible: !!qc.c.refresh
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12 * qc.s
            Label { text: "REFRESH" }
            Repeater {
                model: qc.c.refresh ? qc.c.refresh.rates : []
                delegate: Choice {
                    required property int modelData
                    label: modelData + " Hz"
                    // No choice made: games get the highest rate.
                    active: (qc.c.refresh.choice || qc.c.refresh.rates[qc.c.refresh.rates.length - 1]) === modelData
                    onChosen: qc.dashboard.setControls({ refreshHz: modelData })
                }
            }
            Text {
                visible: qc.c.refresh && qc.c.refresh.hz > 0
                text: qc.c.refresh ? "now " + qc.c.refresh.hz + " Hz" : ""
                color: Skin.colors.text
                opacity: 0.55
                font { family: Skin.font; pixelSize: 22 * qc.s }
                Layout.preferredWidth: 130 * qc.s
                horizontalAlignment: Text.AlignRight
            }
        }
    }
}

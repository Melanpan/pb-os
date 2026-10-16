// The Cloud skin's ring: label, value and unit inside a 270° arc, in the
// skin's colors (Barry.Input's Skin).
import QtQuick
import QtQuick.Shapes
import Barry.Input

Item {
    id: g
    property string label
    property string value: "--"
    property string unit
    property color color: "white"
    property real fraction: 0
    property real scale_: 1

    readonly property real f: Math.max(0, Math.min(1, fraction))
    readonly property real r: Math.min(width, height) / 2 - 10 * scale_

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: Qt.rgba(1, 1, 1, 0.7)
            strokeWidth: 12 * g.scale_
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: g.width / 2; centerY: g.height / 2
                radiusX: g.r; radiusY: g.r
                startAngle: 135; sweepAngle: 270
            }
        }
        ShapePath {
            strokeColor: g.f > 0 ? g.color : "transparent"
            strokeWidth: 12 * g.scale_
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: g.width / 2; centerY: g.height / 2
                radiusX: g.r; radiusY: g.r
                startAngle: 135; sweepAngle: 270 * g.f
            }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -41 * g.scale_
        text: g.label
        color: Skin.colors.text
        font { family: Skin.font; pixelSize: 20 * g.scale_; bold: true }
    }
    Text {
        anchors.centerIn: parent
        text: g.value
        color: Skin.colors.text
        font { family: Skin.font; pixelSize: 40 * g.scale_; bold: true }
    }
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: 38 * g.scale_
        text: g.unit
        color: Skin.colors.text
        font { family: Skin.font; pixelSize: 18 * g.scale_ }
    }
}

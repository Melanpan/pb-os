// Battery: the charge in a ring, and whether it is charging, with the power
// going in or out. Read from barry_launcher_statsd's /stats every few
// seconds while the page shows. An example widget pack (README.md).
import QtQuick
import QtQuick.Shapes
import Barry.Input

Item {
    id: root
    property real s: 1
    property bool shown: true

    property int percent: -1
    property string status: ""
    property real watts: 0

    function read() {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE || x.status !== 200)
                return
            try {
                const st = JSON.parse(x.responseText)
                root.percent = st.battery.percent
                root.status = st.battery.status
                root.watts = st.powerW
            } catch (e) {}
        }
        x.open("GET", "http://127.0.0.1:47823/stats")
        x.send()
    }
    onShownChanged: if (shown) read()
    Component.onCompleted: read()
    Timer {
        interval: 5000
        repeat: true
        running: root.shown
        onTriggered: root.read()
    }

    readonly property bool charging: status === "Charging" || status === "Full"
    readonly property real ring: Math.min(width, height - 34 * s) - 8 * s
    readonly property color fill: percent >= 0 && percent <= 15 && !charging ? Skin.colors.danger : Skin.colors.accent

    Item {
        id: dial
        anchors.horizontalCenter: parent.horizontalCenter
        width: root.ring
        height: root.ring
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Skin.colors.raised
                strokeWidth: 12 * root.s
                fillColor: "transparent"
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: dial.width / 2 - 8 * root.s; radiusY: radiusX
                    startAngle: 0; sweepAngle: 360
                }
            }
            ShapePath {
                strokeColor: root.fill
                strokeWidth: 12 * root.s
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: dial.width / 2; centerY: dial.height / 2
                    radiusX: dial.width / 2 - 8 * root.s; radiusY: radiusX
                    startAngle: -90; sweepAngle: 360 * Math.max(0, root.percent) / 100
                }
            }
        }
        Text {
            anchors.centerIn: parent
            text: root.percent >= 0 ? root.percent + "%" : "–"
            color: Skin.colors.text
            font { family: Skin.font || "Noto Sans"; pixelSize: root.ring * 0.26; weight: Font.Bold }
        }
    }
    Text {
        anchors.top: dial.bottom
        anchors.topMargin: 6 * root.s
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.status === "" ? "" : (root.charging ? "Charging" : "On battery")
              + (root.watts !== 0 ? " · " + Math.abs(root.watts).toFixed(1) + " W" : "")
        color: Skin.colors.text
        opacity: 0.7
        font { family: Skin.font || "Noto Sans"; pixelSize: 20 * root.s }
    }
}

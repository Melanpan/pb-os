// The Jello skin's trackpad: a glossy, see-through slab of thick jelly on
// a polka-dot plate, which shows through it bent as the jelly moves. A
// finger coming down sends a slow, heavy wobble out from where it landed
// and presses a dent that trails it and oozes back when it lifts; kick()
// (a click, the moment Barry buzzes) sends a stronger one. The pad's touch area goes
// inside (the default property) and keeps its touches: the PointHandler
// over it only watches where the finger is (a passive grab) to aim the
// wobble, and all the moving is jelly.vert's.
import QtQuick
import Barry.Input

Item {
    id: jelly
    property real s: 1
    property real radius: 36 * s
    // The flavor: the slab's top and bottom colors.
    property color topColor: "#ff5a70"
    property color bottomColor: "#c40f2e"
    property bool lit: false  // a finger on it: the rim lights
    property string hint
    property bool showHint: true
    default property alias content: touchLayer.data
    // Drawn in the slab, wobbling with it (the dashboard's FPS).
    property alias inside: extra.data

    // Where the finger is (item pixels) and whether there is one.
    readonly property bool pressing: finger.active
    readonly property point fingerAt: finger.active ? finger.point.position : lastAt
    property point lastAt: Qt.point(width / 2, height / 2)

    // A wobble from x, y; strength 1 is a click's.
    function kick(x, y, strength) {
        const k = Qt.vector4d(x, y, now, strength * hapticsScale)
        i3 = i2; i2 = i1; i1 = i0; i0 = k
        lastKick = now
    }
    // A click at the finger (or where it last was): Barry buzzes for it.
    function kickAtFinger(strength) {
        kick(fingerAt.x, fingerAt.y, strength)
    }

    // Stronger buzzes, bigger wobbles; buzzes off, still a wobble.
    readonly property real hapticsScale: !Haptics.enabled ? 0.8 : [0.75, 0.75, 1, 1.3][Haptics.strength] || 1

    property real now: 0
    property real lastKick: -10
    property vector4d i0: Qt.vector4d(0, 0, -10, 0)
    property vector4d i1: i0
    property vector4d i2: i0
    property vector4d i3: i0
    // Thick jelly: the dent sinks and oozes back slowly, overshooting a
    // little, and trails a moving finger.
    property real dentDepth: pressing ? 1 : 0
    Behavior on dentDepth { SpringAnimation { spring: 1.8; damping: 0.22; mass: 1.6; epsilon: 0.002 } }
    property real dentX: fingerAt.x
    property real dentY: fingerAt.y
    Behavior on dentX { enabled: jelly.pressing; SpringAnimation { spring: 2.5; damping: 0.5 } }
    Behavior on dentY { enabled: jelly.pressing; SpringAnimation { spring: 2.5; damping: 0.5 } }

    // The clock runs only while something wobbles.
    FrameAnimation {
        running: jelly.visible && (jelly.now - jelly.lastKick < 5 || jelly.pressing || Math.abs(jelly.dentDepth) > 0.002)
        onTriggered: jelly.now += Math.min(frameTime, 0.05)
    }

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    // The plate the jelly sits on: only seen through it.
    Rectangle {
        id: plate
        anchors.fill: parent
        radius: jelly.radius
        color: "#fffaf6"
        // Polka dots, 64 px apart (none while the layout has no size yet).
        readonly property real step: 64 * jelly.s
        readonly property int cols: step > 0 && width > 0 ? Math.ceil(width / step) : 0
        readonly property int rows: step > 0 && height > 0 ? Math.ceil(height / step) : 0
        Repeater {
            model: plate.cols * plate.rows
            delegate: Rectangle {
                required property int index
                readonly property int row: Math.floor(index / plate.cols)
                width: 18 * jelly.s
                height: width
                radius: width / 2
                x: (index % plate.cols) * 64 * jelly.s + (row % 2 ? 32 : 0) * jelly.s + 14 * jelly.s
                y: row * 64 * jelly.s + 14 * jelly.s
                color: "#f3c6cc"
            }
        }
    }
    ShaderEffectSource {
        id: plateTexture
        sourceItem: plate
        hideSource: true
        smooth: true
    }

    // The slab as drawn before it moves: see-through, deeper in color at
    // its thick rim.
    Item {
        id: look
        anchors.fill: parent

        Rectangle {
            id: slab
            anchors.fill: parent
            radius: jelly.radius
            border.color: jelly.lit ? Qt.lighter(jelly.topColor, 1.25) : jelly.tint(Qt.darker(jelly.bottomColor, 1.15), 0.9)
            border.width: (jelly.lit ? 5 : 3) * jelly.s
            gradient: Gradient {
                GradientStop { position: 0; color: jelly.tint(jelly.topColor, 0.5) }
                GradientStop { position: 0.55; color: jelly.tint(Qt.darker(jelly.topColor, 1.08), 0.58) }
                GradientStop { position: 1; color: jelly.tint(jelly.bottomColor, 0.76) }
            }
        }
        Rectangle {
            anchors { fill: parent; margins: slab.border.width }
            radius: jelly.radius - slab.border.width
            color: "transparent"
            border.color: jelly.tint(jelly.bottomColor, 0.32)
            border.width: 18 * jelly.s
        }
        // The gloss: a soft shine along the top and a streak at the corner.
        Rectangle {
            x: 24 * jelly.s
            y: 14 * jelly.s
            width: parent.width - 48 * jelly.s
            height: Math.min(parent.height * 0.38, 220 * jelly.s)
            radius: jelly.radius - 10 * jelly.s
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0.42) }
                GradientStop { position: 1; color: Qt.rgba(1, 1, 1, 0) }
            }
        }
        Rectangle {
            x: 44 * jelly.s
            y: 34 * jelly.s
            width: Math.min(parent.width * 0.22, 220 * jelly.s)
            height: 22 * jelly.s
            radius: height / 2
            rotation: -6
            color: Qt.rgba(1, 1, 1, 0.75)
        }
        Rectangle {
            x: 44 * jelly.s + Math.min(parent.width * 0.22, 220 * jelly.s) + 18 * jelly.s
            y: 30 * jelly.s
            width: 22 * jelly.s
            height: width
            radius: width / 2
            color: Qt.rgba(1, 1, 1, 0.75)
        }
        // A glow low in the slab, as light through jelly.
        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 26 * jelly.s }
            height: parent.height * 0.3
            radius: jelly.radius - 14 * jelly.s
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0) }
                GradientStop { position: 1; color: Qt.rgba(1, 0.85, 0.8, 0.22) }
            }
        }
        // Bubbles set in the jelly.
        Repeater {
            model: [[0.18, 0.62, 18], [0.27, 0.74, 10], [0.71, 0.32, 14], [0.83, 0.58, 22], [0.62, 0.81, 9], [0.42, 0.28, 8]]
            delegate: Rectangle {
                required property var modelData
                width: modelData[2] * jelly.s
                height: width
                radius: width / 2
                x: modelData[0] * look.width - width / 2
                y: modelData[1] * look.height - height / 2
                color: Qt.rgba(1, 1, 1, 0.16)
                border.color: Qt.rgba(1, 1, 1, 0.5)
                border.width: 2 * jelly.s
            }
        }
        Text {
            anchors.centerIn: parent
            visible: jelly.showHint
            text: jelly.hint
            horizontalAlignment: Text.AlignHCenter
            color: "white"
            opacity: 0.92
            style: Text.Raised
            styleColor: Qt.rgba(0.35, 0, 0.05, 0.6)
            lineHeight: 1.3
            font { family: Skin.font; pixelSize: 28 * jelly.s; weight: Font.DemiBold }
        }
        Item {
            id: extra
            anchors.fill: parent
        }
    }

    ShaderEffectSource {
        id: picture
        sourceItem: look
        hideSource: true
        smooth: true
    }

    ShaderEffect {
        anchors.fill: parent
        mesh: GridMesh { resolution: Qt.size(64, 48) }
        property variant source: picture
        property variant behind: plateTexture
        property size size: Qt.size(width, height)
        property real refraction: 2.6
        property real now: jelly.now
        property real unit: jelly.s
        property vector4d i0: jelly.i0
        property vector4d i1: jelly.i1
        property vector4d i2: jelly.i2
        property vector4d i3: jelly.i3
        property vector4d dent: Qt.vector4d(jelly.dentX, jelly.dentY, jelly.dentDepth, 0)
        vertexShader: Qt.resolvedUrl("shaders/jelly.vert.qsb")
        fragmentShader: Qt.resolvedUrl("shaders/jelly.frag.qsb")
    }

    Item {
        id: touchLayer
        anchors.fill: parent
    }
    // Over the touch area, watching: the PointHandler's grab is passive, so
    // every touch still goes on down to the area (a handler on JellyPad
    // itself would never see a touch the area takes).
    Item {
        anchors.fill: parent
        PointHandler {
            id: finger
            target: null
            onActiveChanged: {
                if (active) {
                    jelly.lastAt = point.position
                    jelly.kick(point.position.x, point.position.y, 0.55)
                }
            }
            onPointChanged: if (active) jelly.lastAt = point.position
        }
    }
}

// The Water skin's trackpad: a tiled pool of clear blue water, light
// netting its floor. A finger landing drops rings out from where it
// touched; a finger moving leaves a trail of them behind it; a finger held
// still sends a slow ring now and then; splash() (a click, the moment Barry
// buzzes) drops a bigger one. The pad's touch area goes inside (the
// default property) and keeps its touches: the PointHandler over it only
// watches where the finger is (a passive grab), and all the rippling is
// water.frag's.
import QtQuick
import Barry.Input

Item {
    id: water
    property real s: 1
    property real radius: 36 * s
    // The water: its color in the shallows (top) and the deep end (bottom).
    property color topColor: "#3fc4e8"
    property color bottomColor: "#0b5e9c"
    property real caustics: 1  // how bright the light on the floor is
    property bool lit: false  // a finger on it: the rim lights
    default property alias content: touchLayer.data
    // Drawn under the water, rippling with it (the dashboard's FPS).
    property alias inside: extra.data

    // Where the finger is (item pixels) and whether there is one.
    readonly property bool pressing: finger.active
    readonly property point fingerAt: finger.active ? finger.point.position : lastAt
    property point lastAt: Qt.point(width / 2, height / 2)

    // Rings from x, y; strength 1 is a click's.
    function splash(x, y, strength) {
        surface["r" + slot] = Qt.vector4d(x, y, now, strength * hapticsScale)
        slot = (slot + 1) % 24
        lastDrop = now
        lastDropAt = Qt.point(x, y)
    }
    // A click at the finger (or where it last was): Barry buzzes for it.
    function splashAtFinger(strength) {
        splash(fingerAt.x, fingerAt.y, strength)
    }

    // Stronger buzzes, bigger rings; buzzes off, still rings.
    readonly property real hapticsScale: !Haptics.enabled ? 0.8 : [0.75, 0.75, 1, 1.3][Haptics.strength] || 1

    property real now: 0
    property real lastDrop: -10
    property point lastDropAt
    property int slot: 0

    // The clock runs only while the water moves (a ring lasts 3 s) or a
    // finger is on it.
    FrameAnimation {
        running: water.visible && (water.now - water.lastDrop < 3 || water.pressing)
        onTriggered: {
            water.now += Math.min(frameTime, 0.05)
            // A finger held still: a slow, small ring now and then.
            if (water.pressing && water.now - water.lastDrop > 0.7)
                water.splash(water.fingerAt.x, water.fingerAt.y, 0.3)
        }
    }

    function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    // The pool as it looks through still water: only seen through the
    // surface (the shader).
    Item {
        id: pool
        anchors.fill: parent

        Rectangle {
            id: basin
            anchors.fill: parent
            radius: water.radius
            gradient: Gradient {
                GradientStop { position: 0; color: water.topColor }
                GradientStop { position: 1; color: water.bottomColor }
            }
            border.color: water.lit ? "#bff3ff" : water.tint(Qt.lighter(water.topColor, 1.35), 0.85)
            border.width: (water.lit ? 6 : 4) * water.s
        }
        // Tiles on the floor: thin grout lines, 72 px apart.
        Item {
            id: tiles
            anchors { fill: parent; margins: basin.border.width }
            clip: true
            readonly property real step: 72 * water.s
            Repeater {
                model: tiles.step > 0 && tiles.width > 0 ? Math.ceil(tiles.width / tiles.step) : 0
                delegate: Rectangle {
                    required property int index
                    x: (index + 0.5) * tiles.step
                    width: 2 * water.s
                    height: tiles.height
                    color: Qt.rgba(1, 1, 1, 0.13)
                }
            }
            Repeater {
                model: tiles.step > 0 && tiles.height > 0 ? Math.ceil(tiles.height / tiles.step) : 0
                delegate: Rectangle {
                    required property int index
                    y: (index + 0.5) * tiles.step
                    height: 2 * water.s
                    width: tiles.width
                    color: Qt.rgba(1, 1, 1, 0.13)
                }
            }
        }
        // The deep end's shade along the bottom and the pool wall's shadow
        // along the top.
        Rectangle {
            anchors { fill: parent; margins: basin.border.width }
            radius: water.radius - basin.border.width
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.rgba(0, 0.1, 0.2, 0.22) }
                GradientStop { position: 0.12; color: Qt.rgba(0, 0.1, 0.2, 0) }
                GradientStop { position: 0.7; color: Qt.rgba(0, 0.05, 0.15, 0) }
                GradientStop { position: 1; color: Qt.rgba(0, 0.05, 0.15, 0.3) }
            }
        }
        Item {
            id: extra
            anchors.fill: parent
        }
    }

    ShaderEffectSource {
        id: picture
        sourceItem: pool
        hideSource: true
        smooth: true
    }

    ShaderEffect {
        id: surface
        anchors.fill: parent
        readonly property vector4d none: Qt.vector4d(0, 0, -10, 0)
        property variant source: picture
        property size size: Qt.size(width, height)
        property real now: water.now
        property real unit: water.s
        property real caustics: water.caustics
        property vector4d r0: none
        property vector4d r1: none
        property vector4d r2: none
        property vector4d r3: none
        property vector4d r4: none
        property vector4d r5: none
        property vector4d r6: none
        property vector4d r7: none
        property vector4d r8: none
        property vector4d r9: none
        property vector4d r10: none
        property vector4d r11: none
        property vector4d r12: none
        property vector4d r13: none
        property vector4d r14: none
        property vector4d r15: none
        property vector4d r16: none
        property vector4d r17: none
        property vector4d r18: none
        property vector4d r19: none
        property vector4d r20: none
        property vector4d r21: none
        property vector4d r22: none
        property vector4d r23: none
        fragmentShader: Qt.resolvedUrl("shaders/water.frag.qsb")
    }

    Item {
        id: touchLayer
        anchors.fill: parent
    }
    // Over the touch area, watching: the PointHandler's grab is passive, so
    // every touch still goes on down to the area (a handler on WaterPad
    // itself would never see a touch the area takes).
    Item {
        anchors.fill: parent
        PointHandler {
            id: finger
            target: null
            onActiveChanged: {
                if (active) {
                    water.lastAt = point.position
                    water.splash(point.position.x, point.position.y, 0.7)
                }
            }
            // Moving: a ring every 30 px, at most 14 a second.
            onPointChanged: {
                if (!active)
                    return
                const p = point.position
                water.lastAt = p
                const dx = p.x - water.lastDropAt.x, dy = p.y - water.lastDropAt.y
                if (dx * dx + dy * dy >= Math.pow(30 * water.s, 2) && water.now - water.lastDrop >= 0.07)
                    water.splash(p.x, p.y, 0.5)
            }
        }
    }
}

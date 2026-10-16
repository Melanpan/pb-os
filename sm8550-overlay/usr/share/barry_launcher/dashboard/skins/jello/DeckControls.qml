// The Jello skin's Deck controls: the built-in page (shell/DeckControls.qml)
// with the pads as slabs of lime and berry-blue jelly (JellyPad.qml) that
// jiggle out from wherever a finger lands, and harder from the finger as
// the pad's Click goes down (Barry buzzes); the buttons wobble as they go
// down. The built-in page's notes follow.
//
// The Steam Deck's trackpads and back buttons on the bottom screen
// (DeckApp.qml's page). Steam sees them on the controller it already knows,
// the virtual Steam Deck InputPlumber makes (Barry.Input's DeckPad and
// DeckButtons), so every game's Deck layout uses them: a pad as a mouse,
// a joystick, radial menus; L4 L5 R4 R5 as Steam Input binds them.
//
// The pads: one finger each, its place on the pad is the Deck pad's touch.
// Their clicks (the Deck's press-down) are the buttons under them, held
// while the finger moves. Steam's own trackpad haptics buzz through
// InputPlumber; Barry buzzes for its buttons.
//
// Only the layout and its look: the touches are Barry.Input's (DeckPadArea,
// PressArea); the colors are the skin's.
import QtQuick
import Barry.Input

Rectangle {
    id: deck
    property real s: 1
    color: Skin.colors.background

    DeckPad { id: leftPad; side: "left" }
    DeckPad { id: rightPad; side: "right" }
    DeckButtons { id: buttons }

    // Nothing stays held when the page goes away: the touch areas let go as
    // they hide, and the devices with everything held as they go.

    readonly property real margin: 20 * s
    readonly property real rowHeight: 130 * s
    readonly property real padSize: Math.min((width - 3 * margin) / 2,
                                             height - 2 * rowHeight - 5 * margin)

    component DeckButton: Rectangle {
        id: btn
        property string button
        property string label: button
        // The pad this button clicks: it jiggles from the finger.
        property var jelly: null
        readonly property bool down: hold.pressed
        onDownChanged: if (down) {
            wobble.restart()
            if (jelly)
                jelly.kickAtFinger(1)
        }
        transform: Scale {
            id: squash
            origin.x: btn.width / 2
            origin.y: btn.height
        }
        ParallelAnimation {
            id: wobble
            NumberAnimation { target: squash; property: "xScale"; from: 1.08; to: 1; duration: 700; easing.type: Easing.OutElastic; easing.amplitude: 1.2; easing.period: 0.28 }
            NumberAnimation { target: squash; property: "yScale"; from: 0.86; to: 1; duration: 700; easing.type: Easing.OutElastic; easing.amplitude: 1.2; easing.period: 0.28 }
        }
        radius: 28 * deck.s
        color: hold.pressed ? Skin.colors.accentFill : Skin.colors.panel
        border.color: hold.pressed ? Skin.colors.accent : Skin.colors.border
        border.width: (hold.pressed ? 3 : 2) * deck.s
        PressArea {
            id: hold
            anchors.fill: parent
            deck: buttons
            button: btn.button
        }
        Text {
            anchors.centerIn: parent
            text: btn.label
            color: Skin.colors.text
            opacity: 0.85
            font { family: Skin.font; pixelSize: 34 * deck.s; weight: Font.DemiBold }
        }
    }

    component Pad: JellyPad {
        id: padFace
        property DeckPad pad
        property string label
        width: deck.padSize
        height: deck.padSize
        s: deck.s
        radius: 44 * deck.s
        lit: pad.touching
        showHint: false  // no text on the pads (as the built-in page)
        DeckPadArea {
            id: area
            anchors.fill: parent
            pad: padFace.pad
        }
    }

    // Back buttons along the top: L4 L5 on the left, R5 R4 on the right, as
    // they sit under the fingers on a Deck's back.
    Row {
        x: deck.margin
        y: deck.margin
        spacing: deck.margin
        DeckButton { button: "L4"; width: (deck.padSize - deck.margin) / 2; height: deck.rowHeight }
        DeckButton { button: "L5"; width: (deck.padSize - deck.margin) / 2; height: deck.rowHeight }
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: deck.margin
        y: deck.margin
        spacing: deck.margin
        DeckButton { button: "R5"; width: (deck.padSize - deck.margin) / 2; height: deck.rowHeight }
        DeckButton { button: "R4"; width: (deck.padSize - deck.margin) / 2; height: deck.rowHeight }
    }

    // The pads, each with its click under it.
    Column {
        x: deck.margin
        y: deck.margin * 2 + deck.rowHeight
        spacing: deck.margin
        Pad { id: leftJelly; pad: leftPad; topColor: "#a6e85f"; bottomColor: "#3f961a" }
        DeckButton { button: "LeftPadClick"; label: ""; jelly: leftJelly; width: deck.padSize; height: deck.rowHeight }
    }
    Column {
        anchors.right: parent.right
        anchors.rightMargin: deck.margin
        y: deck.margin * 2 + deck.rowHeight
        spacing: deck.margin
        Pad { id: rightJelly; pad: rightPad; topColor: "#62b2ff"; bottomColor: "#1f47c2" }
        DeckButton { button: "RightPadClick"; label: ""; jelly: rightJelly; width: deck.padSize; height: deck.rowHeight }
    }
}

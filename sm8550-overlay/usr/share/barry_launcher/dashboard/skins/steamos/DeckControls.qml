// The SteamOS skin's Deck controls: the built-in one (shell/), whose properties
// and behavior this keeps, drawn the way SteamOS's own Game Mode draws
// (Steam's CSS): small corners (2 to 8 px), flat dark panels, and a press
// that turns the control white with dark text. Steam's own font, Motiva
// Sans, is Valve's and comes encrypted in SteamOS, so the text is Barry's
// Noto Sans. The built-in file's notes follow.
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
import "file:///usr/share/barry_launcher/shell" as Shell

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
        radius: 4 * deck.s
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

    component Pad: Rectangle {
        id: padFace
        property DeckPad pad
        property string label
        width: deck.padSize
        height: deck.padSize
        radius: 8 * deck.s
        color: Skin.colors.surface
        border.color: pad.touching ? Skin.colors.accent : Skin.colors.borderQuiet
        border.width: 3 * deck.s

        // Where fingers have been, when the skin has the thermochromic trail.
        HeatTrail {
            id: heat
            anchors.fill: parent
            anchors.margins: parent.border.width
            visible: Skin.trackpadHeat.length > 1
            source: area
            colors: Skin.trackpadHeat
            radius: parent.radius - parent.border.width
            fingerSize: 110 * deck.s
        }

        DeckPadArea {
            id: area
            anchors.fill: parent
            pad: padFace.pad
        }
        // Where the finger is (the trail shows it, when there is one).
        Rectangle {
            visible: area.touching && Skin.trackpadHeat.length < 2
            width: 56 * deck.s
            height: width
            radius: width / 2
            color: Skin.colors.accent
            opacity: 0.6
            x: area.fingerX * padFace.width - width / 2
            y: area.fingerY * padFace.height - height / 2
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
        Pad { pad: leftPad }
        DeckButton { button: "LeftPadClick"; label: ""; width: deck.padSize; height: deck.rowHeight }
    }
    Column {
        anchors.right: parent.right
        anchors.rightMargin: deck.margin
        y: deck.margin * 2 + deck.rowHeight
        spacing: deck.margin
        Pad { pad: rightPad }
        DeckButton { button: "RightPadClick"; label: ""; width: deck.padSize; height: deck.rowHeight }
    }
}

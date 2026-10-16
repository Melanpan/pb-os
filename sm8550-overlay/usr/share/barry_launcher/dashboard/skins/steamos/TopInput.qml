pragma ComponentBehavior: Bound
// The SteamOS skin's Trackpad page: the built-in one (shell/), whose properties
// and behavior this keeps, drawn the way SteamOS's own Game Mode draws
// (Steam's CSS): small corners (2 to 8 px), flat dark panels, and a press
// that turns the control white with dark text. Steam's own font, Motiva
// Sans, is Valve's and comes encrypted in SteamOS, so the text is Barry's
// Noto Sans. The built-in file's notes follow.
//
// The bottom screen as the top screen's trackpad and keyboard. Moves a
// virtual mouse and keyboard (Barry.Input: uinput devices of this process,
// called directly), which only the top screen sees. Used by the Trackpad app
// (TopInputApp.qml) and the Desktop Mode window (Desktop.qml).
//
// Trackpad: one finger moves the pointer (faster strokes go further), a tap
// clicks, a two-finger tap right-clicks, a three-finger tap middle-clicks,
// two fingers scroll, and a tap followed by a touch holds the button down to
// drag. The buttons along the bottom can be held while another finger moves.
// Keyboard: the keys, with a smaller trackpad above them. The keys slide up
// from the bottom edge, overshooting a little and settling back, while the
// pad shrinks to make room; put away (the ⌄ key), they slide down and the pad
// fills the screen again. The app's keyboard button between the click buttons
// brings them back, and so does barry_launcher_shelld when a click lands in
// a top-screen text field (followShelld).
//
// Only the layout and its look: the pad's gestures (TrackpadArea, and
// Desktop Mode's TouchTrackpad), the click buttons (PressArea) and the keys
// (KeyboardController) are Barry.Input's; the colors are the skin's.
import QtQuick
import Barry.Input
import "file:///usr/share/barry_launcher/shell" as Shell

Rectangle {
    id: ti
    property real s: 1
    property string mode: "trackpad"  // or "keyboard"
    property bool showTabs: true  // switch between trackpad and keyboard here
    property string closeLabel: ""  // a close button, when set
    // Follow barry_launcher_shelld's asks to bring the keyboard up (a click
    // in a top-screen text field, Steam's keyboard).
    property bool followShelld: false
    // Pointer speed: pixels on the top screen per pixel on the pad, before
    // acceleration.
    property real speed: 1.6
    signal closeRequested()

    VirtualMouse { id: vmouse }
    VirtualKeyboard { id: vkeys }
    // Text goes through the uinput keyboard (games read it) while the
    // keyboard pack's letters are all on the US layout; else to the focused
    // window itself: XTest on the top screen's X server in Game Mode, KWin's
    // input method in Desktop Mode.
    Typer {
        id: textTyper
        desktop: ti.desktop
        display: ti.desktop ? "" : "$BARRY_TOP_DISPLAY"
    }
    KeyboardController {
        id: keyLogic
        typer: textTyper
        keyboard: vkeys
        preferKeyboard: true
        // Put away: the trackpad fills the screen.
        onHideRequested: ti.mode = "trackpad"
    }
    // barry_launcher_shelld's asks to bring the keyboard up (a click in a
    // top-screen text field) or down.
    KeyboardState {
        followTop: ti.followShelld && ti.visible
        onTopKeyboardAsked: function (up) { ti.mode = up ? "keyboard" : "trackpad" }
    }

    // Desktop Mode's trackpad: KWin would take the pad's touches while it
    // moves a window, so the engine reads the touchscreen itself (on a
    // thread of its own) while the pad is up, told where its pad and buttons
    // are. This only draws.
    property bool desktop: false
    readonly property bool engine: desktop && mode === "trackpad" && visible
    readonly property bool engineTouching: engineTp.reading && engineTp.touching
    readonly property var engineHeld: engineTp.held  // the Left and Right click buttons, as the engine sees them
    readonly property var enginePresses: engineTp.reading ? engineTp.presses : null
    property real desktopSpeed: 1.6
    property bool built: false  // set as the page is complete (below)

    function frac(item) {
        const p = item.mapToItem(ti, 0, 0)
        return [p.x / width, p.y / height, item.width / width, item.height / height]
    }

    function engineRegions() {
        const r = { pad: frac(padFace) }
        for (let i = 0; i < buttons.children.length; i++) {
            const b = buttons.children[i]
            if (b.modelData !== undefined)
                r[b.modelData] = frac(b)
        }
        return r
    }

    TouchTrackpad {
        id: engineTp
        mouse: vmouse
        active: ti.engine
        speed: ti.desktopSpeed
        // Again whenever the layout moves: the arguments are only there to
        // make the binding follow them. Not before the page is built: the
        // click buttons, half made, cannot be mapped yet.
        regions: ti.engine && ti.built ? ti.engineRegions(ti.width, ti.height, ti.kb, padFace.height, buttons.y) : ({})
        onDismissTapped: ti.middleTapped()
    }
    color: Skin.colors.background

    // How far the keyboard is up: 0 down (full trackpad), 1 up; a little
    // past either end as it springs into place.
    property real kb: 0
    readonly property real kbUp: Math.max(0, Math.min(1, kb))
    NumberAnimation {
        id: slide
        target: ti
        property: "kb"
        easing.type: Easing.OutBack
    }
    function slideKeyboard() {
        const up = mode === "keyboard"
        slide.stop()
        slide.to = up ? 1 : 0
        // Up: a quick rise with a visible overshoot; down: a softer settle.
        slide.duration = up ? 460 : 380
        slide.easing.overshoot = up ? 1.4 : 0.8
        slide.start()
    }
    onModeChanged: {
        slideKeyboard()
        if (mode !== "keyboard")
            keyLogic.reset()  // no shift or Ctrl left on for next time
    }
    // Opened with the keyboard up: it rises as the window appears.
    Component.onCompleted: {
        built = true
        if (mode === "keyboard")
            slideKeyboard()
    }

    // The middle button (apps): the keyboard. Desktop Mode's window has tabs
    // for that and its middle region closes it.
    function middleTapped() {
        if (showTabs)
            closeRequested()
        else
            mode = "keyboard"
    }

    // Nothing stays held when the page goes away.
    onVisibleChanged: if (!visible) { vmouse.releaseAll(); vkeys.releaseAll() }

    component Button: Rectangle {
        id: btn
        property string label
        property bool active: false
        signal clicked()
        height: 96 * ti.s
        width: Math.max(170 * ti.s, lbl.implicitWidth + 60 * ti.s)
        radius: 4 * ti.s
        color: tap.pressed ? Skin.colors.pressed : active ? Skin.colors.lit : Skin.colors.panel
        border.color: Skin.colors.border
        border.width: active ? 0 : 2 * ti.s
        Text {
            id: lbl
            anchors.centerIn: parent
            text: btn.label
            color: tap.pressed ? Skin.colors.background : btn.active ? Skin.colors.litText : Skin.colors.text
            font { family: Skin.font; pixelSize: 32 * ti.s; weight: Font.DemiBold }
        }
        TapHandler {
            id: tap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: btn.clicked()
        }
    }

    // Top bar (Desktop Mode): close, the two modes. The apps have none, so
    // the pad starts at the top.
    Row {
        id: bar
        visible: ti.showTabs || ti.closeLabel !== ""
        x: 20 * ti.s
        y: 20 * ti.s
        height: visible ? 96 * ti.s : 0
        spacing: 16 * ti.s
        Button {
            visible: ti.closeLabel !== ""
            label: ti.closeLabel
            onClicked: ti.closeRequested()
        }
        Button {
            visible: ti.showTabs
            label: "Trackpad"
            active: ti.mode === "trackpad"
            onClicked: ti.mode = "trackpad"
        }
        Button {
            visible: ti.showTabs
            label: "Keyboard"
            active: ti.mode === "keyboard"
            onClicked: ti.mode = "keyboard"
        }
    }

    // Where the pad ends: above the click buttons with the keyboard down,
    // above the keys with it up, and in between (springing with it) as it
    // moves.
    readonly property real padBottomDown: height - 20 * s - buttonsHeight
    readonly property real padBottomUp: height - keys.implicitHeight
    readonly property real buttonsHeight: 150 * s

    Rectangle {
        id: padFace
        anchors {
            left: parent.left; right: parent.right
            top: bar.visible ? bar.bottom : parent.top
            margins: 20 * ti.s
        }
        height: ti.padBottomDown + ti.kb * (ti.padBottomUp - ti.padBottomDown) - 20 * ti.s - y
        radius: 8 * ti.s
        color: Skin.colors.surface
        border.color: pad.touching || ti.engineTouching ? Skin.colors.accent : Skin.colors.borderQuiet
        border.width: 3 * ti.s

        // Where fingers have been, when the skin has the thermochromic trail.
        HeatTrail {
            id: heat
            anchors.fill: parent
            anchors.margins: parent.border.width
            visible: Skin.trackpadHeat.length > 1
            source: pad
            colors: Skin.trackpadHeat
            radius: parent.radius - parent.border.width
            fingerSize: 110 * ti.s
        }


        TrackpadArea {
            id: pad
            anchors.fill: parent
            mouse: vmouse
            s: ti.s
            speed: ti.speed
        }
    }

    // Trackpad mode: mouse buttons to hold while another finger moves.
    // They slide down out of the way as the keyboard comes up, fading.
    Row {
        id: buttons
        z: 1
        visible: ti.kb < 1
        x: 20 * ti.s
        width: parent.width - 40 * ti.s
        y: ti.padBottomDown + ti.kb * (ti.buttonsHeight + 40 * ti.s)
        opacity: 1 - Math.min(1, ti.kbUp * 1.6)
        height: ti.buttonsHeight
        spacing: 20 * ti.s
        // Apps: a keyboard button between the two (the region keeps the
        // name "dismiss" for the engine's TouchTrackpad).
        readonly property real dismissWidth: ti.showTabs ? 0 : 160 * ti.s
        Repeater {
            model: ti.showTabs ? ["left", "right"] : ["left", "dismiss", "right"]
            delegate: Rectangle {
                id: mb
                required property string modelData
                readonly property bool dismiss: modelData === "dismiss"
                width: dismiss ? buttons.dismissWidth
                     : (buttons.width - buttons.dismissWidth - (ti.showTabs ? 1 : 2) * buttons.spacing) / 2
                height: buttons.height
                radius: 4 * ti.s
                // Lit while pressed, and a moment after, so a quick click shows.
                readonly property bool lit: !dismiss && (hold.pressed || ti.engineHeld[modelData] === true || flash.running)
                readonly property bool down: hold.pressed || dismissTap.pressed
                readonly property int presses: ti.enginePresses ? (ti.enginePresses[modelData] || 0) : -1
                property int lastPresses: -1
                onPressesChanged: {
                    if (lastPresses >= 0 && presses > lastPresses)
                        flash.restart()
                    lastPresses = presses
                }
                Timer { id: flash; interval: 200 }
                color: lit ? Skin.colors.accentFill : down ? Skin.colors.pressed : Skin.colors.panel
                border.color: lit ? Skin.colors.accent : Skin.colors.border
                border.width: (lit ? 3 : 2) * ti.s
                Shell.Icon {
                    anchors.centerIn: parent
                    visible: mb.dismiss
                    width: 80 * ti.s
                    height: 80 * ti.s
                    kind: "keyboard"
                    color: mb.down ? Skin.colors.background : Skin.colors.text
                    lineWidth: 6 * ti.s
                }
                // Left and Right click: held while a finger is on them.
                PressArea {
                    id: hold
                    anchors.fill: parent
                    visible: !mb.dismiss
                    mouse: vmouse
                    button: mb.modelData
                }
                TapHandler {
                    id: dismissTap
                    enabled: mb.dismiss
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: ti.middleTapped()
                }
            }
        }
    }

    // Below the keys: their color, so the overshoot shows no gap under them.
    Rectangle {
        visible: keys.visible
        anchors { left: parent.left; right: parent.right; top: keys.bottom }
        height: 120 * ti.s
        color: keys.item && keys.item.color !== undefined ? keys.item.color : Skin.colors.panel
    }

    // KeyPanel.qml, or the skin's.
    Shell.SkinView {
        id: keys
        file: "KeyPanel.qml"
        props: ({ controller: keyLogic })
        visible: ti.kb > 0.001
        enabled: ti.mode === "keyboard"  // no keys typed as they slide away
        x: 0
        width: parent.width
        y: parent.height - ti.kb * implicitHeight
        height: implicitHeight
    }
    Binding { target: keys.item; property: "s"; value: ti.s }
}

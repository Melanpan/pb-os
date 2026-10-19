// Barry Launcher's on-screen keyboard for the AYN Thor's bottom screen.
// barry_launcher_shelld shows it when a text field gets focus (AT-SPI) and
// marks this window as a gamescope overlay that leaves key focus on the app
// below; what is tapped is typed there (Barry.Input: KeyboardController
// with a Typer, XTest), and shelld only plans autocorrect's swaps. Covers
// the screen while up: keys (KeyPanel.qml) on the lower (or, for a field in
// the lower half, upper) part; tapping the rest hides it. The keys slide
// in from their screen edge and back out (only the look: shown is
// KeyboardState's, and the keys take no touches once it goes).
// The window stays mapped and hides by opacity, as Steam's overlay does
// (gamescope ignores property changes on unmapped windows).
// It also covers the screen while an app opens (KeyboardState's curtain:
// the app's name), hiding its flicker (Firefox resizing itself and painting
// black and grey, Signal's white first frames) until it is ready.
//
// Desktop Mode ("desktop" argument, under KWin; a KWin rule keeps it from
// taking focus): just the keys, along the bottom output's lower edge (KWin's
// script puts it there). It stays mapped there, invisible and click-through
// while hidden: a window mapped anew lands on the screen with the pointer
// first, and would flash on the top screen. Text goes through KWin's input
// method.
//
// This file is only the window and how it looks: the keyboard's behavior is
// Barry.Input's (KeyboardController, KeyboardState).
import QtQuick
import QtQuick.Window
import Barry.Input

Window {
    id: win
    title: "Barry Launcher Keyboard"
    flags: Qt.WindowDoesNotAcceptFocus | (desktop && !shown ? Qt.WindowTransparentForInput : 0)
    color: "transparent"
    readonly property bool desktop: Qt.application.arguments.indexOf("desktop") >= 0
    visibility: desktop ? Window.Windowed : Window.FullScreen
    // Window opacity does nothing under Wayland: in Desktop Mode the keys
    // themselves hide, over a transparent window.
    opacity: desktop || shown || covering || sliding ? 1 : 0
    width: 1240
    height: desktop ? panel.implicitHeight : 1080

    readonly property real s: desktop ? width / 1240 : Math.min(width / 1240, height / 1080)
    readonly property bool shown: kbState.shown
    readonly property bool atTop: kbState.atTop
    readonly property string curtain: kbState.curtain  // the app opening under the cover
    property string curtainName: ""  // kept while the cover fades away
    readonly property bool covering: !desktop && (curtain !== "" || coverFade.running)
    onCurtainChanged: curtain !== "" ? curtainName = curtain : coverFade.restart()

    // How far the keys are in: 0 off the screen's edge, 1 in place. Up
    // like Barry's pages come in (Shell.qml), down a little quicker; a
    // slide turned around midway goes back from where the keys are. The
    // window stays drawn until they are off. Desktop Mode: no slide.
    property real kb: 0
    readonly property bool sliding: kb > 0.001
    NumberAnimation {
        id: slide
        target: win
        property: "kb"
    }
    function slideKeys() {
        slide.stop()
        const to = shown ? 1 : 0
        if (desktop) {
            kb = to
            return
        }
        slide.to = to
        // Time for the way left, but never a snap.
        slide.duration = Math.round((shown ? 300 : 220) * Math.max(0.4, Math.abs(to - kb)))
        slide.easing.type = shown ? Easing.OutCubic : Easing.InCubic
        slide.start()
    }
    onShownChanged: slideKeys()
    Component.onCompleted: kb = shown ? 1 : 0

    // Desktop Mode's shortcuts (Ctrl+C, ...) go through a real keyboard to
    // KWin, so the focused window gets the modifiers.
    VirtualKeyboard {
        id: shortcutKeys
        name: "Barry Launcher Shortcut Keys"
        active: win.desktop
    }
    Typer {
        id: typer
        desktop: win.desktop
        keyboard: shortcutKeys
    }
    KeyboardController {
        id: keys
        typer: typer
        autocorrect: true
        onHideRequested: kbState.hide()
    }
    KeyboardState {
        id: kbState
        bottom: true
        controller: keys
        panelHeight: Math.round(panel.height)
    }

    // Tapping outside the keys hides the keyboard.
    TapHandler {
        enabled: !win.covering
        onTapped: kbState.hide()
    }

    // KeyPanel.qml, or the skin's.
    SkinView {
        id: panel
        file: "KeyPanel.qml"
        props: ({ controller: keys })
        // Only while up or sliding: the window's opacity follows a moment
        // after the keys (an X property), so keys left drawn in a window
        // going transparent flashed as an opening app's cover went away.
        visible: win.sliding
        enabled: win.shown  // no keys typed as they slide away
        width: parent.width
        height: implicitHeight
        y: win.desktop || !win.atTop ? parent.height - height : 0
        // Off the nearer edge: down below the screen, or up above it for
        // keys at the top.
        transform: Translate {
            y: (1 - win.kb) * panel.height * (win.atTop && !win.desktop ? -1 : 1)
        }
    }
    Binding { target: panel.item; property: "s"; value: win.s }

    // The cover over an opening app: like the home screen it came
    // from, the app's name and a slow pulse; it fades out once the page is in.
    Rectangle {
        id: cover
        anchors.fill: parent
        visible: win.covering
        color: Skin.colors.cover
        opacity: win.curtain !== "" ? 1 : 0
        NumberAnimation on opacity {
            id: coverFade
            running: false
            from: 1
            to: 0
            duration: 180
        }
        // Touches on it go nowhere (not to the app being made beneath).
        TapHandler {}

        Column {
            anchors.centerIn: parent
            spacing: 28 * win.s
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: win.curtainName
                color: Skin.colors.text
                font { family: Skin.font; pixelSize: 44 * win.s; weight: Font.DemiBold }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 18 * win.s
                Repeater {
                    model: 3
                    delegate: Rectangle {
                        id: dot
                        required property int index
                        width: 16 * win.s
                        height: width
                        radius: width / 2
                        color: Skin.colors.accent
                        opacity: 0.25
                        SequentialAnimation on opacity {
                            running: win.covering
                            loops: Animation.Infinite
                            PauseAnimation { duration: dot.index * 160 }
                            NumberAnimation { to: 1; duration: 320 }
                            NumberAnimation { to: 0.25; duration: 320 }
                            PauseAnimation { duration: (2 - dot.index) * 160 }
                        }
                    }
                }
            }
        }
    }
}

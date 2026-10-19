// A tool's window content (the Trackpad app, the Deck controls), Game Mode:
// it slides up from the bottom edge as the window first shows, and down
// and away, fading, as barry_launcher_shelld asks it to go (Barry opens over
// it: ShellState's leaving), then says it has (ShellState.left(); shelld
// ends the app as Barry's window comes forward). Around the skin's view, so
// every skin's TopInput.qml and DeckControls.qml slide without knowing.
//
// Only the look: when it goes is shelld's and Barry.Input's (ShellState).
// An older shelld never asks (it ends the app at once, as before), and
// neither does Desktop Mode (slides off): there the tools are KWin's
// Wayland windows, closed at once.
import QtQuick
import Barry.Input

Item {
    id: stage
    property bool slides: true

    transform: Translate { id: shift; y: stage.slides ? stage.height : 0 }

    // In: as Shell.qml's slide (300 ms, OutCubic), from the first frame the
    // window draws (before that it is not on the screen yet: started
    // earlier, the first part would play unseen). Until then the content
    // waits below the bottom edge, so the window shows its background.
    NumberAnimation {
        id: enter
        target: shift
        property: "y"
        to: 0
        duration: 300
        easing.type: Easing.OutCubic
    }
    Connections {
        target: stage.Window.window
        enabled: stage.slides && !enter.running && shift.y > 0 && !leave.running && !ShellState.leaving
        function onAfterAnimating() { enter.start() }
    }

    // Out: down by the window's height and faded (240 ms, InCubic: slow to
    // start, gone quickly). No touches meanwhile.
    SequentialAnimation {
        id: leave
        ParallelAnimation {
            NumberAnimation { target: shift; property: "y"; to: stage.height; duration: 240; easing.type: Easing.InCubic }
            NumberAnimation { target: stage; property: "opacity"; to: 0; duration: 240; easing.type: Easing.InCubic }
        }
        ScriptAction { script: ShellState.left() }
    }
    Connections {
        target: ShellState
        function onLeavingChanged() {
            if (!ShellState.leaving)
                return
            enter.stop()
            stage.enabled = false
            leave.start()
        }
    }
    Component.onCompleted: ShellState.tool = stage.slides
}

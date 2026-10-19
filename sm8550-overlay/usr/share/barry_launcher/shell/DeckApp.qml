// Barry Launcher's Deck app for the AYN Thor's bottom screen: the Steam
// Deck's trackpads and back buttons (DeckControls.qml) in a window of its
// own, started and closed like the Trackpad app (barry_launcher_shelld starts
// it; Home closes it). "desktop" after "--": Desktop Mode, where it must
// never take focus from the window it plays into. It slides up as it opens
// and away as Barry opens over it (ToolStage.qml), in Game Mode.
import QtQuick
import QtQuick.Window
import Barry.Input

Window {
    id: win
    title: "Barry Launcher Deck"
    readonly property bool desktop: Qt.application.arguments.indexOf("desktop") >= 0
    flags: desktop ? Qt.WindowDoesNotAcceptFocus : Qt.Window
    color: Skin.colors.background
    visibility: Window.FullScreen
    visible: true

    readonly property real s: Math.min(width / 1240, height / 1080)

    ToolStage {
        anchors.fill: parent
        slides: !win.desktop

        // DeckControls.qml, or the skin's.
        SkinView {
            id: deck
            file: "DeckControls.qml"
            anchors.fill: parent
        }
    }
    Binding { target: deck.item; property: "s"; value: win.s }
}

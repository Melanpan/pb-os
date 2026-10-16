// Barry Launcher's Deck app for the AYN Thor's bottom screen: the Steam
// Deck's trackpads and back buttons (DeckControls.qml) in a window of its
// own, started and closed like the Trackpad app (barry_launcher_shelld starts
// it; Home closes it). "desktop" after "--": Desktop Mode, where it must
// never take focus from the window it plays into.
import QtQuick
import QtQuick.Window
import Barry.Input

Window {
    id: win
    title: "Barry Launcher Deck"
    flags: Qt.application.arguments.indexOf("desktop") >= 0 ? Qt.WindowDoesNotAcceptFocus : Qt.Window
    color: Skin.colors.background
    visibility: Window.FullScreen
    visible: true

    readonly property real s: Math.min(width / 1240, height / 1080)

    // DeckControls.qml, or the skin's.
    SkinView {
        id: deck
        file: "DeckControls.qml"
        anchors.fill: parent
    }
    Binding { target: deck.item; property: "s"; value: win.s }
}

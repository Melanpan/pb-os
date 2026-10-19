// Barry Launcher's Trackpad app for the AYN Thor's bottom screen in Game
// Mode: TopInput.qml in a window of its own, so it starts, switches and
// closes like the other apps (barry_launcher_shelld starts this with
// "trackpad" after "--", and "keyboard" to open with the keyboard up). It
// drives the top screen: a trackpad, with a keyboard that slides up below it.
// It slides up as it opens and away as Barry opens over it (ToolStage.qml).
import QtQuick
import QtQuick.Window
import Barry.Input

Window {
    id: win
    readonly property bool keyboard: Qt.application.arguments.indexOf("keyboard") >= 0
    readonly property bool desktop: Qt.application.arguments.indexOf("desktop") >= 0
    title: "Barry Launcher Trackpad"
    // Desktop Mode: one seat for both screens, so this window must never
    // take focus from the top screen's window it types into.
    flags: desktop ? Qt.WindowDoesNotAcceptFocus : Qt.Window
    color: Skin.colors.background
    visibility: Window.FullScreen
    visible: true

    readonly property real s: Math.min(width / 1240, height / 1080)

    ToolStage {
        anchors.fill: parent
        slides: !win.desktop

        // TopInput.qml, or the skin's. Set as it is made: desktop picks the
        // trackpad's engine, mode how it opens.
        SkinView {
            id: input
            file: "TopInput.qml"
            anchors.fill: parent
            props: ({ mode: win.keyboard ? "keyboard" : "trackpad", showTabs: false, followShelld: true,
                      desktop: win.desktop })
        }
    }
    Binding { target: input.item; property: "s"; value: win.s }
    Connections {
        target: input.item
        // Closed by going home (barry_launcher_shelld).
        function onCloseRequested() { Qt.quit() }
    }
}

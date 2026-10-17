// The Pboard skin's grid keys (a Thumb-Key keyboard pack): the built-in
// grid (shell/GridPanel.qml), drawn as this skin draws its other keys
// (KeyPanel.qml): flat rounded keys on the near-black panel, the letters a
// soft gray, the named keys and comma a blue-gray, 123/ABC and Enter as
// pills, Enter in the accent, space a plain letter key.
import QtQuick
import Barry.Input
import "file:///usr/share/barry_launcher/shell" as Shell

Shell.GridPanel {
    panelColor: Skin.colors.background
    keyRadius: 14 * s
    keycaps: false
    pillKeys: ["numbers", "abc", "Return"]
    actionKeys: ["Return"]
    namedKeys: ["numbers", "abc", "symbols", "keys", "lang", "shift", "BackSpace", "Return", "hide", "t,"]
}

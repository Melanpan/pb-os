// The SteamOS skin's grid keys (a Thumb-Key keyboard pack): the built-in
// grid (shell/GridPanel.qml), drawn as this skin's KeyPanel.qml draws
// Steam's keyboard: square #23262e keys 3 px apart on true black, darker
// #16191f named keys with small labels at their foot, Steam blue (#1a9fff)
// for a key pressed (dark legend) or Shift on, in Figtree (fonts/, as
// KeyPanel.qml).
import QtQuick
import Barry.Input
import "file:///usr/share/barry_launcher/shell" as Shell

Shell.GridPanel {
    id: grid
    readonly property var steam: ({
        panel: "#000000", key: "#23262e", named: "#16191f", label: "#ffffff",
        on: "#1a9fff", touched: "#1a9fff", touchedLabel: "#000000",
    })
    FontLoader { id: figtree; source: "fonts/Figtree-wght.ttf" }

    gap: 3 * s
    keyRadius: 0
    stripRadius: 6 * s
    panelColor: steam.panel
    fontFamily: figtree.status === FontLoader.Ready ? figtree.name : Skin.font
    keycaps: false
    labelsAtFoot: true
    actionKeys: []
    keyFace: function(k, pressed, lit, action, named) {
        return pressed ? grid.steam.touched : lit ? grid.steam.on
             : named && k !== "space" ? grid.steam.named : grid.steam.key
    }
    keyLegend: function(k, pressed) {
        return pressed ? grid.steam.touchedLabel : grid.steam.label
    }
}

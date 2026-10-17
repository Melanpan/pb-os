// TileDrop: a home tile flying in toward the screen as Barry opens, as an
// app icon does on a phone being unlocked: it starts large (about three
// times its size) and faint, pushed out from the middle of the tiles, then
// shrinks into its place, fast at first and settling softly. (No blur: a
// layer per tile drew stray specks on the Thor's GPU, 2026-10-17.)
// The tiles near the middle land first, the outer ones a beat later.
//
// A tile wraps what it draws in a TileDrop (it fills the tile) and binds
// `clock` to its home screen's; the home screen calls aim() on each tile
// with the point the tiles fly from, then runs the clock from 0 (ms) to
// TileDrop.length and back to -1 (at rest). One clock drives every tile.
import QtQuick

Item {
    id: drop
    anchors.fill: parent

    // Milliseconds since the drop began; below 0: at rest.
    property real clock: -1
    // Set by aim(): when this tile starts, and how far from the middle it is.
    property real delay: 0
    property real awayX: 0
    property real awayY: 0

    readonly property real startScale: 3.2
    readonly property real duration: 320
    // The outermost tile starts this much after the middle one.
    readonly property real spread: 200
    // The whole drop, for the home screen's clock.
    readonly property real length: duration + spread

    // from: the point (in `area`'s coordinates) the tiles fly from; near,
    // reach: the distances of the nearest and the outermost tile from it.
    // The nearest start at once, the outermost `spread` later.
    function aim(area, fromX, fromY, near, reach) {
        const c = mapToItem(area, width / 2, height / 2)
        awayX = c.x - fromX
        awayY = c.y - fromY
        const d = Math.hypot(awayX, awayY)
        delay = reach > near ? Math.max(0, Math.min(1, (d - near) / (reach - near))) * spread : 0
    }

    // 0 (start) .. 1 (in place); an ease-out curve (fast, then settling).
    readonly property real t: clock < 0 ? 1 : Math.max(0, Math.min(1, (clock - delay) / duration))
    readonly property real p: 1 - Math.pow(1 - t, 3)
    readonly property real grow: (startScale - 1) * (1 - p)

    opacity: clock < 0 ? 1 : Math.min(1, t / 0.4)
    // Enlarged about its own middle, then pushed out from the tiles' middle
    // as much as it is enlarged, as if seen closer: the tiles part around
    // the middle and close up as they land.
    transform: [
        Scale {
            origin.x: drop.width / 2
            origin.y: drop.height / 2
            xScale: 1 + drop.grow
            yScale: 1 + drop.grow
        },
        Translate { x: drop.awayX * drop.grow; y: drop.awayY * drop.grow }
    ]

}

pragma ComponentBehavior: Bound
// The keys of a grid keyboard pack (Thumb-Key's layout): big keys, each a
// letter to tap and more to swipe out of it, shown small around its edge.
// SkinView.qml loads this in KeyPanel.qml's place while the controller's
// gridPage is up (its letters or its numbers); the symbols and Fn pages
// stay KeyPanel's.
//
// Only how they look, as KeyPanel: the controller (Barry.Input's
// KeyboardController, keyboardgrid.cpp) gets every touch, raw, and decides
// the key, the swipe, the space bar's cursor slide and Backspace's word.
// Colors are the skin's, the same as KeyPanel's keys (keycap edges, action
// keys). A skin can replace this file (SkinView.qml); one that only wants
// its own shapes can load this one with the style properties below set,
// as the Pboard and SteamOS skins do.
import QtQuick
import Barry.Input

Rectangle {
    id: panel
    required property KeyboardController controller
    property real s: 1
    property real keyHeight: 92  // at 1240 x 1080, as KeyPanel's

    // --- style: what a skin's GridPanel.qml may set --------------------------
    property real gap: 10 * s
    property real keyRadius: 16 * s
    property color panelColor: Skin.colors.panel
    property string fontFamily: Skin.font
    // Keys drawn as pills.
    property var pillKeys: []
    // Keys in the action colors (Skin.colors.keyAction), if the skin has them.
    property var actionKeys: ["space", "Return"]
    // Keys in the named keys' colors: by default every named key.
    property var namedKeys: null
    // A keycap's lower edge (Skin.colors.keyShadow) under each key.
    property bool keycaps: Skin.colors.keyShadow !== ""
    // Named keys' labels at their foot (SteamOS), not in the middle.
    property bool labelsAtFoot: false
    property var keyFace: function(k, pressed, lit, action, named) {
        return pressed ? Skin.colors.pressed : lit ? Skin.colors.lit
             : action ? Skin.colors.keyAction : named ? Skin.colors.keySpecial : Skin.colors.key
    }
    property var keyLegend: function(k, pressed, lit, action, named) {
        return pressed ? Skin.colors.text : lit ? Skin.colors.litText
             : action && Skin.colors.keyActionText !== "" ? Skin.colors.keyActionText
             : named ? Skin.colors.keySpecialText : Skin.colors.keyText
    }
    property color stripColor: Skin.colors.raised
    property real stripRadius: 16 * s

    readonly property real stripHeight: controller.glide ? 72 * s : 0
    // As tall as KeyPanel's five rows, so switching pages does not move it.
    readonly property real keysHeight: 5 * keyHeight * s + 4 * gap
    implicitHeight: keysHeight + 24 * s + (controller.glide ? stripHeight + gap : 0)
    color: panelColor

    Binding { target: panel.controller; property: "panel"; value: panel }
    Binding { target: panel.controller; property: "keyUnit"; value: keysArea.width / 4 }

    readonly property var labels: ({
        "numbers": "123", "abc": "ABC", "symbols": "#+=", "keys": "Fn",
    })
    // Where a swipe's label sits on its key, as fractions of it.
    readonly property var spots: ({
        "top": [0.5, 0.16], "topRight": [0.86, 0.16], "right": [0.86, 0.5], "bottomRight": [0.86, 0.84],
        "bottom": [0.5, 0.84], "bottomLeft": [0.14, 0.84], "left": [0.14, 0.5], "topLeft": [0.14, 0.16],
    })

    // Take taps on the edges, so they do not reach what is behind.
    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds }

    Column {
        anchors.fill: parent
        anchors.margins: 12 * panel.s
        spacing: panel.gap

        // The strip: the controller's words; tap one.
        Item {
            visible: panel.controller.glide
            width: parent.width
            height: panel.stripHeight

            Row {
                anchors.centerIn: parent
                height: parent.height
                spacing: 16 * panel.s

                Repeater {
                    model: panel.controller.alternatives
                    delegate: Rectangle {
                        id: alt
                        required property string modelData
                        required property int index
                        height: parent.height
                        width: Math.max(160 * panel.s, altLabel.implicitWidth + 48 * panel.s)
                        radius: panel.stripRadius
                        color: altTap.pressed ? Skin.colors.pressed : panel.stripColor

                        Text {
                            id: altLabel
                            anchors.centerIn: parent
                            text: panel.controller.stripMode === "undo" ? "“" + alt.modelData + "”" : alt.modelData
                            color: Skin.colors.text
                            font { family: panel.fontFamily; pixelSize: 36 * panel.s }
                        }
                        TapHandler {
                            id: altTap
                            gesturePolicy: TapHandler.ReleaseWithinBounds
                            onPressedChanged: if (pressed) panel.controller.keyDown("strip")
                            onTapped: panel.controller.pickStrip(alt.index)
                        }
                    }
                }
            }
        }

        // The keys: four rows of four columns' width. Each key's item is its
        // whole cell (no gaps to miss); its face is drawn inside. Made once
        // per row's length: Shift or a page only changes what they show, so
        // a finger never lands on a key being made again.
        Item {
            id: keysArea
            width: parent.width
            height: panel.keysHeight

            Column {
                anchors.fill: parent
                Repeater {
                    model: panel.controller.grid.length
                    delegate: Row {
                        id: row
                        required property int index
                        readonly property var keys: panel.controller.grid[index] || []
                        width: keysArea.width
                        height: keysArea.height / 4
                        Repeater {
                            model: row.keys.length
                            delegate: Cell {
                                required property int index
                                info: row.keys[index] || ({})
                                width: keysArea.width * (info.span || 1) / 4
                                height: row.height
                            }
                            // Gone from the controller at once (destroyed later).
                            onItemRemoved: (i, item) => panel.controller.unregisterCell(item.registered, item)
                        }
                    }
                }
            }

            // Every touch to the controller, raw: it decides what it typed.
            MultiPointTouchArea {
                anchors.fill: parent
                mouseEnabled: true
                function report(points, what) {
                    for (const p of points) {
                        const at = mapToItem(panel, p.x, p.y)
                        what(p.pointId, at.x, at.y)
                    }
                }
                onPressed: (points) => report(points, panel.controller.touchDown)
                onUpdated: (points) => report(points, panel.controller.touchMove)
                onReleased: (points) => report(points, panel.controller.touchUp)
                onCanceled: (points) => { for (const p of points) panel.controller.touchCancel(p.pointId) }
            }
        }
    }

    component Cell: Item {
        id: cell
        property var info: ({})
        readonly property string k: info.key || ""
        readonly property bool special: panel.controller.isNamed(k)
        readonly property bool named: panel.namedKeys ? panel.namedKeys.indexOf(k) >= 0 : special
        readonly property bool action: panel.actionKeys.indexOf(k) >= 0 && Skin.colors.keyAction !== ""
        readonly property bool pressed: panel.controller.pressedCells.indexOf(k) >= 0
        readonly property bool lit: (k === "shift" && panel.controller.shifted)
        readonly property color face: panel.keyFace(k, pressed, lit, action, named)
        readonly property color legend: panel.keyLegend(k, pressed, lit, action, named)
        readonly property real edge: panel.keycaps && !pressed ? 5 * panel.s : 0
        readonly property bool iconKey: ["shift", "BackSpace", "Return", "hide"].indexOf(k) >= 0
        readonly property bool foot: panel.labelsAtFoot && special && k !== "space"

        // Where the controller finds this key: under its key, as that changes.
        property string registered
        function register() {
            if (registered !== "")
                panel.controller.unregisterCell(registered, cell)
            registered = k
            if (k !== "")
                panel.controller.registerCell(k, cell)
        }
        onKChanged: register()
        Component.onCompleted: register()
        Component.onDestruction: panel.controller.unregisterCell(registered, cell)

        // The key: inside its cell by half a gap all round.
        Rectangle {
            id: base
            anchors { fill: parent; margins: panel.gap / 2 }
            radius: panel.pillKeys.indexOf(cell.k) >= 0 ? height / 2 : panel.keyRadius
            color: panel.keycaps ? Skin.colors.keyShadow : cell.face

            Rectangle {
                id: cap
                anchors { fill: parent; bottomMargin: cell.edge; topMargin: panel.keycaps ? 5 * panel.s - cell.edge : 0 }
                radius: base.radius
                color: cell.face
            }

            // What a tap types (or the named key's label).
            Text {
                visible: !icon.visible
                x: !cell.foot ? (cap.width - width) / 2 : cell.k === "Return" || cell.k === "BackSpace" ? cap.width - width - 16 * panel.s : 16 * panel.s
                y: cap.y + (!cell.foot ? (cap.height - height) / 2 : cap.height - height - 10 * panel.s)
                text: cell.k === "space" ? (panel.controller.manyLanguages ? panel.controller.language.name : "")
                    : cell.k === "lang" ? panel.controller.language.label
                    : panel.labels[cell.k] !== undefined ? panel.labels[cell.k]
                    : cell.info.center || ""
                width: Math.min(implicitWidth, cap.width - 24 * panel.s)
                elide: Text.ElideRight
                color: cell.legend
                font {
                    family: panel.fontFamily
                    pixelSize: (cell.special ? (cell.k === "space" ? 28 : 32) : 52) * panel.s
                }
            }
            Icon {
                id: icon
                x: !cell.foot ? (cap.width - width) / 2 : cell.k === "Return" || cell.k === "BackSpace" ? cap.width - width - 14 * panel.s : 14 * panel.s
                y: cap.y + (!cell.foot ? (cap.height - height) / 2 : cap.height - height - 8 * panel.s)
                width: (cell.foot ? 34 : 52) * panel.s
                height: width
                visible: cell.iconKey
                kind: cell.k === "BackSpace" ? "backspace" : cell.k === "Return" ? "enter" : cell.k
                color: cell.legend
                lineWidth: (cell.foot ? 3.5 : 5) * panel.s
            }

            // The swipes, small around the edge.
            Repeater {
                model: Object.keys(cell.info.swipes || {})
                delegate: Text {
                    required property string modelData
                    readonly property var spot: panel.spots[modelData] || [0.5, 0.5]
                    x: cap.width * spot[0] - width / 2
                    y: cap.y + cap.height * spot[1] - height / 2
                    text: cell.info.swipes[modelData] === "shift" ? "⇧" : cell.info.swipes[modelData]
                    color: cell.legend
                    opacity: 0.6
                    font { family: panel.fontFamily; pixelSize: 26 * panel.s }
                }
            }
        }
    }
}

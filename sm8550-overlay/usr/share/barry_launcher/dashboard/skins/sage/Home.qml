pragma ComponentBehavior: Bound
// The Sage skin's home screen: the built-in one's tiles (Home.qml in the
// shell, whose properties and signals this keeps) as keycaps on the cream
// case.
import QtQuick
import Barry.Input
import "file:///usr/share/barry_launcher/shell" as Shell

Rectangle {
    id: home
    property real s: 1
    property var running: ({})
    property var status: ({})  // app -> "Installing…" etc. while it gets ready
    // The tiles in order, without the hidden ones (shelld's /apps; the
    // Barry Launcher plugin's Apps tab sets them). These until it answers.
    property var tiles: [
        { app: "browser", name: "Firefox", icon: "firefox", closable: true },
        { app: "discord", name: "Discord", icon: "discord", closable: true },
        { app: "signal", name: "Signal", icon: "signal", closable: true },
        { app: "trackpad", name: "Trackpad", icon: "trackpad", closable: false },
        { app: "keyboard", name: "Keyboard", icon: "keyboard", closable: false },
    ]
    readonly property int rows: Math.max(1, Math.ceil(tiles.length / 3))
    // The tiles' scale: as big as before for two rows, smaller to fit three.
    readonly property real ts: s * Math.min(1, (height - 110 * s) / (Math.min(3, rows) * 370 * s - 30 * s))
    signal launch(string app)
    signal close(string app)

    readonly property var c: Skin.colors
    gradient: Gradient {
        GradientStop { position: 0; color: Qt.lighter(home.c.background, 1.04) }
        GradientStop { position: 1; color: Qt.darker(home.c.background, 1.04) }
    }

    component Tile: Item {
        id: tile
        property string app
        property string name
        property string iconKind  // an Icon kind, or a user app's icon file (file://)
        readonly property bool imageIcon: iconKind.startsWith("file:")
        property bool closable: true  // Trackpad and Keyboard close on Home by themselves
        readonly property bool isRunning: closable && home.running[app] === true
        readonly property string statusText: home.status[app] || ""

        width: 300 * home.ts
        height: 340 * home.ts

        // A keycap: pressed, it sinks onto its lower edge.
        Rectangle {
            id: face
            anchors.horizontalCenter: parent.horizontalCenter
            width: 260 * home.ts
            height: 260 * home.ts
            radius: 56 * home.ts
            color: home.c.keyShadow
            Rectangle {
                id: cap
                anchors { fill: parent; bottomMargin: tap.pressed ? 0 : 10 * home.ts; topMargin: tap.pressed ? 10 * home.ts : 0 }
                radius: parent.radius
                color: tile.isRunning || tile.statusText ? home.c.keyAction : home.c.key
            }

            SequentialAnimation on opacity {
                running: tile.statusText !== ""
                loops: Animation.Infinite
                alwaysRunToEnd: true
                NumberAnimation { to: 0.45; duration: 700; easing.type: Easing.InOutQuad }
                NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutQuad }
            }

            Shell.Icon {
                anchors.centerIn: cap
                visible: !tile.imageIcon
                width: 150 * home.ts
                height: 150 * home.ts
                kind: tile.imageIcon ? "" : tile.iconKind
                color: tile.isRunning || tile.statusText ? home.c.keyActionText : home.c.icon
                lineWidth: 9 * home.ts
            }
            // A user app's own icon (barry_apps): a picture file.
            Image {
                anchors.centerIn: cap
                visible: tile.imageIcon
                width: 170 * home.ts
                height: 170 * home.ts
                source: tile.imageIcon ? tile.iconKind : ""
                sourceSize: Qt.size(width, height)
                fillMode: Image.PreserveAspectFit
                smooth: true
                asynchronous: true
            }
            TapHandler {
                id: tap
                onTapped: home.launch(tile.app)
            }
        }

        // Close badge on a running app. Its touch area is wider than the
        // badge, so a finger needn't hit the badge itself.
        Item {
            visible: tile.isRunning
            x: badge.x - (width - badge.width) / 2
            y: badge.y - (height - badge.height) / 2
            width: 150 * home.ts
            height: 150 * home.ts
            TapHandler {
                id: closeTap
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: home.close(tile.app)
            }
        }
        Rectangle {
            id: badge
            visible: tile.isRunning
            x: face.x + face.width - width * 0.7
            y: -height * 0.3
            width: 76 * home.ts
            height: 76 * home.ts
            radius: width / 2
            color: closeTap.pressed ? home.c.danger : home.c.border
            border.color: home.c.background
            border.width: 4 * home.ts
            Shell.Icon {
                anchors.centerIn: parent
                width: 40 * home.ts
                height: 40 * home.ts
                kind: "close"
                color: home.c.text
                lineWidth: 6 * home.ts
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: face.bottom
            anchors.topMargin: 24 * home.ts
            // Long names (user apps have up to 20 characters) shrink to fit.
            width: tile.width + 50 * home.ts
            horizontalAlignment: Text.AlignHCenter
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: 26 * home.ts
            elide: Text.ElideRight
            text: tile.statusText || tile.name
            color: home.c.text
            font { family: Skin.font; pixelSize: 40 * home.ts; weight: Font.DemiBold }
        }
    }

    // Up to three rows (nine apps) fit the screen, the tiles a little
    // smaller for the third; with more (user apps) they scroll, above the
    // hint at the bottom.
    Flickable {
        id: flick
        anchors { fill: parent; bottomMargin: 70 * home.s }
        contentWidth: width
        contentHeight: Math.max(height, grid.height + 60 * home.s)
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Grid {
            id: grid
            anchors.horizontalCenter: parent.horizontalCenter
            // Centred on the screen when there is room, as high as needed
            // when not.
            y: Math.max(20 * home.s, Math.min((flick.height - height) / 2 + 35 * home.s,
                                              flick.height - height - 20 * home.s))
            columns: 3
            columnSpacing: 60 * home.ts
            rowSpacing: 30 * home.ts

            Repeater {
                model: home.tiles
                delegate: Tile {
                    required property var modelData
                    app: modelData.app
                    name: modelData.name
                    iconKind: modelData.icon
                    closable: modelData.closable
                }
            }
        }
    }

    Text {
        visible: home.tiles.length === 0
        anchors.centerIn: parent
        width: parent.width * 0.8
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: "All apps are hidden: Quick Access → Barry Launcher → Apps shows them again."
        color: home.c.text
        opacity: 0.6
        font { family: Skin.font; pixelSize: 36 * home.s }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30 * home.s
        text: "swipe right: performance dashboard  ·  hold AYN: home"
        color: home.c.text
        opacity: 0.4
        font { family: Skin.font; pixelSize: 24 * home.s }
    }
}

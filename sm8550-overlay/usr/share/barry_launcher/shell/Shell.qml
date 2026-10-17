// Barry Launcher on the AYN Thor's bottom screen during Game Mode: the first
// window of barry_launcher_session, under everything else. Because this
// window is always shown, gamescope falls back to it when an app quits,
// instead of freezing on the app's last frame.
//
// Its base is the hero art of the game selected on the top screen
// (Hero.qml). Over it Barry opens and closes: a short press of the AYN
// button, or a swipe up from the bottom edge (barry_launcher_shelld says so
// in GET /home: "open", and "serial" counting each opening). Barry is three
// pages side by side: the performance dashboard, the apps (Home.qml) and
// the quick settings (QuickSettings.qml), left to right; it opens on the
// apps. A left-to-right swipe from anywhere brings in the page to the left,
// right to left the one to the right; Barry.Input's PageSwipe tells a swipe
// from a tap and says where it goes, and here it slides there, springing
// past a little and back, as the keyboard does coming up. An app's tile
// starts it (or brings it forward) through barry_launcher_shelld, and Barry
// closes; the AYN button opens Barry over it and closes back to it.
//
// The game art is standby. A tap on it wakes Barry too: its pages come over
// the art, which stays behind as if behind frosted glass (Hero.qml's
// frost), its logo gone. Untouched for standbySeconds (Barry's settings,
// 15 by default; 0: never), Barry goes back to the art, unless it is open
// over an app or on the dashboard.
//
// Opening, the apps fly in toward the screen, the middle ones first
// (Home.qml's dropIn, Barry.Input's TileDrop; a skin's home screen without
// it slides up instead): shelld counts the openings, this puts Barry at
// the start while still hidden and says so (/home-ready), and then the
// window is brought forward. Closing, it fades down and the hero shows
// again.
import QtQuick
import QtQuick.Window
import Barry.Input
import "../dashboard"

Window {
    id: win
    title: "Barry Launcher"
    color: Skin.colors.background
    visibility: Window.FullScreen
    visible: true

    readonly property string api: "http://127.0.0.1:47824"
    readonly property real s: Math.min(width / 1240, height / 1080)

    function post(path, body) {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState === XMLHttpRequest.DONE)
                win.refresh()
        }
        x.open("POST", api + path)
        x.setRequestHeader("Content-Type", "application/json")
        x.send(JSON.stringify(body))
    }

    function refresh() {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE || x.status !== 200)
                return
            try {
                const apps = JSON.parse(x.responseText)
                const r = {}, st = {}
                for (const name in apps) {
                    r[name] = apps[name].running
                    st[name] = apps[name].status || ""
                }
                win.running = r
                win.status = st
                // Only when it changed: a new model rebuilds the tiles.
                const tiles = Object.keys(apps)
                    .filter(name => apps[name].name !== undefined && !apps[name].hidden)
                    .sort((a, b) => apps[a].pos - apps[b].pos)
                    .map(name => ({ app: name, name: apps[name].name, icon: apps[name].icon,
                                    closable: apps[name].closable !== false }))
                if (tiles.length || Object.keys(apps).some(name => apps[name].hidden))
                    if (JSON.stringify(tiles) !== JSON.stringify(win.tiles))
                        win.tiles = tiles
            } catch (e) {}
        }
        x.open("GET", api + "/apps")
        x.send()
    }

    // What Home shows (shelld's /apps); null: Home's own until it answers.
    property var running: ({})
    property var status: ({})
    property var tiles: null

    // What shelld says (GET /home).
    property bool barryOpen: false
    property bool barryOver: false  // open over an app it closes back to
    // The art behind Barry: 0 in focus, 1 frosted (animated as Barry comes
    // and goes).
    property real frost: 0
    property int standbySeconds: 15

    function setBarry(open, why) {
        win.post("/barry", { open: open, why: why })
    }
    function readSettings() {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE || x.status !== 200)
                return
            try {
                const v = JSON.parse(x.responseText).standbySeconds
                if (typeof v === "number")
                    win.standbySeconds = v
            } catch (e) {}
        }
        x.open("GET", "http://127.0.0.1:47823/settings")
        x.send()
    }
    Component.onCompleted: readSettings()

    // The hero art: what shows while Barry is closed.
    SkinView {
        id: hero
        file: "Hero.qml"
        anchors.fill: parent
    }
    Binding { target: hero.item; property: "s"; value: win.s }
    Binding { target: hero.item; property: "shown"; value: !win.barryOpen || barry.opacity < 1 }
    Binding { target: hero.item; property: "frost"; value: win.frost }
    // Standby: a tap on the art wakes Barry.
    Item {
        anchors.fill: hero
        TapHandler {
            enabled: !win.barryOpen
            onTapped: win.setBarry(true, "tap on the game art")
        }
    }
    // The pages go see-through over the art (with no game, they stay solid).
    readonly property real glass: SteamGame.available ? 0.2 : 1

    // Barry: its three pages over the hero.
    Item {
        id: barry
        anchors.fill: parent
        opacity: 0
        visible: opacity > 0
        transform: Translate { id: barryShift }

        PageSwipe {
            id: pages
            anchors.fill: parent
            minPage: -1
            maxPage: 1
            threshold: 24 * win.s
            swipeEnabled: win.barryOpen
            onDragStarted: settleAnim.stop()
            onSettle: function (target, velocity) {
                settleAnim.stop()
                // As the keyboard: toward a side page, a quick move with a
                // visible overshoot; back to the apps, a softer settle.
                const opening = target !== 0
                settleAnim.to = target
                settleAnim.duration = opening ? 460 : 380
                settleAnim.easing.overshoot = opening ? 1.4 : 0.8
                settleAnim.start()
            }

            // Home.qml, or the skin's: the apps, in the middle.
            Item {
                width: pages.width
                height: pages.height
                x: pages.progress * pages.width
                SkinView {
                    id: home
                    file: "Home.qml"
                    anchors.fill: parent
                    onLoaded: if (win.tiles !== null) item.tiles = win.tiles
                }
            }

            Dashboard {
                id: dashboard
                width: pages.width
                height: pages.height
                x: (pages.progress - 1) * pages.width
                shown: win.barryOpen && (pages.progress > 0.001 || (pages.dragging && pages.page >= 0))
                visible: pages.progress > 0.001
                onHideRequested: pages.goTo(0)
            }

            Item {
                width: pages.width
                height: pages.height
                x: (pages.progress + 1) * pages.width
                visible: pages.progress < -0.001
                SkinView {
                    id: quick
                    file: "QuickSettings.qml"
                    anchors.fill: parent
                }
            }
        }
    }
    Binding { target: quick.item; property: "s"; value: win.s }
    Binding { target: quick.item; property: "glass"; value: win.glass }
    Binding { target: quick.item; property: "standbySeconds"; value: win.standbySeconds }
    Connections {
        target: quick.item
        ignoreUnknownSignals: true
        function onStandbyChosen(seconds) { win.standbySeconds = seconds }
    }
    Binding { target: home.item; property: "glass"; value: win.glass }
    Binding { target: quick.item; property: "shown"; value: win.barryOpen && pages.progress < -0.001 }

    NumberAnimation {
        id: settleAnim
        target: pages
        property: "progress"
        easing.type: Easing.OutBack
    }
    Binding { target: home.item; property: "s"; value: win.s }
    Binding { target: home.item; property: "running"; value: win.running }
    Binding { target: home.item; property: "status"; value: win.status }
    // Only once shelld has answered: until then Home keeps its own list (a
    // Binding with "when" still writes null into a view loaded later).
    onTilesChanged: if (tiles !== null && home.item) home.item.tiles = tiles
    Connections {
        target: home.item
        ignoreUnknownSignals: true
        function onLaunch(app) { win.post("/launch", { app: app }) }
        function onClose(app) { win.post("/close", { app: app }) }
    }

    property int homeSerial: -1  // the last opening seen; -1: none yet

    function slideIn(serial) {
        slideOut.stop()
        slide.stop()
        // Barry opens on the apps.
        settleAnim.stop()
        pages.goTo(0)
        settleAnim.stop()
        pages.progress = 0
        // A home screen with dropIn() (the built-in one, and skins that
        // use Barry.Input's TileDrop): its tiles fly in over a quick fade.
        // Else Barry slides up into place.
        const drops = !!home.item && typeof home.item.dropIn === "function"
        barryShift.y = drops ? 0 : 0.18 * height
        barry.opacity = 0
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE)
                return
            slide.start()
            if (drops)
                home.item.dropIn()
        }
        x.open("POST", api + "/home-ready")
        x.setRequestHeader("Content-Type", "application/json")
        x.send(JSON.stringify({ serial: serial }))
    }

    ParallelAnimation {
        id: slide
        NumberAnimation { target: barryShift; property: "y"; to: 0; duration: 300; easing.type: Easing.OutCubic }
        NumberAnimation { target: barry; property: "opacity"; to: 1; duration: 220; easing.type: Easing.OutQuad }
        NumberAnimation { target: win; property: "frost"; to: 1; duration: 380; easing.type: Easing.OutCubic }
    }
    // Back to the art: the apps fade out, the art comes back into focus and
    // the logo fades back in.
    ParallelAnimation {
        id: slideOut
        NumberAnimation { target: barry; property: "opacity"; to: 0; duration: 320; easing.type: Easing.InOutQuad }
        NumberAnimation { target: win; property: "frost"; to: 0; duration: 520; easing.type: Easing.InOutCubic }
    }

    // Any touch counts as use (a passive look: the touch goes on to what
    // is under it).
    Item {
        anchors.fill: parent
        z: 100
        PointHandler {
            onActiveChanged: if (active) standby.restart()
        }
    }
    Timer {
        id: standby
        interval: Math.max(1, win.standbySeconds) * 1000
        running: win.barryOpen && !win.barryOver && win.standbySeconds > 0
                 && pages.page !== 1 && !pages.dragging
        onTriggered: win.setBarry(false, "untouched for " + win.standbySeconds + " s")
    }
    Connections {
        target: pages
        function onPageChanged() { standby.restart() }
    }

    Timer {
        id: homePoll
        interval: 50
        running: true
        repeat: true
        property bool polling: false
        onTriggered: {
            if (polling)
                return
            polling = true
            const x = new XMLHttpRequest()
            x.onreadystatechange = function () {
                if (x.readyState !== XMLHttpRequest.DONE)
                    return
                homePoll.polling = false
                if (x.status !== 200)
                    return
                let answer = null
                try { answer = JSON.parse(x.responseText) } catch (e) { return }
                const open = answer.open === true
                if (win.homeSerial >= 0 && answer.serial !== win.homeSerial && open) {
                    win.readSettings()
                    win.slideIn(answer.serial)
                } else if (win.barryOpen && !open) {
                    slide.stop()
                    slideOut.start()
                } else if (win.homeSerial < 0 && open) {
                    // Open already as this started.
                    barryShift.y = 0
                    barry.opacity = 1
                    win.frost = 1
                }
                win.barryOver = answer.over === true
                win.homeSerial = answer.serial
                win.barryOpen = open
            }
            x.open("GET", api + "/home")
            x.send()
        }
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: win.refresh()
    }
}

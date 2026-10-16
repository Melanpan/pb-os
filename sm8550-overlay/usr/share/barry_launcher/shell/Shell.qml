// Barry Launcher's home screen on the AYN Thor's bottom screen during Game
// Mode: the first window of barry_launcher_session, under everything else.
// Tiles start the bottom screen's apps (Firefox, Discord, Signal) through
// barry_launcher_shelld, or bring a running one forward; holding the
// AYN button, or an app ending, comes back here. Because this window is
// always shown, gamescope falls back to it when an overlay such as the
// dashboard hides, instead of freezing on the overlay's last frame.
//
// Coming home (a swipe up from the bottom edge, the AYN button) slides the
// home screen up into place: barry_launcher_shelld counts the trips (GET
// /home), this puts the screen at the slide's start while still hidden and
// says so (/home-ready), and then it is brought forward.
//
// The performance dashboard is a page to the left of home: a left-to-right
// swipe from anywhere on home pulls it in, right to left (or going home)
// puts it away. Barry.Input's PageSwipe tells the swipe from a tap and says
// where the page goes; here it slides there, springing past a little and
// back, as the keyboard does coming up.
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

    PageSwipe {
        id: pages
        anchors.fill: parent
        threshold: 24 * win.s
        onDragStarted: settleAnim.stop()
        onSettle: function (target, velocity) {
            settleAnim.stop()
            const opening = target > pages.progress
            settleAnim.to = target
            // As the keyboard: in, a quick move with a visible overshoot;
            // away, a softer settle.
            settleAnim.duration = opening ? 460 : 380
            settleAnim.easing.overshoot = opening ? 1.4 : 0.8
            settleAnim.start()
        }

        // Home.qml, or the skin's; pushed right as the dashboard comes in.
        Item {
            width: pages.width
            height: pages.height
            x: pages.progress * pages.width
            SkinView {
                id: home
                file: "Home.qml"
                anchors.fill: parent
                transform: Translate { id: homeShift }
                onLoaded: if (win.tiles !== null) item.tiles = win.tiles
            }
        }

        Dashboard {
            id: dashboard
            width: pages.width
            height: pages.height
            x: (pages.progress - 1) * pages.width
            shown: pages.progress > 0.001 || pages.dragging
            visible: shown
            onHideRequested: pages.closePage()
        }
    }

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

    property int homeSerial: -1  // the last trip home seen; -1: none yet

    function slideIn(serial) {
        slide.stop()
        homeShift.y = 0.18 * height
        home.opacity = 0
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState === XMLHttpRequest.DONE)
                slide.start()
        }
        x.open("POST", api + "/home-ready")
        x.setRequestHeader("Content-Type", "application/json")
        x.send(JSON.stringify({ serial: serial }))
    }

    ParallelAnimation {
        id: slide
        NumberAnimation { target: homeShift; property: "y"; to: 0; duration: 300; easing.type: Easing.OutCubic }
        NumberAnimation { target: home; property: "opacity"; to: 1; duration: 220; easing.type: Easing.OutQuad }
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
                let serial = -1
                try { serial = JSON.parse(x.responseText).serial } catch (e) { return }
                if (win.homeSerial >= 0 && serial !== win.homeSerial) {
                    if (pages.open || pages.progress > 0)
                        pages.closePage()  // going home puts the dashboard away
                    win.slideIn(serial)
                }
                win.homeSerial = serial
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

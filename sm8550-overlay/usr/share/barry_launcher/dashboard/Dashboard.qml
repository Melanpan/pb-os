// Barry Launcher's performance dashboard: a page to the left of the home
// screen (Shell.qml swipes it in and away). Shows the chosen skin's
// Skin.qml, or the built-in one (skins/ayn) when the skin has none or it
// fails; the skin changing reloads it. Stats and the quick controls come
// from barry_launcher_statsd over HTTP, so a skin is a plain QML folder;
// see README.md next to this file.
import QtQuick
import Barry.Input

Item {
    id: page

    // On screen (even partly); stats are fetched only then.
    property bool shown: false
    // Put the page away (Shell.qml slides it out).
    signal hideRequested()

    readonly property string api: "http://127.0.0.1:47823"
    readonly property url fallbackSkin: Qt.resolvedUrl("skins/ayn/Skin.qml")
    property int idleSeconds: 0
    property double lastActivity: 0

    // What skins see (as their "dashboard" property).
    QtObject {
        id: dashboard
        readonly property int api: 1
        property var stats: ({})
        // Quick controls (fan profile, lighting, refresh rate); see README.md.
        property var controls: ({})
        // Change controls, e.g. { fanProfile: "max" }; controls updates.
        function setControls(body) {
            page.request("POST", "/controls", body, function (c) { if (c) dashboard.controls = c })
            dashboard.poke()
        }
        readonly property bool shown: page.shown
        property string skinDir: ""
        // Put the dashboard away (it slides back to the left).
        function hide() { page.hideRequested() }
        // Count as activity for the idle timeout (page touches count already).
        function poke() { page.lastActivity = Date.now() }
        // Raw access to barry_launcher_statsd for anything newer than this page.
        function request(method, path, body, callback) { page.request(method, path, body, callback) }
    }

    function request(method, path, body, callback) {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE)
                return
            let obj = null
            if (x.status === 200) {
                try { obj = JSON.parse(x.responseText) } catch (e) { obj = null }
            }
            if (callback)
                callback(obj)
        }
        x.open(method, api + path)
        if (body !== undefined && body !== null) {
            x.setRequestHeader("Content-Type", "application/json")
            x.send(JSON.stringify(body))
        } else {
            x.send()
        }
    }

    function load() {
        const url = Skin.view("Skin.qml", fallbackSkin)
        dashboard.skinDir = url === fallbackSkin ? "" : Skin.dir
        skin.setSource(url, { dashboard: dashboard })
        if (skin.status === Loader.Error && url !== fallbackSkin) {
            console.warn("skin", url, "failed to load; using the built-in one")
            dashboard.skinDir = ""
            skin.setSource(fallbackSkin, { dashboard: dashboard })
        }
    }

    function refresh() {
        request("GET", "/stats", null, function (s) { if (s) dashboard.stats = s })
    }

    // Controls change elsewhere too (Steam's refresh slider, a game).
    function refreshControls() {
        request("GET", "/controls", null, function (c) { if (c) dashboard.controls = c })
    }

    // Loaded while on screen; unloaded once away, so an edited skin is
    // reread next time.
    onShownChanged: {
        if (shown) {
            request("GET", "/settings", null, function (s) { page.idleSeconds = s ? s.idleSeconds : 0 })
            refresh()
            refreshControls()
            lastActivity = Date.now()
            load()
        } else {
            skin.source = ""
        }
    }
    Connections {
        target: Skin
        function onViewsChanged() { if (page.shown) page.load() }
    }

    // Behind the skin, wider than the page: an overshoot shows no gap.
    Rectangle {
        x: -parent.width * 0.3
        width: parent.width * 1.3
        height: parent.height
        color: "#0d0e12"
    }

    Loader {
        id: skin
        anchors.fill: parent
    }

    // Watches touches without taking them from the skin.
    Item {
        anchors.fill: parent
        z: 1
        PointHandler {
            onActiveChanged: dashboard.poke()
            onPointChanged: dashboard.poke()
        }
    }

    // The idle timeout (settings' idleSeconds; 0: none).
    Timer {
        interval: 1000
        running: page.shown
        repeat: true
        onTriggered: {
            page.refresh()
            if (page.idleSeconds > 0 && Date.now() - page.lastActivity > page.idleSeconds * 1000)
                dashboard.hide()
        }
    }

    Timer {
        interval: 3000
        running: page.shown
        repeat: true
        onTriggered: page.refreshControls()
    }
}

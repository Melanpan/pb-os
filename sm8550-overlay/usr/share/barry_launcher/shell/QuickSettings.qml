pragma ComponentBehavior: Bound
// Barry Launcher's quick settings: the page to the right of the apps.
// Each screen's brightness, the fan profile, charging, and the game profile
// of the game running (else the one Steam played last). They are the PB-OS
// tab's own settings, read and changed through pbos-qamd (Barry.Input's
// PbosQam); the fan profile is the dashboard's (barry_launcher_statsd's
// /controls), and how soon Barry goes back to the game art is Barry's own
// setting (statsd's /settings standbySeconds). A setting this device lacks is left out. While the page
// shows, it reads them again every few seconds (the charger may come or
// go). Colors are the skin's; a skin can also replace this file
// (SkinView.qml).
import QtQuick
import QtQuick.Layouts
import Barry.Input

Rectangle {
    id: qs
    property real s: 1
    property bool shown: false
    // Untouched this long, Barry goes back to the game art (0: never);
    // Shell.qml sets it and hears a new choice.
    property int standbySeconds: 15
    signal standbyChosen(int seconds)
    readonly property var standbyChoices: [10, 15, 30, 60, 0]
    // Over the game art, frosted (Shell.qml sets it): how solid this page's
    // own background is, 1 fully.
    property real glass: 1
    function tint(color) {
        const c = Qt.color(color)
        return Qt.rgba(c.r, c.g, c.b, c.a * glass)
    }

    readonly property string statsd: "http://127.0.0.1:47823"
    property var screens: null      // { top, bottom } 0-100
    property var controls: ({})     // statsd /controls
    property var charge: null       // tools get_charge_control
    property var profiles: null     // tools get_game_profiles
    property string error: ""
    property double movedAt: 0      // a slider moved: leave it alone a moment

    readonly property var fanNames: ({ eco: "Quiet", balanced: "Balanced", performance: "Performance", max: "Max" })
    readonly property var chargeModes: [
        { id: "normal", label: "Normal", desc: "Charges to 100 %." },
        { id: "limit80", label: "Stop at 80 %", desc: "The battery firmware stops charging at 80 %." },
        { id: "hold", label: "Hold", desc: "Charges to the level below, then leaves the battery alone." },
        { id: "bypass", label: "Run from USB", desc: "Runs from USB power and doesn't charge.", feature: "bypass" },
    ]
    readonly property var coreChoices: [
        { id: "all", label: "All" },
        { id: "performance", label: "Big + Prime" },
        { id: "big", label: "Big" },
        { id: "little", label: "Little" },
    ]

    color: tint(Skin.colors.background)

    function settled() { return Date.now() - movedAt > 2000 }

    function refresh() {
        PbosQam.call("barry", "get_state", [], function (ok, v) {
            if (ok && v && v.brightness && qs.settled())
                qs.screens = v.brightness
        })
        PbosQam.call("tools", "get_charge_control", [], function (ok, v) {
            if (ok && qs.settled())
                qs.charge = v && v.supported ? v : null
        })
        PbosQam.call("tools", "get_game_profiles", [], function (ok, v) {
            if (ok)
                qs.profiles = v && v.available ? v : null
            qs.error = ok || PbosQam.connected ? "" : "The PB-OS tab's backend (pbos-qamd) isn't answering."
        })
        statsdRequest("GET", "/controls", null, function (c) { if (c) qs.controls = c })
    }

    function statsdRequest(method, path, body, callback) {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE)
                return
            let obj = null
            if (x.status === 200) {
                try { obj = JSON.parse(x.responseText) } catch (e) { obj = null }
            }
            callback(obj)
        }
        x.open(method, statsd + path)
        if (body) {
            x.setRequestHeader("Content-Type", "application/json")
            x.send(JSON.stringify(body))
        } else {
            x.send()
        }
    }

    function setBrightness(screen, value) {
        movedAt = Date.now()
        const next = Object.assign({}, screens)
        next[screen] = value
        screens = next
        PbosQam.call("barry", "set_brightness", [screen, value])
    }

    function setCharge(patch) {
        movedAt = Date.now()
        const next = Object.assign({}, charge.settings, patch)
        charge = Object.assign({}, charge, { settings: next })
        PbosQam.call("tools", "set_charge_control", [next], function (ok, v) {
            if (ok && v && v.supported && qs.settled())
                qs.charge = v
        })
    }

    readonly property string game: SteamGame.profileAppid
    readonly property var profile: profiles && profiles.games && profiles.games[game] ? profiles.games[game] : ({})

    function setProfile(patch) {
        const next = Object.assign({}, profile, patch, { name: SteamGame.profileName })
        for (const k in patch)
            if (patch[k] === undefined)
                delete next[k]
        const games = Object.assign({}, profiles.games)
        games[game] = next
        profiles = Object.assign({}, profiles, { games: games })
        PbosQam.call("tools", "set_game_profile", [game, next], function (ok, v) {
            if (!ok || !v || !v.ok) {
                qs.error = "Not saved: " + (ok && v ? v.error || "unknown error" : v)
                return
            }
            qs.error = ""
            const all = Object.assign({}, qs.profiles.games)
            if (v.profile)
                all[qs.game] = v.profile
            else
                delete all[qs.game]
            qs.profiles = Object.assign({}, qs.profiles, { games: all })
        })
    }

    onShownChanged: if (shown) refresh()
    Timer {
        interval: 5000
        repeat: true
        running: qs.shown
        onTriggered: qs.refresh()
    }

    component Heading: Text {
        color: Skin.colors.text
        opacity: 0.6
        font { family: Skin.font || "Noto Sans"; pixelSize: 24 * qs.s; weight: Font.DemiBold; letterSpacing: 2 * qs.s }
        Layout.topMargin: 14 * qs.s
    }
    component Note: Text {
        color: Skin.colors.text
        opacity: 0.55
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        font { family: Skin.font || "Noto Sans"; pixelSize: 22 * qs.s }
    }
    component Choice: Rectangle {
        id: choice
        property string label
        property bool active: false
        signal chosen()
        Layout.fillWidth: true
        Layout.preferredHeight: 76 * qs.s
        radius: 20 * qs.s
        color: tap.pressed ? Skin.colors.pressed : active ? Skin.colors.lit : Skin.colors.panel
        border.color: active ? Skin.colors.accent : Skin.colors.border
        border.width: 2 * qs.s
        Text {
            anchors.fill: parent
            anchors.margins: 8 * qs.s
            text: choice.label
            color: choice.active ? Skin.colors.litText : Skin.colors.text
            font { family: Skin.font || "Noto Sans"; pixelSize: 24 * qs.s; weight: Font.DemiBold }
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: 14 * qs.s
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        TapHandler {
            id: tap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: choice.chosen()
        }
    }
    // A slider whose touch the page swipe never takes (preventStealing);
    // it sends a few times a second while moving, and once on release.
    component Slider: Item {
        id: slider
        property string label
        property real value: 0
        property real from: 0
        property real to: 100
        property real step: 1
        property string valueText: Math.round(shownValue) + " %"
        signal moved(real value)
        property bool dragging: false
        property real dragValue: 0
        readonly property real shownValue: dragging ? dragValue : value
        property real sent: NaN
        Layout.fillWidth: true
        Layout.preferredHeight: 92 * qs.s
        opacity: enabled ? 1 : 0.4

        function send() {
            if (dragValue !== sent) {
                sent = dragValue
                slider.moved(dragValue)
            }
        }
        Text {
            text: slider.label
            color: Skin.colors.text
            font { family: Skin.font || "Noto Sans"; pixelSize: 24 * qs.s }
        }
        Text {
            anchors.right: parent.right
            text: slider.valueText
            color: Skin.colors.text
            opacity: 0.7
            font { family: Skin.font || "Noto Sans"; pixelSize: 24 * qs.s }
        }
        Rectangle {
            id: track
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14 * qs.s
            x: knob.width / 2
            width: parent.width - knob.width
            height: 14 * qs.s
            radius: height / 2
            color: Skin.colors.panel
            Rectangle {
                width: parent.width * (slider.shownValue - slider.from) / Math.max(1, slider.to - slider.from)
                height: parent.height
                radius: parent.radius
                color: Skin.colors.accent
            }
        }
        Rectangle {
            id: knob
            width: 44 * qs.s
            height: width
            radius: width / 2
            anchors.verticalCenter: track.verticalCenter
            x: (slider.shownValue - slider.from) / Math.max(1, slider.to - slider.from) * track.width
            color: Skin.colors.text
            border.color: Skin.colors.accent
            border.width: 4 * qs.s
        }
        MouseArea {
            anchors.fill: parent
            enabled: slider.enabled
            preventStealing: true
            function place(mx) {
                const f = Math.max(0, Math.min(1, (mx - knob.width / 2) / track.width))
                const v = slider.from + f * (slider.to - slider.from)
                slider.dragValue = Math.round(v / slider.step) * slider.step
            }
            onPressed: function (e) { place(e.x); slider.dragging = true }
            onPositionChanged: function (e) { place(e.x) }
            onReleased: function (e) { place(e.x); slider.send(); slider.dragging = false }
            onCanceled: slider.dragging = false
        }
        Timer {
            interval: 150
            repeat: true
            running: slider.dragging
            onTriggered: slider.send()
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 40 * qs.s
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: column
            width: flick.width
            spacing: 12 * qs.s

            Text {
                text: "Quick Settings"
                color: Skin.colors.text
                font { family: Skin.font || "Noto Sans"; pixelSize: 44 * qs.s; weight: Font.Bold }
            }
            Note {
                visible: qs.error !== ""
                text: qs.error
                opacity: 0.85
            }

            // Standby: back to the game art
            Heading { text: "BACK TO THE GAME ART"; visible: Skin.settings.heroArt !== false }
            RowLayout {
                visible: Skin.settings.heroArt !== false
                Layout.fillWidth: true
                spacing: 12 * qs.s
                Repeater {
                    model: qs.standbyChoices
                    delegate: Choice {
                        required property int modelData
                        label: modelData === 0 ? "Never" : modelData < 60 ? modelData + " s" : modelData / 60 + " min"
                        active: qs.standbySeconds === modelData
                        onChosen: qs.statsdRequest("POST", "/settings", { standbySeconds: modelData }, function (v) {
                            if (v && typeof v.standbySeconds === "number")
                                qs.standbyChosen(v.standbySeconds)
                        })
                    }
                }
            }

            // Brightness
            Heading { text: "BRIGHTNESS"; visible: !!qs.screens }
            Slider {
                visible: !!qs.screens
                label: "Top screen"
                value: qs.screens ? qs.screens.top : 0
                onMoved: function (v) { qs.setBrightness("top", v) }
            }
            Slider {
                visible: !!qs.screens
                label: "Bottom screen"
                value: qs.screens ? qs.screens.bottom : 0
                onMoved: function (v) { qs.setBrightness("bottom", v) }
            }

            // Fan profile
            Heading { text: "FAN"; visible: !!qs.controls.fan }
            RowLayout {
                visible: !!qs.controls.fan
                Layout.fillWidth: true
                spacing: 12 * qs.s
                Repeater {
                    model: qs.controls.fan ? qs.controls.fan.profiles : []
                    delegate: Choice {
                        required property string modelData
                        label: qs.fanNames[modelData] || modelData
                        active: qs.controls.fan.profile === modelData
                        onChosen: qs.statsdRequest("POST", "/controls", { fanProfile: modelData },
                                                   function (c) { if (c) qs.controls = c })
                    }
                }
            }

            // Charging
            Heading { text: "BATTERY"; visible: !!qs.charge }
            RowLayout {
                visible: !!qs.charge
                Layout.fillWidth: true
                spacing: 12 * qs.s
                Repeater {
                    model: qs.chargeModes.filter(m => !m.feature || (qs.charge && qs.charge.features && qs.charge.features[m.feature]))
                    delegate: Choice {
                        required property var modelData
                        label: modelData.label
                        active: !!qs.charge && qs.charge.settings.mode === modelData.id
                        onChosen: qs.setCharge({ mode: modelData.id })
                    }
                }
            }
            Slider {
                visible: !!qs.charge && qs.charge.settings.mode === "hold"
                label: "Hold at"
                from: qs.charge && qs.charge.limits ? qs.charge.limits.hold_at[0] : 50
                to: qs.charge && qs.charge.limits ? qs.charge.limits.hold_at[1] : 90
                step: 5
                value: qs.charge ? qs.charge.settings.hold_at : 80
                onMoved: function (v) { qs.setCharge({ hold_at: v }) }
            }
            RowLayout {
                visible: !!qs.charge && !!qs.charge.features && !!qs.charge.features.slow
                Layout.fillWidth: true
                spacing: 12 * qs.s
                Choice {
                    label: "Full speed"
                    active: !!qs.charge && !qs.charge.settings.slow
                    onChosen: qs.setCharge({ slow: false })
                }
                Choice {
                    label: "Slow charging"
                    active: !!qs.charge && !!qs.charge.settings.slow
                    onChosen: qs.setCharge({ slow: true })
                }
            }
            Note {
                visible: !!qs.charge
                text: {
                    const live = qs.charge && qs.charge.state ? qs.charge.state : {}
                    const mode = qs.chargeModes.find(m => qs.charge && m.id === qs.charge.settings.mode)
                    const status = !live.plugged ? "Not on a charger."
                        : live.held ? "On the charger, holding at " + live.capacity + " %."
                        : "On the charger, " + String(live.status || "charging").toLowerCase() + " at " + live.capacity + " %."
                    return (mode ? mode.desc + " " : "") + status
                }
            }

            // Game profile
            Heading { text: "GAME PROFILE"; visible: !!qs.profiles }
            Note {
                visible: !!qs.profiles
                text: qs.game === "" ? "No game yet: start one, or play one first."
                    : SteamGame.profileName + (SteamGame.running ? " (playing)" : " (played last)") + ". Applies the next time it starts."
                opacity: 0.8
            }
            Text {
                visible: !!qs.profiles && qs.game !== ""
                text: "x86 emulation"
                color: Skin.colors.text
                font { family: Skin.font || "Noto Sans"; pixelSize: 24 * qs.s }
            }
            GridLayout {
                visible: !!qs.profiles && qs.game !== ""
                Layout.fillWidth: true
                columns: 3
                columnSpacing: 12 * qs.s
                rowSpacing: 12 * qs.s
                Repeater {
                    model: {
                        const presets = qs.profiles && qs.profiles.presets && qs.profiles.presets.presets ? qs.profiles.presets.presets : {}
                        const out = []
                        for (const id in presets)
                            out.push({ id: id, label: presets[id].label || id })
                        if (!out.some(p => p.id === "default"))
                            out.unshift({ id: "default", label: "Default" })
                        return out
                    }
                    delegate: Choice {
                        required property var modelData
                        label: modelData.label
                        active: (qs.profile.fex || "default") === modelData.id
                        onChosen: qs.setProfile(modelData.id === "default" ? { fex: undefined, fexConfig: undefined }
                                                                            : { fex: modelData.id, fexConfig: undefined })
                    }
                }
            }
            Note {
                visible: !!qs.profiles && qs.game !== "" && qs.profile.fex === "custom"
                text: "Custom switches: change them in the PB-OS tab (Game Profiles)."
            }
            Text {
                visible: !!qs.profiles && qs.game !== ""
                text: "CPU cores"
                color: Skin.colors.text
                font { family: Skin.font || "Noto Sans"; pixelSize: 24 * qs.s }
            }
            RowLayout {
                visible: !!qs.profiles && qs.game !== ""
                Layout.fillWidth: true
                spacing: 12 * qs.s
                Choice {
                    label: "Default"
                    active: !qs.profile.cores
                    onChosen: qs.setProfile({ cores: undefined })
                }
                Repeater {
                    model: qs.coreChoices.filter(c => qs.profiles && qs.profiles.cores && qs.profiles.cores[c.id])
                    delegate: Choice {
                        required property var modelData
                        label: modelData.label
                        active: qs.profile.cores === modelData.id
                        onChosen: qs.setProfile({ cores: modelData.id })
                    }
                }
            }
            Item { Layout.preferredHeight: 20 * qs.s }
        }
    }
}

// One of the shell's views (KeyPanel.qml or GridPanel.qml, TopInput.qml,
// DeckControls.qml, Home.qml): the skin's own file when it has one
// (Barry.Input's Skin), else the built-in one next to this file, which is
// also what loads if the skin's fails. Loaded again when the skin changes.
//
// `props` are set as it is made (a view's required properties); anything
// that changes later the user binds on `item`.
import QtQuick
import Barry.Input

Loader {
    id: view
    required property string file
    property var props: ({})
    // A keyboard on a grid pack's grid page (Thumb-Key's keys): GridPanel.qml
    // in KeyPanel.qml's place, the skin's own if it has one.
    // (Told by the controller's signal: a binding through props misses it.)
    readonly property QtObject keys: file === "KeyPanel.qml" && props.controller ? props.controller : null
    property bool grid: keys ? keys.gridPage : false
    readonly property string shown: grid ? "GridPanel.qml" : file
    onShownChanged: if (status !== Loader.Null) load()
    Connections {
        target: view.keys
        function onGridChanged() { view.grid = view.keys.gridPage }
    }

    function load() {
        const builtIn = Qt.resolvedUrl(shown)
        const url = Skin.view(shown, builtIn)
        setSource(url, props)
        if (status === Loader.Error && url !== builtIn) {
            console.warn("skin view", url, "failed to load; using the built-in one")
            setSource(builtIn, props)
        }
    }
    Component.onCompleted: load()
    Connections {
        target: Skin
        function onViewsChanged() { view.load() }
    }
}

// One of the shell's views (KeyPanel.qml, TopInput.qml, DeckControls.qml,
// Home.qml): the skin's own file when it has one (Barry.Input's Skin), else
// the built-in one next to this file, which is also what loads if the
// skin's fails. Loaded again when the skin changes.
//
// `props` are set as it is made (a view's required properties); anything
// that changes later the user binds on `item`.
import QtQuick
import Barry.Input

Loader {
    id: view
    required property string file
    property var props: ({})
    readonly property url builtIn: Qt.resolvedUrl(file)

    function load() {
        const url = Skin.view(file, builtIn)
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

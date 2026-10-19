// tool_stage.py's window: shell/ToolStage.qml around a stand-in view, as
// TopInputApp.qml has it, logging the stage's offset and opacity each frame
// ("frame <ms> <y> <opacity>", ms since the epoch) and its leaving.
import QtQuick
import QtQuick.Window
import Barry.Input

Window {
    id: win
    width: 1240
    height: 1080
    color: "black"
    visible: true

    ToolStage {
        id: stage
        anchors.fill: parent
        Rectangle { anchors.fill: parent; color: "white" }
    }
    onAfterAnimating: console.log("frame", Date.now(), stage.transform[0].y.toFixed(1),
                                  stage.opacity.toFixed(3))
    Connections {
        target: ShellState
        function onLeavingChanged() { console.log("leaving", ShellState.leaving) }
    }
}

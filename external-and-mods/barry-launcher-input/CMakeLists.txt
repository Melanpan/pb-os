# Barry.Input: Barry Launcher's keyboards and trackpad as a QML module, so
# the QML that draws them (and any skin) calls straight into the devices:
# no daemon, no socket, no request per key or pointer move.
cmake_minimum_required(VERSION 3.21)
project(barry_input LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Network DBus)
find_package(X11 REQUIRED)
if(NOT X11_Xtst_FOUND)
  message(FATAL_ERROR "Barry.Input needs libXtst")
endif()
qt_standard_project_setup(REQUIRES 6.5)

set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)
qt_add_qml_module(barryinput
  URI Barry.Input
  VERSION 1.0
  PLUGIN_TARGET barryinput
  SOURCES
    keymap.cpp keymap.h
    uinputdevice.cpp uinputdevice.h
    virtualmouse.cpp virtualmouse.h
    virtualkeyboard.cpp virtualkeyboard.h
    gesture.cpp gesture.h
    touchtrackpad.cpp touchtrackpad.h
    trackpadarea.cpp trackpadarea.h
    xtesttyper.cpp xtesttyper.h
    typer.cpp typer.h
    keyboardlanguages.cpp keyboardlanguages.h
    deckcontrols.cpp deckcontrols.h
    haptics.cpp haptics.h
    keyboardcontroller.cpp keyboardcontroller.h
    keyboardstate.cpp keyboardstate.h
    skin.cpp skin.h
    heattrail.cpp heattrail.h
)
target_compile_options(barryinput PRIVATE -Wall -Wextra -Werror)
target_link_libraries(barryinput PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::DBus X11::X11 X11::Xtst)

# Proton PB-OS (ARM64)

Valve's **Proton 11.0 (ARM64)** with one file rebuilt: Wine's X11 driver
(`files/lib/wine/aarch64-unix/winex11.so`), built from Valve's own source
(`ValveSoftware/Proton` tag `proton-11.0-2e`, its `wine` submodule) in Valve's
arm64 Proton SDK image, with the patch here.

## What the patch fixes

In Game Mode, gamescope gives Steam's own window X focus whenever Quick Access
or the Steam menu opens. Wine took that as "the game lost focus" and made the
desktop the foreground window (in `focus_out()`, and again from
`_NET_ACTIVE_WINDOW` in `X11DRV_GetWindowStateUpdates()`). A game in exclusive
fullscreen then left fullscreen and minimized itself, as on Windows, and under
ARM64 Proton it came back shrunk into the top-left corner (Tomb Raider 2013,
FlatOut). The game's own window, swapchain and gamescope were all unchanged.

With the patch, when focus or the active window moves to one of Steam's windows
(WM_CLASS `steam`/`Steam`, not a game's `steam_app_*`), the game stays in the
foreground, like Steam's overlay on Windows, which never takes focus from the
game. Proton already blocks the game's input while the overlay shows. Every
other focus change behaves as before.

## Build

`build.sh` on the arm64 build VM (Docker), see its header. The unpatched build
has the same exports, imports and libraries as Valve's shipped `winex11.so`;
the patch adds one import (`XGetClassHint`, libX11).

The driver only fits the Proton version it was built from (`PROTON_TAG`). When
Valve updates Proton 11.0 (ARM64), rebuild from the new tag.

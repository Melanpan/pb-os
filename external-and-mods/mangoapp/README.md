# mangoapp

PB-OS replaces SteamOS's `mangoapp` (the Performance Overlay) with upstream
[MangoHud](https://github.com/flightlessmango/MangoHud) at the commit in `REF`,
plus the patches here. The rest of MangoHud (`mangohud`, the layer libraries)
stays stock.

**Why:** Valve's ARM `mangoapp` reads GPU load from its own
`/proc/self/fdinfo`, so the overlay shows GPU 0 % while a game runs. Upstream
reads the focused game's fdinfo (`drm-engine-gpu`).

**Patch** `0001-qualcomm-handhelds-use-the-steam-frame-layout.patch`: upstream
tunes the overlay for the Steam Frame only (SM8650 display controller). With
it, every Qualcomm GPU gets the Frame's rows (FEX stats on level 4, no network,
no GPU voltage/fan rows). CPU power is hidden without Valve's power-monitor.
Devices that are not SM8650 also hide the memory clock (a Frame constant
upstream), the memory temperature (no `ddr-thermal` zone) and the battery row
(SM8550 battmgr `power_now` is not in watts).

**Build:** `scripts/apply-overlays.sh` calls
`scripts/build-mangoapp-in-rootfs.sh`, which builds inside a throwaway overlay
of the Frame rootfs (glibc 2.39) and reuses the result while `REF`, the
patches and the rootfs build stay the same.

**Changing it:** edit in a MangoHud checkout at `REF`, `git diff` into a
patch here, or move `REF` to a newer upstream commit and refresh the patches.

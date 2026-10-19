# pbos-platformd

PB-OS's platform daemons as modules of one event-driven C++ program
(they were a Python process each). Each unit runs its own module, under
its old unit name, so everything that starts, reloads or signals a unit by
name keeps working.

| Module | Unit | Was | Listens to |
|---|---|---|---|
| `fan` | `sm8550-fand.service` | `sm8550-fand` | a 3 s tick (thermal zones can't notify); SIGHUP reloads the profile |
| `power-button` | `sm8550-powerbuttond.service` | `sm8550-powerbuttond` | the key (evdev), input uevents, the charge-rest flag (inotify, pidfd) |
| `charge-control` | `sm8550-charge-control.service` | the daemon of `sm8550-charge-control` (the script stays as the settings CLI) | power_supply uevents, logind's wake signal, its settings file (inotify), SIGHUP |
| `volume-keeper` | `sm8550-volume-keeper.service` (user) | `sm8550-volume-keeper` | the sound server's events (libpulse) |
| `pbosd` | `pbosd.service` | `/usr/lib/konkr/pbosd` | input, power_supply and USB uevents, the /run flags (inotify, pidfd), process events (game boost), the webhelper (inotify); a 1 s fan tick; SIGHUP |
| `konkr-volume` | `konkr-volume.service` (user) | `konkr-volume` | the sound server's events, ALSA control events (card 0), the PipeWire node's params |
| `volume-keys` | `sm8550-volume-keys.service` (user) | `sm8550-volume-keys` | the keys (evdev), input uevents, `/dev/input` permissions (inotify) |

`pbos-platformd --system|--user [--only NAME,...] [--list]`. Built in the
rootfs by `build.sh` with pbos-qamd's event loop (`../pbos-qam/daemon`),
installed by `scripts/apply-overlays.sh` as `/usr/lib/pbos-platformd/pbos-platformd`.

Measured on a Retroid Pocket Nova (Game Mode, idle, CPU per minute with
child processes): the five Python daemons ~1690 ms (volume-keeper's wpctl
polling alone 1662 ms), ~33 MB; these modules 17 ms, ~2 MB.

`tests/` holds the off-device equivalence harnesses (Python vs C++ in a
Linux VM with a fake sysfs, uinput devices and a real PipeWire). The
power-button test sends KEY_POWER and runs under a logind inhibitor, or the
VM's own logind would power it off.

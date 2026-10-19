#!/usr/bin/env python3
"""One case of the pbosd equivalence test, for one implementation.

  scenario.py py|cpp CASE

Runs inside run.sh's sandbox: /sys, /etc, /run, /var/lib, /home and
/usr/share are writable fakes, and systemctl, runuser and pgrep are stubs
that log their argv (pgrep finds plasmashell while /tmp/ctl/desktop exists).
Every write() to /sys by the daemon is logged by wlog.so (LD_PRELOAD).

Two kinds of case:
  step  the fan curve and the power LED, tick by tick. Python: the script
        imported, its Daemon built, Fan.tick() and Leds.tick() called once per
        step (what its loop does every second), a reload done as its loop does
        on SIGHUP. C++: pbos-platformd --system --only pbosd with the test-only
        PBOS_PLATFORMD_TEST_PBOSD_TICK file: each write of it is one tick.
  live  both daemons as they run, in real time: keys written to FIFOs posing
        as /dev/input/event* (a fake pmic_pwrkey and InputPlumber keyboard,
        so no real power key exists), or real uinput devices (F13-F15 and a
        touchscreen only), state.json edits plus SIGHUP, /run flags.

Writes /tmp/out/CASE-IMPL.json: a snapshot of the files after each labeled
step, the commands run, the sysfs writes, and the fan after stopping.
"""
from __future__ import annotations

import ctypes
import fcntl
import importlib.machinery
import importlib.util
import json
import os
import signal
import struct
import subprocess
import sys
import time

IMPL, CASE = (sys.argv + ["", ""])[1:3]
T = "/tmp/t"
CTL = "/tmp/ctl"
WLOG = f"{CTL}/wlog"
CMDS = f"{CTL}/cmds"
DLOG = f"{CTL}/daemon.log"
TICK = f"{CTL}/tick"
STATE = "/var/lib/pbosd/state.json"
FAN = "/sys/class/hwmon/hwmon1"
PLED = "/sys/class/leds/rgb:power-led"
SLED = "/sys/class/leds/konkr:rgb:joysticks"
GPU = "/sys/class/devfreq/3d00000.gpu"
BAT = "/sys/class/power_supply/battery"
POLICIES = {0: ([0, 1], [307200, 672000, 1036800, 1497600, 1958400, 2265600]),
            2: ([2, 3, 4], [499200, 1094400, 1574400, 2035200, 2265600, 2611200, 3014400]),
            5: ([5, 6], [499200, 1132800, 1689600, 2035200, 2400000, 2803200]),
            7: ([7], [480000, 1132800, 1689600, 2035200, 2803200, 3302400])}
GPU_FREQS = [231000000, 310000000, 366000000, 422000000, 500000000, 578000000, 629000000, 680000000,
             720000000, 770000000, 834000000, 903000000, 1000000000, 1050000000]
ZONES = ["cpu-0-0-0", "cpu-2-1-0", "cpuss-0", "gpuss-0", "gpu-top", "ddr", "battery", "aoss0", "video"]
ZONE_OFFSETS = [0, 2, -3, 1, -1, 25, 40, 30, 35]
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
KEY_POWER, KEY_F13, KEY_F14, KEY_F15 = 116, 183, 184, 185
FIFO_PWR, FIFO_KBD = "/dev/input/event90", "/dev/input/event91"
WEBHELPER = "/home/steamos/.local/share/Steam/steamrtarm64/steamwebhelper.sh"
WEBHELPER_ORIG = ('#!/bin/bash\nexport LD_LIBRARY_PATH=.\n'
                  'exec taskset 0x7c $(pwd)/steamwebhelper "$@" &> ~/.steam/steam/logs/steamwebhelper.log\n')


def w(path: str, value) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(str(value))


def rd(path: str) -> str:
    try:
        with open(path) as fh:
            return fh.read()
    except OSError:
        return "<none>"


# --------------------------------------------------------------- fake sysfs --
def build_sys(c: dict) -> None:
    S = "/sys"
    w(f"{S}/firmware/devicetree/base/model", c.get("model", "KONKR Pocket FIT") + "\0")
    w(f"{S}/firmware/devicetree/base/compatible", "konkr,pocket-fit\0qcom,sm8650\0")
    for n, cap in enumerate([216, 216, 855, 855, 855, 855, 855, 1024]):
        w(f"{S}/devices/system/cpu/cpu{n}/cpu_capacity", cap)
    for p, (cpus, freqs) in POLICIES.items():
        d = f"{S}/devices/system/cpu/cpufreq/policy{p}"
        w(f"{d}/related_cpus", " ".join(map(str, cpus)) + "\n")
        w(f"{d}/scaling_available_governors", "ondemand userspace powersave performance schedutil\n")
        w(f"{d}/scaling_governor", "performance\n")
        w(f"{d}/schedutil/rate_limit_us", "1000\n")
        w(f"{d}/cpuinfo_max_freq", f"{freqs[-1]}\n")
        w(f"{d}/cpuinfo_min_freq", f"{freqs[0]}\n")
        w(f"{d}/scaling_available_frequencies", " ".join(map(str, freqs)) + " \n")
        w(f"{d}/scaling_max_freq", f"{freqs[-1]}\n")
        w(f"{d}/scaling_min_freq", f"{freqs[0]}\n")
        for cpu in cpus:
            w(f"{S}/devices/system/cpu/cpu{cpu}/cpufreq/cpuinfo_max_freq", freqs[-1])
    w(f"{GPU}/available_frequencies", " ".join(map(str, GPU_FREQS)) + "\n")
    for k, v in (("max_freq", GPU_FREQS[-1]), ("min_freq", GPU_FREQS[0]), ("polling_interval", 50)):
        w(f"{GPU}/{k}", f"{v}\n")
    w(f"{S}/class/devfreq/soc:qcom,cpu-bwmon/available_frequencies", "100 200\n")
    w(f"{S}/class/hwmon/hwmon0/name", "pm8550_tz\n")
    if c.get("fan", True):
        w(f"{FAN}/name", "pwmfan\n")
        w(f"{FAN}/pwm1", "70\n")
        w(f"{FAN}/pwm1_enable", "1\n")
    set_temps(c.get("temp", 45))
    for led in (PLED, SLED):
        w(f"{led}/multi_intensity", "0 0 0\n")
        w(f"{led}/brightness", "0\n")
        w(f"{led}/max_brightness", "255\n")
    w(f"{SLED}/effect", "static\n")
    w(f"{S}/class/leds/mmc0::/brightness", "0\n")
    w(f"{S}/class/power_supply/usb/type", "USB\n")
    set_battery(*c.get("battery", (50, "Discharging")))
    if c.get("serial", True):
        w(f"{S}/bus/serial/devices/serial0-0/gamepad_mode", "xbox\n")
    for name, (vid, pid) in c.get("usb", {"1-1": ("045e", "028e"), "usb1": ("1d6b", "0002")}).items():
        w(f"{S}/bus/usb/devices/{name}/idVendor", vid + "\n")
        w(f"{S}/bus/usb/devices/{name}/idProduct", pid + "\n")
    if c.get("input") == "fifo":
        for ev, name in (("event90", "pmic_pwrkey"), ("event91", "InputPlumber Keyboard"),
                         ("event92", "Microsoft X-Box 360 pad")):
            w(f"{S}/class/input/{ev}/device/name", name + "\n")
            w(f"{S}/class/input/{ev}/device/properties", "0\n")
            w(f"{S}/class/input/{ev}/device/capabilities/abs", "0\n")
            os.mkfifo(f"/dev/input/{ev}")


def set_temps(c, raw: dict | None = None) -> None:
    for i, (z, off) in enumerate(zip(ZONES, ZONE_OFFSETS)):
        v = (raw or {}).get(z, int((c + off) * 1000 + 371) if c is not None else "")
        w(f"/sys/class/thermal/thermal_zone{i}/type", z + "\n")
        w(f"/sys/class/thermal/thermal_zone{i}/temp", f"{v}\n")


def set_battery(cap, status) -> None:
    w(f"{BAT}/type", "Battery\n")
    w(f"{BAT}/capacity", f"{cap}\n")
    w(f"{BAT}/status", f"{status}\n")


WATCH = ([f"{FAN}/pwm1", f"{FAN}/pwm1_enable", f"{PLED}/multi_intensity", f"{PLED}/brightness",
          f"{SLED}/multi_intensity", f"{SLED}/brightness", f"{SLED}/effect",
          f"{GPU}/max_freq", f"{GPU}/min_freq", f"{GPU}/polling_interval",
          "/sys/bus/serial/devices/serial0-0/gamepad_mode",
          "/etc/inputplumber/capability_maps.d/konkr_pocketfit_sysbtn.yaml", WEBHELPER, STATE,
          "/var/lib/pbosd/other", "/var/lib/konkrd"]
         + [f"/sys/devices/system/cpu/cpufreq/policy{p}/{a}" for p in POLICIES
            for a in ("scaling_governor", "scaling_max_freq", "scaling_min_freq", "schedutil/rate_limit_us")])


def snapshot(label: str) -> dict:
    files = {p: rd(p) for p in WATCH}
    files["plugins"] = " ".join(sorted(os.listdir("/home/steamos/homebrew/plugins"))) \
        if os.path.isdir("/home/steamos/homebrew/plugins") else "<none>"
    cmds = sorted(l for l in rd(CMDS).splitlines() if l)
    files.update(thread_attrs())
    return {"label": label, "files": files, "cmds": cmds}


# --------------------------------------------------------------- processes --
_libc = ctypes.CDLL(None, use_errno=True)
PROCS: list = []


def spawn(*argv):
    PROCS.append(subprocess.Popen([f"{T}/{argv[0]}", *argv[1:]], env={**os.environ, "PROCS_LATE_S": "4"}))


def spawn_game():
    spawn("reaper", "SteamLaunch", "AppId=1", "--", f"{T}/game")


def thread_attrs() -> dict:
    """Per fake process (by name, in start order): each thread's uclamp.min,
    I/O priority and CPUs, threads in creation order."""
    out = {}
    for n, p in enumerate(PROCS):
        pids = [p.pid] + [int(c) for c in rd(f"/proc/{p.pid}/task/{p.pid}/children").split()]
        for pid in pids:
            comm = rd(f"/proc/{pid}/comm").strip()
            rows = []
            for tid in sorted(int(t) for t in os.listdir(f"/proc/{pid}/task")):
                attr = ctypes.create_string_buffer(56)
                _libc.syscall(275, tid, attr, 56, 0)  # sched_getattr
                umin = struct.unpack_from("I", attr, 48)[0]
                io = _libc.syscall(31, 1, tid)  # ioprio_get(IOPRIO_WHO_PROCESS)
                cpus = ",".join(map(str, sorted(os.sched_getaffinity(tid))))
                rows.append(f"uclamp.min={umin} ioprio={io >> 13}/{io & 0xff} cpus={cpus}")
            out[f"proc{n}:{comm}"] = " | ".join(rows)
    return out


# ------------------------------------------------------------------ uinput --
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT, UI_SET_PROPBIT = 0x40045564, 0x40045565, 0x40045567, 0x4004556e
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x401c5504, 0x5501, 0x5502


class Uinput:
    """A real input device: F13-F15 only, or a touchscreen (never a power key)."""

    def __init__(self, name: str, touch: bool = False):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        if touch:
            fcntl.ioctl(self.fd, UI_SET_PROPBIT, 1)  # INPUT_PROP_DIRECT
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, 0x14a)  # BTN_TOUCH
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
            for code in (0x00, 0x01, 0x2f, 0x35, 0x36, 0x39):
                fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
                fcntl.ioctl(self.fd, UI_ABS_SETUP, struct.pack("H2x6i", code, 0, 0, 1079 if code != 0x2f else 9,
                                                                  0, 0, 0))
        else:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
            for k in (KEY_F13, KEY_F14, KEY_F15):
                fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, struct.pack("HHHH80sI", 3, 0x1234, 0x5678, 1, name.encode(), 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(0.5)

    def emit(self, etype, code, value):
        os.write(self.fd, struct.pack("llHHi", 0, 0, etype, code, value))

    def key(self, code):
        for v in (1, 0):
            self.emit(EV_KEY, code, v)
            self.emit(EV_SYN, 0, 0)
            time.sleep(0.05)

    def touch(self, seconds: float, hz: int = 100):
        end = time.monotonic() + seconds
        x = 100
        while time.monotonic() < end:
            x = (x + 7) % 1000
            self.emit(EV_ABS, 0x35, x)
            self.emit(EV_ABS, 0x00, x)
            self.emit(EV_SYN, 0, 0)
            time.sleep(1 / hz)

    def close(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


# ------------------------------------------------------------------- cases --
def ramp(lo, hi, step=3):
    return list(range(lo, hi + 1, step)) if lo <= hi else list(range(lo, hi - 1, -step))


def ticks(temps):
    out = []
    for t in temps:
        out += [("temps", t), ("tick",), ("snap", f"t{t}")]
    return out


def state(**kw) -> dict:
    st = {"profile": "balanced", "rgb": {"mode": "breath", "color": "8c00ff", "brightness": 160},
          "fan": {"mode": "auto", "fixed": 50}, "power_led": True,
          "buttons": {"F13": "rgb-next", "F14": "profile-next"}, "buttons_mode": "steam"}
    st.update(kw)
    return st


def press(code, hold=0.1):
    return [("key", code, 1), ("sleep", hold), ("key", code, 0), ("settle",)]


UP_DOWN = ramp(40, 96) + ramp(95, 40)
GAME_STEPS = [("sleep", 2.5), ("snap", "boosted"), ("sleep", 2.5), ("snap", "late-thread"),
              ("state", state(profile="lowpower")), ("hup",), ("sleep", 1.6), ("snap", "lowpower"),
              ("spawn", "gamescope-wl"), ("spawn", "other"), ("sleep", 1.6), ("snap", "new-ui"),
              ("spawn-game",), ("sleep", 1.6), ("snap", "second-game"),
              ("state", state(profile="balanced")), ("hup",), ("sleep", 1.6), ("snap", "balanced")]
CASES = {
    # ---- step: the fan curve
    "fan-balanced": {"mode": "step", "steps": ticks(UP_DOWN)},
    "fan-lowpower": {"mode": "step", "state": state(profile="lowpower"), "steps": ticks(UP_DOWN)},
    "fan-switch": {"mode": "step", "steps": ticks(ramp(50, 80, 2)) + [("state", state(profile="lowpower")),
                                                                     ("hup",), ("snap", "lowpower")]
                   + ticks(ramp(80, 60, 1)) + [("state", state(profile="balanced")), ("hup",)]
                   + ticks(ramp(60, 70, 1))},
    "fan-fixed": {"mode": "step", "state": state(fan={"mode": "fixed", "fixed": 30}),
                  "steps": ticks(ramp(60, 95, 2) + ramp(94, 70, 2))
                  + [("state", state(fan={"mode": "fixed", "fixed": "45"})), ("hup",)] + ticks([70, 71])
                  + [("state", state(fan={"mode": "fixed", "fixed": 50.7})), ("hup",)] + ticks([72, 91, 85, 80])
                  + [("state", state(fan={"mode": "auto"})), ("hup",)] + ticks([80, 79, 60, 50])},
    "fan-sensorloss": {"mode": "step", "steps": ticks([60, 62, 64]) + [("temps", None), ("tick",), ("snap", "lost"),
                                                                       ("tick",), ("snap", "lost2")]
                       + ticks([64, 63, 60, 55, 50, 45])
                       + [("temps_raw", {z: "-30000" if z != "gpuss-0" else "200000" for z in ZONES}),
                          ("tick",), ("snap", "implausible")]
                       + ticks([50, 52])},
    "fan-minpwm": {"mode": "step", "conf": "[fan]\nmin_pwm = 60\nfailsafe_pwm = 200\n",
                   "steps": ticks(ramp(40, 90, 5) + ramp(85, 40, 5))},
    "fan-disabled": {"mode": "step", "conf": "[fan]\nenabled = no\n", "steps": ticks([50, 90, 40])},
    # ---- step: the power LED
    "leds": {"mode": "step", "steps": [
        ("tick",), ("snap", "discharging"),
        ("battery", 50, "Charging"), ("tick",), ("snap", "charging"),
        ("battery", 100, "Full"), ("tick",), ("snap", "full"),
        ("battery", 99, "Not charging"), ("tick",), ("snap", "notcharging99"),
        ("battery", 98, "Not charging"), ("tick",), ("snap", "notcharging98"),
        ("battery", 80, "Discharging"), ("flag", "sm8550-charge-held", "live"), ("tick",), ("snap", "held"),
        ("flag", "sm8550-charge-held", "kill"), ("tick",), ("snap", "held-dead"),
        ("flag", "sm8550-charge-held", None), ("tick",), ("snap", "held-gone"),
        ("battery", 14, "Discharging"), ("tick",), ("snap", "low1"), ("tick",), ("snap", "low2"),
        ("tick",), ("snap", "low3"), ("battery", 9, "Discharging"), ("tick",), ("snap", "low4"),
        ("battery", 9, "Charging"), ("tick",), ("snap", "low-charging"),
        ("battery", 0, "Discharging"), ("tick",), ("snap", "zero1"), ("tick",), ("snap", "zero2"),
        ("battery", 40, "Discharging"), ("tick",), ("snap", "back"),
        ("battery", 40, "Charging"), ("flag", "konkr-standby", ""), ("tick",), ("snap", "standby"),
        ("flag", "konkr-standby", None), ("tick",), ("snap", "awake"),
        ("state", state(power_led=False)), ("hup",), ("sleep", 2.6), ("tick",), ("snap", "led-off"),
        ("state", state(power_led=True, profile="lowpower")), ("hup",), ("snap", "flash"),
        ("sleep", 2.6), ("tick",), ("snap", "after-flash")]},
    # ---- live: profiles, keys, power key, flags
    "profiles": {"mode": "live", "input": "fifo", "steps": [
        ("snap", "start"), ("state", state(profile="lowpower")), ("hup",), ("settle",), ("snap", "lowpower"),
        ("state", state(profile="silent")), ("hup",), ("settle",), ("snap", "legacy-silent"),
        ("state", state(profile="turbo")), ("hup",), ("settle",), ("snap", "legacy-turbo")]},
    "keys": {"mode": "live", "input": "fifo", "state": state(buttons={"F13": "rgb-next", "F14": "profile-next",
                                                                      "F15": "sticks-toggle"}),
             "steps": sum([press(KEY_F13) + [("snap", f"rgb{i}")] for i in range(8)], [])
             + press(KEY_F15) + [("snap", "toggle-off")] + press(KEY_F15) + [("snap", "toggle-on")]
             + press(KEY_F13) + press(KEY_F13) + press(KEY_F13) + press(KEY_F13) + press(KEY_F13) + press(KEY_F13)
             + [("snap", "rgb-off")] + press(KEY_F15) + [("snap", "toggle-from-off")]
             + press(KEY_F14) + [("settle",), ("snap", "profile1")]
             + [("desktop", True)] + press(KEY_F14) + [("settle",), ("snap", "profile2-desktop")]
             + [("state", state(buttons={"F13": "profile-turbo", "F14": "fan-boost", "F15": ""}))]
             + [("hup",), ("settle",)] + press(KEY_F13) + press(KEY_F14) + press(KEY_F15) + [("settle",),
                                                                                           ("snap", "legacy")]},
    "keys-noconf": {"mode": "live", "input": "fifo",
                    "conf": "[buttons]\nF13 = sticks-toggle\nF14 = profile-lowpower\nF15 = profile-balanced\n",
                    "state": {"profile": "balanced"},
                    "steps": press(KEY_F15) + [("snap", "f15"), ("sleep", 2.5)] + press(KEY_F13) + [("snap", "f13")]
                    + press(KEY_F14) + [("settle",), ("snap", "f14")]},
    "power": {"mode": "live", "input": "fifo", "steps":
              press(KEY_POWER) + [("snap", "short")] + press(KEY_POWER, 1.5) + [("snap", "long")]
              + [("desktop", True)] + press(KEY_POWER) + [("snap", "desk-short")]
              + press(KEY_POWER, 1.6) + [("snap", "desk-long")]
              + [("key", KEY_POWER, 1), ("key", KEY_POWER, 1), ("sleep", 0.4), ("key", KEY_POWER, 0), ("settle",),
                 ("snap", "double-down")]},
    "standby": {"mode": "live", "input": "fifo", "steps":
                [("flag", "konkr-standby", ""), ("settle",)] + press(KEY_POWER) + press(KEY_F13)
                + [("snap", "in-standby"), ("flag", "konkr-standby", None), ("sleep", 0.3)] + press(KEY_POWER)
                + press(KEY_F13) + [("snap", "grace"), ("sleep", 3.2)] + press(KEY_POWER) + [("snap", "after")]
                + [("flag", "sm8550-charge-rest", "live"), ("settle",)] + press(KEY_POWER)
                + [("snap", "resting"), ("flag", "sm8550-charge-rest", "kill"), ("sleep", 0.6)] + press(KEY_POWER)
                + [("snap", "rest-grace"), ("sleep", 3.2)] + press(KEY_POWER) + [("snap", "rest-over")]},
    "buttons-mode": {"mode": "live", "input": "fifo", "variants": True, "steps": [
        ("snap", "start"), ("state", state(buttons_mode="system")), ("hup",), ("settle",), ("snap", "system"),
        ("hup",), ("settle",), ("snap", "same"), ("state", state(buttons_mode="bogus")), ("hup",), ("settle",),
        ("snap", "bogus")]},
    "buttons-missing": {"mode": "live", "input": "fifo", "steps": [("snap", "start")]},
    "gamepad": {"mode": "live", "input": "fifo", "steps": [("snap", "start")]},
    "gamepad-xbox": {"mode": "live", "input": "fifo", "conf": "[controller]\nmode = xbox\n",
                     "usb": {"usb1": ("1d6b", "0002")}, "steps": [("snap", "start")]},
    "gamepad-s2": {"mode": "live", "input": "fifo", "model": "AYANEO Pocket S2", "steps": [("snap", "start")]},
    "webhelper": {"mode": "live", "input": "fifo", "webhelper": True, "steps": [
        ("snap", "start"), ("file", WEBHELPER, WEBHELPER_ORIG), ("sleep", 62), ("snap", "rewritten")]},
    "migration": {"mode": "live", "input": "fifo", "legacy": True, "steps": [
        ("snap", "start")] + press(KEY_F14) + [("settle",), ("snap", "f14")] + press(KEY_F13) + [("snap", "f13")]},
    "legacy-state": {"mode": "live", "input": "fifo",
                     "state": {"profile": "silent", "fan": {"mode": "fixed", "fixed": 40, "boost": True},
                               "buttons": {"F13": "profile-turbo", "F14": "fan-boost"}, "buttons_mode": 3,
                               "extra": [1, 2.5, None]},
                     "steps": [("snap", "start")] + press(KEY_F15) + press(KEY_F13) + [("settle",), ("snap", "f13")]},
    "nostate": {"mode": "live", "input": "fifo", "steps": [("snap", "start")] + press(KEY_F13) + [("snap", "f13")]},
    # ---- live, real uinput devices (F13-F15 and a touchscreen only)
    "touch": {"mode": "live", "input": "real", "touch": True, "steps": [
        ("sleep", 1), ("snap", "idle"), ("touch", 0.6), ("sleep", 0.1), ("snap", "touching"), ("sleep", 2.5),
        ("snap", "released"), ("touch", 3.0), ("snap", "long-touch"), ("sleep", 2.5), ("snap", "released2"),
        ("touch", 0.3), ("flag", "konkr-standby", ""), ("sleep", 1.0), ("snap", "standby"),
        ("touch", 0.3), ("sleep", 0.3), ("snap", "standby-touch"), ("flag", "konkr-standby", None), ("sleep", 2),
        ("touch", 0.3), ("sleep", 0.2), ("snap", "after-standby"), ("sleep", 2.5), ("snap", "end")]},
    "hotplug": {"mode": "live", "input": "real", "kbd": True, "steps": [
        ("sleep", 1), ("ukey", KEY_F13), ("settle",), ("snap", "first"), ("kbd", "destroy"), ("sleep", 1),
        ("kbd", "create"), ("sleep", 6), ("ukey", KEY_F13), ("settle",), ("snap", "after-replug"),
        ("kbd", "destroy"), ("sleep", 1), ("kbd", "create"), ("sleep", 6), ("ukey", KEY_F14), ("settle",),
        ("snap", "again")]},
    # ---- live: game and UI boost (fake processes; uclamp, ioprio, affinity read back per thread)
    "games": {"mode": "live", "input": "fifo", "procs": True, "scan": True, "steps": GAME_STEPS},
    "games-cn": {"mode": "live", "input": "fifo", "procs": True, "steps": GAME_STEPS},
}


# ---------------------------------------------------------------- daemons --
class Py:
    """The Python pbosd: for step cases a child that imports the script and
    runs one line's command at a time (step_child below), for live ones the
    script itself."""

    def __init__(self, live: bool):
        self.live = live
        env = {**os.environ, "LD_PRELOAD": f"{T}/wlog.so", "PBOSD_WLOG": WLOG}
        if live:
            self.proc = subprocess.Popen([sys.executable, f"{T}/pbosd.py"], stdout=open(DLOG, "w"),
                                         stderr=subprocess.STDOUT, env=env)
            wait_log("profile ", 1)
            time.sleep(1.0)  # its first loop pass
        else:
            self.proc = subprocess.Popen([sys.executable, __file__, "--step-child", CASE], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=open(DLOG, "w"), env=env, text=True)
            self.cmd("init")

    def cmd(self, line: str) -> None:
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        if self.proc.stdout.readline().strip() != "ok":
            raise SystemExit(f"py {CASE}: step child failed on {line}:\n{rd(DLOG)}")

    def tick(self):
        self.cmd("tick")

    def hup(self):
        if self.live:
            self.proc.send_signal(signal.SIGHUP)
        else:
            self.cmd("hup")

    def stop(self):
        if self.live:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(10)
        else:
            self.cmd("stop")
            self.proc.stdin.close()
            self.proc.wait(10)


def step_child() -> None:
    """The Python pbosd imported, driven a command per line from stdin."""
    answers = os.fdopen(os.dup(1), "w")
    os.dup2(2, 1)
    sys.stdout = sys.stderr  # the script's log
    loader = importlib.machinery.SourceFileLoader("pbosd_py", f"{T}/pbosd.py")
    spec = importlib.util.spec_from_loader("pbosd_py", loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    d = None
    for line in sys.stdin:
        op = line.strip()
        if op == "init":
            m.migrate_from_konkrd()
            d = m.Daemon()
        elif op == "tick":  # the 1 s part of run()
            d.fan.tick(d.st["profile"], d.st.get("fan"))
            d.leds.tick(d.st.get("power_led", True))
        elif op == "hup":  # what run() does once SIGHUP set d.reload
            d.conf = m.load_conf()
            d.st = m.load_state()
            d.apply_profile()
            d.leds.apply_sticks(d.st)
            m.apply_buttons_mode(d.st["buttons_mode"])
            m.ensure_gamepad_mode(d.conf)
        elif op == "stop":  # its SIGTERM handler, without the exit
            d.fan.failsafe_now()
        print("ok", file=answers, flush=True)


class Cpp:
    def __init__(self, live: bool):
        env = {**os.environ, "LD_PRELOAD": f"{T}/wlog.so", "PBOSD_WLOG": WLOG}
        if not live:
            w(TICK, "0")
            env["PBOS_PLATFORMD_TEST_PBOSD_TICK"] = TICK
        if CASES[CASE].get("scan"):
            env["PBOS_PLATFORMD_TEST_PBOSD_SCAN"] = "1"
        self.proc = subprocess.Popen([f"{T}/pbos-platformd", "--system", "--only", "pbosd"], stdout=open(DLOG, "w"),
                                     stderr=subprocess.STDOUT, env=env)
        wait_log("pbosd: profile", 1)
        time.sleep(0.3)

    def tick(self):
        n = rd(DLOG).count("pbosd: test tick")
        w(TICK, str(n))
        wait_log("pbosd: test tick", n + 1)

    def hup(self):
        n = rd(DLOG).count("pbosd: profile")
        self.proc.send_signal(signal.SIGHUP)
        if not CASES[CASE]["mode"] == "live":
            wait_log("pbosd: profile", n + 1)

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        self.proc.wait(10)


def wait_log(text: str, count: int, timeout: float = 10) -> None:
    end = time.monotonic() + timeout
    while rd(DLOG).count(text) < count:
        if time.monotonic() > end:
            raise SystemExit(f"{IMPL} {CASE}: timed out waiting for {text!r} x{count}:\n{rd(DLOG)}")
        time.sleep(0.01)


# -------------------------------------------------------------------- run --
def main() -> None:
    c = CASES[CASE]
    live = c["mode"] == "live"
    for d in (CTL, "/var/lib/pbosd", "/run/user/1000", "/home/steamos"):
        os.makedirs(d, exist_ok=True)
    build_sys(c)
    os.makedirs("/etc/inputplumber/capability_maps.d", exist_ok=True)
    if c.get("variants", True) and CASE != "buttons-missing":
        w("/usr/share/pbosd/inputplumber/sysbtn-steam.yaml", "# steam\nversion: 1\nmapping: steam\n\n")
        w("/usr/share/pbosd/inputplumber/sysbtn-system.yaml", "# system\nversion: 1\nmapping: system\n")
        w("/etc/inputplumber/capability_maps.d/konkr_pocketfit_sysbtn.yaml",
          "# steam\nversion: 1\nmapping: steam\n")
    w("/etc/pbosd.conf", c.get("conf", "[buttons]\nF14 = profile-next\nF13 = rgb-next\nF15 = none\n"))
    if "state" in c:
        w(STATE, json.dumps(c["state"], indent=2))
    if c.get("legacy"):
        w("/var/lib/konkrd/state.json", json.dumps({"profile": "turbo", "buttons": {"F14": "fan-boost"}}, indent=2))
        w("/var/lib/konkrd/other", "x")
        for p in ("konkr-control", "pbos-control", "pbos-utils", "keep-me"):
            w(f"/home/steamos/homebrew/plugins/{p}/plugin.json", "{}")
    if c.get("webhelper"):
        w(WEBHELPER, WEBHELPER_ORIG)
        os.chown(WEBHELPER, 1000, 1000)
        os.chmod(WEBHELPER, 0o755)
    fifos = {}
    if c.get("input") == "fifo":
        for p in (FIFO_PWR, FIFO_KBD):
            fifos[p] = os.open(p, os.O_RDWR | os.O_NONBLOCK)
    devs = {}
    if c.get("touch"):
        devs["touch"] = Uinput("pbosd-test touchscreen", touch=True)
    if c.get("kbd"):
        devs["kbd"] = Uinput("InputPlumber Keyboard")
    helpers = {}
    if c.get("procs"):
        spawn("gamescope-wl")
        spawn("steam")
        spawn("other")
        spawn_game()
        time.sleep(0.3)

    daemon = (Py if IMPL == "py" else Cpp)(live)
    snaps = []
    for step in c["steps"]:
        op, args = step[0], step[1:]
        if op == "temps":
            set_temps(args[0])
        elif op == "temps_raw":
            set_temps(0, args[0])
        elif op == "tick":
            daemon.tick()
        elif op == "battery":
            set_battery(*args)
        elif op == "state":
            w(STATE + ".tmp", json.dumps(args[0], indent=2))
            os.replace(STATE + ".tmp", STATE)
        elif op == "hup":
            daemon.hup()
        elif op == "flag":
            name, how = args
            path = f"/run/{name}"
            if how is None:
                if os.path.exists(path):
                    os.remove(path)
            elif how == "live":
                helpers[name] = subprocess.Popen(["sleep", "1000"])
                w(path, helpers[name].pid)
            elif how == "kill":
                helpers[name].kill()
                helpers[name].wait()
            else:
                w(path, how)
            if not live:
                time.sleep(0.3)
        elif op == "key":
            code, value = args
            fd = fifos[FIFO_PWR if code == KEY_POWER else FIFO_KBD]
            os.write(fd, struct.pack("llHHi", 0, 0, EV_KEY, code, value) + struct.pack("llHHi", 0, 0, EV_SYN, 0, 0))
            time.sleep(0.02)
        elif op == "ukey":
            devs["kbd"].key(args[0])
        elif op == "kbd":
            if args[0] == "destroy":
                devs["kbd"].close()
            else:
                devs["kbd"] = Uinput("InputPlumber Keyboard")
        elif op == "touch":
            devs["touch"].touch(args[0])
        elif op == "desktop":
            if args[0]:
                w(f"{CTL}/desktop", "1")
            elif os.path.exists(f"{CTL}/desktop"):
                os.remove(f"{CTL}/desktop")
        elif op == "file":
            w(args[0], args[1])
        elif op == "spawn":
            spawn(args[0])
        elif op == "spawn-game":
            spawn_game()
        elif op == "sleep":
            time.sleep(args[0])
        elif op == "settle":
            time.sleep(0.9 if live else 0.1)
        elif op == "snap":
            snaps.append(snapshot(args[0]))
        else:
            raise SystemExit(f"unknown step {op}")
    time.sleep(3.2 if live else 0.3)  # a flash over (Python: on its next 1 s tick)
    snaps.append(snapshot("end"))
    daemon.stop()
    stop_raw = rd(f"{FAN}/pwm1")
    # The unit's ExecStopPost
    if rd(f"{FAN}/name").strip() == "pwmfan":
        w(f"{FAN}/pwm1", 255)
    stop_final = rd(f"{FAN}/pwm1")
    for h in list(helpers.values()) + PROCS:
        h.kill()
    for dv in devs.values():
        try:
            dv.close()
        except OSError:
            pass
    writes: dict[str, list[str]] = {}
    for line in rd(WLOG).splitlines():
        path, _, value = line.partition("\t")
        seq = writes.setdefault(path, [])
        if not seq or seq[-1] != value:
            seq.append(value)
    with open(f"/tmp/out/{CASE}-{IMPL}.json", "w") as fh:
        json.dump({"snaps": snaps, "writes": writes, "stop_raw": stop_raw, "stop_final": stop_final,
                   "log": rd(DLOG)}, fh, indent=1)


if __name__ == "__main__":
    if IMPL == "--list":
        print(" ".join(CASES))
    elif IMPL == "--step-child":
        step_child()
    else:
        main()

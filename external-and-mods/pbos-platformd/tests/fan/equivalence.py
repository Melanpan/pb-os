#!/usr/bin/env python3
"""Off-device equivalence test: the Python sm8550-fand and pbos-platformd's
fan module over the same temperature sequences on a fake sysfs (thermal
zones and a pwmfan hwmon, regular files), comparing each tick's line (temp,
smoothed, target, pwm), pwm1 and pwm1_enable after every tick, and the fan
after stopping.

  equivalence.py SCRIPT BINARY FAN_CONF [CASE...]

Runs inside run.sh's sandbox: /sys, /etc, /usr/share/sm8550-fand and
/var/lib/steamos-sm8550 are writable fakes, so both daemons read their real
paths.

Python: the script imported, FanDaemon.tick() called once per step (what its
run() loop does every 3 s, the exception path included), reload by setting
.reload as its SIGHUP handler does, stop = its SIGTERM handler's safe().
C++: pbos-platformd --system --only fan with the test-only
PBOS_PLATFORMD_TEST_FAN_TICK_S (short ticks, each tick's line printed); the
next step's temperatures are written as soon as a tick's line arrives, a
reload is SIGHUP, stop is SIGTERM. Writes to pwm1/pwm1_enable are counted
with inotify (the C++ module writes only when a value is not what it wants).

A config the Python daemon cannot start with makes it exit (systemd's
ExecStopPost then writes 128 and restarts it); the C++ module holds 128
instead. Both end with the fan at 128: that is what such a case compares.
"""
from __future__ import annotations

import ctypes
import importlib.machinery
import importlib.util
import json
import os
import select
import shutil
import signal
import struct
import subprocess
import sys
import time

HW = "/sys/class/hwmon/hwmon1"
FACTORY = "/usr/share/sm8550-fand/fan.conf"
USER = "/etc/sm8550-fand.conf"
QUICK = "/var/lib/steamos-sm8550/fan-profile.conf"
TICK = "0.25"
CONTROLSD = "# Written by sm8550-thor-controlsd (Barry Launcher's quick controls).\n[general]\nprofile={}\n"


def w(path: str, value) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(str(value))


def rd(path: str) -> str:
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return "<none>"


# ------------------------------------------------------------------ cases --
ZONES = ["cpu-0-0-0", "CPU-1-big", "gpuss-0", "video", "mem", "ddr", "battery", "gpu-top"]


def ramp(lo, hi, step):
    return list(range(lo, hi + 1, step)) if lo <= hi else list(range(lo, hi - 1, -step))


def temps(*cs):
    """Steps from °C values: zone temps spread around each value."""
    out = []
    for c in cs:
        out.append({"temps": {z: (c + d) * 1000 + 37 for z, d in
                              zip(ZONES, [0, 3, -4, -9, -12, 20, 30, 1])}})
    return out


SWEEP = ramp(30, 100, 7) + ramp(100, 25, 9) + [60, 60, 60, 61, 95, 20]

CASES = {}


def case(name, steps, factory=None, user=None, quick=None, pwm1="255", dry=False, fan=True):
    CASES[name] = {"steps": steps, "factory": factory, "user": user, "quick": quick, "pwm1": pwm1,
                   "dry": dry, "fan": fan}


case("balanced_factory", temps(*SWEEP))
for p in ("eco", "performance", "max"):
    case(f"quick_{p}", temps(*SWEEP), quick=CONTROLSD.format(p))
# Profiles switched by controlsd (file + systemctl reload), then a profile
# with no curve (reload fails, previous kept), then back.
st = temps(*SWEEP)
st[6].update(quick=CONTROLSD.format("performance"), reload=True)
st[12].update(quick=CONTROLSD.format("eco"), reload=True)
st[18].update(quick=CONTROLSD.format("max"), reload=True)
st[22].update(quick=CONTROLSD.format("turbo"), reload=True)
st[25].update(quick=CONTROLSD.format("balanced"), reload=True)
case("reload_profiles", st)
# /etc override exercising the parser: [DEFAULT], interpolation, ':' and '=',
# comments, case, continuation lines with a blank line, quantum 1 (float
# pwm truncated), no smoothing, an uppercase profile name.
USER_PARSER = """; user overrides
[DEFAULT]
hot = 90

[general]
Profile :  Custom

# comment between sections
[profile.custom]
FAN_CURVE = Mine

[fan_curve.mine]
curve = 0:40,
    50:41, 61:97,

    77:200 , %(hot)s:250, bad, 99:x, 100:300
[fan]
min_pwm: 30
max_pwm =  240
ramp_up = 1_00
ramp_down = 9
smoothing = 0
pwm_quantum = 1
"""
case("user_parser", temps(*SWEEP), user=USER_PARSER)
# Quantum ties: round() is half to even (5 -> 0, 15 -> 20, 25 -> 20, 35 -> 40).
case("quantum_ties", temps(0, 0, 10, 10, 20, 20, 30, 30, 5, 15, 25, 31), pwm1="x", user="""[general]
profile=t
[profile.t]
fan_curve=t
[fan_curve.t]
curve=0:5,10:15,20:25,30:35
[fan]
min_pwm=0
max_pwm=255
ramp_up=255
ramp_down=255
smoothing=0.0
pwm_quantum=10
""")
case("odd_values", temps(*SWEEP), user="""[fan]
smoothing = nan
ramp_down = -3
min_pwm = 300
max_pwm = 7
""")
case("big_smoothing", temps(*SWEEP[:12]), user="[fan]\nsmoothing=1e9\nramp_up=+5\npwm_quantum=0\n")
# Bad configs on reload: each fails and the previous config stays.
BAD = [
    "[fan]\nmin_pwm=51 # inline comments are not comments\n",
    "min_pwm=51\n",                                  # no section header
    "[fan]\nmin_pwm=51\nmin_pwm=52\n",               # duplicate option
    "[fan]\n[fan]\n",                                # duplicate section
    "[fan]\nsmoothing=50%\n",                        # bad interpolation
    "[fan]\nsmoothing=%(nope)s\n",                   # missing reference
    "[fan]\ngarbage line\n",                         # parse error
    b"[fan]\nmin_pwm=5\xff1\n",                      # not UTF-8
    "[fan]\nramp_up=0x10\n",                         # not base 10
    "[fan]\nsmoothing=0x1p-1\n",                     # hex float
    "[general]\nprofile=nowhere\n",                  # no such curve
]
st = temps(*SWEEP[:len(BAD) * 2 + 4])
for i, b in enumerate(BAD):
    st[1 + 2 * i].update(user=b, reload=True)
st[1 + 2 * len(BAD)].update(user="[fan]\nramp_down=40\n", reload=True)  # a good one at the end
case("bad_reloads", st)
case("bad_at_start", temps(50, 60, 70), user="[fan]\nmin_pwm=abc\n")
# No readable zone (missing, negative, junk): the safe duty.
st = temps(50, 50, 50)
st += [{"temps": {z: v for z, v in zip(ZONES, ["-5000", "abc", "", "-1", "x", "1.5e4", "nan", "-0"])}}] * 3
st += temps(40, 40)
case("no_temps", st)
# Something else writes the fan between ticks: both put it back.
st = temps(*[55] * 10)
st[3]["ext"] = {"pwm1": "255"}
st[5]["ext"] = {"pwm1_enable": "1"}
st[7]["ext"] = {"pwm1": "0", "pwm1_enable": "2"}
case("external_writes", st)
case("dry_run", temps(40, 70, 90, 95, 60), dry=True)
case("dry_run_no_fan", temps(40, 70, 90, 95, 60), dry=True, fan=False)


# ------------------------------------------------------------------ setup --
def setup(c: dict, fan_conf: str) -> None:
    for d in ("/sys/class", "/sys/firmware", "/var/lib/steamos-sm8550"):
        shutil.rmtree(d, ignore_errors=True)
    for f in (USER, QUICK):
        if os.path.exists(f):
            os.remove(f)
    w("/sys/firmware/devicetree/base/compatible", "ayn,thor\0qcom,sm8550\0")
    for i, z in enumerate(ZONES):
        w(f"/sys/class/thermal/thermal_zone{i}/type", z)
    w("/sys/class/hwmon/hwmon0/name", "battery")  # not the fan
    if c["fan"]:
        w(f"{HW}/name", "pwmfan")
        w(f"{HW}/pwm1", c["pwm1"])
        w(f"{HW}/pwm1_enable", "2")
    w(FACTORY, c["factory"] or fan_conf)
    if c["user"] is not None:
        wconf(USER, c["user"])
    if c["quick"] is not None:
        wconf(QUICK, c["quick"])


def wconf(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(text if isinstance(text, bytes) else text.encode())


def pre(step: dict) -> None:
    """A step's changes, before its tick (not the reload itself)."""
    for i, z in enumerate(ZONES):
        if z in step.get("temps", {}):
            w(f"/sys/class/thermal/thermal_zone{i}/temp", step["temps"][z])
    if "user" in step:
        wconf(USER, step["user"])
    if "quick" in step:
        wconf(QUICK, step["quick"])
    for k, v in step.get("ext", {}).items():
        w(f"{HW}/{k}", v)


def files() -> list:
    return [rd(f"{HW}/pwm1"), rd(f"{HW}/pwm1_enable")]


# --------------------------------------------------------------- inotify --
libc = ctypes.CDLL(None, use_errno=True)
IN_CLOSE_WRITE = 0x8


class Writes:
    """Writes (open for writing, then close) to pwm1 and pwm1_enable."""

    def __init__(self):
        self.fd = libc.inotify_init1(os.O_NONBLOCK)
        self.wd = {}
        for k in ("pwm1", "pwm1_enable"):
            if os.path.exists(f"{HW}/{k}"):
                self.wd[libc.inotify_add_watch(self.fd, f"{HW}/{k}".encode(), IN_CLOSE_WRITE)] = k

    def take(self) -> list:
        got = []
        while True:
            try:
                buf = os.read(self.fd, 4096)
            except BlockingIOError:
                return got
            off = 0
            while off < len(buf):
                wd, _mask, _cookie, ln = struct.unpack_from("iIII", buf, off)
                got.append(self.wd.get(wd, "?"))
                off += 16 + ln


# ---------------------------------------------------------------- Python --
def run_py(script: str, c: dict) -> dict:
    loader = importlib.machinery.SourceFileLoader("fand", script)
    spec = importlib.util.spec_from_loader("fand", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    logs = []
    mod.log = lambda msg: logs.append(msg)
    seen = {}
    rt, tp = mod.read_temp, mod.target_pwm

    def read_temp(paths):
        seen["temp"] = rt(paths)
        seen.pop("target", None)
        return seen["temp"]

    def target_pwm(conf, temp):
        seen["target"] = tp(conf, temp)
        return seen["target"]

    mod.read_temp, mod.target_pwm = read_temp, target_pwm
    steps = c["steps"]
    pre(steps[0])
    out = {"ticks": [], "logs": logs}
    try:
        d = mod.FanDaemon(dry_run=c["dry"])
    except Exception as exc:  # the daemon exits; ExecStopPost writes 128
        logs.append(f"CRASH {exc!r}")
        if c["fan"]:
            w(f"{HW}/pwm1", 128)
        out["stopped"] = files()
        return out
    n = 5 if c["dry"] else len(steps)
    for i in range(n):
        if i:
            pre(steps[i])
        if steps[i].get("reload"):
            d.reload = True
        try:
            d.tick()
        except Exception as exc:
            logs.append(f"tick failed: {exc!r}")
            d.safe()
        target = seen.get("target", 128) if seen.get("temp") else 128
        line = f"temp {seen.get('temp')} C (smoothed {d.smoothed:.1f}) target {target} -> pwm {d.last_pwm}"
        if c["dry"]:
            line = logs[-1]
        out["ticks"].append({"line": line, "files": files()})
    if not c["dry"]:
        d.safe()
        logs.append(f"stopped (fan at {mod.FAN_SAFE_PWM})")
    out["stopped"] = files()
    return out


# ------------------------------------------------------------------- C++ --
def run_cpp(binary: str, c: dict) -> dict:
    steps = c["steps"]
    pre(steps[0])
    writes = Writes()
    env = dict(os.environ, PBOS_PLATFORMD_TEST_FAN_TICK_S=TICK)
    if c["dry"]:
        env["PBOS_PLATFORMD_DRY_RUN"] = "1"
    p = subprocess.Popen([binary, "--system", "--only", "fan"], env=env, stderr=subprocess.PIPE, text=True)
    out = {"ticks": [], "logs": [], "writes": []}
    i = 0
    deadline = time.monotonic() + 30
    want = 5 if c["dry"] else len(steps)
    while time.monotonic() < deadline:
        r, _, _ = select.select([p.stderr], [], [], 0.5)
        if not r:
            if c.get("expect_no_ticks") or (out["logs"] and out["logs"][-1].startswith("config unusable")
                                            and time.monotonic() > deadline - 28):
                break
            continue
        line = p.stderr.readline()
        if not line:
            break
        line = line.rstrip("\n")
        if not line.startswith("fan: "):
            continue
        msg = line[5:]
        if not msg.startswith("temp "):
            out["logs"].append(msg)
            continue
        out["ticks"].append({"line": msg, "files": files()})
        out["writes"].append(writes.take())
        i += 1
        if i >= want:
            break
        pre(steps[i])
        if steps[i].get("reload"):
            p.send_signal(signal.SIGHUP)
            # Wait for the reload to be done before the tick.
            while True:
                l2 = p.stderr.readline().rstrip("\n")
                if l2.startswith("fan: "):
                    out["logs"].append(l2[5:])
                    if "reload" in l2:
                        break
        writes.take()  # our own writes
    if c["dry"]:
        try:
            p.wait(timeout=5)
            out["exited"] = p.returncode
        except subprocess.TimeoutExpired:
            out["exited"] = "still running"
    if p.poll() is None:
        p.send_signal(signal.SIGTERM)
        p.wait(timeout=10)
    rest = p.stderr.read()
    out["logs"] += [l[5:] for l in rest.splitlines() if l.startswith("fan: ")]
    out["stopped"] = files()
    return out


# ---------------------------------------------------------------- compare --
def norm_logs(logs: list) -> list:
    out = []
    for m in logs:
        if m.startswith("temp "):
            continue
        if m.startswith("reload failed"):
            m = "reload failed"
        out.append(m)
    return out


def main() -> int:
    script, binary, fan_conf = sys.argv[1:4]
    with open(fan_conf) as fh:
        fan_conf = fh.read()
    names = sys.argv[4:] or list(CASES)
    rc = 0
    results = {}
    for name in names:
        c = CASES[name]
        setup(c, fan_conf)
        py = run_py(script, c)
        setup(c, fan_conf)
        cpp = run_cpp(binary, c)
        results[name] = {"py": py, "cpp": cpp}
        problems = []
        if name == "bad_at_start":
            ok = py["stopped"][0] == "128" and not cpp["ticks"] and files()[0] == "128" \
                and any(m.startswith("config unusable") for m in cpp["logs"])
            if not ok:
                problems.append(f"py {py['stopped']} {py['logs']} / cpp {cpp['ticks']} {cpp['logs']}")
        else:
            pt, ct = py["ticks"], cpp["ticks"]
            if len(pt) != len(ct):
                problems.append(f"{len(pt)} vs {len(ct)} ticks")
            for k, (a, b) in enumerate(zip(pt, ct)):
                if a != b:
                    problems.append(f"tick {k}: py {a} / cpp {b}")
                    break
            if py["stopped"] != cpp["stopped"]:
                problems.append(f"stopped: py {py['stopped']} / cpp {cpp['stopped']}")
            if norm_logs(py["logs"]) != norm_logs(cpp["logs"]):
                problems.append(f"logs: py {norm_logs(py['logs'])} / cpp {norm_logs(cpp['logs'])}")
            if c["dry"] and cpp.get("exited") != 0:
                problems.append(f"dry run exit {cpp.get('exited')}")
        nw = sum(len(x) for x in cpp.get("writes", []))
        extra = f"; C++ wrote the fan {nw}x in {len(cpp['ticks'])} ticks (Python {2 * len(py['ticks'])}x)" \
            if c["fan"] and not c["dry"] and name != "bad_at_start" else ""
        if problems:
            rc = 1
            print(f"{name}: DIFF {problems[:3]}")
        else:
            print(f"{name}: SAME ({len(cpp['ticks'])} ticks{extra})")
    with open("/tmp/out/fan-results.json", "w") as fh:
        json.dump(results, fh, indent=1, default=str)
    return rc


if __name__ == "__main__":
    sys.exit(main())

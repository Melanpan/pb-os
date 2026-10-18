#!/usr/bin/env python3
"""Off-device equivalence test: the Python sm8550-charge-control daemon and
pbos-platformd's charge-control module over the same scenarios on a fake
sysfs (SM8550_CHARGE_CONTROL_ROOT), comparing the files after every step,
the HELD flag and the log lines.

  equivalence.py run IMPL SCRIPT BINARY OUT.json   one implementation
  equivalence.py compare A.json B.json [...]        diff the results

IMPL: py (the script's daemon, with 1 s ticks), cpp (pbos-platformd).

The kernel's uevents can't be faked, so after each change of a fake sysfs
file the C++ side gets the same event through its test path: ROOT/test/uevent
(KEY=value lines, written and renamed). The Python side needs none: its
ticks find the change. Each step waits SETTLE seconds, longer than a tick.

The unit's ExecStopPost runs (on the fake tree) after the Python daemon
exits, as systemd would; the C++ module must restore by itself in stop().
"""
from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import time

SETTLE = 1.6
BATTMGR = "sys/bus/auxiliary/devices/pmic_glink.power-supply.0"
BAT = "sys/class/power_supply/battery"
USB = "sys/class/power_supply/qcom-battmgr-usb"
EXEC_STOP_POST = (  # the unit's, with the fake root in front of every path
    'for d in "$R"/sys/bus/auxiliary/devices/pmic_glink.power-supply.*; do [ -e $d/retroid_charge_now ] && '
    'echo 1 > $d/retroid_charge_now; [ -e $d/retroid_limit_capacity ] && echo 0 > $d/retroid_limit_capacity; '
    'done; f="$R"/sys/class/power_supply/battery/constant_charge_current_max; [ -w $f ] && echo 10000000 > $f; '
    'rm -f "$R"/run/sm8550-charge-held; true')


def w(root: str, rel: str, v) -> None:
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as fh:
        fh.write(str(v))


def tree(root: str, kind: str, cap=75, online=0, status="Discharging") -> None:
    os.makedirs(f"{root}/run", exist_ok=True)
    if kind in ("oem", "odin2"):
        w(root, f"{BATTMGR}/retroid_charge_now", 1)
        if kind == "oem":
            w(root, f"{BATTMGR}/retroid_limit_capacity", 0)
    if kind == "thresholds":
        w(root, f"{BAT}/charge_control_start_threshold", 70)
        w(root, f"{BAT}/charge_control_end_threshold", 80)
    w(root, f"{BAT}/capacity", cap)
    w(root, f"{BAT}/status", status)
    w(root, f"{BAT}/constant_charge_current_max", 3000000)
    w(root, f"{USB}/online", online)


# Each scenario: (tree kind, state or None, wait_s, steps). Steps:
#   ("set", {rel: value})  the kernel changed files (+ a power_supply uevent)
#   ("save", {changes})    the CLI's save (+ HUP to the daemon)
#   ("battmgr", kind)      the battery manager shows up late (+ an auxiliary uevent)
#   ("dropped",)           uevents were dropped (C++: empty map)
#   ("wait", s)            just time passing
#   ("term",)              SIGTERM; the snapshot is taken after exit
P = f"{BAT}/capacity"
ON = f"{USB}/online"
ST = f"{BAT}/status"
CN = f"{BATTMGR}/retroid_charge_now"
SCENARIOS = {
    "oem_hold": ("oem", {"mode": "hold", "hold_at": 80}, 60, [
        ("set", {ON: 1, ST: "Charging"}),
        ("set", {P: 78}), ("set", {P: 79}), ("set", {P: 80}),      # stop at 80
        ("set", {P: 79}), ("set", {P: 78}), ("set", {P: 77}),      # resume at 77
        ("set", {P: 79}), ("set", {P: 81}),
        ("set", {CN: 1}),                                          # firmware drop, no force
        ("dropped",),
        ("set", {ON: 0, ST: "Discharging"}),                       # unplug: HELD goes
        ("set", {CN: 1}),
        ("set", {ON: 1}),                                          # plug-in: forced again
        ("set", {P: 70}),
        ("term",),
    ]),
    "oem_limit_bypass_slow": ("oem", {"mode": "limit80"}, 60, [
        ("set", {ON: 1, ST: "Charging"}),
        ("set", {P: 79}), ("set", {P: 80}),
        ("save", {"mode": "bypass"}),
        ("set", {P: 40}), ("set", {P: 16}), ("set", {P: 15}),      # floor: charge
        ("set", {P: 17}), ("set", {P: 18}),                        # back to USB
        ("save", {"slow": True, "slow_ma": 1500}),
        ("save", {"slow_ma": 99999}),                              # held to 3000
        ("save", {"slow": False, "mode": "normal"}),
        ("save", {"mode": "hold", "hold_at": 10}),                 # held to 50
        ("set", {P: 50}),
        ("save", {"mode": "nonsense"}),
        ("set", {ON: 0}),
        ("term",),
    ]),
    "odin2_limit80": ("odin2", {"mode": "limit80", "slow": True}, 60, [
        ("set", {P: 82}),
        ("set", {ON: 1}),
        ("set", {P: 79}), ("set", {P: 77}), ("set", {P: 80}),
        ("set", {P: "garbage"}),                                   # capacity unreadable
        ("set", {P: 81}),
        ("term",),
    ]),
    "thresholds_first_run": ("thresholds", None, 60, [
        ("set", {ON: 1, ST: "Charging", P: 78}),
        ("set", {P: 79, ST: "Not charging"}),
        ("save", {"mode": "hold", "hold_at": 90}),
        ("set", {P: 86, ST: "Not charging"}),
        ("save", {"hold_at": 40}),                                 # held to 55
        ("save", {"mode": "bypass"}),                              # not here: normal
        ("set", {f"{BAT}/charge_control_end_threshold": 80}),      # firmware moved it
        ("save", {"slow": True, "slow_ma": 200}),                  # held to 500
        ("set", {ON: 0}),
        ("term",),
    ]),
    "battmgr_late": ("none", {"mode": "hold", "hold_at": 60}, 30, [
        ("set", {ON: 1, P: 70}),
        ("battmgr", "oem"),
        ("set", {P: 58}),
        ("set", {P: 57}),
        ("term",),
    ]),
    # C++ only (the Python daemon's wake check is the clock gap, not fakeable):
    # a dropped switch stays dropped on a plain event, and is written again
    # on a wake.
    "resume": ("oem", {"mode": "hold", "hold_at": 80}, 60, [
        ("set", {ON: 1, P: 85}),
        ("set", {CN: 1}),
        ("resume",),
        ("term",),
    ]),
    "give_up": ("none", {"mode": "hold"}, 2, [
        ("wait", 3),
        ("battmgr", "oem"),                                        # too late
        ("set", {ON: 1, P: 90}),
        ("term",),
    ]),
}


def snapshot(root: str, pid: int) -> dict:
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            rel = os.path.relpath(p, root)
            if rel.startswith(("test/", "var/")) or rel.endswith(".tmp"):
                continue
            with open(p) as fh:
                v = fh.read().strip()
            if rel == "run/sm8550-charge-held":
                v = "PID(self)" if v == str(pid) else f"PID(other:{v})"
            out[rel] = v
    return out


def event(root: str, impl: str, **env) -> None:
    if impl != "cpp":
        return
    os.makedirs(f"{root}/test", exist_ok=True)
    with open(f"{root}/test/uevent.tmp", "w") as fh:
        fh.write("".join(f"{k}={v}\n" for k, v in env.items()))
    os.replace(f"{root}/test/uevent.tmp", f"{root}/test/uevent")


def start(impl: str, script: str, binary: str, root: str, wait_s: int, log) -> subprocess.Popen:
    env = {**os.environ, "SM8550_CHARGE_CONTROL_ROOT": root, "SM8550_CHARGE_CONTROL_WAIT_S": str(wait_s),
           "PYTHONUNBUFFERED": "1"}
    if impl == "py":
        code = ("import importlib.machinery as M, importlib.util as U, sys\n"
                "l = M.SourceFileLoader('cc', sys.argv[1]); m = U.module_from_spec(U.spec_from_loader('cc', l))\n"
                "l.exec_module(m); m.TICK_S = 1; m.WAIT_S = int(sys.argv[2]); sys.argv = sys.argv[1:2]\n"
                "sys.exit(m.main())\n")
        argv = [sys.executable, "-c", code, script, str(wait_s)]
    else:
        argv = [binary, "--system", "--only", "charge-control"]
    return subprocess.Popen(argv, env=env, stdout=log, stderr=subprocess.STDOUT)


def run_scenario(impl, script, binary, name, spec) -> dict:
    kind, state, wait_s, steps = spec
    root = tempfile.mkdtemp(prefix=f"cc-{impl}-{name}-")
    tree(root, kind)
    if state is not None:
        w(root, "var/lib/pbos-utils/charge-control.json", json.dumps(state))
    logf = open(f"{root}.log", "w+")
    proc = start(impl, script, binary, root, wait_s, logf)
    stopped = False

    def stop_post():
        nonlocal stopped
        if stopped:
            return
        stopped = True
        if impl == "py":  # systemd's ExecStopPost; C++ restores in stop()
            subprocess.run(["/bin/sh", "-c", EXEC_STOP_POST], env={**os.environ, "R": root})

    time.sleep(SETTLE)
    snaps = [("start", snapshot(root, proc.pid))]
    for step in steps:
        op = step[0]
        if op == "set":
            for rel, v in step[1].items():
                w(root, rel, v)
            event(root, impl, SUBSYSTEM="power_supply", ACTION="change")
        elif op == "save":
            subprocess.run([sys.executable, script, "save", json.dumps(step[1])], capture_output=True,
                           env={**os.environ, "SM8550_CHARGE_CONTROL_ROOT": root})
            if proc.poll() is None:
                proc.send_signal(signal.SIGHUP)
        elif op == "battmgr":
            w(root, f"{BATTMGR}/retroid_charge_now", 1)
            w(root, f"{BATTMGR}/retroid_limit_capacity", 0)
            event(root, impl, SUBSYSTEM="auxiliary", ACTION="bind")
        elif op == "resume":
            event(root, impl, RESUME=1)
        elif op == "dropped":
            event(root, impl, DROPPED=1)
        elif op == "wait":
            time.sleep(step[1])
        elif op == "term":
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            proc.wait(10)
        if proc.poll() is not None:
            stop_post()
        time.sleep(SETTLE if op != "term" else 0.2)
        if proc.poll() is not None:
            stop_post()
        snaps.append((repr(step), snapshot(root, proc.pid)))
    if proc.poll() is None:
        proc.kill()
    logf.seek(0)
    lines = []
    for line in logf.read().splitlines():
        if line.startswith("pbos-platformd:"):
            continue
        for pre in ("sm8550-charge-control: ", "charge-control: "):
            if line.startswith(pre):
                line = line[len(pre):]
        lines.append(line)
    return {"snapshots": snaps, "log": lines, "exit": proc.returncode}


def main() -> int:
    if sys.argv[1] == "run":
        impl, script, binary, out = sys.argv[2:6]
        only = sys.argv[6].split(",") if len(sys.argv) > 6 else list(SCENARIOS)
        res = {n: run_scenario(impl, script, binary, n, SCENARIOS[n]) for n in only}
        with open(out, "w") as fh:
            json.dump(res, fh, indent=1)
        return 0
    results = [(p, json.load(open(p))) for p in sys.argv[2:]]
    base_name, base = results[0]
    bad = 0
    for name in base:
        for other_name, other in results[1:]:
            a, b = base[name], other[name]
            diffs = []
            for (sa, xa), (sb, xb) in zip(a["snapshots"], b["snapshots"]):
                if xa != xb:
                    keys = sorted(set(xa) | set(xb))
                    diffs.append(f"  after {sa}: " + ", ".join(
                        f"{k}: {xa.get(k)} vs {xb.get(k)}" for k in keys if xa.get(k) != xb.get(k)))
            if a["log"] != b["log"]:
                diffs.append(f"  log:\n    {base_name}: {a['log']}\n    {other_name}: {b['log']}")
            status = "PASS" if not diffs else "FAIL"
            bad += bool(diffs)
            print(f"{status} {name}: {base_name} vs {other_name} ({len(a['snapshots'])} snapshots,"
                  f" {len(a['log'])} log lines)")
            for d in diffs:
                print(d)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

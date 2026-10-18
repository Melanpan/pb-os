#!/usr/bin/env python3
"""Off-device equivalence test: the Python sm8550-powerbuttond and
pbos-platformd's power-button module, both running at once on the same fake
pmic_pwrkey (uinput), the same /run (the charge-rest flag) and stub
runuser/systemctl/pgrep that log their argv and environment with the caller
(python3 or pbos-platformd). Each scenario's commands are compared per caller.

  equivalence.py SCRIPT BINARY

Runs inside run.sh's sandbox (root; stubs bound over /usr/bin; /tmp/ctl shared).
The pwrkey device is made after both daemons started: the Python one finds
it by its 2 s retry, the C++ one by the input uevent.
"""
from __future__ import annotations

import fcntl
import os
import struct
import subprocess
import sys
import time

FLAG = "/run/sm8550-charge-rest"
LOG = "/tmp/ctl/log"
DESKTOP = "/tmp/ctl/desktop"
KEY_POWER = 116
UI_SET_EVBIT = (1 << 30) | (4 << 16) | (ord("U") << 8) | 100
UI_SET_KEYBIT = (1 << 30) | (4 << 16) | (ord("U") << 8) | 101
UI_DEV_CREATE = (ord("U") << 8) | 1
UI_DEV_DESTROY = (ord("U") << 8) | 2

ui = None


def make_key() -> None:
    global ui
    ui = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(ui, UI_SET_EVBIT, 1)
    fcntl.ioctl(ui, UI_SET_KEYBIT, KEY_POWER)
    os.write(ui, struct.pack("80sHHHHi", b"pmic_pwrkey", 0x19, 1, 1, 1, 0) + bytes(4 * 64 * 4))
    fcntl.ioctl(ui, UI_DEV_CREATE)


def key(value: int) -> None:
    t = time.time()
    os.write(ui, struct.pack("llHHi", int(t), int((t % 1) * 1e6), 1, KEY_POWER, value))
    os.write(ui, struct.pack("llHHi", int(t), int((t % 1) * 1e6), 0, 0, 0))


def press(hold: float) -> None:
    key(1)
    time.sleep(hold)
    key(0)


def mark(name: str) -> None:
    with open(LOG, "a") as fh:
        fh.write(f"== {name}\n")


def flag(pid) -> None:
    with open(FLAG, "w") as fh:  # as sm8550-charge-rest's claim()
        fh.write(str(pid))


def unflag() -> None:
    try:
        os.remove(FLAG)
    except OSError:
        pass


def desktop(on: bool) -> None:
    if on:
        open(DESKTOP, "w").close()
    elif os.path.exists(DESKTOP):
        os.remove(DESKTOP)


def rester():
    return subprocess.Popen(["sleep", "1000"])


# Each scenario: (name, steps, expected difference or None).
def s_short():
    press(0.2)
    time.sleep(0.8)


def s_long():
    press(1.6)
    time.sleep(0.8)


def s_two_short():
    press(0.15)
    time.sleep(0.4)
    press(0.15)
    time.sleep(0.8)


def s_desktop_short():
    desktop(True)
    press(0.2)
    time.sleep(0.8)
    desktop(False)


def s_desktop_long():
    desktop(True)
    press(1.6)
    time.sleep(0.8)
    desktop(False)


def s_rest_live_then_flag_removed():
    r = rester()
    flag(r.pid)
    time.sleep(0.3)
    press(0.2)          # the wake press: ignored
    time.sleep(0.2)
    unflag()            # charge-rest stops resting, removes its flag
    r.kill(); r.wait()
    time.sleep(0.6)
    press(0.2)          # within the grace: ignored
    time.sleep(3.2)
    press(0.2)          # after it: Steam's sleep
    time.sleep(0.8)


def s_rest_pid_dies_flag_stays():
    r = rester()
    flag(r.pid)
    time.sleep(0.3)
    press(0.2)
    time.sleep(0.2)
    r.kill(); r.wait()  # gone without removing the flag
    time.sleep(0.6)
    press(0.2)          # ignored (grace)
    time.sleep(3.2)
    press(0.2)          # handled
    time.sleep(0.8)
    unflag()


def s_rest_starts_while_held():
    r = rester()
    key(1)
    time.sleep(0.4)
    flag(r.pid)         # no long press
    time.sleep(1.4)
    key(0)              # no short press either
    time.sleep(0.5)
    unflag()
    r.kill(); r.wait()
    time.sleep(3.2)


def s_dead_pid():
    flag(4194300)       # no such process
    time.sleep(0.3)
    press(0.2)
    time.sleep(0.8)
    unflag()


def s_junk_flag():
    with open(FLAG, "w") as fh:
        fh.write("not a pid")
    time.sleep(0.3)
    press(0.2)
    time.sleep(0.8)
    unflag()


def s_zombie_pid():
    pid = os.fork()
    if pid == 0:
        os._exit(0)
    time.sleep(0.2)     # a zombie: exited, not reaped
    flag(pid)
    time.sleep(0.3)
    press(0.2)
    time.sleep(0.8)
    unflag()
    os.waitpid(pid, 0)
    time.sleep(0.3)


def s_grace_from_rest_end():
    r = rester()
    flag(r.pid)
    time.sleep(0.3)
    press(0.2)          # ignored by both
    time.sleep(1.8)
    unflag()            # the rest ends 2 s after the press
    r.kill(); r.wait()
    time.sleep(1.0)
    press(0.2)          # Python: its grace ran from the press; C++: from the rest's end
    time.sleep(3.0)


SCENARIOS = [
    ("short", s_short, None),
    ("long", s_long, None),
    ("two_short", s_two_short, None),
    ("desktop_short", s_desktop_short, None),
    ("desktop_long", s_desktop_long, None),
    ("rest_live_then_flag_removed", s_rest_live_then_flag_removed, None),
    ("rest_pid_dies_flag_stays", s_rest_pid_dies_flag_stays, None),
    ("rest_starts_while_held", s_rest_starts_while_held, None),
    ("dead_pid", s_dead_pid, None),
    ("junk_flag", s_junk_flag, None),
    ("zombie_pid", s_zombie_pid, "intended: a zombie is not resting (Python counted /proc/PID existing)"),
    ("grace_from_rest_end", s_grace_from_rest_end,
     "intended: the 2.5 s grace counts from the rest's end (Python: from the last event seen while resting)"),
]


def main() -> int:
    script, binary = sys.argv[1:3]
    only = sys.argv[3:]
    open(LOG, "w").close()
    unflag()
    py = subprocess.Popen(["python3", "-u", script], stdout=open("/tmp/out/pb-python.log", "w"),
                          stderr=subprocess.STDOUT)
    cpp = subprocess.Popen([binary, "--system", "--only", "power-button"],
                           stderr=open("/tmp/out/pb-cpp.log", "w"))
    time.sleep(0.5)
    make_key()
    time.sleep(2.6)  # the Python daemon's retry
    for name, fn, _ in SCENARIOS:
        if only and name not in only:
            continue
        mark(name)
        fn()
    time.sleep(0.5)
    for p in (py, cpp):
        p.terminate()
        p.wait(timeout=5)
    fcntl.ioctl(ui, UI_DEV_DESTROY)

    got: dict[str, dict[str, list[str]]] = {}
    cur = None
    for line in open(LOG):
        line = line.rstrip("\n")
        if line.startswith("== "):
            cur = got.setdefault(line[3:], {"python3": [], "pbos-platformd": []})
            continue
        caller, _, rest = line.partition(" ")
        if cur is not None:
            cur.setdefault(caller, []).append(rest)
    rc = 0
    for name, _, expected in SCENARIOS:
        if name not in got:
            continue
        a, b = got[name]["python3"], got[name]["pbos-platformd"]
        if a == b:
            print(f"{name}: SAME {a or '(nothing run)'}")
            if expected:
                rc = 1
        else:
            print(f"{name}: DIFF python {a or '(nothing)'} / cpp {b or '(nothing)'}" +
                  (f" -- {expected}" if expected else ""))
            if not expected:
                rc = 1
    with open("/tmp/out/pb-python.log") as fh:
        print("python:", " | ".join(l.strip() for l in fh if "watch" in l))
    with open("/tmp/out/pb-cpp.log") as fh:
        print("cpp:", " | ".join(l.strip() for l in fh if "watch" in l or "waiting" in l))
    return rc


if __name__ == "__main__":
    sys.exit(main())

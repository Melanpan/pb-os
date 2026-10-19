#!/usr/bin/env python3
"""One konkr-standby run (Python or C++) inside the fake root of run.sh.

  scenario.py py|cpp CASE     CASE: key, usr1, freeze-timeout, no-key

/sys, /run, /dev/input and /proc/self/cgroup are fakes; the power key is a
FIFO posing as /dev/input/event5; the game tree is fake processes in this
pid namespace. A thread plays the kernel's freezer: a cgroup.freeze of 1
turns cgroup.events to "frozen 1" 150 ms later (never for app.slice in
freeze-timeout). Writes, kills and commands go to /tmp/ctl/log; the result
(normalized) to /tmp/out/CASE.IMPL.json.
"""
import json
import os
import struct
import subprocess
import sys
import threading
import time

IMPL, CASE = sys.argv[1], sys.argv[2]
LOG = "/tmp/ctl/log"
EV = struct.Struct("llHHi")
CG = "/sys/fs/cgroup/user.slice/user-1000.slice"


def put(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)


def build_sys():
    put("/sys/class/input/event2/device/name", "gpio-keys\n")
    if CASE != "no-key":
        put("/sys/class/input/event5/device/name", "pmic_pwrkey\n")
        os.mkfifo("/dev/input/event5")
    put("/sys/class/backlight/ae94000.dsi.0/brightness", "1200\n")
    put("/sys/class/backlight/aux-bl/brightness", "40\n")
    put("/sys/class/backlight/off-bl/brightness", "\n")           # empty: not restored
    put("/sys/class/leds/konkr:rgb:joysticks/brightness", "255\n")
    put("/sys/class/leds/konkr:power-led/brightness", "128\n")
    put("/sys/class/leds/mmc0::/brightness", "7\n")               # not ours
    caps = {0: 325, 1: 325, 2: 1024, 3: 1024, 4: 1024, 5: 1024, 6: 1024, 7: 1024}
    caps.update({2: 870, 3: 870, 4: 870, 5: 870, 6: 870})
    for n, c in caps.items():
        put(f"/sys/devices/system/cpu/cpu{n}/cpu_capacity", f"{c}\n")
        put(f"/sys/devices/system/cpu/cpu{n}/online", "1\n")
    os.makedirs("/sys/devices/system/cpu/cpuidle", exist_ok=True)
    for pol, (lo, mx) in {"policy0": (364800, 2265600), "policy2": (499200, 3187200),
                          "policy7": (480000, 3302400)}.items():
        put(f"/sys/devices/system/cpu/cpufreq/{pol}/cpuinfo_min_freq", f"{lo}\n")
        put(f"/sys/devices/system/cpu/cpufreq/{pol}/scaling_min_freq", "1200000\n")
        put(f"/sys/devices/system/cpu/cpufreq/{pol}/scaling_max_freq", f"{mx}\n")
    put("/sys/class/devfreq/3d00000.gpu/max_freq", "903000000\n")
    put("/sys/class/devfreq/3d00000.gpu/available_frequencies",
        "903000000 834000000 0680000000 231000000 x\n")
    put("/sys/class/devfreq/soc:qcom,cpu-llcc/max_freq", "1000\n")
    units = ["session-2.scope", "session-7.scope",
             "user@1000.service/app.slice", "user@1000.service/init.scope",
             "user@1000.service/background.slice", "user@1000.service/pipewire.service",
             "user@1000.service/session.slice/pipewire.service",
             "user@1000.service/session.slice/pipewire-pulse.service",
             "user@1000.service/session.slice/wireplumber.service",
             "user@1000.service/session.slice/dbus-broker.service",
             "user@1000.service/session.slice/steam-launcher.service",
             "user@1000.service/session.slice/gamescope-session.service"]
    for u in units:
        put(f"{CG}/{u}/cgroup.freeze", "0\n")
        put(f"{CG}/{u}/cgroup.events", "populated 1\nfrozen 0\n")
    put(f"{CG}/user@1000.service/cgroup.procs", "")
    put(f"{CG}/cgroup.procs", "")
    # The program's own cgroup: inside session-2.scope (never frozen).
    put("/tmp/ctl/cgroup", "0::/user.slice/user-1000.slice/session-2.scope/sub\n")


def snapshot():
    out = {}
    for root, _dirs, files in os.walk("/sys"):
        for f in files:
            p = os.path.join(root, f)
            if f in ("name", "available_frequencies", "cpu_capacity", "cgroup.procs"):
                continue
            with open(p) as fh:
                out[p] = fh.read().strip()
    return out


def freezer(stop):
    """The kernel's freezer, as seen through cgroup.events."""
    due = {}
    while not stop.is_set():
        for root, _dirs, files in os.walk(CG):
            if "cgroup.freeze" not in files:
                continue
            want = open(f"{root}/cgroup.freeze").read().strip() == "1"
            ev = f"{root}/cgroup.events"
            now = "frozen 1" in open(ev).read()
            if want == now:
                due.pop(root, None)
                continue
            if want and CASE == "freeze-timeout" and root.endswith("/app.slice"):
                continue
            t = due.setdefault(root, time.monotonic() + (0.15 if want else 0))
            if time.monotonic() >= t:
                with open(ev, "w") as fh:                 # in place: IN_MODIFY
                    fh.write(f"populated 1\nfrozen {1 if want else 0}\n")
                due.pop(root, None)
        time.sleep(0.01)


def state(pid):
    try:
        st = open(f"/proc/{pid}/stat").read()
        return st[st.rfind(")") + 2]
    except OSError:
        return "-"


def kids(pid):
    out = []
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                st = open(f"/proc/{p}/stat").read()
            except OSError:
                continue
            if int(st[st.rfind(")") + 2:].split()[1]) == pid:
                out.append(int(p))
    return out


def procs_tree():
    t = {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            cmd = open(f"/proc/{p}/cmdline", "rb").read().split(b"\0")[0].decode()
        except OSError:
            continue
        if os.path.basename(cmd) in ("reaper", "game", "other"):
            t[f"{os.path.basename(cmd)}:{p}"] = state(p)
    return dict(sorted(t.items()))


def ev(fd, code, val):
    os.write(fd, EV.pack(0, 0, 1, code, val) + EV.pack(0, 0, 0, 0, 0))


def main():
    build_sys()
    reaper = subprocess.Popen(["/tmp/t/reaper", "SteamLaunch", "AppId=1", "--", "/tmp/t/game", "2"])
    other = subprocess.Popen(["/tmp/t/other"])
    time.sleep(0.3)
    kfd = os.open("/dev/input/event5", os.O_RDWR) if CASE != "no-key" else None
    stop = threading.Event()
    threading.Thread(target=freezer, args=(stop,), daemon=True).start()
    before = snapshot()
    prog = (["python3", "/tmp/t/konkr-standby.py"] if IMPL == "py"
            else ["/tmp/t/pbos-platformd", "konkr-standby"])
    env = dict(os.environ, KSTANDBY_LOG=LOG, LD_PRELOAD="/tmp/t/wlog.so",
               PATH="/tmp/t/bin:/usr/bin")
    out = open("/tmp/ctl/stdout", "w")
    t0 = time.monotonic()
    p = subprocess.Popen(["sh", "-c", 'mount --bind /tmp/ctl/cgroup /proc/$$/cgroup && exec "$@"', "sh"] + prog,
                         stdout=out, stderr=subprocess.PIPE, env=env)
    res = {"case": CASE, "impl": IMPL}
    if kfd is not None:
        ev(kfd, 116, 1)        # the press that triggered sleep...
        time.sleep(0.2)
        ev(kfd, 116, 0)        # ...and its release, during the drain
    t_standby = None
    while p.poll() is None:
        if "standby:" in open("/tmp/ctl/stdout").read():
            t_standby = time.monotonic()
            break
        time.sleep(0.005)
    if t_standby is not None:
        res["during"] = {"flag": os.path.exists("/run/konkr-standby"), "procs": procs_tree(),
                         "sys": {k: v for k, v in snapshot().items() if before.get(k) != v}}
        time.sleep(0.3)
        t_wake = time.monotonic()
        if CASE == "usr1":
            os.kill(p.pid, 10)
        else:
            ev(kfd, 115, 1)        # another key: no wake
            ev(kfd, 116, 0)        # a release without a press: no wake
            time.sleep(0.2)
            ev(kfd, 116, 1)
            time.sleep(0.1)
            ev(kfd, 116, 0)
            t_wake = time.monotonic()
    err = p.communicate(timeout=30)[1].decode()
    t_exit = time.monotonic()
    stop.set()
    res["exit"] = p.returncode
    res["stdout"] = open("/tmp/ctl/stdout").read().splitlines()
    res["stderr"] = err.splitlines()
    res["flag_after"] = os.path.exists("/run/konkr-standby")
    res["procs_after"] = procs_tree()
    after = snapshot()
    res["sys_after"] = {k: (before.get(k), v) for k, v in after.items() if before.get(k) != v}
    lines = open(LOG).read().splitlines() if os.path.exists(LOG) else []
    res["log"] = [line.split("\t", 1)[1] for line in lines]
    ts = [float(line.split("\t", 1)[0]) for line in lines if not line.split("\t", 1)[1].startswith(("konkr-sleep", "systemctl"))]
    res["timing"] = {
        "first_action_s": round(ts[0] - t0, 4) if ts else None,
        "to_standby_line_s": round(t_standby - t0, 4) if t_standby else None,
        "wake_to_exit_s": round(t_exit - t_wake, 4) if t_standby else None,
        "total_s": round(t_exit - t0, 4),
    }
    for pr in (reaper, other):
        os.kill(pr.pid, 18)
    for c in kids(reaper.pid):
        os.kill(c, 9)
    reaper.kill()
    other.kill()
    with open(f"/tmp/out/{CASE}.{IMPL}.json", "w") as fh:
        json.dump(res, fh, indent=1)
    print(json.dumps(res["timing"]), "exit", res["exit"])


main()

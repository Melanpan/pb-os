#!/usr/bin/env python3
"""One charge-rest run (the Python script or pbos-platformd) inside the fake
root of run.sh.

  scenario.py py|cpp CASE

/sys, /run, /dev/input, /var/lib/pbos-utils and /usr/lib/konkr are fakes:
plain files and folders built here for the case's device. The power key
and the lid are FIFOs posing as /dev/input/event0 and event1; the HTR3212s'
/dev/i2c-2 is a plain file (wlog.so fakes the ioctl and logs the bytes);
the game tree is fake processes in this pid namespace; uevents are sent on
this network namespace's own netlink (nothing reaches the VM's udev). A
thread plays the kernel's freezer (cgroup.freeze 1 -> "frozen 1" in
cgroup.events 20 ms later), one the KONKR MCU's LED (its class folder
appears after a bind, goes after an unbind).

systemd-sleep, systemctl, konkr-suspend and konkr-sleep-mode are stubs
that log their argv; the case's hook plays what a sleep did (a doorbell, a
charger, a key). Nothing here suspends, freezes a real cgroup, takes a CPU
offline or makes an input device.

CREST_VCLOCK=1 (default): vclock.so runs the program's clock (the pulse
frames must match); 0: real time, for the timing numbers.
The result goes to /tmp/out/CASE.IMPL[.real].json.
"""
import json
import os
import resource
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

IMPL, CASE = sys.argv[1], sys.argv[2]
VCLOCK = os.environ.get("CREST_VCLOCK", "1") == "1"
SCALE = os.environ.get("CREST_VSCALE", "0.2")
LOG = "/tmp/ctl/log"
EV = struct.Struct("llHHi")
CG = "/sys/fs/cgroup/user.slice/user-1000.slice"
USB = "/sys/class/power_supply/qcom-battmgr-usb"
BAT = "/sys/class/power_supply/battery"
SETTINGS = {"solid_color": "#00FF00", "solid_level": 30, "on_s": 2.5, "off_s": 2,
            "pulse_max": 60, "pulse_min": 3, "pulse_color": "8c00ff"}

# dev: nova / thor (SM8550, stick zones), fit (KONKR Pocket FIT, its MCU),
# s2 (AYANEO Pocket S2: neither). Hooks: what systemd-sleep's Nth call did.
CASES = {
    "exec-konkr": dict(dev="s2", plugged=True),
    "exec-sleep-battery": dict(dev="nova", plugged=False, ipcc=False),
    "fit-standby": dict(dev="fit", plugged=True, mode="standby"),
    "watch-wake": dict(dev="nova", plugged=False),
    "watch-keyfifo": dict(dev="thor", plugged=False, hook="key"),
    "doorbells-key": dict(dev="nova", plugged=False, hook="doorbells"),
    "doorbell-plug-full-key": dict(dev="thor", plugged=False, hook="plug"),
    "rest-unplug-sleep": dict(dev="nova", plugged=True),
    "rest-replug-usr1": dict(dev="nova", plugged=True),
    "rest-lid": dict(dev="thor", plugged=True),
    "rest-full-usr1": dict(dev="nova", plugged=True, held=True),
    "rest-konkr-key": dict(dev="fit", plugged=True, mode="kernel"),
    "rest-konkr-bound": dict(dev="fit", plugged=True, mode="kernel", led=True),
    "rest-no-htr": dict(dev="nova", plugged=True, htr=False),
    "rest-no-i2c": dict(dev="nova", plugged=True, i2c=False),
    "rest-disabled": dict(dev="nova", plugged=True, settings={"enabled": False}),
    "rest-sigterm": dict(dev="nova", plugged=True),
    "rest-sigint": dict(dev="nova", plugged=True),
    "freeze-timeout": dict(dev="nova", plugged=True, stuck=True),
    "preview": dict(dev="nova", cmd="preview"),
    "preview-term": dict(dev="nova", cmd="preview"),
    "preview-nolights": dict(dev="s2", cmd="preview"),
}
C = CASES[CASE]


def put(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(text)


def link(path, target):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    os.makedirs(target, exist_ok=True)
    os.symlink(target, path)


def zone(name, driver, index, inten="10 20 30", br="128"):
    d = f"/sys/class/leds/{name}"
    link(f"{d}/device/driver", f"/sys/bus/platform/drivers/{driver}")
    put(f"{d}/multi_intensity", inten + "\n")
    put(f"{d}/brightness", br + "\n")
    put(f"{d}/max_brightness", "255\n")
    if index:
        put(f"{d}/multi_index", index + "\n")


def build_sys():
    dev = C["dev"]
    compat = {"nova": "retroidpocket,rpnova\0qcom,sm8550\0", "thor": "ayn,thor\0qcom,sm8550\0",
              "fit": "konkr,pocket-fit\0qcom,sm8650\0", "s2": "ayaneo,pocket-s2\0qcom,sm8650\0"}[dev]
    model = {"nova": "Retroid Pocket Nova", "thor": "AYN Thor", "fit": "KONKR Pocket FIT",
             "s2": "AYANEO Pocket S2"}[dev]
    put("/sys/firmware/devicetree/base/compatible", compat)
    put("/sys/firmware/devicetree/base/model", model + "\0")
    put(f"{USB}/online", "1\n" if C.get("plugged") else "0\n")
    put(f"{BAT}/status", "Charging\n" if C.get("plugged") else "Discharging\n")
    put(f"{BAT}/capacity", "57\n")
    put("/sys/power/mem_sleep", "s2idle [deep]\n")
    if C.get("ipcc", True):
        put("/sys/class/wakeup/wakeup3/name", "ipcc-wake\n")
        put("/sys/class/wakeup/wakeup3/event_count", "5\n")
    put("/sys/class/wakeup/wakeup7/name", "c440000.spmi:pmic@0:pon:pwrkey\n")
    put("/sys/class/wakeup/wakeup7/event_count", "2\n")
    put("/sys/class/wakeup/wakeup1/name", "alarmtimer.0.auto\n")
    put("/sys/class/wakeup/wakeup1/event_count", "9\n")
    put("/sys/module/qcom_ipcc/parameters/deep_wake", "Y\n")
    if dev in ("nova", "thor"):
        zone("rgb:l1", "leds_group_multicolor", "red green blue")
        zone("rgb:r1", "leds_group_multicolor", "blue green red", "40 50 60", "200")
        zone("rgb:zz", "some_other_driver", "red green blue")
        zone("left-joystick", "leds_pwm_multicolor", None, "1 2 3", "0")
        if C.get("htr", True):
            for d in ("2-003c", "2-003f"):
                os.makedirs(f"/sys/bus/i2c/drivers/htr3212/{d}", exist_ok=True)
            os.makedirs("/sys/bus/i2c/drivers/htr3212/module", exist_ok=True)
            if C.get("i2c", True):
                put("/dev/i2c-2", "")
    put("/sys/class/leds/mmc0::/brightness", "7\n")
    if dev == "fit":
        put("/run/konkr-sleep-mcu", "serial0-0\n")
        put("/sys/bus/serial/drivers/konkr_sysbtn/bind", "")
        put("/sys/bus/serial/drivers/konkr_sysbtn/unbind", "")
        if C.get("led"):
            make_led()
    put("/sys/class/backlight/ae94000.dsi.0/bl_power", "0\n")
    put("/sys/class/backlight/ae94000.dsi.0/brightness", "1200\n")
    if dev == "thor":
        put("/sys/class/backlight/ae96000.dsi.0/bl_power", "0\n")
        put("/sys/class/backlight/ae96000.dsi.0/brightness", "300\n")
        put("/tmp/ctl/shadow", "77\n")
        subprocess.run(["mount", "--bind", "/tmp/ctl/shadow", "/sys/class/backlight/ae96000.dsi.0/brightness"],
                       check=True)
        put("/sys/class/backlight/aux-off/bl_power", "4\n")
        put("/sys/class/backlight/aux-off/brightness", "0\n")
    put("/sys/class/input/input3/properties", "2\n")
    put("/sys/class/input/input3/inhibited", "0\n")
    put("/sys/class/input/input4/properties", "0\n")
    put("/sys/class/input/input4/inhibited", "0\n")
    put("/sys/class/input/input5/properties", "0x6\n")
    put("/sys/class/input/input5/inhibited", "1\n")
    put("/sys/class/input/input6/properties", "zz\n")
    put("/sys/class/input/event0/device/name", "pmic_pwrkey\n")
    os.mkfifo("/dev/input/event0")
    put("/sys/class/input/event2/device/name", "gpio-keys\n")
    if dev == "thor":
        put("/sys/class/input/event1/device/name", "gpio-keys-lid\n")
        os.mkfifo("/dev/input/event1")
    caps = {0: 400, 1: 400, 2: 400, 3: 870, 4: 870, 5: 870, 6: 870, 7: 1024}
    for n, c in caps.items():
        put(f"/sys/devices/system/cpu/cpu{n}/cpu_capacity", f"{c}\n")
        put(f"/sys/devices/system/cpu/cpu{n}/online", "1\n")
    os.makedirs("/sys/devices/system/cpu/cpuidle", exist_ok=True)
    for pol, (lo, mx) in {"policy0": (307200, 2016000), "policy3": (499200, 2803200),
                          "policy7": (480000, 3302400)}.items():
        put(f"/sys/devices/system/cpu/cpufreq/{pol}/cpuinfo_min_freq", f"{lo}\n")
        put(f"/sys/devices/system/cpu/cpufreq/{pol}/scaling_min_freq", "1200000\n")
        put(f"/sys/devices/system/cpu/cpufreq/{pol}/scaling_max_freq", f"{mx}\n")
    put("/sys/class/devfreq/3d00000.gpu/max_freq", "903000000\n")
    put("/sys/class/devfreq/3d00000.gpu/available_frequencies", "903000000 834000000 0231000000 x 680000000\n")
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
    s = dict(SETTINGS)
    s.update(C.get("settings", {}))
    put("/var/lib/pbos-utils/charging.json", json.dumps(s))
    if C.get("held"):
        put("/run/sm8550-charge-held", f"{os.getpid()}\n")
    os.makedirs("/tmp/ctl", exist_ok=True)
    if "mode" in C:
        put("/tmp/ctl/out-konkr-sleep-mode", C["mode"] + "\n")
    hooks = {
        # systemd-sleep's Nth call: 1-3 a doorbell (ipcc-wake +1), then the power
        # key held back by the kernel (the pon key's wakeup count +1).
        "doorbells": "n <= 3 and bump('/sys/class/wakeup/wakeup3/event_count')\n"
                     "n > 3 and bump('/sys/class/wakeup/wakeup7/event_count')\n",
        # a doorbell with the charger plugged in
        "plug": "n == 1 and bump('/sys/class/wakeup/wakeup3/event_count')\n"
                "n == 1 and open('/sys/class/power_supply/qcom-battmgr-usb/online', 'w').write('1\\n')\n",
        # woken by the power key (its press in the FIFO)
        "key": "fd = os.open('/dev/input/event0', os.O_WRONLY | os.O_NONBLOCK)\n"
               "os.write(fd, struct.pack('llHHi', 0, 0, 1, 116, 1) + struct.pack('llHHi', 0, 0, 0, 0, 0))\n",
    }
    if C.get("hook"):
        put("/tmp/ctl/hook-systemd-sleep",
            "#!/usr/bin/python3\nimport os, struct, sys\nn = int(sys.argv[1])\n"
            "def bump(p):\n    v = int(open(p).read())\n    open(p, 'w').write(f'{v + 1}\\n')\n    return True\n"
            + hooks[C["hook"]])
        os.chmod("/tmp/ctl/hook-systemd-sleep", 0o755)


def make_led():
    d = "/sys/class/leds/konkr:rgb:joysticks"
    put(f"{d}/brightness", "255\n")
    put(f"{d}/multi_intensity", "255 0 128\n")
    put(f"{d}/effect", "rainbow\n")


def snapshot():
    out = {}
    for root, _dirs, files in os.walk("/sys"):
        for f in files:
            p = os.path.join(root, f)
            if f in ("name", "available_frequencies", "cpu_capacity", "cgroup.procs", "multi_intensity"):
                continue
            try:
                with open(p) as fh:
                    out[p] = fh.read().strip()
            except OSError:
                pass
    for p in ("/run/sm8550-charge-rest", "/run/pbos-charge-indicator", "/var/lib/pbos-utils/charging.json"):
        if os.path.exists(p):
            out[p] = open(p).read().strip()
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
            if want and C.get("stuck") and root.endswith("/app.slice"):
                continue
            t = due.setdefault(root, time.monotonic() + (0.02 if want else 0))
            if time.monotonic() >= t:
                with open(ev, "w") as fh:
                    fh.write(f"populated 1\nfrozen {1 if want else 0}\n")
                due.pop(root, None)
        time.sleep(0.005)


def mcu(stop):
    """The KONKR MCU's LED class device: there after a bind, gone after an unbind."""
    base = "/sys/bus/serial/drivers/konkr_sysbtn"
    while not stop.is_set():
        if open(f"{base}/bind").read().strip():
            open(f"{base}/bind", "w").close()
            time.sleep(0.03)
            make_led()
            uevent("leds", "/devices/platform/konkr/leds/konkr:rgb:joysticks", "add")
        if open(f"{base}/unbind").read().strip():
            open(f"{base}/unbind", "w").close()
            for f in ("brightness", "multi_intensity", "effect"):
                os.remove(f"/sys/class/leds/konkr:rgb:joysticks/{f}")
            os.rmdir("/sys/class/leds/konkr:rgb:joysticks")
        time.sleep(0.005)


def uevent(sub, devpath, action="change"):
    s = socket.socket(socket.AF_NETLINK, socket.SOCK_DGRAM, 15)
    s.bind((0, 0))
    msg = f"{action}@{devpath}\0ACTION={action}\0DEVPATH={devpath}\0SUBSYSTEM={sub}\0SEQNUM=1\0"
    s.sendto(msg.encode(), (0, 1))
    s.close()


def state(pid):
    try:
        st = open(f"/proc/{pid}/stat").read()
        return st[st.rfind(")") + 2]
    except OSError:
        return "-"


def procs_tree():
    t = {}
    for p in sorted(os.listdir("/proc"), key=lambda x: int(x) if x.isdigit() else 0):
        if not p.isdigit():
            continue
        try:
            cmd = open(f"/proc/{p}/cmdline", "rb").read().split(b"\0")[0].decode()
        except OSError:
            continue
        if os.path.basename(cmd) in ("reaper", "game", "other"):
            t[f"{os.path.basename(cmd)}:{p}"] = state(p)
    return t


def norm(entry, pid):
    """The flags hold the program's PID."""
    if entry.startswith("W /run/") and entry.endswith(f" = {pid}"):
        return entry[: -len(pid)] + "PID"
    return entry


class Run:
    def __init__(self):
        self.out = "/tmp/ctl/stdout"
        self.fifos = {}

    def lines(self):
        try:
            return open(self.out).read()
        except OSError:
            return ""

    def wait_line(self, text, timeout=30):
        end = time.monotonic() + timeout
        while text not in self.lines():
            if self.p.poll() is not None or time.monotonic() > end:
                raise RuntimeError(f"no line {text!r}")
            time.sleep(0.002)
        return time.monotonic()

    def ev(self, which, typ, code, val):
        os.write(self.fifos[which], EV.pack(0, 0, typ, code, val) + EV.pack(0, 0, 0, 0, 0))

    def key(self, val):
        self.ev("key", 1, 116, val)

    def start(self):
        build_sys()
        self.reaper = subprocess.Popen(["/tmp/t/reaper", "SteamLaunch", "AppId=1", "--", "/tmp/t/game", "2"])
        self.other = subprocess.Popen(["/tmp/t/other"])
        time.sleep(0.2)
        self.fifos["key"] = os.open("/dev/input/event0", os.O_RDWR)
        if os.path.exists("/dev/input/event1"):
            self.fifos["lid"] = os.open("/dev/input/event1", os.O_RDWR)
        self.stop = threading.Event()
        threading.Thread(target=freezer, args=(self.stop,), daemon=True).start()
        if C["dev"] == "fit":
            threading.Thread(target=mcu, args=(self.stop,), daemon=True).start()
        self.before = snapshot()
        cmd = C.get("cmd", "suspend")
        prog = (["python3", "/tmp/t/sm8550-charge-rest", cmd] if IMPL == "py"
                else ["/tmp/t/pbos-platformd", "charge-rest", cmd])
        pre = "/tmp/t/wlog.so" + (" /tmp/t/vclock.so" if VCLOCK else "")
        env = dict(os.environ, CREST_LOG=LOG, LD_PRELOAD=pre, CREST_VSCALE=SCALE,
                   PATH="/tmp/t/bin:/usr/bin", LANG="en_US.UTF-8")
        self.ru0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        self.t0 = time.monotonic()
        self.p = subprocess.Popen(prog, stdout=open(self.out, "w"), stderr=subprocess.PIPE, env=env)

    def finish(self, res):
        try:
            err = self.p.communicate(timeout=60)[1].decode()
        except subprocess.TimeoutExpired:
            self.p.kill()
            err = self.p.communicate()[1].decode() + "\n(killed: timeout)"
        self.t_exit = time.monotonic()
        ru = resource.getrusage(resource.RUSAGE_CHILDREN)
        self.stop.set()
        pid = str(self.p.pid)
        res["exit"] = self.p.returncode
        res["stdout"] = self.lines().splitlines()
        res["stderr"] = [line for line in err.splitlines() if line]
        if any(line.startswith("Traceback") for line in res["stderr"]):
            res["stderr"] = res["stderr"][-1:]  # Python's traceback: its last line
        res["procs_after"] = procs_tree()
        after = snapshot()
        res["sys_after"] = {k: (self.before.get(k), v) for k, v in after.items() if self.before.get(k) != v}
        lines = open(LOG).read().splitlines() if os.path.exists(LOG) else []
        res["log"] = [norm(line.split("\t", 1)[1], pid) for line in lines]
        stamps = [float(line.split("\t", 1)[0]) for line in lines if not line.startswith("-")]
        res["timing"] = {"total_s": round(self.t_exit - self.t0, 4),
                         "first_action_s": round(stamps[0] - self.t0, 4) if stamps else None,
                         "cpu_s": round(ru.ru_utime + ru.ru_stime - self.ru0.ru_utime - self.ru0.ru_stime, 4)}
        for k, v in getattr(self, "marks", {}).items():
            res["timing"][k] = round(v, 4)
        for pr in (self.reaper, self.other):
            os.kill(pr.pid, signal.SIGCONT)
        for p in os.listdir("/proc"):
            if p.isdigit() and int(p) not in (os.getpid(), 1):
                try:
                    if os.path.basename(open(f"/proc/{p}/cmdline", "rb").read().split(b"\0")[0].decode()) \
                            in ("reaper", "game", "other"):
                        os.kill(int(p), signal.SIGKILL)
                except OSError:
                    pass

    def snap(self, res, name):
        pid = str(self.p.pid)
        now = snapshot()
        res[name] = {"flag": now.get("/run/sm8550-charge-rest", "").replace(pid, "PID"),
                     "indicator": now.get("/run/pbos-charge-indicator", "").replace(pid, "PID"),
                     "procs": procs_tree(),
                     "sys": {k: v for k, v in now.items() if self.before.get(k) != v and not k.startswith("/run/")}}


def drive(r, res):
    """The case's events, while the program runs."""
    r.marks = {}
    if CASE in ("exec-konkr", "exec-sleep-battery", "fit-standby", "watch-wake", "watch-keyfifo",
                "doorbells-key", "preview", "preview-nolights"):
        return
    if CASE == "preview-term":
        time.sleep(0.6)
        r.p.send_signal(signal.SIGTERM)
        return
    if CASE == "doorbell-plug-full-key":
        r.wait_line("charger plugged in")
        time.sleep(1.5)
        r.snap(res, "resting")
        put(f"{BAT}/status", "Full\n")
        uevent("power_supply", "/devices/platform/battery/power_supply/battery")
        time.sleep(0.6)
        r.key(1)
        time.sleep(0.1)
        r.key(0)
        return
    resting = "stick lights: mcu" if C["dev"] == "fit" else "resting on the charger"
    r.wait_line(resting)
    time.sleep(1.2)
    r.snap(res, "resting")
    t = time.monotonic()
    if CASE == "rest-unplug-sleep":
        put(f"{USB}/online", "0\n")
        uevent("power_supply", "/devices/platform/usb/power_supply/qcom-battmgr-usb")
        r.wait_line("charger pulled")
        return
    if CASE == "rest-replug-usr1":
        put(f"{USB}/online", "0\n")
        put(f"{BAT}/status", "Discharging\n")
        uevent("power_supply", "/devices/platform/usb/power_supply/qcom-battmgr-usb")
        r.wait_line("charger unplug event")
        put(f"{USB}/online", "1\n")
        put(f"{BAT}/status", "Charging\n")
        uevent("typec", "/devices/platform/typec/port0")
        time.sleep(1.0)
        if "charger pulled" in r.lines():
            res["replug_too_late"] = True
    if CASE == "rest-lid":
        r.ev("lid", 5, 0, 1)          # closed: nothing
        r.key(0)                      # a release without its press: nothing
        time.sleep(0.3)
        t = time.monotonic()
        r.ev("lid", 5, 0, 0)
    elif CASE in ("rest-konkr-key", "rest-konkr-bound"):
        r.ev("key", 1, 115, 1)        # another key: nothing
        r.key(1)
        time.sleep(0.1)
        t = time.monotonic()
        r.key(0)
    elif CASE == "rest-sigterm":
        r.p.send_signal(signal.SIGTERM)
    elif CASE == "rest-sigint":
        r.p.send_signal(signal.SIGINT)
    else:
        r.p.send_signal(signal.SIGUSR1)
    r.marks["wake_sent_s"] = t - r.t0
    r.wake_at = t


def main():
    r = Run()
    res = {"case": CASE, "impl": IMPL}
    r.start()
    try:
        drive(r, res)
    except RuntimeError as e:
        res["driver_error"] = str(e)
    r.finish(res)
    if hasattr(r, "wake_at"):
        res["timing"]["wake_to_exit_s"] = round(r.t_exit - r.wake_at, 4)
    name = f"/tmp/out/{CASE}.{IMPL}{'' if VCLOCK else '.real'}.json"
    with open(name, "w") as fh:
        json.dump(res, fh, indent=1)
    print(CASE, IMPL, "exit", res["exit"], json.dumps(res["timing"]))


main()

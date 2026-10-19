#!/usr/bin/env python3
"""Off-device equivalence test: the Python sm8550-thor-controlsd and
pbos-platformd's thor-controls module over the same scenarios: fake stick
LEDs (/sys/class/leds/rgb:*), fan configs and saved state, the same socket
requests. Compares each reply line byte for byte, then every file the
daemon may write (the LEDs, /var/lib/steamos-sm8550, the systemctl stub's
log) after each request.

  equivalence.py run IMPL OUT.json [SCENARIO...]   one implementation (in the sandbox)
  equivalence.py compare PY.json CPP.json          diff the results

IMPL: py (/tmp/t/sm8550-thor-controlsd), cpp (/tmp/t/pbos-platformd
--system --only thor-controls).
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import time

SOCK = "/run/sm8550-thor/controls.sock"
LEDS = "/sys/class/leds"
VAR = "/var/lib/steamos-sm8550"
LOG = "/tmp/t/systemctl.log"
SETTLE = 0.3  # after a set: the C++ side's systemctl runs on a thread of its own


def w(path: str, value) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(str(value))


def led(name: str, top=255, index="red green blue", intensity="0 0 0", level=0, multi=True) -> None:
    d = f"{LEDS}/{name}"
    os.makedirs(d, exist_ok=True)
    w(f"{d}/max_brightness", f"{top}\n")
    w(f"{d}/brightness", f"{level}\n")
    w(f"{d}/multi_index", f"{index}\n")
    if multi:
        w(f"{d}/multi_intensity", f"{intensity}\n")


def thor_leds(**kw) -> None:
    for side in "lr":
        for i in range(1, 5):
            led(f"rgb:{side}{i}", **kw)


def reset() -> None:
    for d in (LEDS, VAR, "/usr/share/sm8550-fand", "/run/sm8550-thor"):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d, exist_ok=True)
    for f in ("/etc/sm8550-fand.conf", LOG):
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass
    shutil.copy("/tmp/t/fan.conf", "/usr/share/sm8550-fand/fan.conf")
    w("/sys/firmware/devicetree/base/compatible", "ayn,thor\0qcom,sm8550\0")


def snapshot() -> dict:
    out = {}
    for root in (LEDS, VAR):
        for dirpath, _, files in os.walk(root):
            for f in sorted(files):
                p = os.path.join(dirpath, f)
                with open(p, "rb") as fh:
                    out[p] = fh.read().decode(errors="replace")
    try:
        with open(LOG) as fh:
            out[LOG] = fh.read()
    except FileNotFoundError:
        pass
    return out


def ask(raw: bytes, shut: bool = False, timeout: float = 5) -> str:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    if raw:
        s.sendall(raw)
    if shut:
        s.shutdown(socket.SHUT_WR)
    data = b""
    try:
        while not data.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except OSError as err:
        data += f"<{err}>".encode()
    s.close()
    return data.decode(errors="replace")


def req(obj) -> bytes:
    return json.dumps(obj).encode() + b"\n"


ALL_SETS = [
    {"op": "get"},
    {"op": "set", "lighting": {"color": "FF0000"}},
    {"op": "set", "lighting": {"brightness": 0}},
    {"op": "set", "lighting": {"brightness": 75, "enabled": False}},
    {"op": "set", "lighting": {"enabled": True, "dimmer": 20}},
    {"op": "set", "lighting": {"dimmer": 10}},
    {"op": "set", "lighting": {"brightness": 50.7, "dimmer": 99.9}},
    {"op": "set", "lighting": {"brightness": True}},
    {"op": "set", "lighting": {"color": "zzzzzz"}},
    {"op": "set", "lighting": {"color": "12345"}},
    {"op": "set", "lighting": {"foo": 1, "color": "00ff00", "bar": None}},
    {"op": "set", "lighting": {"été": 1, "it's": 2}},
    {"op": "set", "lighting": {"enabled": 1}},
    {"op": "set", "lighting": {"brightness": 100, "dimmer": 100, "color": "A1b2C3"}},
    {"op": "set", "lighting": {"brightness": 1, "dimmer": 20}},
    {"op": "set", "lighting": {}},
    {"op": "set", "lighting": [1]},
    {"op": "set", "fanProfile": "Performance"},
    {"op": "set", "fanProfile": "nope"},
    {"op": "set", "fanProfile": "it's \"x\""},
    {"op": "set", "fanProfile": 3},
    {"op": "set", "fanProfile": "eco", "lighting": {"color": "0000ff"}},
    {"op": "set", "fanProfile": "bad", "lighting": {"color": "ffffff"}},
    {"op": "set"},
    {"op": "bogus"},
    {"op": None},
    {},
    {"fanProfile": "max"},
    [1, 2],
    "get",
    {"op": "get"},
]

# Each scenario: (setup function, [requests]); a request is an object (sent
# as one JSON line), or ("raw", bytes, shutdown_write).
SCENARIOS = {
    "fresh": (lambda: thor_leds(), ALL_SETS),
    "saved_partial": (lambda: (thor_leds(), w(f"{VAR}/thor-lighting.json",
                                              '{"color": "00FF00", "brightness": 200, "dimmer": 30, "x": 1}')),
                      [{"op": "get"}, {"op": "set", "lighting": {"brightness": 40}}, {"op": "get"}]),
    "saved_bad_json": (lambda: (thor_leds(intensity="255 0 128", level=100),
                                w(f"{VAR}/thor-lighting.json", "{not json")),
                       [{"op": "get"}, {"op": "set", "lighting": {"dimmer": 50}}]),
    "saved_not_object": (lambda: (thor_leds(intensity="10 20 30", level=255), w(f"{VAR}/thor-lighting.json", "[1]")),
                         [{"op": "get"}]),
    "describe_off": (lambda: (thor_leds(), w(f"{VAR}/thor-lighting.json", "")), [{"op": "get"}]),
    "describe_1000": (lambda: (thor_leds(top=1000, index="green blue red", intensity="1000 0 500", level=333),
                               w(f"{VAR}/thor-lighting.json", "x")),
                      [{"op": "get"}, {"op": "set", "lighting": {"color": "8c00ff"}}, {"op": "get"}]),
    "no_leds": (lambda: (led("white:status", multi=False), led("rgb:nomulti", multi=False)),
                [{"op": "get"}, {"op": "set", "lighting": {"color": "ff0000"}}, {"op": "set", "fanProfile": "max"}]),
    "fan_configs": (lambda: (thor_leds(),
                             w("/etc/sm8550-fand.conf",
                               "# user\n[general]\nprofile = Max \n\n[profile.custom]\nfan_curve = full\n"
                               "[profile.eco]\nfan_curve=relaxed\n"),
                             w(f"{VAR}/fan-profile.conf", "[profile.quick]\nfan_curve: full\n")),
                    [{"op": "get"}, {"op": "set", "fanProfile": "custom"}, {"op": "get"},
                     {"op": "set", "fanProfile": "quick"}, {"op": "get"}]),
    "fan_default_section": (lambda: (thor_leds(), w("/etc/sm8550-fand.conf", "[DEFAULT]\nprofile=eco\n")),
                            [{"op": "get"}]),
    "raw": (lambda: thor_leds(),
            [("raw", b"\n", False), ("raw", b"", True), ("raw", b'{"op": "get"}', True),
             ("raw", b'  {"op":"get"}  \r\n', False), ("raw", b"   \n", False),
             ("raw", b'{"op": "set", "lighting": {"color": "\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9"}}\n', False)]),
    # Not JSON: both answer {"error": ...}, Python's texts are its json
    # module's (not copied); see INTENTIONAL.
    "bad_json": (lambda: thor_leds(),
                 [("raw", b"{not json}\n", False), ("raw", b'{"op": "get"}\n{"op": "x"}\n', False),
                  ("raw", b"{" + b" " * 5000 + b'"op": "get"}\n', False)]),
    "timeout": (lambda: thor_leds(), [("raw", b"", False), ("raw", b'{"op"', False)]),
}


# Scenarios whose replies only have to agree on being errors.
INTENTIONAL = {"bad_json"}


def start(impl: str) -> subprocess.Popen:
    if impl == "py":
        cmd = ["python3", "/tmp/t/sm8550-thor-controlsd"]
    else:
        cmd = ["/tmp/t/pbos-platformd", "--system", "--only", "thor-controls"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 5
    while not os.path.exists(SOCK) and time.monotonic() < deadline:
        time.sleep(0.02)
    time.sleep(0.1)
    return p


def run(impl: str, out_path: str, only: list[str]) -> int:
    results = {}
    for name, (setup, reqs) in SCENARIOS.items():
        if only and name not in only:
            continue
        reset()
        setup()
        p = start(impl)
        st = os.stat(SOCK)
        steps = [("start", "", snapshot())]
        steps.append(("socket", f"{stat.filemode(st.st_mode)} gid={st.st_gid}", {}))
        for r in reqs:
            if isinstance(r, tuple):
                reply = ask(r[1], r[2])
                label = repr(r[1][:60])
            else:
                reply = ask(req(r))
                label = json.dumps(r)
            time.sleep(SETTLE if "set" in label else 0)
            steps.append((label, reply, snapshot()))
        p.terminate()
        try:
            out, _ = p.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            out, _ = p.communicate()
        results[name] = {"steps": steps, "log": out.decode(errors="replace")}
        print(f"{impl} {name}: {len(steps)} steps", flush=True)
    # Two slow clients at once: the C++ side answers the second (a full
    # request) at once while the first is still idle; Python makes it wait.
    if not only or "concurrent" in only:
        reset()
        thor_leds()
        p = start(impl)
        slow = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        slow.connect(SOCK)
        time.sleep(0.05)
        t0 = time.monotonic()
        reply = ask(req({"op": "get"}))
        waited = time.monotonic() - t0
        slow.settimeout(5)
        late = slow.recv(4096).decode()
        slow.close()
        p.terminate()
        p.communicate(timeout=5)
        results["concurrent"] = {"steps": [("get while another client idles", reply, {}),
                                           ("idle client", late, {})], "log": "", "waited": round(waited, 2)}
        print(f"{impl} concurrent: second client answered after {waited:.2f} s", flush=True)
    with open(out_path, "w") as fh:
        json.dump(results, fh, indent=1)
    return 0


def compare(a_path: str, b_path: str) -> int:
    a, b = (json.load(open(p)) for p in (a_path, b_path))
    passed = failed = 0
    for name in a:
        sa, sb = a[name]["steps"], b.get(name, {}).get("steps", [])
        bad = []
        for i, (x, y) in enumerate(zip(sa, sb)):
            if name in INTENTIONAL and x[0] not in ("start", "socket"):
                same = x[1].startswith('{"error": ') and y[1].startswith('{"error": ')
            else:
                same = x[1] == y[1]
            if not same:
                bad.append(f"  step {i} {x[0]}: reply\n    py:  {x[1]!r}\n    cpp: {y[1]!r}")
            if x[2] != y[2]:
                keys = sorted(set(x[2]) | set(y[2]))
                diffs = [f"    {k}: py {x[2].get(k)!r} cpp {y[2].get(k)!r}" for k in keys if x[2].get(k) != y[2].get(k)]
                bad.append(f"  step {i} {x[0]}: files\n" + "\n".join(diffs))
        if len(sa) != len(sb):
            bad.append(f"  {len(sa)} vs {len(sb)} steps")
        n = len(sa)
        if name in INTENTIONAL and not bad:
            print(f"PASS {name} ({n} steps; error texts differ on purpose)")
            for x, y in zip(sa, sb):
                if x[1] != y[1]:
                    print(f"     py {x[1].strip()!r} / cpp {y[1].strip()!r}")
            passed += 1
            continue
        if bad:
            failed += 1
            print(f"FAIL {name} ({n} steps)\n" + "\n".join(bad))
        else:
            passed += 1
            print(f"PASS {name} ({n} steps)")
        if "waited" in a[name]:
            print(f"     second client waited: py {a[name]['waited']} s, cpp {b[name]['waited']} s")
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    if sys.argv[1] == "run":
        sys.exit(run(sys.argv[2], sys.argv[3], sys.argv[4:]))
    sys.exit(compare(sys.argv[2], sys.argv[3]))

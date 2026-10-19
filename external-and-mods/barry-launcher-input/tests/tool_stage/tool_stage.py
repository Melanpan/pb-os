#!/usr/bin/env python3
"""shell/ToolStage.qml with Barry.Input's ShellState, offscreen, against a
stand-in shelld (its event stream socket, SHELLD-EVENTS.md).

Runs inside run.sh's sandbox (a SteamOS rootfs with qml6; the built module
on QML_IMPORT_PATH; ToolStage.qml beside ToolStageTest.qml):

  tool_stage.py <dir with ToolStageTest.qml and ToolStage.qml>

1. In: the first frame has the content a window's height down; it slides
   up to 0 in about 300 ms, eased out (most of the way early).
2. A tool: it says {"tool": true} as it connects.
3. A snapshot naming another pid as leaving: it stays.
4. Its pid leaving: it slides down by the height and fades, eased in (little
   of the way early), and says {"tool_left": true} about 240 ms after the
   ask; it is still up then (shelld ends it), and quits by itself about
   1 s later when no one does.
"""
from __future__ import annotations

import json
import os
import select
import socket
import subprocess
import sys
import threading
import time

RUN = "/tmp/run"
SOCK = f"{RUN}/barry_launcher/shelld.sock"
H = 1080.0

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    print(("PASS " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def line(topic: str, data) -> bytes:
    return json.dumps({"topic": topic, "data": data}).encode() + b"\n"


def snapshot(leaving: dict) -> bytes:
    return (line("home", {"serial": 0, "open": False}) + line("keyboard", {"visible": False})
            + line("top-keyboard", {"serial": 0, "visible": False}) + line("keyd", {}) + line("apps", {})
            + line("tools", {"serial": 1, "leaving": leaving}))


class Qml:
    """The qml6 process; its console lines with the time they came."""

    def __init__(self, folder: str) -> None:
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software", XDG_RUNTIME_DIR=RUN,
                   QT_FORCE_STDERR_LOGGING="1", QT_LOGGING_RULES="qt.qpa.*=false")
        self.proc = subprocess.Popen(["qml6", os.path.join(folder, "ToolStageTest.qml")], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        self.lines: list[tuple[float, str]] = []
        self.lock = threading.Lock()
        threading.Thread(target=self.read, daemon=True).start()

    def read(self) -> None:
        for text in self.proc.stderr:
            text = text.strip()
            with self.lock:
                self.lines.append((time.time(), text))
            if not text.startswith(("frame", "qml: frame")):
                print("  qml:", text, flush=True)

    def frames(self, since: float = 0.0) -> list[tuple[float, float, float]]:
        """(seconds since the epoch, y, opacity) of the frames drawn since."""
        out = []
        with self.lock:
            for _, text in self.lines:
                parts = text.split()
                if parts[:1] == ["qml:"]:
                    parts = parts[1:]
                if len(parts) == 4 and parts[0] == "frame" and int(parts[1]) / 1000 >= since:
                    out.append((int(parts[1]) / 1000, float(parts[2]), float(parts[3])))
        return out


class Server:
    def __init__(self) -> None:
        os.makedirs(os.path.dirname(SOCK), mode=0o700, exist_ok=True)
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(SOCK)
        self.srv.listen(4)

    def accept(self, seconds: float) -> socket.socket | None:
        r, _, _ = select.select([self.srv], [], [], seconds)
        return self.srv.accept()[0] if r else None


def messages(conn: socket.socket, seconds: float, buf: list) -> list[tuple[float, dict]]:
    """The client's lines within seconds, with the time each came."""
    out = []
    end = time.time() + seconds
    while (left := end - time.time()) > 0:
        r, _, _ = select.select([conn], [], [], left)
        if not r:
            break
        data = conn.recv(4096)
        if not data:
            break
        buf[0] += data
        while b"\n" in buf[0]:
            text, buf[0] = buf[0].split(b"\n", 1)
            out.append((time.time(), json.loads(text)))
    return out


def main() -> int:
    folder = sys.argv[1]
    server = Server()
    qml = Qml(folder)
    conn = server.accept(10)
    check(conn is not None, "ShellState connects to the socket")
    if conn is None:
        qml.proc.kill()
        return 1
    buf = [b""]
    # Another process's ask, still in the snapshot: not this one's.
    conn.sendall(snapshot({"trackpad": 1}))
    got = messages(conn, 1.5, buf)
    check(any(m == {"tool": True} for _, m in got), f'says {{"tool": true}} ({[m for _, m in got]})')

    frames = qml.frames()
    check(bool(frames) and abs(frames[0][1] - H) < 1 and frames[0][2] == 1,
          f"first frame: content {H:.0f} px down, opaque ({frames[:1]})")
    settled = [f for f in frames if f[1] == 0]
    if frames and settled:
        took = settled[0][0] - frames[0][0]
        check(0.25 <= took <= 0.5, f"slides up in {took:.3f} s (300 ms animation)")
        # OutCubic: past 70 % of the way within the first third.
        third = [f for f in frames if f[0] - frames[0][0] <= 0.1 + 0.02]
        check(bool(third) and third[-1][1] <= 0.45 * H, f"eased out: at ~100 ms y={third[-1][1] if third else '?'}")
    else:
        check(False, f"slides up to 0 ({frames[-3:]})")
    check(not any("leaving true" in t for _, t in qml.lines), "another pid's ask: stays")

    # Its own ask.
    asked = time.time()
    conn.sendall(line("tools", {"serial": 2, "leaving": {"trackpad": qml.proc.pid}}))
    got = messages(conn, 0.8, buf)
    left = [t for t, m in got if m == {"tool_left": True}]
    check(len(left) == 1, f'one {{"tool_left": true}} ({[m for _, m in got]})')
    if left:
        took = left[0] - asked
        check(0.22 <= took <= 0.4, f"tool_left {took:.3f} s after the ask (240 ms slide)")
        print(f"  slide away: tool_left after {took * 1000:.0f} ms", flush=True)
    out = qml.frames(asked)
    if out:
        last = out[-1]
        check(abs(last[1] - H) < 1 and last[2] == 0, f"ends {H:.0f} px down, transparent ({last})")
        mid = [f for f in out if f[0] - asked <= 0.12 + 0.02]
        check(bool(mid) and mid[-1][1] <= 0.25 * H, f"eased in: at ~120 ms y={mid[-1][1] if mid else '?'}")
        print(f"  frames out: {len(out)}; y " + " ".join(f"{f[1]:.0f}" for f in out), flush=True)
    else:
        check(False, "frames while sliding away")
    check(qml.proc.poll() is None, "still up after tool_left (shelld ends it)")
    try:
        qml.proc.wait(2.0)
        quit_after = time.time() - (left[0] if left else asked)
        check(0.8 <= quit_after <= 1.6, f"quits by itself {quit_after:.2f} s after tool_left (1 s)")
    except subprocess.TimeoutExpired:
        check(False, "quits by itself when not ended")
        qml.proc.kill()
    print("ALL PASS" if not failures else f"{len(failures)} FAILED: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

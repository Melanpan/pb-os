#!/usr/bin/env python3
"""qml6 for shelld_events.py: runs until it is ended, its arguments in ps.

Started as the Trackpad app or the Deck controls (a tool), it plays
ToolStage.qml's part on the event stream as /t/tool-mode says:
  answer  says {"tool": true}; asked to leave (its pid in the tools topic),
          "slides" for TOOL_SLIDE_S, sends {"tool_left": true} and waits to
          be ended
  silent  says {"tool": true}, never answers
  (else)  an older tool: says nothing
It logs what happened, with monotonic times, to /t/tool-<pid>.log:
"asked <t>", "left <t>", "term <t>".
"""
import json
import os
import signal
import socket
import sys
import time

TOOL_SLIDE_S = 0.24
SOCK = "/t/run/barry_launcher/shelld.sock"


def log(what: str) -> None:
    with open(f"/t/tool-{os.getpid()}.log", "a", encoding="utf-8") as fh:
        fh.write(f"{what} {time.monotonic():.4f}\n")


def term(_sig, _frame) -> None:
    log("term")
    os._exit(0)


def main() -> None:
    signal.signal(signal.SIGTERM, term)
    tool = "trackpad" in sys.argv or any(a.endswith("DeckApp.qml") for a in sys.argv)
    try:
        with open("/t/tool-mode", encoding="utf-8") as fh:
            mode = fh.read().strip()
    except OSError:
        mode = ""
    if not tool or mode not in ("answer", "silent"):
        while True:
            time.sleep(1000)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(SOCK)
    sock.sendall(b'{"tool": true}\n')
    log("ready")
    buf = b""
    asked = False
    while True:
        data = sock.recv(65536)
        if not data:
            while True:
                time.sleep(1000)
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            msg = json.loads(line)
            if msg["topic"] != "tools" or asked or os.getpid() not in msg["data"]["leaving"].values():
                continue
            asked = True
            log("asked")
            if mode == "answer":
                time.sleep(TOOL_SLIDE_S)
                sock.sendall(b'{"tool_left": true}\n')
                log("left")


if __name__ == "__main__":
    main()

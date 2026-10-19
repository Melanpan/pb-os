#!/usr/bin/env python3
"""One line per fact of the sound server's state that barry-audio keeps:
the default output, each output's volume and mute, each test stream's
output and volume (by its test.label), then the daemon's state file.

Usage: snapshot.py <state file> <step name>"""
import json
import subprocess
import sys


def pj(what):
    out = subprocess.run(["pactl", "-f", "json", "list", what], capture_output=True, text=True).stdout
    return json.loads(out or "[]")


def raw(vol):
    return "/".join(str(c["value"]) for c in vol.values()) if vol else "-"


sinks = pj("sinks")
names = {s["index"]: s["name"] for s in sinks}
lines = []
for s in sinks:
    lines.append(f"sink {s['name']} {raw(s['volume'])} mute={s['mute']}")
for i in pj("sink-inputs"):
    p = i["properties"]
    label = p.get("test.label") or p.get("node.name")
    lines.append(f"stream {label} on={names.get(i['sink'])} {raw(i['volume'])}")
lines.sort()
default = subprocess.run(["pactl", "get-default-sink"], capture_output=True, text=True).stdout.strip()
lines.insert(0, "default " + default)
try:
    with open(sys.argv[1], encoding="utf-8") as fh:
        lines.append("state " + fh.read())
except OSError:
    lines.append("state -")
print(f"== {sys.argv[2]}")
print("\n".join(lines), flush=True)

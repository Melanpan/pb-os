#!/usr/bin/env python3
"""Compare the Python and C++ results of tests/pbosd's cases.

  compare.py OUTDIR CASE...

Per case: each labeled snapshot (the watched files and the commands run so
far, as a sorted list: the daemons run commands on their own threads), the
sysfs writes per file (repeats in a row dropped: the Python version wrote the
power LED every second, the C++ module when it changes), and the fan after
stopping and after the unit's ExecStopPost. Prints SAME or DIFF per case.

Known, intended differences are reported as such, not as DIFF:
  stop_raw   the Python version wrote failsafe_pwm (180) on SIGTERM and the
             unit's ExecStopPost wrote 255; the module writes 255 itself.
"""
import json
import sys

out, cases = sys.argv[1], sys.argv[2:]
bad = 0
for case in cases:
    try:
        py = json.load(open(f"{out}/{case}-py.json"))
        cpp = json.load(open(f"{out}/{case}-cpp.json"))
    except OSError as exc:
        print(f"{case:16} MISSING ({exc})")
        bad += 1
        continue
    diffs = []
    if len(py["snaps"]) != len(cpp["snaps"]):
        diffs.append(f"snapshot count {len(py['snaps'])} vs {len(cpp['snaps'])}")
    for sp, sc in zip(py["snaps"], cpp["snaps"]):
        for path in sorted(set(sp["files"]) | set(sc["files"])):
            a, b = sp["files"].get(path), sc["files"].get(path)
            if a != b:
                diffs.append(f"[{sp['label']}] {path}: py {a!r} cpp {b!r}")
        if sp["cmds"] != sc["cmds"]:
            pa, pb = set(sp["cmds"]), set(sc["cmds"])
            diffs.append(f"[{sp['label']}] cmds: py only {sorted(pa - pb)} cpp only {sorted(pb - pa)}"
                         f" (counts {len(sp['cmds'])} vs {len(sc['cmds'])})")
    pwm = "/sys/class/hwmon/hwmon1/pwm1"
    if py["stop_raw"] != cpp["stop_raw"] and cpp["stop_raw"].strip() == "255":
        # The stop's own write (the known difference): compared below as stop_raw.
        for r, v in ((py, py["stop_raw"].strip()), (cpp, "255")):
            if r["writes"].get(pwm, [])[-1:] == [v]:
                r["writes"][pwm] = r["writes"][pwm][:-1]
    for path in sorted(set(py["writes"]) | set(cpp["writes"])):
        a, b = py["writes"].get(path, []), cpp["writes"].get(path, [])
        if a != b:
            diffs.append(f"writes {path}:\n      py  {a}\n      cpp {b}")
    if py["stop_final"] != cpp["stop_final"]:
        diffs.append(f"after ExecStopPost: py {py['stop_final']!r} cpp {cpp['stop_final']!r}")
    note = ""
    if py["stop_raw"] != cpp["stop_raw"]:
        note = f" (stop: py {py['stop_raw'].strip()} then ExecStopPost 255; cpp {cpp['stop_raw'].strip()} itself)"
    seen = set()
    # A diff already seen in an earlier snapshot is the same diff carried on.
    shown = []
    for d in diffs:
        key = d.split("] ", 1)[-1]
        if key in seen:
            continue
        seen.add(key)
        shown.append(d)
    print(f"{case:16} {'SAME' if not diffs else 'DIFF'}{note}")
    for d in shown[:40]:
        print("    " + d)
    bad += bool(diffs)
sys.exit(1 if bad else 0)

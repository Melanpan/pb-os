#!/usr/bin/env python3
"""SAME/DIFF of each case's Python and C++ results (timing aside).

The pulse frames (writes of the zones' multi_intensity with a dark middle
channel: the test's pulse color has no green, the saved and the full
colors do) come from the same clock in both (vclock.so), but how many of
them pass before an event lands is up to real time. So each run of them is
compared per zone: one side's values must be the start of the other's.
Everything else must be the same, in the same order.

  compare.py OUTDIR CASE...
"""
import json
import sys


def pulse(entry):
    if not entry.startswith("W ") or "/multi_intensity = " not in entry:
        return None
    path, value = entry[2:].split(" = ", 1)
    parts = value.split()
    return (path, value) if len(parts) == 3 and parts[1] == "0" else None


def skeleton(log):
    out = []
    for e in log:
        p = pulse(e)
        if p is None:
            out.append(e)
            continue
        if not out or not isinstance(out[-1], dict):
            out.append({})
        out[-1].setdefault(p[0], []).append(p[1])
    return out


def same_log(a, b, notes):
    sa, sb = skeleton(a), skeleton(b)
    if len(sa) != len(sb):
        return False
    for x, y in zip(sa, sb):
        if isinstance(x, dict) != isinstance(y, dict):
            return False
        if not isinstance(x, dict):
            if x != y:
                return False
            continue
        if set(x) != set(y):
            return False
        for z in x:
            n = min(len(x[z]), len(y[z]))
            if x[z][:n] != y[z][:n]:
                return False
        z = sorted(x)[0]
        notes.append(f"{len(x[z])}/{len(y[z])}")
    return True


def first_diff(a, b):
    sa, sb = skeleton(a), skeleton(b)
    for i, (x, y) in enumerate(zip(sa, sb)):
        if x != y and not (isinstance(x, dict) and isinstance(y, dict)):
            return f"#{i}: py {x!r} | cpp {y!r}"
        if isinstance(x, dict) and isinstance(y, dict):
            for z in x:
                n = min(len(x[z]), len(y.get(z, [])))
                if x[z][:n] != y.get(z, [])[:n]:
                    k = next(k for k in range(n) if x[z][k] != y[z][k])
                    return f"#{i} pulse {z} frame {k}: py {x[z][k-2:k+3]} | cpp {y[z][k-2:k+3]}"
    return f"lengths {len(sa)} vs {len(sb)}"


def main():
    out, cases = sys.argv[1], sys.argv[2:]
    bad = 0
    for c in cases:
        try:
            r = {i: json.load(open(f"{out}/{c}.{i}.json")) for i in ("py", "cpp")}
        except OSError as e:
            print(f"{c}: MISSING {e}")
            bad += 1
            continue
        t = {i: r[i].pop("timing") for i in r}
        for x in r.values():
            x.pop("impl")
        notes = []
        log_same = same_log(r["py"].pop("log"), r["cpp"].pop("log"), notes)
        rest_same = r["py"] == r["cpp"]
        same = log_same and rest_same
        bad += not same
        frames = f"  pulse frames py/cpp {', '.join(notes)}" if notes else ""
        print(f"{c}: {'SAME' if same else 'DIFF'}{frames}")
        if not log_same:
            a = json.load(open(f"{out}/{c}.py.json"))["log"]
            b = json.load(open(f"{out}/{c}.cpp.json"))["log"]
            print("  log:", first_diff(a, b))
        if not rest_same:
            for k in sorted(set(r["py"]) | set(r["cpp"])):
                if r["py"].get(k) != r["cpp"].get(k):
                    print(f"  {k}:\n    py  {r['py'].get(k)}\n    cpp {r['cpp'].get(k)}")
    sys.exit(1 if bad else 0)


main()

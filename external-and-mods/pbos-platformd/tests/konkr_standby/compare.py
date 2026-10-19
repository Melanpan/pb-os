#!/usr/bin/env python3
"""SAME/DIFF of each case's Python and C++ results (timing aside)."""
import json
import sys

out, cases = sys.argv[1], sys.argv[2:]
bad = 0
for c in cases:
    try:
        r = {i: json.load(open(f"{out}/{c}.{i}.json")) for i in ("py", "cpp")}
    except OSError as e:
        print(f"{c}: MISSING {e}")
        bad += 1
        continue
    for x in r.values():
        x.pop("impl")
        x["stderr"] = [line for line in x["stderr"] if line]
    t = {i: r[i].pop("timing") for i in r}
    same = r["py"] == r["cpp"]
    bad += not same
    print(f"{c}: {'SAME' if same else 'DIFF'}  py {t['py']}  cpp {t['cpp']}")
    if not same:
        for k in r["py"]:
            if r["py"][k] != r["cpp"].get(k):
                print(f"  {k}:\n    py  {r['py'][k]}\n    cpp {r['cpp'].get(k)}")
sys.exit(1 if bad else 0)

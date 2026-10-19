#!/usr/bin/env python3
"""Compares glide-cli's answers with ref.py's, case by case, per pack and
operation: exact matches, order mismatches (same words, other order) and
other mismatches. Exit 1 on any difference.

    compare.py cases.jsonl ref.jsonl cpp.jsonl
"""
import json
import sys
from collections import defaultdict


def load(path):
    with open(path, encoding="utf-8") as fh:
        return {r["id"]: r["result"] for r in map(json.loads, filter(str.strip, fh))}


def main():
    cases_path, ref_path, cpp_path = sys.argv[1:4]
    ref, cpp = load(ref_path), load(cpp_path)
    stats = defaultdict(lambda: [0, 0, 0, 0])  # (lang, op) -> total, exact, order, other
    shown = 0
    with open(cases_path, encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            c = json.loads(line)
            key = (c["lang"], c["op"])
            s = stats[key]
            s[0] += 1
            a, b = ref.get(c["id"], "<missing>"), cpp.get(c["id"], "<missing>")
            if a == b:
                s[1] += 1
                continue
            if isinstance(a, list) and isinstance(b, list) and sorted(a) == sorted(b):
                s[2] += 1
            else:
                s[3] += 1
            if shown < 25:
                shown += 1
                brief = {k: v for k, v in c.items() if k not in ("path", "keys")}
                print(f"DIFF {brief}: python {a!r} c++ {b!r}")
    bad = 0
    by_lang = defaultdict(lambda: [0, 0, 0, 0])
    for (lang, op), s in sorted(stats.items()):
        print(f"{lang:12} {op:9} {s[1]:6}/{s[0]:<6} exact  order-mismatch {s[2]}  other {s[3]}")
        for i in range(4):
            by_lang[lang][i] += s[i]
        bad += s[2] + s[3]
    for lang, s in sorted(by_lang.items()):
        print(f"{lang:12} ALL       {s[1]:6}/{s[0]:<6} exact  order-mismatch {s[2]}  other {s[3]}")
    sys.exit(1 if bad else 0)


main()

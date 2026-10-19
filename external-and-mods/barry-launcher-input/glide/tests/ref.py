#!/usr/bin/env python3
"""The reference: barry_glide.py answering the harness's cases the way
barry_launcher_shelld does (its keyboard_pack() and dictionary(), copied
here without GLib), one JSON answer per case line, as glide-cli does.

    ref.py --glide DIR --dirs D1:D2 --data DIR [--load LANG,...] [--rss]
           [--repeat N] [cases.jsonl]
"""
import argparse
import json
import os
import re
import sys
import time

LANG_ID = re.compile(r"^[A-Za-z0-9_-]{1,32}$")


def memory(when):
    with open("/proc/self/status") as fh:
        rows = [l.strip() for l in fh if l.startswith(("VmRSS", "RssAnon", "RssFile"))]
    print(f"memory {when}: " + " ".join(rows), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glide", required=True)
    ap.add_argument("--dirs", required=True)
    ap.add_argument("--data", default="")
    ap.add_argument("--load", default="")
    ap.add_argument("--rss", action="store_true")
    ap.add_argument("--repeat", type=int, default=0)
    ap.add_argument("cases", nargs="?")
    a = ap.parse_args()
    if a.rss:
        memory("start")
    sys.path.insert(0, a.glide)
    import barry_glide

    dirs = [d for d in a.dirs.split(":") if d]

    def keyboard_pack(lang, borrow=True):
        if not isinstance(lang, str) or not LANG_ID.match(lang):
            return None
        for base in reversed(dirs):
            folder = os.path.join(base, lang)
            try:
                with open(os.path.join(folder, "keyboard.json"), encoding="utf-8") as fh:
                    pack = json.load(fh)
            except (OSError, ValueError):
                continue
            if not isinstance(pack, dict):
                continue
            words = None
            if isinstance(pack.get("words"), str) and pack["words"]:
                path = os.path.realpath(os.path.join(folder, pack["words"]))
                if path.startswith(os.path.realpath(folder) + os.sep) and os.path.isfile(path):
                    words = path
            rows = [[k for k in r if isinstance(k, str)] for r in pack.get("rows", []) if isinstance(r, list)]
            grid = pack.get("grid") if isinstance(pack.get("grid"), list) else None
            more = pack.get("more") if isinstance(pack.get("more"), dict) else {}
            more = {k: [x for x in v if isinstance(x, str)] for k, v in more.items() if isinstance(v, list)}
            words_lang = lang
            source = pack.get("wordsFrom")
            if words is None and borrow and isinstance(source, str) and source != lang:
                lender = keyboard_pack(source, borrow=False)
                if lender and lender["words"]:
                    words, words_lang = lender["words"], source
            return {"folder": folder, "words": words, "rows": rows, "grid": grid, "more": more,
                    "words_lang": words_lang}
        return None

    packs, decoders = {}, {}

    def dictionary(lang):
        if lang in packs:
            return packs[lang]
        pack = keyboard_pack(lang)
        if pack is None or pack["words"] is None:
            return None
        words_lang = pack["words_lang"]
        english = words_lang == "en"
        decoder = decoders.get(pack["words"])
        if decoder is None:
            try:
                decoder = barry_glide.Decoder(pack["words"], english)
            except (OSError, ValueError) as err:
                print(f"keyboard {lang}: {err}", file=sys.stderr)
                return None
            decoders[pack["words"]] = decoder
        personal = (os.path.join(a.data, "personal-words.txt" if english else f"personal-words-{words_lang}.txt")
                    if a.data else None)
        positions = barry_glide.grid_positions(pack["grid"]) if pack["grid"] else None
        entry = {"decoder": decoder,
                 "corrector": barry_glide.Corrector(decoder, personal, pack["rows"], pack["more"], positions),
                 "fold": {x.lower(): base.lower() for base, alts in pack["more"].items() if len(base) == 1
                          for x in alts if len(x) == 1}}
        packs[lang] = entry
        return entry

    def answer(c):
        d = dictionary(c.get("lang", "en"))
        op = c["op"]
        if op == "glide":
            return d["decoder"].decode(c["path"], c["keys"], float(c["size"]), fold=d["fold"]) if d else []
        if op == "correct":
            return d["corrector"].correct(c["word"]) if d else None
        if op == "suggest":
            return d["corrector"].suggest(c["prefix"]) if d else []
        if op == "near":
            return d["corrector"].near(c["word"]) if d else []
        if op == "complete":
            return d["corrector"].complete(c["word"]) if d else []
        if op == "known":
            return bool(d and d["corrector"].known(c["word"]))
        raise ValueError(op)

    if a.load:
        for lang in a.load.split(","):
            dictionary(lang)
        if a.rss:
            memory("loaded")
    if not a.cases:
        return
    times = {}
    out = sys.stdout
    with open(a.cases, encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            c = json.loads(line)
            try:
                r = answer(c)
            except Exception as err:  # shelld answers 500; the harness flags it
                r = f"<error {type(err).__name__}>"
            for _ in range(a.repeat):
                t0 = time.perf_counter()
                answer(c)
                t = times.setdefault(c["op"], [0.0, 0])
                t[0] += (time.perf_counter() - t0) * 1e6
                t[1] += 1
            out.write(json.dumps({"id": c.get("id"), "result": r}, ensure_ascii=False) + "\n")
    for op, (us, n) in sorted(times.items()):
        print(f"time {op}: {us / n:.1f} us/call over {n} calls", file=sys.stderr)
    if a.rss:
        memory("end")


main()

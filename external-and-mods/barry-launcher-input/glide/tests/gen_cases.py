#!/usr/bin/env python3
"""Cases for the barryglide equivalence harness: glides over each pack's
keyboard (words glided with a shaky finger, scribbles, taps), and tapped
words for autocorrect, the suggestion strip and completions (words, typos,
capitals, accents dropped, junk), as JSONL.

    gen_cases.py --dirs D1:D2 --langs en,es,... --out cases.jsonl [--seed N] [--scale F]
    gen_cases.py --fuzz-pack DIR [--seed N]   # writes a random pack to DIR

Coordinates are written as Qt's JSON writes a double: an integral value
without a decimal point (Python then reads an int).
"""
import argparse
import json
import math
import os
import random
import sys


def load_pack(dirs, lang):
    for base in reversed(dirs):
        folder = os.path.join(base, lang)
        try:
            with open(os.path.join(folder, "keyboard.json"), encoding="utf-8") as fh:
                pack = json.load(fh)
        except (OSError, ValueError):
            continue
        words = pack.get("words")
        if not words and pack.get("wordsFrom"):
            lender = load_pack(dirs, pack["wordsFrom"])
            words = lender and lender["_words"]
        else:
            words = os.path.join(folder, words) if words else None
        pack["_words"] = words
        return pack
    return None


def read_words(path):
    out = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            if len(parts) == 2:
                out.append((parts[0], parts[1]))
    return out


def qt_number(v, rnd):
    """A coordinate as Barry would send it: some fractional, some whole."""
    style = rnd.random()
    if style < 0.25:
        v = round(v)
    elif style < 0.5:
        v = round(v, 1)
    elif style < 0.75:
        v = round(v, 2)
    return int(v) if float(v).is_integer() and abs(v) < 2 ** 53 else v


def layout(pack, rnd):
    """Key centers in pixels for a pack's letter rows (QWERTY for a grid pack)."""
    rows = pack.get("rows") or [list("qwertyuiop"), list("asdfghjkl"), list("zxcvbnm")]
    w = rnd.choice([rnd.uniform(40, 120), float(rnd.randint(50, 110))])
    h = w * rnd.uniform(1.1, 1.5)
    x0, y0 = rnd.uniform(0, 30), rnd.uniform(0, 300)
    widest = max(len(r) for r in rows)
    keys = {}
    for y, row in enumerate(rows):
        for x, k in enumerate(row):
            if not isinstance(k, str):
                continue
            keys[k] = [qt_number(x0 + (x + (widest - len(row)) / 2 + 0.5) * w, rnd),
                       qt_number(y0 + (y + 0.5) * h, rnd)]
    if rnd.random() < 0.2:  # keys the decoder must leave out
        keys["shift"] = [x0, y0 + 3 * h]
        keys["'"] = [x0 + w, y0 + 3 * h]
    items = list(keys.items())
    if rnd.random() < 0.7:
        items.sort()  # QJsonObject's order
    else:
        rnd.shuffle(items)
    return dict(items), w


def fold_of(pack):
    return {a.lower(): b.lower() for b, alts in (pack.get("more") or {}).items() if len(b) == 1
            for a in alts if isinstance(a, str) and len(a) == 1}


def glide_path(word, keys, w, fold, rnd):
    centers = []
    for c in word.lower():
        k = c if c in keys else fold.get(c)
        if k in keys:
            centers.append(keys[k])
    if len(centers) < 1:
        return None
    shake = rnd.choice([0.1, 0.25, 0.4, 0.6])
    pts = [(x + rnd.gauss(0, shake * w * 0.5), y + rnd.gauss(0, shake * w * 0.4)) for x, y in centers]
    path = []
    for a, b in zip(pts, pts[1:]):
        n = rnd.randint(2, 14)
        for i in range(n):
            t = i / n
            path.append((a[0] + (b[0] - a[0]) * t + rnd.gauss(0, w * 0.04),
                         a[1] + (b[1] - a[1]) * t + rnd.gauss(0, w * 0.04)))
    path.append(pts[-1])
    if rnd.random() < 0.2:  # dwell at the end
        path += [pts[-1]] * rnd.randint(1, 4)
    return [[qt_number(x, rnd), qt_number(y, rnd)] for x, y in path[:4000]]


def scribble(keys, w, rnd):
    xs = [v[0] for v in keys.values()]
    ys = [v[1] for v in keys.values()]
    x, y = rnd.uniform(min(xs), max(xs)), rnd.uniform(min(ys), max(ys))
    path = []
    for _ in range(rnd.randint(1, 80)):
        path.append([qt_number(x, rnd), qt_number(y, rnd)])
        x += rnd.gauss(0, w * 0.5)
        y += rnd.gauss(0, w * 0.3)
    return path


NEIGHBORS = {}


def typo(word, letters, rnd):
    w = list(word)
    if not w:
        return word
    kind = rnd.randrange(6)
    i = rnd.randrange(len(w))
    if kind == 0:
        w[i] = rnd.choice(letters)
    elif kind == 1 and len(w) > 1:
        j = min(i + 1, len(w) - 1)
        w[i], w[j] = w[j], w[i]
    elif kind == 2 and len(w) > 2:
        del w[i]
    elif kind == 3:
        w.insert(i, rnd.choice(letters))
    elif kind == 4:
        w.insert(i, w[i])
    else:
        w[i] = rnd.choice(letters)
        w[rnd.randrange(len(w))] = rnd.choice(letters)
    return "".join(w)


STRIP_ACCENTS = str.maketrans("áéíóúüñàèìòùäëïöâêîôûç", "aeiouunaeiouaeioaeiouc")
JUNK = ["", "a", "I", "x1", "hello!", "don't", "dont", "cant", "ITS", "iPhone", "ÄÖÜ", "straße", "İstanbul",
        "ΟΔΟΣ", "ὈΔΥΣΣΕΎΣ", "naïve", "é", "über", "ñu", "  ", "a-b", "teh", "Teh", "TEH", "im", "Im",
        "alot", "recieve", "Recieve", "thor", "Thor", "steamos", "Steamos", "couplings", "hoping", "tried",
        "ǅemal", "ﬁne", "ß", "ŉ", "Σ", "σς", "ΣΑΣ", "İ", "i̇", "x" * 30, "qqqq", "zzz", "señor",
        "senor", "manana", "mañana", "cancion", "cancíon", "Mexico", "méxico", "telefono", "aun", "esta"]


def corrector_cases(lang, words, letters, rnd, n):
    out = []
    pool = [w for w, _ in words]
    for _ in range(n):
        r = rnd.random()
        base = rnd.choice(pool)
        if r < 0.3:
            word = typo(base, letters, rnd)
        elif r < 0.45:
            word = typo(typo(base, letters, rnd), letters, rnd)
        elif r < 0.6:
            word = base
        elif r < 0.68:
            word = base.translate(STRIP_ACCENTS)
        elif r < 0.75:
            word = typo(base, letters, rnd)
            word = word[:1].upper() + word[1:]
        elif r < 0.8:
            word = base.upper() if rnd.random() < 0.5 else base.capitalize()
        elif r < 0.9:
            word = rnd.choice(JUNK)
        else:
            word = "".join(rnd.choice(letters) for _ in range(rnd.randint(1, 12)))
        for op in ("correct", "near", "known"):
            out.append({"lang": lang, "op": op, "word": word})
        # The strip while typing: a prefix of a word (or of a typo).
        cut = word[: rnd.randint(1, max(1, len(word)))] if rnd.random() < 0.6 else word
        out.append({"lang": lang, "op": "suggest", "prefix": cut})
        out.append({"lang": lang, "op": "complete", "word": cut})
    return out


def glide_cases(lang, pack, words, rnd, n):
    fold = fold_of(pack)
    weighted = [w for w, lvl in words for _ in range({"10": 6, "20": 3}.get(lvl, 1))]
    out = []
    long_words = ["internationalization", "characteristically", "responsibilities", "unfortunately"] \
        if lang.startswith("en") else []
    for i in range(n):
        keys, w = layout(pack, rnd)
        r = rnd.random()
        if i < len(long_words):
            path = glide_path(long_words[i], keys, w, fold, rnd)
        elif r < 0.75:
            word = rnd.choice(weighted)
            path = glide_path(word, keys, w, fold, rnd)
        elif r < 0.92:
            path = scribble(keys, w, rnd)
        else:
            x, y = rnd.choice(list(keys.values()))
            path = [[x, y], [qt_number(x + rnd.uniform(-5, 5), rnd), qt_number(y + rnd.uniform(-5, 5), rnd)]]
        if not path or len(path) < 2:
            continue
        out.append({"lang": lang, "op": "glide", "path": path, "keys": keys, "size": qt_number(w, rnd)})
    return out


FUZZ_LETTERS = list("abcdeéfghiíjklmnñoóprsßtuúüvwxyzσςαβγδ") + ["İ", "Σ", "Ä", "'"]


def fuzz_pack(folder, rnd):
    """A random non-English pack: Unicode words (capitals, final sigma,
    dotted I, ß), apostrophes, repeats, odd levels, comments, CRLF."""
    os.makedirs(folder, exist_ok=True)
    rows = [list("qwertyuiopü"), list("asdfghjklñσ"), list("zxcvbnmßαβ")]
    more = {"a": ["á", "à", "ä", "ª"], "e": ["é", "ë"], "i": ["í", "İ"], "o": ["ó", "ö"], "u": ["ú"],
            "n": ["ñ"], "s": ["ß", "ś"], "σ": ["ς", "Σ"], ",": ["¿"]}
    with open(os.path.join(folder, "keyboard.json"), "w", encoding="utf-8") as fh:
        json.dump({"name": "Fuzz", "label": "FZ", "rows": rows, "more": more, "words": "words.txt"},
                  fh, ensure_ascii=False)
    lines = ["# fuzz words", ""]
    seen = []
    for _ in range(6000):
        n = rnd.randint(1, 11)
        word = "".join(rnd.choice(FUZZ_LETTERS) for _ in range(n))
        if rnd.random() < 0.15:
            word = word.capitalize()
        if rnd.random() < 0.05 and seen:
            word = rnd.choice(seen)  # a repeat
        if rnd.random() < 0.03:
            word = word.upper() + "Σ"
        seen.append(word)
        level = rnd.choice(["10", "10", "20", "20", "35", "40", "15", "99", "010", "+20", "2_0"])
        lines.append(f"{word}\t{level}" if rnd.random() < 0.1 else f"{word} {level}")
    data = "\n".join(lines) + "\n"
    if rnd.random() < 2:  # always: some CRLF lines
        data = data.replace("0\n", "0\r\n", 200)
    with open(os.path.join(folder, "words.txt"), "w", encoding="utf-8", newline="") as fh:
        fh.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dirs")
    ap.add_argument("--langs")
    ap.add_argument("--out")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--fuzz-pack")
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    if a.fuzz_pack:
        fuzz_pack(a.fuzz_pack, rnd)
        return
    dirs = a.dirs.split(":")
    cases = []
    for lang in a.langs.split(","):
        pack = load_pack(dirs, lang)
        if pack is None or not pack["_words"]:
            sys.exit(f"no pack {lang}")
        words = read_words(pack["_words"])
        letters = sorted({c for w, _ in words for c in w.lower() if c.isalpha()})
        cases += glide_cases(lang, pack, words, rnd, int(1500 * a.scale))
        cases += corrector_cases(lang, words, letters, rnd, int(1500 * a.scale))
    with open(a.out, "w", encoding="utf-8") as fh:
        for i, c in enumerate(cases):
            c["id"] = i
            fh.write(json.dumps(c, ensure_ascii=False) + "\n")
    print(f"{len(cases)} cases -> {a.out}")


main()

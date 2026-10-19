#!/usr/bin/env python3
"""GlideEngine (glide-engine-test) against barry_launcher_shelld's HTTP
answers, case by case (run.sh's sandbox):

    compare.py <shelld> <glide-engine-test> <cases.jsonl> <langs>

shelld is imported from the worktree and its Handler served on a test port,
as the keyboards asked it: POST /glide, /suggest and /type with Qt's compact
JSON. The cases are glide/tests/gen_cases.py's glides and suggestions, and
/type plans made from its words: autocorrect after a space or punctuation and
before Return (the field ending with the word, with more of it, or not with
it), a completion picked (the field as expected, or not), an undone
correction kept. A stub barry_launcher_keyd answers "BEFORE n" with the
field each case sets. A glide over shelld's 4 KB body limit (its 400) is
compared with shelld's glide() itself, which answers what the keyboard now
gets. Then: a word kept by one keyboard (process) is not corrected by the
other (its personal words watched), and the time each takes.
"""
from __future__ import annotations

import http.client
import importlib.machinery
import importlib.util
import json
import os
import random
import socket
import statistics
import subprocess
import sys
import threading
import time
from collections import defaultdict
from http.server import ThreadingHTTPServer

SHELLD, ENGINE, CASES, LANGS = sys.argv[1:5]
HOME = "/tmp/home"
RUN = "/tmp/run"
PORT = 47991
TYPE_CASES = 400  # words per pack made into plans
os.makedirs(RUN, exist_ok=True)
DATA = f"{HOME}/.local/share/barry_launcher"
os.makedirs(DATA, exist_ok=True)
os.environ.update(HOME=HOME, XDG_RUNTIME_DIR=RUN, XDG_DATA_HOME=f"{HOME}/.local/share",
                  XDG_CACHE_HOME=f"{HOME}/.cache", XDG_CONFIG_HOME=f"{HOME}/.config",
                  BARRY_KEYBOARDS_DIRS="/t/shipped:/t/user", BARRY_PORT=str(PORT), NO_AT_BRIDGE="1")
# Personal words, as glide/tests/run.sh has them.
for name, words in (("personal-words.txt", "thorr\nbarri\nSteamOSS\n"), ("personal-words-es.txt", "holaa\n"),
                    ("personal-words-fuzz.txt", "qqq\n")):
    with open(f"{DATA}/{name}", "w", encoding="utf-8") as fh:
        fh.write(words)

# --- the stub keyd: the field's text before the caret --------------------------------
FIELD = [""]


def keyd() -> None:
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(f"{RUN}/barry_launcher_keyd.sock")
    srv.listen(16)
    while True:
        conn, _ = srv.accept()
        with conn:
            data = b""
            while not data.endswith(b"\n"):
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
            n = int(data.split()[1])
            conn.sendall((json.dumps({"text": FIELD[0][-n:] if n else ""}) + "\n").encode())


threading.Thread(target=keyd, daemon=True).start()

# --- shelld's handler, from the worktree ------------------------------------------------
sys.path.insert(0, os.path.dirname(SHELLD))
loader = importlib.machinery.SourceFileLoader("shelld", SHELLD)
spec = importlib.util.spec_from_loader("shelld", loader)
shelld = importlib.util.module_from_spec(spec)
loader.exec_module(shelld)
http_srv = ThreadingHTTPServer(("127.0.0.1", PORT), shelld.Handler)
http_srv.daemon_threads = True
threading.Thread(target=http_srv.serve_forever, daemon=True).start()


def post(path: str, body: dict) -> tuple[int, dict, float, int]:
    data = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode()  # Qt's Compact
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=30)
    t0 = time.perf_counter()
    c.request("POST", path, data, {"Content-Type": "application/json"})
    r = c.getresponse()
    out = json.loads(r.read() or b"{}")
    t = time.perf_counter() - t0
    c.close()
    return r.status, out, t, len(data)


class Engine:
    def __init__(self, *args: str) -> None:
        self.p = subprocess.Popen([ENGINE, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  encoding="utf-8")

    def ask(self, case: dict) -> dict:
        self.p.stdin.write(json.dumps(case, ensure_ascii=False) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def close(self) -> None:
        self.p.stdin.close()
        self.p.wait(10)


def words_answer(status: int, obj: dict) -> dict:
    return {"words": obj.get("words")} if status == 200 else {"status": status}


# --- the cases ---------------------------------------------------------------------------------
cases = [json.loads(l) for l in open(CASES, encoding="utf-8")]
rnd = random.Random(7)
plans = []  # (lang, field, body)
by_lang = defaultdict(list)
for c in cases:
    if c["op"] == "correct":
        by_lang[c["lang"]].append(c["word"])
# English typos autocorrect fixes, for the kept-word checks.
TYPOS = {"en": ["recieve", "teh", "becuase", "wierd", "untill", "definately"]}
for lang, ws in by_lang.items():
    for w in rnd.sample(ws, min(TYPE_CASES, len(ws))) + TYPOS.get(lang, []):
        sep = rnd.choice([" ", " ", ",", ".", "?", "!"])
        plans.append((lang, "so " + w, {"text": sep, "correct": w}))
        plans.append((lang, w + "x", {"text": sep, "correct": w}))  # the field has more
        plans.append((lang, "x" + w, {"text": sep, "correct": w}))  # not a whole word
        plans.append((lang, "\n" + w, {"key": "Return", "correct": w}))
        cut = w[: max(1, len(w) // 2)]
        plans.append((lang, "so " + cut, {"back": len(cut), "text": w + " ", "expect": cut}))  # completion
        plans.append((lang, "so " + cut + "q", {"back": len(cut), "text": w + " ", "expect": cut}))  # 409
plans.append(("en", "", {"text": ""}))  # nothing to type: 400
# Long glides, as a slow finger gives (Qt's touch points, 8 between each two
# of the case's): over shelld's 4 KB body limit, the keyboard's old problem.
long_glides = []
for lang in by_lang:
    for c in [c for c in cases if c["op"] == "glide" and c["lang"] == lang][:40]:
        path = [c["path"][0]]
        for (x0, y0), (x1, y1) in zip(c["path"], c["path"][1:]):
            path += [[round(x0 + (x1 - x0) * k / 9, 3), round(y0 + (y1 - y0) * k / 9, 3)] for k in range(1, 10)]
        long_glides.append(dict(c, path=path[:4000]))
plans.append(("en", "", {"back": 101, "text": "x"}))  # 400

engine = Engine()
stats = defaultdict(lambda: [0, 0])  # (lang, op) -> [equal, total]
bad = []
times = defaultdict(lambda: ([], []))  # op -> (shelld s, engine s)
over4k = [0, 0]  # glides over shelld's body limit: [equal, total]


def record(lang: str, op: str, want, got, case) -> None:
    s = stats[(lang, op)]
    s[1] += 1
    if want == got:
        s[0] += 1
    elif len(bad) < 20:
        bad.append((lang, op, case, want, got))


for c in cases + long_glides:
    if c["op"] not in ("glide", "suggest"):
        continue
    if c["op"] == "glide":
        body = {"path": c["path"], "keys": c["keys"], "size": c["size"], "lang": c["lang"]}
        status, obj, t, size = post("/glide", body)
        want = words_answer(status, obj)
        if size > 4096:
            # shelld cannot read it (its 400); its glide() answers what the
            # keyboard now gets.
            with_limit = want
            want = words_answer(*shelld.glide(json.loads(json.dumps(body))))
            over4k[1] += 1
        else:
            with_limit = None
    else:
        status, obj, t, _ = post("/suggest", {"prefix": c["prefix"], "lang": c["lang"]})
        want = words_answer(status, obj)
        with_limit = None
    a = engine.ask(c)
    got = a["result"]
    if with_limit is not None and want == got:
        over4k[0] += 1
    record(c["lang"], c["op"], want, got, c)
    if with_limit is None:
        times[c["op"]][0].append(t)
        times[c["op"]][1].append(a["us"] / 1e6)

for lang, field, body in plans:
    FIELD[0] = field
    status, obj, t, _ = post("/type", dict(body, lang=lang))
    want = {"status": status, "reply": obj}
    a = engine.ask({"id": 0, "lang": lang, "op": "type", "body": body})
    record(lang, "type", want, a["result"], {"field": field, "body": body})
    times["type"][0].append(t)
    times["type"][1].append(a["us"] / 1e6)

# --- an undone correction kept: both stop correcting it -----------------------------------------
corrected = []
for lang, field, body in plans:
    if field.startswith("so ") and body.get("correct"):
        FIELD[0] = field
        st, obj, _, _ = post("/type", dict(body, lang=lang))
        if obj.get("corrected"):
            corrected.append((lang, body["correct"], obj["corrected"]))
kept = {}
for lang, w, fixed in corrected:
    if lang not in kept and lang != "en":
        kept[lang] = (w, fixed)
taken = {w.lower() for w, _ in kept.values()}  # en-thumbkey's words are en's
en_words = [(w, f) for l, w, f in corrected if l == "en" and w.lower() not in taken]
if len(en_words) >= 2:
    kept["en"] = en_words[0]
for lang, (w, fixed) in kept.items():
    FIELD[0] = "so " + fixed + " "
    undo = {"back": len(fixed) + 1, "text": w + " ", "expect": fixed + " ", "keep": w}
    status, obj, _, _ = post("/type", dict(undo, lang=lang))
    a = engine.ask({"id": 0, "lang": lang, "op": "type", "body": undo})
    record(lang, "keep", {"status": status, "reply": obj}, a["result"], undo)
    FIELD[0] = "so " + w
    again = {"text": " ", "correct": w}
    status, obj, _, _ = post("/type", dict(again, lang=lang))
    a = engine.ask({"id": 0, "lang": lang, "op": "type", "body": again})
    record(lang, "keep", {"status": status, "reply": obj}, a["result"], again)
    print(f"kept {lang}: {w!r} (was corrected to {fixed!r}); corrected after: shelld "
          f"{obj.get('corrected')!r}, engine {a['result']['reply'].get('corrected')!r}")

# --- the other keyboard: personal words watched -----------------------------------------------
sync_ok = False
if len(en_words) >= 2:
    w, fixed = en_words[1]
    other = Engine()  # the other keyboard's process
    ask = lambda e: e.ask({"id": 0, "lang": "en", "op": "type", "body": {"text": " ", "correct": w}})
    FIELD[0] = "so " + w
    before = ask(other)["result"]["reply"].get("corrected")
    FIELD[0] = "so " + fixed + " "
    engine.ask({"id": 0, "lang": "en", "op": "type",
                "body": {"back": len(fixed) + 1, "text": w + " ", "expect": fixed + " ", "keep": w}})
    other.ask({"id": 0, "op": "wait", "ms": 300})
    FIELD[0] = "so " + w
    after = ask(other)["result"]["reply"].get("corrected")
    lines = open(f"{DATA}/personal-words.txt", encoding="utf-8").read().split()
    sync_ok = before == fixed and after is None and lines.count(w.lower()) == 1
    print(f"{'PASS' if sync_ok else 'FAIL'} the other keyboard: {w!r} corrected to {before!r} before, "
          f"{after!r} after the first kept it; in personal-words.txt {lines.count(w.lower())} time(s)")
    other.close()
engine.close()

# --- results ---------------------------------------------------------------------------------------
print("\n== equal answers (lang, op: equal/total)")
total = [0, 0]
for (lang, op), (eq, n) in sorted(stats.items()):
    total[0] += eq
    total[1] += n
    print(f"{lang:12} {op:8} {eq}/{n}")
print(f"all: {total[0]}/{total[1]}; glides over shelld's 4 KB body limit: {over4k[1]} "
      f"(equal to shelld's glide(): {over4k[0]})")
for lang, op, case, want, got in bad:
    print(f"DIFF {lang} {op}: {json.dumps(case, ensure_ascii=False)[:300]}\n  shelld: {want}\n  engine: {got}")


def ms(v: list) -> str:
    v = sorted(v)
    return f"mean {statistics.mean(v) * 1e3:.2f} p50 {v[len(v) // 2] * 1e3:.2f} p95 {v[int(len(v) * 0.95)] * 1e3:.2f} ms"


print("\n== time from ask to answer (shelld: HTTP; engine: worker and back to its thread)")
for op, (s, e) in sorted(times.items()):
    if s:
        print(f"{op:8} shelld {ms(s)} | engine {ms(e)} ({len(s)} cases)")
ok = total[0] == total[1] and sync_ok
print("\nPASS" if ok else "\nFAIL")
sys.exit(0 if ok else 1)

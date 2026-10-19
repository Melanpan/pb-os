#!/usr/bin/env python3
"""The CLI the QAM's Power tab runs (settings, save JSON) and the usage
errors, Python vs C++, on many settings files and inputs: stdout, stderr,
exit code, the file left behind and every write (the temp file's text, the
rename). Run inside run.sh's fake root. A crash compares by its last line
(Python's traceback ends with the same "Error: text" the C++ prints).

  cli.py
"""
import json
import os
import subprocess
import sys

STATE = "/var/lib/pbos-utils/charging.json"
LOG = "/tmp/ctl/log"

FILES = {
    "missing": None,
    "valid": b'{"enabled": true, "pulse_color": "8c00ff", "pulse_min": 0, "pulse_max": 20, "on_s": 4.0, "off_s": 8.0}',
    "types": b'{"pulse_max": "40", "pulse_min": 50, "on_s": "2.75", "off_s": -1, "solid_level": 999, '
             b'"enabled": 0, "pulse_color": "#ABCDEF", "solid_color": 123456}',
    "floats": b'{"on_s": 1e-05, "off_s": 1e300, "pulse_max": 20.9, "pulse_min": -0.5, "solid_level": true}',
    "floats2": b'{"on_s": 3.3333333333333335, "off_s": 0.1, "pulse_max": 254.99, "pulse_min": 253.5}',
    "nan": b'{"on_s": NaN, "off_s": Infinity, "pulse_max": NaN, "solid_level": -1e999}',
    "neg-inf-int": b'{"pulse_max": -Infinity}',
    "bad-json": b'{"pulse_max": }',
    "bom": b'\xef\xbb\xbf{"pulse_max": 99}',
    "list": b'[1, 2]',
    "bad-utf8": b'{"pulse_color": "\xff\xfe"}',
    "unicode": '{"pulse_color": "éééééé", "solid_color": " 8C00FF"}'.encode(),
    "colors": b'{"pulse_color": "##0a0B0c", "solid_color": "#12345"}',
    "huge-int": b'{"pulse_max": 1' + b"0" * 5000 + b'}',
    "big-int": b'{"pulse_max": 1' + b"0" * 400 + b', "pulse_min": -' + b"9" * 30 + b'}',
    "underscores": b'{"pulse_max": "1_0_0", "on_s": "1_0.5", "off_s": " 3_0 ", "solid_level": "_5"}',
    "words": b'{"on_s": "inf", "off_s": "-nan", "pulse_max": " +42 ", "solid_level": "0x10", "pulse_min": "\\t7\\n"}',
    "float-strs": b'{"on_s": "1.e0", "off_s": ".5", "pulse_max": "4.5", "solid_level": "1e1", "pulse_min": "\\u00a05"}',
    "dupes": b'{"pulse_max": 30, "on_s": 2, "pulse_max": 40, "zz": 1}',
    "nested": b'{"enabled": [], "pulse_color": {"a": 1}, "solid_color": null, "on_s": [1], "pulse_max": null}',
    "enabled-str": b'{"enabled": "false", "solid_color": 100000}',
    "escapes": b'{"pulse_color": "\\u0038c00ff", "solid_color": "\\ud800\\udc00x"}',
    "ws": b' \r\n\t{ "pulse_max" :25 } \n',
    "trailing": b'{"pulse_max": 25} x',
    "dir": "DIR",
}

SAVES = [
    ["save", '{"pulse_max": 100}'],
    ["save", '{"enabled": false, "on_s": 3}'],
    ["save", '{"bogus": 1, "solid_color": "#123ABC", "solid_level": 7}'],
    ["save", '{"on_s": 1.1, "off_s": 0.1, "pulse_min": "7", "pulse_max": "x"}'],
    ["save", '{"on_s": NaN, "off_s": Infinity}'],
    ["save", '{"pulse_max": 1e400}'],
    ["save", '{"solid_level": -5, "pulse_min": 300, "pulse_max": 12345678901234567890123}'],
    ["save", '{"pulse_color": "café!", "solid_color": [1, {"a": null}], "enabled": {}}'],
    ["save", b'{"pulse_color": "\xff\x80"}'],
    ["save", '{"on_s": 2, "on_s": 9.5}', "extra"],
    ["save", ' {"off_s": 0} '],
    ["save", "[]"],
    ["save", "not json"],
    ["save", '{"a": 1} {}'],
    ["save", ""],
    ["save"],
    [],
    ["bogus"],
    ["settings", "extra"],
]


def put_state(content):
    if os.path.isdir(STATE):
        os.rmdir(STATE)
    if os.path.exists(STATE):
        os.remove(STATE)
    if content == "DIR":
        os.makedirs(STATE)
    elif content is not None:
        with open(STATE, "wb") as fh:
            fh.write(content)


def run(impl, args, state):
    put_state(state)
    if os.path.exists(LOG):
        os.remove(LOG)
    argv = (["python3", "/tmp/t/sm8550-charge-rest"] if impl == "py"
            else ["/tmp/t/pbos-platformd", "charge-rest"])
    argv = [a.encode() if isinstance(a, str) else a for a in argv + list(args)]
    env = dict(os.environ, CREST_LOG=LOG, LD_PRELOAD="/tmp/t/wlog.so", LANG="en_US.UTF-8")
    p = subprocess.run(argv, capture_output=True, env=env, timeout=30)
    err = p.stderr.decode(errors="replace").splitlines()
    if any(line.startswith("Traceback") for line in err):
        err = ["CRASH " + err[-1]]
    elif err and err[-1].startswith("sm8550-charge-rest: ") and "Error" in err[-1]:
        err = ["CRASH " + err[-1][len("sm8550-charge-rest: "):]]
    try:
        left = open(STATE, "rb").read().decode(errors="replace")
    except OSError as e:
        left = f"({type(e).__name__})"
    log = open(LOG).read().splitlines() if os.path.exists(LOG) else []
    return {"exit": p.returncode, "stdout": p.stdout.decode(errors="replace"), "stderr": err, "file": left,
            "writes": [line.split("\t", 1)[1] for line in log]}


def main():
    bad = n = 0
    jobs = [(f"settings/{k}", ["settings"], v) for k, v in FILES.items()]
    for k in ("valid", "types", "missing", "nan", "bad-json"):
        jobs += [(f"{' '.join(a if isinstance(a, str) else repr(a) for a in s)[:60]} / {k}", s, FILES[k])
                 for s in SAVES]
    for name, args, state in jobs:
        r = {i: run(i, args, state) for i in ("py", "cpp")}
        n += 1
        if r["py"] != r["cpp"]:
            bad += 1
            print(f"DIFF {name}")
            for k in r["py"]:
                if r["py"][k] != r["cpp"][k]:
                    print(f"  {k}:\n    py  {r['py'][k]!r}\n    cpp {r['cpp'][k]!r}")
        elif os.environ.get("VERBOSE"):
            print(f"SAME {name}: exit {r['py']['exit']} {r['py']['stdout'][:100]!r} {r['py']['stderr']}")
    print(f"cli: {n - bad}/{n} SAME")
    sys.exit(1 if bad else 0)


main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""protondb-hints.py — the ProtonDB Community Fix section of the Performance tab's Per Game page.

Builds steamos-overlay/usr/share/pb-os/game-launch/protondb-hints.json from a
ProtonDB reports dump (https://github.com/bdefore/protondb-data, ODbL 1.0),
the archive as published there:

    tools/protondb-hints.py reports/reports_oct1_2026.tar.gz [out.json]

Per Steam app id with a fix, from reports of the last three years:
  w, n    how many reports said it works, out of how many
  env     the launch fix most working reports agree on: environment variables
  args    ... and game arguments (after %command%)
  k       how many working reports used exactly that fix (at least MIN_AGREE)

Only settings that make sense on PB-OS (Proton with FEX on Adreno) are kept:
variables Proton, Wine, DXVK and VKD3D read (plus a few known ones), minus
what is risky on ARM (Wine's OpenGL renderer instead of DXVK, forced DX12 or
OpenGL, VKD3D claiming desktop features or ray tracing, anti-cheat runtimes,
software rendering), other GPUs' switches, tools PB-OS doesn't ship and
anything with a file path. Arguments keep their values (--rendering-driver
vulkan). What is left of each report is compared; a game whose most-agreed
fix was all risky gets its next most-agreed one. Games without a fix are left
out.

The output is a derived database of ProtonDB's reports and is published under
the same license (ODbL 1.0, contents DbCL 1.0), with ProtonDB's notice in the
file itself; see CREDITS.md.
"""
import collections
import datetime
import json
import os
import re
import shlex
import sys
import tarfile

YEARS = 3
MIN_AGREE = 3
OUT = "steamos-overlay/usr/share/pb-os/game-launch/protondb-hints.json"

# Variables a fix may set: the ones Proton, Wine, DXVK and VKD3D read, and a
# few known others. Everything else (other GPUs' drivers, overlays, tools,
# typos) never carries over.
KEEP_ENV = re.compile(
    r"^(PROTON_[A-Z0-9_]+|WINE[A-Z0-9_]*|DXVK_[A-Z0-9_]+|VKD3D_[A-Z0-9_]+|STEAMDECK|PULSE_LATENCY_MSEC|"
    r"PIPEWIRE_LATENCY|GST_PLUGIN_FEATURE_RANK|FNA3D_FORCE_DRIVER|SDL_AUDIODRIVER)$")
# ... minus these: risky on ARM (Wine's OpenGL renderer for DXVK, desktop-only
# VKD3D features, anti-cheat runtimes), other GPUs' switches, debugging, and
# what PB-OS sets itself.
DROP_ENV = re.compile(
    r"^(PROTON_USE_WINED3D.*|WINED3D|PROTON_NO_D3D1[01]|PROTON_OLD_GL_STRING|VKD3D_FEATURE_LEVEL|"
    r"VKD3D_SHADER_MODEL|PROTON_USE_EAC_LINUX|PROTON_EAC_RUNTIME|PROTON_BATTLEYE_RUNTIME|"
    r"PROTON_ENABLE_NVAPI|PROTON_DISABLE_NVAPI|PROTON_HIDE_NVIDIA_GPU|PROTON_DLSS.*|PROTON_FSR4.*|"
    r"PROTON_ENABLE_NGX_UPDATER|PROTON_ENABLE_AMD_AGS|DXVK_ENABLE_NVAPI|DXVK_NVAPI.*|PROTON_LOG.*|"
    r"WINEDEBUG|DXVK_HUD|DXVK_LOG.*|VKD3D_DEBUG|VKD3D_DISABLE_EXTENSIONS|DXVK_FILTER_DEVICE_NAME|"
    r"DXVK_HDR|PROTON_ENABLE_HDR|PROTON_ENABLE_WAYLAND|PROTON_USE_WAYLAND|PROTON_USE_NTSYNC|DXVK_ASYNC|"
    r"WINE_CPU_TOPOLOGY|PROTON_CPU_TOPOLOGY|WINE_FULLSCREEN_FSR.*|PROTON_USE_WOW64|PROTONFIXES_DISABLE)$")
# VKD3D_CONFIG / PROTON_CONFIG words that turn on ray tracing (desktop GPUs);
# the rest stay.
DROP_VKD3D = {"dxr", "dxr11", "dxr12"}
# Arguments that are risky on ARM: forced DX12 (VKD3D) or OpenGL, software
# rendering, anti-cheat launchers, a benchmark run instead of the game; and
# the reporter's own screen size or VRAM, wrong for a handheld.
DROP_ARGS = {"-dx12", "-d3d12", "-use-d3d12", "-force-d3d12", "-force-opengl", "-opengl", "-force-glcore",
             "--disable-gpu", "--disable-gpu-compositing", "--enable-software-rasterizer",
             "-eac_launcher", "-benchmark", "-screen-width", "-screen-height", "-width", "-height",
             "-w", "-h", "-res", "-resolution", "-monitor", "-adapter"}
DROP_ARG_PREFIX = ("-maxvram", "-screen-width=", "-screen-height=", "-width=", "-height=", "-res=")
DROP_WORDS = {"gamemoderun", "mangohud", "prime-run", "obs-gamecapture", "env", "--", "gamescope"}
# File paths, quoting gone wrong (smart quotes, spaces) and other junk.
PATHISH = re.compile(r"[/~$\\\s]|[^\x21-\x7e]")


def clean_env(k, v):
    """The value to set, or None when the variable doesn't carry over."""
    if (not KEEP_ENV.match(k) or DROP_ENV.match(k) or PATHISH.search(v) or len(v) > 200
            or "opengl" in v.lower()):
        return None
    if k in ("VKD3D_CONFIG", "PROTON_CONFIG"):
        v = ",".join(w for w in v.split(",") if w.strip() and w.strip().lower() not in DROP_VKD3D)
    return v or None


def clean_args(words):
    """Flags with their values (--rendering-driver vulkan), risky ones out."""
    out, i = [], 0
    while i < len(words):
        w = words[i]
        i += 1
        if not w.startswith("-") or len(w) > 40 or re.fullmatch(r"-+\d*", w):
            continue
        flag = [w]
        if "=" not in w and i < len(words) and not words[i].startswith("-"):
            flag.append(words[i])
            i += 1
        if (w.lower() in DROP_ARGS or w.lower().startswith(DROP_ARG_PREFIX)
                or any(PATHISH.search(x) or len(x) > 40 for x in flag)
                or "opengl" in " ".join(flag[1:]).lower()):
            continue
        out += flag
    return out


def fix_of(text):
    """(env, args) from a launch-option line, or None when nothing portable is in it."""
    text = text.replace("%COMMAND%", "%command%")
    if "%command%" not in text:
        return None
    pre, _, post = text.partition("%command%")
    try:
        before, after = shlex.split(pre), shlex.split(post)
    except ValueError:
        return None
    if any(w in DROP_WORDS or w.startswith("gamescope") for w in before):
        # gamescope's own switches and a nested command line: the variables
        # before it may belong to the wrapper, not the game.
        before = [w for w in before if "=" in w]
    env = {}
    for w in before:
        if "=" not in w:
            continue
        k, v = w.split("=", 1)
        v = clean_env(k, v)
        if v is not None:
            env[k] = v
    args = clean_args(after)
    if not env and not args:
        return None
    return env, args


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    with tarfile.open(src) as tar:
        member = next(m for m in tar.getmembers() if m.isfile() and m.name.endswith(".json"))
        reports = json.load(tar.extractfile(member))
    newest = max(r.get("timestamp", 0) for r in reports)
    since = newest - YEARS * 365 * 86400
    works, total = collections.Counter(), collections.Counter()
    fixes = collections.defaultdict(collections.Counter)
    for r in reports:
        app = ((r.get("app") or {}).get("steam") or {}).get("appId")
        if not app or not str(app).isdigit() or r.get("timestamp", 0) < since:
            continue
        total[app] += 1
        resp = r.get("responses") or {}
        if resp.get("verdict") != "yes":
            continue
        works[app] += 1
        fx = fix_of(resp.get("launchOptions") or "")
        if fx:
            env, args = fx
            fixes[app][(tuple(sorted(env.items())), tuple(args))] += 1

    games = {}
    for app, counts in fixes.items():
        (env, args), k = counts.most_common(1)[0]
        if k < MIN_AGREE:
            continue
        g = {"w": works[app], "n": total[app]}
        if env:
            g["env"] = dict(env)
        if args:
            g["args"] = list(args)
        g["k"] = k
        games[app] = g

    day = lambda t: datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%d")
    doc = {
        "notice": "Contains information from ProtonDB (https://www.protondb.com), which is made available "
                  "here under the Open Database License (ODbL).",
        "source": "https://github.com/bdefore/protondb-data reports/" + os.path.basename(src),
        "license": "ODbL-1.0 https://opendatacommons.org/licenses/odbl/1-0/ (database), "
                   "DbCL-1.0 https://opendatacommons.org/licenses/dbcl/1-0/ (contents)",
        "made_by": "tools/protondb-hints.py in https://github.com/project-barry/pb-os",
        "reports_since": day(since),
        "reports_until": day(newest),
        "games": dict(sorted(games.items(), key=lambda kv: int(kv[0]))),
    }
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")
    print(f"{out}: {len(games)} games with a fix, {os.path.getsize(out) // 1024} KB "
          f"(reports {day(since)} .. {day(newest)})")


if __name__ == "__main__":
    main()

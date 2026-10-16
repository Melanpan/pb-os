#!/usr/bin/env python3
"""Decky backend: Barry Launcher (AYN Thor, SteamOS-ARM; the plugin's
folder is still dual-screen).

One switch for the bottom screen, and each screen's brightness, as on AYN's
Android: Steam's brightness slider sets both screens (sm8550-thor-backlightd
follows it), the top and bottom sliders here one each (SCREENS, a slider
position 0-1 per screen). Turning the bottom screen off powers its
backlight down and disables its touchscreen until it is turned back on.
The daemon applies and enforces STATE and SCREENS; this only records them.

Also the stick lighting's dimmer (20-100%): sm8550-thor-controlsd lights the
LEDs at the dashboard's brightness times it.

Also stands in Barry Launcher's Keyboard app for Steam's on-screen keyboard:
the frontend asks whether it can (the bottom screen is on and Barry
Launcher's barry_launcher_shelld answers), and opens it there instead of
Steam's. Should Steam's gamescope be showing a dual-screen game's second
window on the bottom screen (GAMESCOPE_BOTTOM_SCREEN_SHOWING), it hands the
screen to Barry Launcher while the keyboard is open (sm8550-thor-backlightd);
if it has not within YIELD_WAIT_S the keyboard would type unseen, so it is
closed again and Steam's keyboard shows on the top screen instead.

Also "Two-screen emulators": the user's own emulators (none ship with PB-OS)
set to draw their second screen in a window of its own, which gamescope's
dual-screen branch shows on the bottom screen. Only their config files
change, a few keys each (TWO_SCREEN); the values found before are kept in
TWO_SCREEN_STATE and put back when the switch goes off. Nothing changes
while one of them runs: they write their settings back when they quit.
The switch is on until the user turns it off, and while it is on, an
emulator found without the settings (installed later, or reset by EmuDeck)
gets them within TWO_SCREEN_CHECK_S once it is not running. DS games that
Steam starts in RetroArch (one window for both screens) are only counted,
for a note: switching them to the standalone melonDS is the user's call.

Also Barry Launcher's home screen: which apps it shows, in what order
(barry_launcher_shelld keeps them in ~/.config/barry_launcher/home.json),
and user apps: an archive picked in Decky's file picker is installed by
barry_launcher_shelld (as the user, into ~/.local/share/barry_launcher/apps),
and a user app can be removed. And apps that open with games: the
frontend watches Steam's running games (Steam's and non-Steam ones alike,
by appid) and tells barry_launcher_shelld, which keeps the links.

Also Barry Launcher's themes (skins, barry_launcher_statsd lists them): the
one in use; adding one (an archive, or a theme folder's skin.json) from a
USB drive when one is plugged in, else from Downloads (SteamOS only
automounts ext4, so a FAT or exFAT stick is mounted read-only here for the
pick, and unmounted after), unpacked safely into
~/.local/share/barry_launcher/skins as the user; and removing an added
theme (the built-in ones stay, as the built-in apps do).
"""
from __future__ import annotations

import asyncio
import glob
import json
import os
import pwd
import re
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
from typing import Any

import decky

TOP = "/sys/class/backlight/ae96000.dsi.0"
BOTTOM = "/sys/class/backlight/ae94000.dsi.0"
STATE = "/var/lib/steamos-sm8550/thor-bottom-screen"
SCREENS = "/var/lib/steamos-sm8550/thor-screen-brightness.json"
CONTROLS = "/run/sm8550-thor/controls.sock"
RGB_DIM_MIN = 20
SHELLD = "http://127.0.0.1:47824"
YIELD_WAIT_S = 2.0
TWO_SCREEN_STATE = "/var/lib/steamos-sm8550/thor-two-screen.json"
TWO_SCREEN_CHECK_S = 30
_two_screen_lock = threading.Lock()

# Each emulator: config files (globs under the user's home, AppImage and
# Flatpak), their kind, and (section, key, value) to set. Kinds: "ini" (Qt,
# key=value), "toml" (key = value), "xml" (<key>value</key>).
TWO_SCREEN = {
    "Azahar": {
        "configs": [".config/azahar-emu/qt-config.ini", ".var/app/*/config/azahar-emu/qt-config.ini"],
        "kind": "ini", "process": r"azahar",
        # View > Screen Layout > Separate Windows (LayoutOption 4).
        "keys": [("Layout", "layout_option", "4"), ("Layout", "layout_option\\default", "false")],
    },
    "Lime3DS": {
        "configs": [".config/lime3ds-emu/qt-config.ini", ".var/app/*/config/lime3ds-emu/qt-config.ini"],
        "kind": "ini", "process": r"lime3ds",
        "keys": [("Layout", "layout_option", "4"), ("Layout", "layout_option\\default", "false")],
    },
    "Citra": {
        "configs": [".config/citra-emu/qt-config.ini", ".var/app/*/config/citra-emu/qt-config.ini"],
        "kind": "ini", "process": r"citra",
        "keys": [("Layout", "layout_option", "4"), ("Layout", "layout_option\\default", "false")],
    },
    "melonDS": {
        "configs": [".config/melonDS/melonDS.toml", ".var/app/*/config/melonDS/melonDS.toml"],
        "kind": "toml", "process": r"melonds",
        # A second window, kept across launches: the top screen in the first
        # (screenSizing_TopOnly 4), the bottom one in the second (BotOnly 5).
        # Both with the Natural layout (0): Hybrid (3) ignores the sizing and
        # draws a big screen with both small ones in each window.
        "keys": [("Instance0.Window0", "ScreenLayout", "0"),
                 ("Instance0.Window0", "ScreenSizing", "4"),
                 ("Instance0.Window1", "Enabled", "true"),
                 ("Instance0.Window1", "ScreenLayout", "0"),
                 ("Instance0.Window1", "ScreenSizing", "5")],
    },
    "Cemu": {
        "configs": [".config/Cemu/settings.xml", ".var/app/*/config/Cemu/settings.xml"],
        "kind": "xml", "process": r"cemu",
        # Options > Separate GamePad view.
        "keys": [(None, "open_pad", "true")],
    },
}


def _steam_display_user() -> str | None:
    """The user running Game Mode's main gamescope (Steam's :0), if any."""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                args = fh.read().split(b"\0")
            if os.path.basename(args[0]) == b"gamescope" and b"--steam" in args:
                return pwd.getpwuid(os.stat(f"/proc/{pid}").st_uid).pw_name
        except (OSError, KeyError):
            continue
    return None


def _bottom_screen_showing() -> bool:
    """Steam's gamescope draws a game's window on the bottom screen."""
    user = _steam_display_user()
    if user is None:
        return False
    try:
        r = subprocess.run(
            ["runuser", "-u", user, "--", "env", "DISPLAY=:0", "xprop", "-root",
             "GAMESCOPE_BOTTOM_SCREEN_SHOWING"],
            capture_output=True, text=True, timeout=3)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.stdout.strip().endswith("= 1")


def _shelld_post(path: str, body: dict) -> dict:
    req = urllib.request.Request(f"{SHELLD}{path}", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=2) as r:
        return json.load(r)


def _shelld_answer(path: str, body: dict, timeout: float = 2) -> dict:
    """barry_launcher_shelld's JSON answer, an error's included."""
    req = urllib.request.Request(f"{SHELLD}{path}", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.load(r)
    except urllib.error.HTTPError as err:
        try:
            return json.load(err)
        except ValueError:
            return {"ok": False, "error": f"Barry Launcher answered {err.code}."}


def _is_on() -> bool:
    try:
        with open(STATE, encoding="utf-8") as fh:
            return fh.read().strip() != "off"
    except OSError:
        return True


def _save(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(tmp, path)


def _screens() -> dict:
    try:
        with open(SCREENS, encoding="utf-8") as fh:
            d = json.load(fh)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def _brightness() -> dict[str, int] | None:
    """Each screen's slider, 0-100; None until the daemon has set them."""
    d = _screens()
    try:
        return {k: round(max(0.0, min(1.0, float(d[k]))) * 100) for k in ("top", "bottom")}
    except (KeyError, ValueError, TypeError):
        return None


def _controlsd(req: dict) -> dict | None:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2)
            sock.connect(CONTROLS)
            sock.sendall(json.dumps(req).encode() + b"\n")
            data = b""
            while b"\n" not in data:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
        reply = json.loads(data)
        return reply if isinstance(reply, dict) and "error" not in reply else None
    except (OSError, ValueError) as err:
        decky.logger.warning(f"sm8550-thor-controlsd: {err}")
        return None


# Two-screen emulators --------------------------------------------------

def _user_home() -> str:
    home = getattr(decky, "DECKY_USER_HOME", None)
    if home:
        return home
    user = _steam_display_user()
    return pwd.getpwnam(user).pw_dir if user else os.path.expanduser("~")


def _sections(lines: list[str]) -> list[tuple[str | None, int, int]]:
    """(name, first line, end) of each [section] of an ini/toml file; None
    for the lines before the first."""
    out, name, start = [], None, 0
    for i, line in enumerate(lines):
        m = re.match(r"\s*\[([^\]]+)\]\s*$", line)
        if m:
            out.append((name, start, i))
            name, start = m.group(1), i + 1
    out.append((name, start, len(lines)))
    return out


def _key_line(line: str, key: str) -> bool:
    return "=" in line and line.split("=", 1)[0].strip() == key


def _eol(text: str) -> str:
    """The file's line ending, kept as found (Cemu writes CRLF)."""
    return "\r\n" if "\r\n" in text else "\n"


def _get(text: str, kind: str, section: str | None, key: str) -> str | None:
    """The key's whole line (ini/toml) or element (xml), None if absent."""
    if kind == "xml":
        m = re.search(rf"^[ \t]*<{re.escape(key)}>.*?</{re.escape(key)}>[ \t]*(?=\r?$)", text, re.M)
        return m.group(0) if m else None
    lines = text.split(_eol(text))
    for name, start, end in _sections(lines):
        if name == section:
            for line in lines[start:end]:
                if _key_line(line, key):
                    return line
    return None


def _line_for(kind: str, key: str, value: str) -> str:
    return {"ini": f"{key}={value}", "toml": f"{key} = {value}", "xml": f"    <{key}>{value}</{key}>"}[kind]


def _put(text: str, kind: str, section: str | None, key: str, line: str | None) -> str:
    """Sets the key's line (None: removes it), adding the section if needed."""
    eol = _eol(text)
    if kind == "xml":
        m = re.search(rf"^[ \t]*<{re.escape(key)}>.*?</{re.escape(key)}>[ \t]*(?:\r?\n)?", text, re.M)
        if m:
            return text[:m.start()] + ((line + eol) if line is not None else "") + text[m.end():]
        if line is None:
            return text
        at = text.find("</content>")
        return text if at < 0 else text[:at] + line + eol + text[at:]
    lines = text.split(eol)
    for name, start, end in _sections(lines):
        if name != section:
            continue
        for i in range(start, end):
            if _key_line(lines[i], key):
                if line is None:
                    del lines[i]
                else:
                    lines[i] = line
                return eol.join(lines)
        if line is None:
            return text
        # After the section's last non-blank line.
        at = end
        while at > start and not lines[at - 1].strip():
            at -= 1
        lines.insert(at, line)
        return eol.join(lines)
    if line is None:
        return text
    body = text.rstrip("\r\n")
    return f"{body}{eol}{eol}[{section}]{eol}{line}{eol}"


def _has_section(text: str, section: str | None) -> bool:
    return section is None or any(name == section for name, _, _ in _sections(text.split(_eol(text))))


def _drop_added_section(text: str, section: str) -> str:
    """Takes away a section _put appended at the end, once it is empty."""
    eol = _eol(text)
    tail = f"{eol}{eol}[{section}]{eol}"
    return text[:-len(tail)] + eol if text.endswith(tail) else text


def _write_as_owner(path: str, text: str) -> None:
    st = os.stat(path)
    tmp = path + ".dual-screen.tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    os.chown(tmp, st.st_uid, st.st_gid)
    os.chmod(tmp, st.st_mode & 0o7777)
    os.replace(tmp, path)


def _two_screen_files() -> list[tuple[str, str, dict]]:
    """(emulator, config path, spec) for each config file present."""
    home = _user_home()
    out = []
    for name, spec in TWO_SCREEN.items():
        for pattern in spec["configs"]:
            for path in sorted(glob.glob(os.path.join(home, pattern))):
                out.append((name, path, spec))
    return out


def _two_screen_running() -> list[str]:
    """Emulators of TWO_SCREEN running now."""
    found = set()
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                args = fh.read().split(b"\0")
        except OSError:
            continue
        exe = os.path.basename(args[0].decode(errors="replace")).lower()
        for name, spec in TWO_SCREEN.items():
            if re.search(spec["process"], exe):
                found.add(name)
    return sorted(found)


def _load_two_screen() -> dict:
    try:
        with open(TWO_SCREEN_STATE, encoding="utf-8") as fh:
            d = json.load(fh)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def _two_screen_applied(text: str, spec: dict) -> bool:
    return all(_get(text, spec["kind"], sec, key) is not None and
               _get(text, spec["kind"], sec, key).strip() == _line_for(spec["kind"], key, value).strip()
               for sec, key, value in spec["keys"])


def _vdf_map(data: bytes, i: int) -> tuple[dict, int]:
    """One map of Steam's binary VDF from data[i]; (map, index after it)."""
    out: dict = {}
    while i < len(data):
        kind = data[i]
        i += 1
        if kind == 0x08:
            return out, i
        end = data.index(b"\0", i)
        key = data[i:end].decode(errors="replace")
        i = end + 1
        if kind == 0x00:
            out[key], i = _vdf_map(data, i)
        elif kind == 0x01:
            end = data.index(b"\0", i)
            out[key] = data[i:end].decode(errors="replace")
            i = end + 1
        elif kind in (0x02, 0x03, 0x04, 0x06):
            i += 4
        elif kind in (0x07, 0x0a):
            i += 8
        else:
            raise ValueError(f"VDF type {kind:#x}")
    return out, i


def _ds_on_retroarch() -> int:
    """Steam shortcuts (Steam ROM Manager's) that run DS games in RetroArch,
    which draws both screens in one window. Only read, never changed: the
    user switches them to the standalone melonDS themselves."""
    home = _user_home()
    seen, count = set(), 0
    for path in glob.glob(os.path.join(home, ".local/share/Steam/userdata/*/config/shortcuts.vdf")):
        real = os.path.realpath(path)
        if real in seen:
            continue
        seen.add(real)
        try:
            with open(real, "rb") as fh:
                shortcuts, _ = _vdf_map(fh.read(), 0)
        except (OSError, ValueError):
            continue
        for entry in shortcuts.get("shortcuts", {}).values():
            if not isinstance(entry, dict):
                continue
            cmd = " ".join(str(entry.get(k, "")) for k in ("exe", "Exe", "LaunchOptions")).lower()
            if (("retroarch" in cmd or "libretro" in cmd) and
                    (re.search(r"(melonds|desmume)\w*_libretro", cmd) or re.search(r"\.nds\b", cmd))):
                count += 1
    return count


def _two_screen_status() -> dict[str, Any]:
    state = _load_two_screen()
    emulators = []
    for name, path, spec in _two_screen_files():
        try:
            with open(path, encoding="utf-8", newline="") as fh:
                applied = _two_screen_applied(fh.read(), spec)
        except OSError:
            continue
        emulators.append({"name": name, "path": path, "twoScreens": applied})
    return {"enabled": _two_screen_enabled(state), "emulators": emulators,
            "running": _two_screen_running(), "dsOnRetroArch": _ds_on_retroarch()}


def _two_screen_enabled(state: dict) -> bool:
    """On unless the user turned it off."""
    return bool(state.get("enabled", True))


def _two_screen_file(path: str, spec: dict, enabled: bool, originals: dict) -> bool:
    """Sets (or puts back) one config file's keys; True if it changed."""
    try:
        with open(path, encoding="utf-8", newline="") as fh:
            text = fh.read()
    except OSError:
        return False
    kind, new = spec["kind"], text
    if enabled:
        saved = originals.setdefault(path, {})
        for sec, key, value in spec["keys"]:
            if kind != "xml" and not _has_section(new, sec):
                saved.setdefault(f"{sec}\t", "added")
            saved.setdefault(f"{sec}\t{key}", _get(text, kind, sec, key))
            new = _put(new, kind, sec, key, _line_for(kind, key, value))
    elif path in originals:
        saved = originals.pop(path)
        for sec, key, _ in spec["keys"]:
            if f"{sec}\t{key}" in saved:
                new = _put(new, kind, sec, key, saved[f"{sec}\t{key}"])
        for sec in {sec for sec, _, _ in spec["keys"]}:
            if saved.get(f"{sec}\t") == "added":
                new = _drop_added_section(new, sec)
    if new == text:
        return False
    _write_as_owner(path, new)
    return True


def _set_two_screen(enabled: bool) -> dict[str, Any]:
    """The switch: sets (or puts back) every present emulator's keys;
    refuses while one of them runs. The reply says what happened."""
    with _two_screen_lock:
        files = _two_screen_files()
        running = [n for n in _two_screen_running() if any(n == f[0] for f in files)]
        if running:
            return {"ok": False, "error": f"Close {', '.join(running)} first: it writes its settings back when it quits."}
        state = _load_two_screen()
        originals: dict = state.get("originals", {})
        changed = [name for name, path, spec in files if _two_screen_file(path, spec, enabled, originals)]
        _save(TWO_SCREEN_STATE, json.dumps({"enabled": enabled, "originals": originals}, indent=1) + "\n")
    decky.logger.info(f"two-screen emulators {'on' if enabled else 'off'}: {', '.join(changed) or 'nothing to change'}")
    return {"ok": True, "changed": changed}


def _keep_two_screen() -> list[str]:
    """While the switch is on, gives the settings to each emulator found
    without them that is not running; returns those changed."""
    with _two_screen_lock:
        state = _load_two_screen()
        if not _two_screen_enabled(state):
            return []
        running = set(_two_screen_running())
        originals: dict = state.get("originals", {})
        changed = []
        for name, path, spec in _two_screen_files():
            if name in running:
                continue
            if _two_screen_file(path, spec, True, originals):
                changed.append(name)
        if changed or "enabled" not in state:
            _save(TWO_SCREEN_STATE, json.dumps({"enabled": True, "originals": originals}, indent=1) + "\n")
    if changed:
        decky.logger.info(f"two-screen emulators: set {', '.join(changed)}")
    return changed


# Themes (Barry Launcher skins) -----------------------------------------

STATSD = "http://127.0.0.1:47823"
# Used when the theme in use is removed: SteamOS, else Barry Launcher's own.
SKINS_FALLBACK = ("steamos", "ayn")
SKIN_FILES_MAX = 400
SKIN_BYTES_MAX = 64 * 1024 * 1024
USB_MOUNTS = "/run/barry-launcher/usb"


def _skins_dir() -> str:
    return os.path.join(_user_home(), ".local/share/barry_launcher/skins")


def _owner() -> tuple[int, int]:
    st = os.stat(_user_home())
    return st.st_uid, st.st_gid


def _make_user_dirs(path: str) -> None:
    """Make path and any missing parents under the user's home, theirs."""
    uid, gid = _owner()
    missing = []
    p = path
    while not os.path.isdir(p):
        missing.append(p)
        p = os.path.dirname(p)
    for d in reversed(missing):
        os.mkdir(d, 0o755)
        os.chown(d, uid, gid)


def _statsd(method: str, path: str, body: dict | None = None) -> dict:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f"{STATSD}{path}", data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data else {})
    with urllib.request.urlopen(req, timeout=2) as r:
        return json.load(r)


def _skins_state() -> dict[str, Any]:
    """Every theme Barry Launcher has (barry_launcher_statsd lists them), the
    one in use, and each theme's color variants with the one chosen (the
    first when none is)."""
    listed = _statsd("GET", "/skins")
    chosen = _statsd("GET", "/settings").get("skinVariants") or {}
    current = (listed.get("current") or {}).get("name") or ""
    skins = []
    for s in listed.get("skins", []):
        builtin = not str(s.get("dir", "")).startswith(_skins_dir() + "/")
        variants = [v for v in s.get("variants") or [] if isinstance(v, dict) and v.get("id")]
        ids = [v["id"] for v in variants]
        skins.append({"name": s["name"], "title": s.get("title") or s["name"],
                      "author": s.get("author", ""), "builtin": builtin,
                      "variants": variants,
                      "variant": chosen.get(s["name"]) if chosen.get(s["name"]) in ids else (ids[0] if ids else "")})
    return {"ok": True, "current": current, "skins": skins}


def _skin_name(raw: str) -> str:
    name = re.sub(r"[^a-z0-9_-]+", "-", raw.lower()).strip("-")
    return name[:40]


def _usb_partitions() -> list[dict]:
    """Partitions on USB drives: lsblk's PATH, FSTYPE, LABEL, MOUNTPOINTS."""
    out = subprocess.run(["lsblk", "-J", "-o", "PATH,TRAN,FSTYPE,LABEL,MOUNTPOINTS,TYPE"],
                         capture_output=True, text=True, timeout=10, check=True).stdout
    found = []

    def walk(devs, tran):
        for d in devs or []:
            t = d.get("tran") or tran
            if t == "usb" and d.get("type") == "part" and d.get("fstype"):
                found.append(d)
            walk(d.get("children"), t)
    walk(json.loads(out).get("blockdevices"), None)
    # A whole USB disk with a file system and no partitions.
    if not found:
        for d in json.loads(out).get("blockdevices") or []:
            if d.get("tran") == "usb" and d.get("fstype") and not d.get("children"):
                found.append(d)
    return found


def _usb_mount() -> dict[str, Any]:
    """A folder to pick a theme from on the USB drive: where it is mounted
    already, or mounted read-only here for the pick (_usb_unmount after)."""
    parts = _usb_partitions()
    if not parts:
        return {"ok": False, "error": "No USB drive found. Plug one in, then try again."}
    for p in parts:
        points = [m for m in (p.get("mountpoints") or []) if m]
        if points:
            return {"ok": True, "path": points[0]}
    p = parts[0]
    point = os.path.join(USB_MOUNTS, _skin_name(p.get("label") or os.path.basename(p["path"])) or "usb")
    os.makedirs(point, exist_ok=True)
    r = subprocess.run(["mount", "-o", "ro,nosuid,nodev,noexec", p["path"], point],
                       capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        return {"ok": False, "error": f"Could not open the USB drive ({r.stderr.strip() or r.returncode})."}
    return {"ok": True, "path": point}


def _usb_unmount() -> None:
    """Unmount and remove what _usb_mount mounted (never other mounts)."""
    if not os.path.isdir(USB_MOUNTS):
        return
    for name in os.listdir(USB_MOUNTS):
        point = os.path.join(USB_MOUNTS, name)
        if os.path.ismount(point):
            subprocess.run(["umount", point], capture_output=True, timeout=30)
        if not os.path.ismount(point):
            try:
                os.rmdir(point)
            except OSError:
                pass


def _skin_source(path: str, work: str) -> tuple[str, str]:
    """The theme folder in what was picked (an archive, or a theme's
    skin.json), unpacked into work if needed, and its name."""
    import shutil, tarfile, zipfile
    path = os.path.realpath(path)
    if os.path.basename(path) == "skin.json":
        src = os.path.dirname(path)
        return src, os.path.basename(src)
    base = os.path.basename(path)
    stem = re.sub(r"(\.tar)?\.[^.]+$", "", base)
    out = os.path.join(work, "x")
    os.mkdir(out)
    total = files = 0
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                name = info.filename
                if name.startswith("/") or ".." in name.split("/"):
                    raise ValueError(f"unsafe path in the archive: {name}")
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError("the archive has links in it")
                total += info.file_size
                files += 1
                if total > SKIN_BYTES_MAX or files > SKIN_FILES_MAX:
                    raise ValueError("the archive is too big for a theme")
            z.extractall(out)
    elif tarfile.is_tarfile(path):
        with tarfile.open(path) as t:
            members = t.getmembers()
            for m in members:
                if m.name.startswith("/") or ".." in m.name.split("/"):
                    raise ValueError(f"unsafe path in the archive: {m.name}")
                if not (m.isfile() or m.isdir()):
                    raise ValueError("the archive has links or special files in it")
                total += m.size
                files += 1
                if total > SKIN_BYTES_MAX or files > SKIN_FILES_MAX:
                    raise ValueError("the archive is too big for a theme")
            t.extractall(out, members=members)
    else:
        raise ValueError("pick a theme's .zip or .tar.gz, or the skin.json in a theme's folder")
    # skin.json at the top, or in the one folder at the top.
    if os.path.isfile(os.path.join(out, "skin.json")):
        return out, stem
    entries = [e for e in os.listdir(out) if not e.startswith(".") and e != "__MACOSX"]
    if len(entries) == 1 and os.path.isfile(os.path.join(out, entries[0], "skin.json")):
        return os.path.join(out, entries[0]), entries[0]
    raise ValueError("no skin.json in it: not a Barry Launcher theme")


def _check_skin_dir(src: str) -> dict[str, Any]:
    total = files = 0
    code = False
    for root, dirs, names in os.walk(src):
        for n in dirs + names:
            p = os.path.join(root, n)
            if os.path.islink(p):
                raise ValueError("the theme has links in it")
        for n in names:
            p = os.path.join(root, n)
            if not os.path.isfile(p):
                raise ValueError("the theme has special files in it")
            total += os.path.getsize(p)
            files += 1
            code = code or n.endswith((".qml", ".js", ".mjs"))
    if total > SKIN_BYTES_MAX or files > SKIN_FILES_MAX:
        raise ValueError("too big for a theme")
    with open(os.path.join(src, "skin.json"), encoding="utf-8") as fh:
        meta = json.load(fh)
    if not isinstance(meta, dict):
        raise ValueError("skin.json is not a theme's")
    return {"title": str(meta.get("title") or ""), "author": str(meta.get("author") or ""), "code": code}


def _install_skin(path: str, check_only: bool) -> dict[str, Any]:
    import shutil, tempfile
    skins = _skins_dir()
    _make_user_dirs(skins)
    work = tempfile.mkdtemp(prefix=".install-", dir=skins)
    try:
        src, raw_name = _skin_source(path, work)
        info = _check_skin_dir(src)
        name = _skin_name(raw_name) or _skin_name(info["title"])
        if not name:
            raise ValueError("the theme has no usable name")
        title = info["title"] or name
        dest = os.path.join(skins, name)
        theme = {"name": name, "title": title, "author": info["author"], "code": info["code"],
                 "replaces": os.path.isdir(dest)}
        if check_only:
            return {"ok": True, "theme": theme}
        stage = os.path.join(work, "stage")
        shutil.copytree(src, stage, symlinks=False)
        uid, gid = _owner()
        for root, dirs, names in os.walk(stage):
            os.chown(root, uid, gid)
            os.chmod(root, 0o755)
            for n in names:
                os.chown(os.path.join(root, n), uid, gid)
                os.chmod(os.path.join(root, n), 0o644)
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.rename(stage, dest)
        return {"ok": True, "theme": theme}
    except (OSError, ValueError, EOFError) as err:
        return {"ok": False, "error": f"Could not add the theme: {err}"}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _remove_skin(name: str) -> dict[str, Any]:
    """Delete a theme the user added (never a built-in one); if it was in
    use, Barry Launcher changes to SKINS_FALLBACK."""
    import shutil
    state = _skins_state()
    theme = next((t for t in state["skins"] if t["name"] == name), None)
    if not theme or theme["builtin"]:
        return {"ok": False, "error": "Only a theme you added can be removed."}
    shutil.rmtree(os.path.join(_skins_dir(), name))
    if state["current"] == name:
        names = {t["name"] for t in state["skins"]} - {name}
        _statsd("POST", "/settings", {"skin": next((f for f in SKINS_FALLBACK if f in names), "ayn")})
    return {"ok": True, **_skins_state()}


def _rgb_dimmer(state: dict | None) -> int | None:
    light = state.get("lighting") if state else None
    return light.get("dimmer", 100) if light else None


class Plugin:
    async def _main(self) -> None:
        decky.logger.info("Barry Launcher plugin ready")
        self._keeper = asyncio.get_event_loop().create_task(self._keep_two_screen())

    async def _unload(self) -> None:
        keeper = getattr(self, "_keeper", None)
        if keeper:
            keeper.cancel()

    async def _keep_two_screen(self) -> None:
        while True:
            try:
                await asyncio.to_thread(_keep_two_screen)
            except Exception as err:  # never let the loop die
                decky.logger.warning(f"two-screen emulators: {err}")
            await asyncio.sleep(TWO_SCREEN_CHECK_S)

    async def get_state(self, **_: Any) -> dict[str, Any]:
        ok = os.path.exists(f"{TOP}/brightness") and os.path.exists(f"{BOTTOM}/brightness")
        return {"supported": ok, "on": _is_on(), "brightness": _brightness(),
                "rgbDimmer": _rgb_dimmer(_controlsd({"op": "get"}))}

    async def set_bottom_screen(self, on: bool = True, **_: Any) -> bool:
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        tmp = STATE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            fh.write("on" if on else "off")
        os.replace(tmp, STATE)
        decky.logger.info(f"bottom screen {'on' if on else 'off'}")
        return _is_on()

    async def set_brightness(self, screen: str = "top", value: int = 100, **_: Any) -> dict[str, int] | None:
        """One screen's brightness slider (0-100), as on Android."""
        d = _screens()
        if screen in ("top", "bottom") and "top" in d and "bottom" in d:
            d[screen] = max(0, min(100, int(value))) / 100
            _save(SCREENS, json.dumps(d) + "\n")
        return _brightness()

    async def set_rgb_dimmer(self, value: int = 100, **_: Any) -> int | None:
        v = max(RGB_DIM_MIN, min(100, int(value)))
        return _rgb_dimmer(_controlsd({"op": "set", "lighting": {"dimmer": v}}))

    async def get_two_screen(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(_two_screen_status)

    async def set_two_screen(self, enabled: bool = True, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(_set_two_screen, bool(enabled))

    async def barry_keyboard_available(self, **_: Any) -> bool:
        """The bottom screen is on and Barry Launcher can open its keyboard."""
        if not _is_on():
            return False
        try:
            with urllib.request.urlopen(f"{SHELLD}/apps", timeout=0.5) as r:
                # The Trackpad app has the keyboard (and "keyboard" opens it so).
                apps = json.load(r)
                return "trackpad" in apps or "keyboard" in apps
        except (OSError, ValueError, TypeError):
            return False

    async def get_barry_apps(self, **_: Any) -> dict[str, Any]:
        """Barry Launcher's home apps in order, each shown or hidden."""
        try:
            def get() -> dict:
                with urllib.request.urlopen(f"{SHELLD}/layout", timeout=1) as r:
                    return json.load(r)
            return {"ok": True, **await asyncio.to_thread(get)}
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def set_barry_apps(self, order: list | None = None, hidden: list | None = None,
                             **_: Any) -> dict[str, Any]:
        try:
            r = await asyncio.to_thread(_shelld_post, "/layout",
                                        {"order": list(order or []), "hidden": list(hidden or [])})
            return {"ok": "apps" in r, **r}
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def barry_apps_folder(self, **_: Any) -> str:
        """Where the file picker starts: Downloads, or home."""
        home = _user_home()
        downloads = os.path.join(home, "Downloads")
        return downloads if os.path.isdir(downloads) else home

    async def check_barry_app(self, path: str = "", **_: Any) -> dict[str, Any]:
        """What the archive at path holds, if it would install."""
        try:
            return await asyncio.to_thread(_shelld_answer, "/apps/check", {"path": str(path)}, 30)
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err}); is the bottom screen on?"}

    async def install_barry_app(self, path: str = "", **_: Any) -> dict[str, Any]:
        """Install (or update) the app in the archive at path."""
        try:
            return await asyncio.to_thread(_shelld_answer, "/apps/install", {"path": str(path)}, 60)
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err}); is the bottom screen on?"}

    async def remove_barry_app(self, app: str = "", **_: Any) -> dict[str, Any]:
        try:
            return await asyncio.to_thread(_shelld_answer, "/apps/remove", {"app": str(app)}, 10)
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def get_themes(self, **_: Any) -> dict[str, Any]:
        """Barry Launcher's themes (skins) and the one in use."""
        try:
            return await asyncio.to_thread(_skins_state)
        except (OSError, ValueError, KeyError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def set_theme(self, name: str = "", **_: Any) -> dict[str, Any]:
        """Use the theme name; Barry Launcher changes its look at once."""
        try:
            await asyncio.to_thread(_statsd, "POST", "/settings", {"skin": str(name)})
            return await asyncio.to_thread(_skins_state)
        except (OSError, ValueError, KeyError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def set_theme_color(self, name: str = "", variant: str = "", **_: Any) -> dict[str, Any]:
        """Use the theme's color variant; Barry Launcher recolors at once."""
        try:
            await asyncio.to_thread(_statsd, "POST", "/settings", {"skinVariants": {str(name): str(variant)}})
            return await asyncio.to_thread(_skins_state)
        except urllib.error.HTTPError as err:
            return {"ok": False, "error": f"That color isn't in the theme ({err.code})."}
        except (OSError, ValueError, KeyError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def themes_folder(self, **_: Any) -> dict[str, Any]:
        """Where the file picker starts: a USB drive when one is plugged in
        (mounted read-only for the pick if SteamOS has not mounted it), else
        Downloads."""
        try:
            usb = await asyncio.to_thread(_usb_mount)
            if usb.get("ok"):
                return usb
            if not str(usb.get("error", "")).startswith("No USB drive"):
                decky.logger.warning(f"themes: {usb.get('error')}")
        except (OSError, ValueError, subprocess.SubprocessError) as err:
            decky.logger.warning(f"themes: cannot look for a USB drive: {err}")
        home = _user_home()
        downloads = os.path.join(home, "Downloads")
        return {"ok": True, "path": downloads if os.path.isdir(downloads) else home}

    async def themes_done(self, **_: Any) -> None:
        """The pick is over: unmount the USB drive if this mounted it."""
        await asyncio.to_thread(_usb_unmount)

    async def check_theme(self, path: str = "", **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(_install_skin, str(path), True)

    async def install_theme(self, path: str = "", **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(_install_skin, str(path), False)

    async def remove_theme(self, name: str = "", **_: Any) -> dict[str, Any]:
        try:
            return await asyncio.to_thread(_remove_skin, str(name))
        except (OSError, ValueError, KeyError) as err:
            return {"ok": False, "error": f"Could not remove the theme ({err})."}

    async def get_game_links(self, **_: Any) -> dict[str, Any]:
        try:
            def get() -> dict:
                with urllib.request.urlopen(f"{SHELLD}/game-links", timeout=1) as r:
                    return json.load(r)
            return {"ok": True, **await asyncio.to_thread(get)}
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def set_game_links(self, links: list | None = None, **_: Any) -> dict[str, Any]:
        try:
            r = await asyncio.to_thread(_shelld_answer, "/game-links", {"links": list(links or [])})
            return {"ok": "links" in r, **r}
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def game_event(self, game: str = "", running: bool = False, **_: Any) -> dict[str, Any]:
        """A game started or stopped: Barry Launcher opens or closes its app."""
        try:
            return await asyncio.to_thread(_shelld_answer, "/game-event",
                                           {"game": str(game), "running": bool(running)}, 5)
        except (OSError, ValueError) as err:
            return {"ok": False, "error": str(err)}

    async def open_barry_keyboard(self, **_: Any) -> bool:
        """Opens Barry Launcher's keyboard; False when it cannot be seen, for
        Steam's keyboard then."""
        try:
            if not _shelld_post("/launch", {"app": "keyboard"}).get("ok"):
                return False
        except (OSError, ValueError, AttributeError) as err:
            decky.logger.warning(f"cannot open Barry Launcher's keyboard: {err}")
            return False
        deadline = time.monotonic() + YIELD_WAIT_S
        while await asyncio.to_thread(_bottom_screen_showing):
            if time.monotonic() >= deadline:
                decky.logger.warning("the bottom screen stayed with the game; Steam's keyboard instead")
                try:
                    _shelld_post("/close", {"app": "keyboard"})
                except (OSError, ValueError) as err:
                    decky.logger.warning(f"cannot close Barry Launcher's keyboard: {err}")
                return False
            await asyncio.sleep(0.1)
        return True

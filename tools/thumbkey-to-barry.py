#!/usr/bin/env python3
"""Turn a Thumb-Key layout (github.com/dessalines/thumb-key, a Kotlin file
under app/src/main/java/com/dessalines/thumbkey/keyboards/) into the "grid"
of a Barry Launcher keyboard pack (keyboard.json; see
sm8550-overlay/usr/share/barry_launcher/keyboards/README.md).

Only the letter keys come over: each key's center and the text its swipes
type (and the swipe that turns Shift on, as "shift"). Barry adds its own
side column and bottom row (123, Shift, Backspace, space, Enter), so
Thumb-Key's emoji, numeric, backspace, space and return keys are left out,
as are its other actions (caps lock, cursor moves).

Usage: thumbkey-to-barry.py LAYOUT.kt [VAL_NAME]   (default: the first
       KeyboardC in the file, e.g. KB_EN_THUMBKEY_MAIN)
Prints the grid as JSON.
"""
import json
import re
import sys

DIRECTIONS = ("top", "topRight", "right", "bottomRight", "bottom", "bottomLeft", "left", "topLeft")


def balanced(text: str, start: int) -> int:
    """Index just past the parenthesis that closes the one at start."""
    depth, i, quote = 0, start, False
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == '"':
                quote = False
        elif c == '"':
            quote = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced parentheses")


def kotlin_string(s: str) -> str:
    return json.loads('"' + s.replace("\\$", "$") + '"')


def args_of(block: str) -> dict:
    """The top-level `name = value` arguments of a call's (...)."""
    inner = block[block.index("(") + 1:-1]
    out, depth, start, quote, i = {}, 0, 0, False, 0
    parts = []
    while i < len(inner):
        c = inner[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == '"':
                quote = False
        elif c == '"':
            quote = True
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(inner[start:i])
            start = i + 1
        i += 1
    parts.append(inner[start:])
    for p in parts:
        m = re.match(r"\s*(\w+)\s*=\s*(.*)\s*$", p, re.S)
        if m:
            out[m.group(1)] = m.group(2).strip()
    return out


def key_text(value: str):
    """What a KeyC types: its text, "shift" for ToggleShiftMode(true), or None."""
    m = re.match(r'KeyC\(\s*"((?:[^"\\]|\\.)*)"', value)
    if m:
        return kotlin_string(m.group(1))
    if value.startswith("KeyC(") and re.search(r"action\s*=\s*ToggleShiftMode\(true\)", value):
        return "shift"
    return None


def grid_of(text: str, name: str | None) -> list:
    m = re.search(r"val\s+%s\s*=\s*KeyboardC\(" % (re.escape(name) if name else r"\w+"), text)
    if not m:
        raise SystemExit(f"no KeyboardC {name or ''} in the file")
    board = text[m.end() - 1:balanced(text, m.end() - 1)]
    grid = []
    # Each row: a listOf(...) holding KeyItemC(...)s.
    for row_m in re.finditer(r"listOf\(\s*(KeyItemC\(|[A-Z_]+,)", board):
        start = row_m.start() + len("listOf")
        row_text = board[start:balanced(board, start)]
        keys = []
        for item in re.finditer(r"KeyItemC\(", row_text):
            block = row_text[item.start():balanced(row_text, item.end() - 1)]
            a = args_of(block)
            center = key_text(a.get("center", ""))
            if not center:
                continue
            key = {"center": center}
            for d in DIRECTIONS:
                if d in a:
                    t = key_text(a[d])
                    if t:
                        key[d] = t
            keys.append(key)
        if len(keys) == 3:
            grid.append(keys)
    return grid


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    with open(sys.argv[1], encoding="utf-8") as fh:
        text = fh.read()
    grid = grid_of(text, sys.argv[2] if len(sys.argv) > 2 else None)
    if len(grid) != 3:
        raise SystemExit(f"found {len(grid)} rows of 3 letter keys, not 3")
    print(json.dumps(grid, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

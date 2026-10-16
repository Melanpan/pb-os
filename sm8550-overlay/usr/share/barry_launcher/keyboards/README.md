# Barry Launcher keyboard packs

Barry Launcher's on-screen keyboards (the bottom screen's own and the
Trackpad app's) type in any language a keyboard pack describes. A pack is a
folder. Barry ships English here (`/usr/share/barry_launcher/keyboards/en`).
Add your own in:

    ~/.local/share/barry_launcher/keyboards/<id>/

`<id>` is the folder's name: letters, digits, `-` or `_` (`de`, `fr-azerty`,
`ru`). A pack of yours with the same id as a shipped one takes its place.

The Barry Launcher Decky plugin's Languages tab (the globe) does this for
you: "Add a language" opens a USB drive when one is plugged in, otherwise
Downloads. Pick the pack's `.zip` (or `.tar.gz`), with `keyboard.json` at
its top or in one folder at its top, or the `keyboard.json` in a pack's
folder. The id is that folder's name (or the archive's), in lowercase. The
tab also picks the language in use and removes packs you added. A pack with
a composer asks before it is added, since its code runs as you.

Barry picks a pack up as soon as it is copied in, changed or removed. No
restart is needed. With two or more packs, the keyboard gets a language key
(it shows the pack's label) next to Fn: tap it for the next language. The
space bar shows the language in use, and both keyboards remember it.

A pack Barry cannot read is left out, and the reason is logged:
`journalctl --user -u barry_launcher` in Game Mode,
`journalctl --user -u barry_launcher_desktop` in Desktop Mode.

## What goes in a pack

| File | Needed | What it does |
|---|---|---|
| `keyboard.json` | yes | The keys |
| `words.txt` | no | Glide typing, suggestions and autocorrect |
| `Composer.qml` | no | For languages whose letters combine, or come from a list of candidates |

## keyboard.json

```json
{
  "name": "Deutsch",
  "label": "DE",
  "rows": [
    ["q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "ü"],
    ["a", "s", "d", "f", "g", "h", "j", "k", "l", "ö", "ä"],
    ["y", "x", "c", "v", "b", "n", "m", "ß"]
  ],
  "more": {
    "a": ["à", "á", "â"],
    "e": ["é", "è", "ê"],
    "s": ["ß"]
  },
  "wordChars": "'-",
  "words": "words.txt"
}
```

- **`name`** (required): the language's name, shown on the space bar.
- **`label`**: up to 4 characters for the language key. The default is the
  id in capitals.
- **`rows`** (required): 3 or 4 rows of up to 14 keys each. Each key is the
  text it types (up to 8 characters). Barry adds the number row above them,
  Shift and Backspace around the last row, and the bottom row (symbols, Fn,
  comma, space, period, Enter). With 4 rows, the first takes the number
  row's place. Long rows get narrower keys, so everything fits.
- **`shiftRows`**: what each key types with Shift, in the same rows and the
  same number of keys. Leave it out and Shift gives each key's capital letter.
- **`more`**: what holding a key offers: tap one to type it. A shifted key
  offers the same in capitals, unless `more` lists that key too.
- **`wordChars`**: the characters, besides letters and digits, that can be
  part of a word (default `'`). Glide typing and autocorrect use this.
- **`words`**: the name of the word list in the pack's folder (see below).
- **`composer`**: the name of the composer in the pack's folder (see below).

Key names Barry uses for its own keys cannot be a pack's keys: `shift`,
`BackSpace`, `space`, `Return`, `symbols`, `keys`, `abc`, `hide`, `lang`,
`ctrl`, `alt`, `super`, `Escape`, `Tab`, `Home`, `End`, `Delete`, `Left`,
`Right`, `Up`, `Down`, `Prior`, `Next`, `copy`, `paste`, `cut`,
`selectall`, `undo`, `redo`.

## words.txt

One word per line, then how common it is, in any order:

```
# comments start with #
und 10
der 10
Straße 20
Wörterbuch 35
```

The number is 10 (very common), 20, 35 or 40 (rare). Write words as they
are typed: names keep their capitals. With a word list, the pack gets glide
typing (slide a finger over the letters), the suggestion strip, and
autocorrect, which only fixes a word into a common one (10 or 20). Letters
from held keys (`ä` from `a`) glide over the key they are on. Without a word
list, the pack types fine but has none of these.

Each language keeps its own list of words you told it to leave alone (by
undoing a correction), in
`~/.local/share/barry_launcher/personal-words-<id>.txt`.

## Composer.qml

For languages where one key changes what was typed before it (Korean
syllables, Vietnamese tone marks), or where the text comes from a list of
candidates (pinyin), a pack can have a composer: a small QML file that sees
every key first. It is a `QtObject` with these functions:

- **`press(text)`**: a text key was tapped. Return `null` to type `text` as
  usual, or `{back: n, text: "..."}` to delete the last `n` characters
  typed and type `text` in their place.
- **`key(name)`** (optional): a named key (`BackSpace`, `Return`, an arrow)
  was tapped. Return `null` for the usual, or `{back, text}`. The space bar
  is a text key: it comes to `press(" ")`.
- **`reset()`** (optional): start over. Called when the keyboard opens, the
  language changes, or something else moved the text.
- **`candidates`** (optional): a list of strings to show in the strip.
- **`pick(i)`** (optional): candidate `i` was tapped. Return `{back, text}`.

A composer only knows what it was shown, so it keeps track of what it typed
itself. Glide typing and autocorrect are off for a pack with a composer.

An example: Esperanto's x-system, where `cx` types `ĉ`.

```qml
import QtQuick

QtObject {
    readonly property var marks: ({ "c": "ĉ", "g": "ĝ", "h": "ĥ", "j": "ĵ", "s": "ŝ", "u": "ŭ",
                                    "C": "Ĉ", "G": "Ĝ", "H": "Ĥ", "J": "Ĵ", "S": "Ŝ", "U": "Ŭ" })
    property string last: ""

    function press(text) {
        if ((text === "x" || text === "X") && marks[last] !== undefined) {
            const r = { back: 1, text: marks[last] }
            last = ""
            return r
        }
        last = text
        return null
    }
    function key(name) { last = ""; return null }
    function reset() { last = "" }
}
```

with `"composer": "Composer.qml"` in its `keyboard.json`.

## Where the text goes

The bottom keyboard types any character into the bottom screen's apps. The
Trackpad app's keyboard types into the top screen. A pack whose letters are
all on the US layout goes through Barry's virtual keyboard, which games read
like a real one. Any other pack's text goes to the focused window directly
(XTest in Game Mode, KWin's input method in Desktop Mode). Most apps and
games take that, but a game that reads only raw keys may not.

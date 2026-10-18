# Barry Launcher widgets

The quick settings page (swipe left from Barry Launcher's apps) is a grid of
widgets you pick and arrange: long-press a toggle or a widget's name, then
move (‹ ›), resize (− +) or hide (✕) them, and add the hidden ones from the
bar at the bottom. Besides Barry's own, a widget can be a **widget pack**: a
folder of QML you write.

## Adding one

In Steam's Quick Access menu, open the PB-OS tab, then **Widgets**, then
**Add a widget**, and pick the pack's `.zip` (or `.tar.gz`), or the
`widget.json` in its folder. The picker opens on a USB drive when one is
plugged in, otherwise in Downloads. A widget is code that runs on the device
as you, so you are asked before it is added. On the Widgets page, a widget's
row puts it on the quick settings page or takes it off; ✕ removes one you
added.

Packs live in `~/.local/share/barry_launcher/widgets/<name>/`; the ones
PB-OS ships are in `/usr/share/barry_launcher/widgets/` (Clock, Calendar,
Stopwatch and Battery, as examples: they start off the page). One of yours
with the same name as a shipped one replaces it.

## A pack

```
mywidget/
  widget.json
  Widget.qml
  (anything else it uses: images, more QML)
```

`widget.json`:

```json
{
  "title": "My widget",
  "author": "You",
  "api": 1,
  "main": "Widget.qml",
  "minSpan": 1,
  "span": 2,
  "height": 200
}
```

| key | meaning |
|---|---|
| `title` | its name on the page and in the PB-OS tab |
| `main` | its QML file, in the pack's top folder (default `Widget.qml`) |
| `minSpan` | the fewest grid units wide it can be, 1-4 (the page is 4 wide; default 2) |
| `span` | how wide it starts, `minSpan`-4 (default `minSpan`) |
| `height` | the tile height it needs at full size, in pixels, 60-800 (default 160); the page is 1080 high |

The page keeps every row full and the whole grid one even rectangle, so a
widget can come out wider or taller than asked: lay it out to fill whatever
it gets (anchors, Layouts), not at a fixed size.

## The QML

Barry draws the tile (its background, border and corners, in the theme's
colors) and puts your item inside it, sized to fill. Draw only the content.

```qml
import QtQuick
import Barry.Input

Item {
    property real s: 1        // scale: multiply your sizes by it
    property bool shown: true // the page is on screen: stop timers when false

    Text {
        anchors.centerIn: parent
        text: "Hello"
        color: Skin.colors.text
        font { family: Skin.font || "Noto Sans"; pixelSize: 40 * s }
    }
}
```

- `s` and `shown` are set only if your root item declares them.
- Colors: `Skin.colors` (`text`, `accent`, `lit` and `litText` for something
  chosen or on, `raised` and `pressed` for buttons, `panel`, `border`,
  `danger`, `icon`) and `Skin.font`: the active theme's, so the widget
  follows whatever theme is in use. Don't hard-code colors.
- Data: `barry_launcher_statsd` answers on `http://127.0.0.1:47823`
  (XMLHttpRequest): `/stats` (battery, CPU, GPU, temperatures, fan, memory,
  network, and `time.utcOffset`, the system's time zone now: JavaScript's
  `Date` keeps the one Barry started with), `/quick` (Wi-Fi, Bluetooth,
  volume). The docstring at the top of
  `/usr/lib/barry_launcher/barry_launcher_statsd` has them all.
- Touch: taps and drags are yours. A long press on the tile (outside your
  own controls) starts customizing the page.
- If it fails to load, its tile says so; the error is in the user journal
  (`journalctl --user -u barry_launcher`).

The four shipped widgets are short examples: `clock` (time zone from
statsd), `calendar` (a grid that fills its tile), `stopwatch` (buttons,
keeps counting while away) and `battery` (reads `/stats`, a ring with
QtQuick.Shapes).

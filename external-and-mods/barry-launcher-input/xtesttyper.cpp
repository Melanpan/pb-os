#include "xtesttyper.h"

#include <QDebug>
#include <QHash>
#include <algorithm>
#include <climits>
#include <thread>

// X11 last: its macros (None, Bool, Status) break Qt's headers.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

namespace barry {
namespace {

// The keymap is read again when older than this: another client (or
// gamescope, with a new layout) may have changed it. A burst of keys reads
// it once.
constexpr auto KEYMAP_FRESH = std::chrono::seconds(1);
constexpr int MAX_TEXT = 1000;
constexpr int MAX_REPEAT = 100;
// A spare keycode's last key may still be on its way to the app this long.
constexpr auto SPARE_BUSY = std::chrono::milliseconds(500);

unsigned long keysymFor(char32_t c)
{
    if (c == U'\n')
        return XK_Return;
    if (c == U'\t')
        return XK_Tab;
    // Latin-1 keysyms are the characters themselves; the rest of Unicode is
    // 0x1000000 + its code point.
    if ((c >= 0x20 && c <= 0x7e) || (c >= 0xa0 && c <= 0xff))
        return c;
    return 0x1000000 + c;
}

// The names Barry's keys go by; XStringToKeysym knows them all, but only
// these may be sent.
const QHash<QString, unsigned long> &namedKeysyms()
{
    static const QHash<QString, unsigned long> names = [] {
        QHash<QString, unsigned long> h;
        for (const char *n : {"BackSpace", "Return", "Tab", "Left", "Right", "Up", "Down", "Escape", "Delete",
                              "space", "Home", "End", "Prior", "Next", "F1", "F2", "F3", "F4", "F5", "F6",
                              "F7", "F8", "F9", "F10", "F11", "F12"})
            h.insert(QString::fromLatin1(n), XStringToKeysym(n));
        return h;
    }();
    return names;
}

// Xlib's own handler ends the process: a keymap change refused (a keycode
// taken meanwhile, say) must not take the keyboard with it.
int quietErrors(Display *, XErrorEvent *e)
{
    qWarning() << "Barry.Input: X error" << e->error_code << "on request" << e->request_code;
    return 0;
}

void connectionLost(Display *, void *typer)
{
    qWarning() << "Barry.Input: the X server went away";
    static_cast<XTestTyper *>(typer)->lost();
}

}  // namespace

XTestTyper::~XTestTyper()
{
    if (!m_dpy || m_lost)
        return;
    // Spare keycodes back as they were: empty.
    KeySym none[2] = {NoSymbol, NoSymbol};
    for (const auto &entry : m_onSpare)
        XChangeKeyboardMapping(m_dpy, int(entry.second), 2, none, 1);
    XCloseDisplay(m_dpy);
}

bool XTestTyper::open()
{
    if (m_dpy && m_lost) {
        // Left as it is: closing a broken connection only breaks again.
        m_dpy = nullptr;
        m_lost = false;
        m_onSpare.clear();
        m_spareUsed.clear();
    }
    if (m_dpy)
        return true;
    XSetErrorHandler(quietErrors);
    m_dpy = XOpenDisplay(m_display.isEmpty() ? nullptr : m_display.constData());
    if (!m_dpy) {
        if (!m_warned)
            qWarning() << "Barry.Input: no X display for the keyboard";
        m_warned = true;
        return false;
    }
    m_warned = false;
    XSetIOErrorExitHandler(m_dpy, connectionLost, this);
    int ev, err, major, minor;
    if (!XTestQueryExtension(m_dpy, &ev, &err, &major, &minor)) {
        qWarning() << "Barry.Input: the X server has no XTEST";
        XCloseDisplay(m_dpy);
        m_dpy = nullptr;
        return false;
    }
    load();
    return true;
}

void XTestTyper::load()
{
    XDisplayKeycodes(m_dpy, &m_min, &m_max);
    int per = 0;
    KeySym *syms = XGetKeyboardMapping(m_dpy, KeyCode(m_min), m_max - m_min + 1, &per);
    m_per = per;
    m_map.assign(syms, syms + size_t(m_max - m_min + 1) * size_t(per));
    XFree(syms);
    m_loaded = std::chrono::steady_clock::now();
    // Spares: keycodes with no symbols, and those holding ours.
    m_spares.clear();
    for (int code = m_max; code >= m_min; code--) {
        bool empty = true;
        for (int col = 0; col < m_per && empty; col++)
            empty = m_map[size_t(code - m_min) * size_t(m_per) + size_t(col)] == NoSymbol;
        bool ours = false;
        for (const auto &entry : m_onSpare)
            ours = ours || entry.second == unsigned(code);
        if (empty || ours)
            m_spares.push_back(unsigned(code));
    }
    // Ours that someone else has taken since are theirs now.
    for (auto it = m_onSpare.begin(); it != m_onSpare.end();) {
        const size_t at = size_t(it->second - unsigned(m_min)) * size_t(m_per);
        if (m_map[at] != it->first)
            it = m_onSpare.erase(it);
        else
            ++it;
    }
}

XTestTyper::Found XTestTyper::find(unsigned long keysym)
{
    if (std::chrono::steady_clock::now() - m_loaded > KEYMAP_FRESH)
        load();
    // First column (no Shift) anywhere, then the second.
    for (int col = 0; col < 2 && col < m_per; col++) {
        for (int code = m_min; code <= m_max; code++) {
            if (m_map[size_t(code - m_min) * size_t(m_per) + size_t(col)] == keysym)
                return {unsigned(code), col == 1};
        }
    }
    // A keymap may list only the lowercase letter; the uppercase one is its
    // Shift.
    KeySym lower, upper;
    XConvertCase(keysym, &lower, &upper);
    if (keysym == upper && lower != upper) {
        Found f = find(lower);
        if (f.code && !f.shift)
            return {f.code, true};
    }
    return {};
}

unsigned XTestTyper::pickSpare(const std::set<unsigned> &taken)
{
    // The longest unused, one never used first; not one this group types on.
    unsigned code = 0;
    auto oldest = std::chrono::steady_clock::time_point::max();
    for (unsigned c : m_spares) {
        if (taken.count(c))
            continue;
        auto used = m_spareUsed.find(c);
        const auto at = used == m_spareUsed.end() ? std::chrono::steady_clock::time_point::min() : used->second;
        if (at < oldest) {
            oldest = at;
            code = c;
        }
    }
    if (!code)
        return 0;
    const auto now = std::chrono::steady_clock::now();
    if (oldest != std::chrono::steady_clock::time_point::min() && now - oldest < SPARE_BUSY) {
        // Every spare is still being read: wait for the oldest.
        XSync(m_dpy, False);
        std::this_thread::sleep_for(SPARE_BUSY - (now - oldest));
    }
    for (auto other = m_onSpare.begin(); other != m_onSpare.end(); ++other) {
        if (other->second == code) {
            m_onSpare.erase(other);
            break;
        }
    }
    return code;
}

void XTestTyper::assign(unsigned code, unsigned long keysym)
{
    m_onSpare[keysym] = code;
    const size_t at = size_t(code - unsigned(m_min)) * size_t(m_per);
    for (int col = 0; col < m_per; col++)
        m_map[at + size_t(col)] = col < 2 ? keysym : NoSymbol;
}

void XTestTyper::sendSpares()
{
    if (m_onSpare.empty())
        return;
    // One change over every spare in use: an Xlib client that has read the
    // keymap takes in only the last change it was told of before a key (seen
    // with xev: a second spare, changed on its own, came out NoSymbol), so
    // each change carries all of ours. The keycodes between them go again as
    // they are (read just before).
    unsigned lo = UINT_MAX, hi = 0;
    for (const auto &entry : m_onSpare) {
        lo = std::min(lo, entry.second);
        hi = std::max(hi, entry.second);
    }
    XChangeKeyboardMapping(m_dpy, int(lo), m_per, &m_map[size_t(lo - unsigned(m_min)) * size_t(m_per)],
                           int(hi - lo + 1));
    // The mapping is in place before the keys: the app sees the change first.
    XSync(m_dpy, False);
}

void XTestTyper::tap(unsigned code, const std::vector<unsigned> &mods)
{
    for (unsigned m : mods)
        XTestFakeKeyEvent(m_dpy, m, True, CurrentTime);
    XTestFakeKeyEvent(m_dpy, code, True, CurrentTime);
    XTestFakeKeyEvent(m_dpy, code, False, CurrentTime);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it)
        XTestFakeKeyEvent(m_dpy, *it, False, CurrentTime);
}

unsigned XTestTyper::modifier(const QString &name)
{
    KeySym sym = NoSymbol;
    if (name == QLatin1String("ctrl"))
        sym = XK_Control_L;
    else if (name == QLatin1String("shift"))
        sym = XK_Shift_L;
    else if (name == QLatin1String("alt"))
        sym = XK_Alt_L;
    else if (name == QLatin1String("super"))
        sym = XK_Super_L;
    if (sym == NoSymbol)
        return 0;
    const Found f = find(sym);
    return f.shift ? 0 : f.code;
}

bool XTestTyper::type(const QString &text)
{
    if (!open())
        return false;
    load();  // as it is now, for the spares sent below
    const unsigned shift = modifier(QStringLiteral("shift"));
    // Keys go out in groups: the spares a group needs are mapped in one
    // change before any of its keys. A new group starts only when the spares
    // run out.
    std::vector<Found> group;
    std::set<unsigned> groupSpares;
    bool remapped = false;
    auto flush = [&] {
        if (remapped)
            sendSpares();
        for (const Found &f : group)
            tap(f.code, f.shift ? std::vector<unsigned>{shift} : std::vector<unsigned>{});
        group.clear();
        groupSpares.clear();
        remapped = false;
    };
    for (uint c : text.left(MAX_TEXT).toUcs4()) {
        const unsigned long sym = keysymFor(char32_t(c));
        Found f = find(sym);
        if (!f.code || (f.shift && !shift)) {
            unsigned code = pickSpare(groupSpares);
            if (!code && !group.empty()) {
                flush();
                code = pickSpare(groupSpares);
            }
            if (!code)
                continue;  // no room in the keymap
            assign(code, sym);
            remapped = true;
            f = {code, false};
        }
        if (m_onSpare.count(sym)) {
            groupSpares.insert(f.code);
            m_spareUsed[f.code] = std::chrono::steady_clock::now();
        }
        group.push_back(f);
    }
    flush();
    XFlush(m_dpy);
    return true;
}

bool XTestTyper::key(const QString &name, int repeat)
{
    const unsigned long sym = namedKeysyms().value(name, NoSymbol);
    if (sym == NoSymbol || !open())
        return false;
    const Found f = find(sym);
    if (!f.code)
        return false;
    const unsigned shift = modifier(QStringLiteral("shift"));
    for (int i = 0; i < qBound(0, repeat, MAX_REPEAT); i++)
        tap(f.code, f.shift && shift ? std::vector<unsigned>{shift} : std::vector<unsigned>{});
    XFlush(m_dpy);
    return true;
}

bool XTestTyper::combo(const QStringList &keys)
{
    if (keys.size() < 2 || keys.size() > 4 || !open())
        return false;
    std::vector<unsigned> mods;
    for (int i = 0; i < keys.size() - 1; i++) {
        const unsigned m = modifier(keys[i]);
        if (!m)
            return false;
        mods.push_back(m);
    }
    const QString &last = keys.last();
    unsigned long sym = namedKeysyms().value(last, NoSymbol);
    if (sym == NoSymbol) {
        const QList<uint> c = last.toUcs4();
        if (c.size() != 1)
            return false;
        sym = keysymFor(char32_t(c[0]));
    }
    const Found f = find(sym);
    if (!f.code)
        return false;
    // A key on the Shift column (ctrl+!) takes Shift too, as xdotool's did.
    const unsigned shift = modifier(QStringLiteral("shift"));
    if (f.shift && shift && std::find(mods.begin(), mods.end(), shift) == mods.end())
        mods.push_back(shift);
    tap(f.code, mods);
    XFlush(m_dpy);
    return true;
}

}  // namespace barry

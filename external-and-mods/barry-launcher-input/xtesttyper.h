// XTestTyper: keys for the focused X window through XTest, as xdotool did,
// on a display connection of its own (DISPLAY: the bottom screen's
// gamescope in Game Mode, Xwayland in Desktop Mode). Opened on first use.
//
// Characters are found in the server's keymap (with Shift for its second
// column); one the keymap lacks (é, €, an emoji) is put on a spare keycode,
// which keeps it until a later one needs the room (the longest unused, and
// never one an app may still be reading), or this goes. Apps look keycodes
// up when they get to the key, not as it is sent: a spare emptied or reused
// at once can reach them as the wrong character.
//
// Xlib would end the process if the X server went (KWin may stop an idle
// Xwayland): a broken connection is dropped instead, and the next key opens
// a new one.
#pragma once

#include <QString>
#include <QStringList>
#include <chrono>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

typedef struct _XDisplay Display;

namespace barry {

class XTestTyper {
public:
    // display: an X display name (":1"), or "" for DISPLAY's.
    explicit XTestTyper(const QByteArray &display = {}) : m_display(display) {}
    ~XTestTyper();
    XTestTyper(const XTestTyper &) = delete;
    XTestTyper &operator=(const XTestTyper &) = delete;

    bool type(const QString &text);
    bool key(const QString &name, int repeat = 1);
    bool combo(const QStringList &keys);

    // Xlib's: the connection broke.
    void lost() { m_lost = true; }

private:
    struct Found {
        unsigned code = 0;
        bool shift = false;
    };
    bool open();
    void load();
    Found find(unsigned long keysym);
    unsigned pickSpare(const std::set<unsigned> &taken);
    void assign(unsigned code, unsigned long keysym);
    void sendSpares();
    void tap(unsigned code, const std::vector<unsigned> &mods);
    unsigned modifier(const QString &name);

    QByteArray m_display;
    Display *m_dpy = nullptr;
    bool m_lost = false;
    bool m_warned = false;
    int m_min = 0, m_max = 0, m_per = 0;
    std::vector<unsigned long> m_map;  // (code - m_min) * m_per + column
    std::chrono::steady_clock::time_point m_loaded;
    std::vector<unsigned> m_spares;   // keycodes with no symbols, ours to use
    std::unordered_map<unsigned long, unsigned> m_onSpare;  // keysym -> spare keycode
    std::map<unsigned, std::chrono::steady_clock::time_point> m_spareUsed;  // keycode -> last key on it
};

}  // namespace barry

#include "keymap.h"

#include <linux/input-event-codes.h>

#include <QHash>
#include <set>

namespace barry {
namespace {

struct Tables {
    QHash<char32_t, Stroke> chars;
    QHash<QString, int> named;
    QHash<QString, int> mods;

    Tables()
    {
        const struct { const char *row; int first; } rows[] = {
            {"1234567890", KEY_1}, {"qwertyuiop", KEY_Q}, {"asdfghjkl", KEY_A}, {"zxcvbnm", KEY_Z},
        };
        for (const auto &r : rows) {
            for (int i = 0; r.row[i]; i++) {
                const char32_t c = static_cast<unsigned char>(r.row[i]);
                chars.insert(c, {r.first + i, false});
                if (c >= 'a' && c <= 'z')
                    chars.insert(c - 'a' + 'A', {r.first + i, true});
            }
        }
        const struct { char plain, shifted; int code; } punct[] = {
            {'-', '_', KEY_MINUS}, {'=', '+', KEY_EQUAL}, {'[', '{', KEY_LEFTBRACE},
            {']', '}', KEY_RIGHTBRACE}, {';', ':', KEY_SEMICOLON}, {'\'', '"', KEY_APOSTROPHE},
            {'`', '~', KEY_GRAVE}, {'\\', '|', KEY_BACKSLASH}, {',', '<', KEY_COMMA},
            {'.', '>', KEY_DOT}, {'/', '?', KEY_SLASH},
        };
        for (const auto &p : punct) {
            chars.insert(char32_t(p.plain), {p.code, false});
            chars.insert(char32_t(p.shifted), {p.code, true});
        }
        const char *digits = "1234567890", *shifted = "!@#$%^&*()";
        for (int i = 0; digits[i]; i++)
            chars.insert(char32_t(shifted[i]), {chars.value(char32_t(digits[i])).code, true});
        chars.insert(U' ', {KEY_SPACE, false});
        chars.insert(U'\n', {KEY_ENTER, false});
        chars.insert(U'\t', {KEY_TAB, false});

        named = {
            {"Escape", KEY_ESC}, {"BackSpace", KEY_BACKSPACE}, {"Tab", KEY_TAB},
            {"Return", KEY_ENTER}, {"space", KEY_SPACE}, {"Home", KEY_HOME}, {"Up", KEY_UP},
            {"Prior", KEY_PAGEUP}, {"Left", KEY_LEFT}, {"Right", KEY_RIGHT}, {"End", KEY_END},
            {"Down", KEY_DOWN}, {"Next", KEY_PAGEDOWN}, {"Delete", KEY_DELETE},
            {"F11", KEY_F11}, {"F12", KEY_F12},
        };
        for (int i = 1; i <= 10; i++)
            named.insert(QStringLiteral("F%1").arg(i), KEY_F1 + i - 1);

        mods = {{"ctrl", KEY_LEFTCTRL}, {"shift", KEY_LEFTSHIFT}, {"alt", KEY_LEFTALT},
                {"super", KEY_LEFTMETA}};
    }
};

const Tables &tables()
{
    static const Tables t;
    return t;
}

}  // namespace

std::optional<Stroke> usChar(char32_t c)
{
    const auto &t = tables().chars;
    auto it = t.constFind(c);
    if (it == t.constEnd())
        return std::nullopt;
    return *it;
}

int namedKey(const QString &name)
{
    return tables().named.value(name, -1);
}

int modifierKey(const QString &name)
{
    return tables().mods.value(name, -1);
}

std::vector<int> allKeys()
{
    std::set<int> codes;
    for (const Stroke &s : tables().chars)
        codes.insert(s.code);
    for (int code : tables().named)
        codes.insert(code);
    for (int code : tables().mods)
        codes.insert(code);
    return {codes.begin(), codes.end()};
}

}  // namespace barry

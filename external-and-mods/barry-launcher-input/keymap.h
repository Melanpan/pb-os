// The US layout as uinput keycodes: what a virtual keyboard presses for a
// character, a named key (BackSpace, Return, ...) or a modifier.
#pragma once

#include <QString>
#include <optional>
#include <vector>

namespace barry {

struct Stroke {
    int code;
    bool shift;
};

std::optional<Stroke> usChar(char32_t c);
int namedKey(const QString &name);     // -1 if unknown
int modifierKey(const QString &name);  // ctrl, shift, alt, super; -1 if unknown
std::vector<int> allKeys();            // every code the above can return

}  // namespace barry

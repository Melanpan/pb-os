// barryglide internals: Python's text-file and str helpers barry_glide.py
// relies on, over code points.
#pragma once

#include "unicode.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace barryglide::text {

// A text file's lines as open(..., encoding="utf-8") yields them
// (universal newlines), without their line ends.
inline std::vector<std::u32string_view> lines(std::u32string_view s)
{
    std::vector<std::u32string_view> out;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == U'\n' || s[i] == U'\r') {
            out.push_back(s.substr(start, i - start));
            if (s[i] == U'\r' && i + 1 < s.size() && s[i + 1] == U'\n')
                ++i;
            start = i + 1;
        }
    }
    if (start < s.size())
        out.push_back(s.substr(start));
    return out;
}

inline std::u32string_view strip(std::u32string_view s)
{
    size_t a = 0, b = s.size();
    while (a < b && uni::isSpace(s[a]))
        ++a;
    while (b > a && uni::isSpace(s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

inline std::vector<std::u32string_view> split(std::u32string_view s)
{
    std::vector<std::u32string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && uni::isSpace(s[i]))
            ++i;
        const size_t start = i;
        while (i < s.size() && !uni::isSpace(s[i]))
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

// int(s) for a word without spaces: sign, decimal digits of any script,
// single underscores between them. Saturates past int32 (levels).
inline std::optional<int32_t> toInt(std::u32string_view s)
{
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == U'+' || s[i] == U'-'))
        neg = s[i++] == U'-';
    if (i >= s.size())
        return std::nullopt;
    int64_t v = 0;
    bool lastDigit = false;
    for (; i < s.size(); ++i) {
        if (s[i] == U'_') {
            if (!lastDigit)
                return std::nullopt;
            lastDigit = false;
            continue;
        }
        const int d = uni::decimal(s[i]);
        if (d < 0)
            return std::nullopt;
        if (v < (int64_t(1) << 40))
            v = v * 10 + d;
        lastDigit = true;
    }
    if (!lastDigit)
        return std::nullopt;
    if (neg)
        v = -v;
    if (v > INT32_MAX)
        v = INT32_MAX;
    if (v < INT32_MIN)
        v = INT32_MIN;
    return int32_t(v);
}

inline std::u32string without(std::u32string_view s, char32_t c)
{
    std::u32string out;
    out.reserve(s.size());
    for (char32_t x : s)
        if (x != c)
            out.push_back(x);
    return out;
}

}  // namespace barryglide::text

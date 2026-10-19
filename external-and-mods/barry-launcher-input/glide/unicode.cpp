#include "unicode.h"

#include <algorithm>
#include <iterator>

namespace barryglide::uni {
namespace {

struct Range {
    char32_t lo, hi;
};
struct Mapping {
    char32_t cp;
    const char32_t *to;
};
struct DigitRun {
    char32_t lo, hi;
    int base;
};

#include "unicode_tables.inc"

template <size_t N>
bool inRanges(const Range (&rs)[N], char32_t c)
{
    auto it = std::upper_bound(std::begin(rs), std::end(rs), c, [](char32_t v, const Range &r) { return v < r.lo; });
    return it != std::begin(rs) && c <= std::prev(it)->hi;
}

template <size_t N>
const char32_t *mapped(const Mapping (&ms)[N], char32_t c)
{
    auto it = std::lower_bound(std::begin(ms), std::end(ms), c, [](const Mapping &m, char32_t v) { return m.cp < v; });
    return it != std::end(ms) && it->cp == c ? it->to : nullptr;
}

constexpr char32_t SIGMA = 0x3A3;

}  // namespace

bool decode(std::string_view in, std::u32string &out)
{
    out.clear();
    out.reserve(in.size());
    const auto *p = reinterpret_cast<const unsigned char *>(in.data());
    const auto *end = p + in.size();
    while (p < end) {
        const unsigned char b = *p;
        if (b < 0x80) {
            out.push_back(b);
            ++p;
            continue;
        }
        int n;
        char32_t c, min;
        if (b >= 0xC2 && b <= 0xDF) {
            n = 1, c = b & 0x1F, min = 0x80;
        } else if (b >= 0xE0 && b <= 0xEF) {
            n = 2, c = b & 0x0F, min = 0x800;
        } else if (b >= 0xF0 && b <= 0xF4) {
            n = 3, c = b & 0x07, min = 0x10000;
        } else {
            return false;
        }
        if (end - p <= n)
            return false;
        for (int i = 1; i <= n; ++i) {
            if ((p[i] & 0xC0) != 0x80)
                return false;
            c = (c << 6) | (p[i] & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            return false;
        out.push_back(c);
        p += n + 1;
    }
    return true;
}

std::u32string decode(std::string_view in)
{
    std::u32string out;
    if (!decode(in, out))
        out.clear();
    return out;
}

void append(std::string &out, char32_t c)
{
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

std::string encode(std::u32string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s)
        append(out, c);
    return out;
}

bool isAlpha(char32_t c) { return inRanges(ALPHA, c); }
bool isUpper(char32_t c) { return inRanges(UPPER, c); }
bool isSpace(char32_t c) { return inRanges(SPACE, c); }

int decimal(char32_t c)
{
    auto it = std::upper_bound(std::begin(DECIMAL), std::end(DECIMAL), c,
                               [](char32_t v, const DigitRun &r) { return v < r.lo; });
    if (it == std::begin(DECIMAL) || c > std::prev(it)->hi)
        return -1;
    --it;
    return it->base + int(c - it->lo);
}

bool isAlpha(std::u32string_view s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char32_t c) { return isAlpha(c); });
}

bool isAscii(std::u32string_view s)
{
    return std::all_of(s.begin(), s.end(), [](char32_t c) { return c < 0x80; });
}

std::u32string lower(std::u32string_view s)
{
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char32_t c = s[i];
        if (c == SIGMA) {
            // CPython's handle_capital_sigma: cased, case-ignorables, Σ,
            // and no cased letter after it (case-ignorables between).
            size_t j = i;
            char32_t prev = 0;
            bool found = false;
            while (j > 0) {
                prev = s[--j];
                if (!inRanges(CASE_IGNORABLE, prev)) {
                    found = true;
                    break;
                }
            }
            bool final = found && inRanges(CASED, prev);
            if (final && i + 1 < s.size()) {
                size_t k = i + 1;
                for (; k < s.size(); ++k)
                    if (!inRanges(CASE_IGNORABLE, s[k]))
                        break;
                final = k == s.size() || !inRanges(CASED, s[k]);
            }
            out.push_back(final ? 0x3C2 : 0x3C3);
            continue;
        }
        if (const char32_t *to = mapped(LOWER, c))
            out += to;
        else
            out.push_back(c);
    }
    return out;
}

std::u32string upper(char32_t c)
{
    if (const char32_t *to = mapped(UPPERMAP, c))
        return to;
    return std::u32string(1, c);
}

}  // namespace barryglide::uni

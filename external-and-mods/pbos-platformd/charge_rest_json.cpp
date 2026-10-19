// charge-rest's JSON: what Python 3.12's json module (its C scanner and
// encoder) accepts and writes, for the settings file and the CLI.

#include "charge_rest_json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pp::pyjson {

namespace {

// Python's sys.int_info.default_max_str_digits.
constexpr size_t MAX_STR_DIGITS = 4300;
// About where CPython's C scanner gives up with RecursionError.
constexpr int MAX_DEPTH = 1000;

bool ws(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool digit(char32_t c) { return c >= '0' && c <= '9'; }
int hexval(char32_t c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
    return -1;
}

struct Parser {
    const std::u32string& s;
    size_t n;
    bool ok = true;

    void skip(size_t& i) const {
        while (i < n && ws(s[i])) ++i;
    }

    bool lit(size_t i, const char* word) const {
        size_t len = std::strlen(word);
        if (i + len > n) return false;
        for (size_t k = 0; k < len; ++k)
            if (s[i + k] != static_cast<char32_t>(word[k])) return false;
        return true;
    }

    std::optional<std::u32string> string(size_t& i) {  // i: after the opening quote
        std::u32string out;
        while (true) {
            if (i >= n) return std::nullopt;  // unterminated
            char32_t c = s[i];
            if (c == '"') {
                ++i;
                return out;
            }
            if (c != '\\') {
                if (c <= 0x1f) return std::nullopt;  // strict: control characters
                out.push_back(c);
                ++i;
                continue;
            }
            if (++i >= n) return std::nullopt;
            c = s[i];
            switch (c) {
                case '"': out.push_back('"'); ++i; continue;
                case '\\': out.push_back('\\'); ++i; continue;
                case '/': out.push_back('/'); ++i; continue;
                case 'b': out.push_back('\b'); ++i; continue;
                case 'f': out.push_back('\f'); ++i; continue;
                case 'n': out.push_back('\n'); ++i; continue;
                case 'r': out.push_back('\r'); ++i; continue;
                case 't': out.push_back('\t'); ++i; continue;
                case 'u': break;
                default: return std::nullopt;
            }
            // \uXXXX
            if (i + 4 >= n) return std::nullopt;
            char32_t u = 0;
            for (size_t k = 1; k <= 4; ++k) {
                int h = hexval(s[i + k]);
                if (h < 0) return std::nullopt;
                u = (u << 4) | static_cast<char32_t>(h);
            }
            i += 5;
            // A high surrogate and a \u low one after it: one character.
            if (u >= 0xd800 && u <= 0xdbff && i + 5 < n && s[i] == '\\' && s[i + 1] == 'u') {
                char32_t u2 = 0;
                for (size_t k = 2; k <= 5; ++k) {
                    int h = hexval(s[i + k]);
                    if (h < 0) return std::nullopt;
                    u2 = (u2 << 4) | static_cast<char32_t>(h);
                }
                if (u2 >= 0xdc00 && u2 <= 0xdfff) {
                    u = 0x10000 + ((u - 0xd800) << 10) + (u2 - 0xdc00);
                    i += 6;
                }
            }
            out.push_back(u);
        }
    }

    std::optional<Value> number(size_t& i) {
        size_t start = i;
        if (s[i] == '-') {
            if (++i >= n) return std::nullopt;
        }
        if (s[i] >= '1' && s[i] <= '9') {
            ++i;
            while (i < n && digit(s[i])) ++i;
        } else if (s[i] == '0') {
            ++i;
        } else {
            return std::nullopt;
        }
        bool is_float = false;
        if (i + 1 < n && s[i] == '.' && digit(s[i + 1])) {
            is_float = true;
            i += 2;
            while (i < n && digit(s[i])) ++i;
        }
        if (i + 1 < n && (s[i] == 'e' || s[i] == 'E')) {
            size_t e = i++;
            if (i + 1 < n && (s[i] == '-' || s[i] == '+')) ++i;
            while (i < n && digit(s[i])) ++i;
            if (digit(s[i - 1])) is_float = true;
            else i = e;
        }
        std::string text;
        for (size_t k = start; k < i; ++k) text.push_back(static_cast<char>(s[k]));
        Value v;
        if (is_float) {
            v.type = Value::Type::Float;
            v.f = std::strtod(text.c_str(), nullptr);
            return v;
        }
        size_t digits = text.size() - (text[0] == '-' ? 1 : 0);
        if (digits > MAX_STR_DIGITS) {
            ok = false;  // ValueError: exceeds the limit for integer string conversion
            return std::nullopt;
        }
        v.type = Value::Type::Int;
        v.i = text == "-0" ? "0" : text;
        return v;
    }

    std::optional<Value> value(size_t& i, int depth) {
        if (i >= n) return std::nullopt;
        char32_t c = s[i];
        switch (c) {
            case '"': {
                ++i;
                auto str = string(i);
                if (!str) return std::nullopt;
                Value v;
                v.type = Value::Type::Str;
                v.s = std::move(*str);
                return v;
            }
            case '{':
            case '[': {
                if (depth >= MAX_DEPTH) throw NotValueError{"RecursionError: maximum recursion depth exceeded"};
                return c == '{' ? object(i, depth + 1) : array(i, depth + 1);
            }
            case 'n':
                if (lit(i, "null")) {
                    i += 4;
                    return Value();
                }
                break;
            case 't':
                if (lit(i, "true")) {
                    i += 4;
                    return Value::boolean(true);
                }
                break;
            case 'f':
                if (lit(i, "false")) {
                    i += 5;
                    return Value::boolean(false);
                }
                break;
            case 'N':
                if (lit(i, "NaN")) {
                    i += 3;
                    return Value::number(std::nan(""));
                }
                break;
            case 'I':
                if (lit(i, "Infinity")) {
                    i += 8;
                    return Value::number(HUGE_VAL);
                }
                break;
            case '-':
                if (lit(i, "-Infinity")) {
                    i += 9;
                    return Value::number(-HUGE_VAL);
                }
                break;
            default: break;
        }
        return number(i);
    }

    std::optional<Value> object(size_t& i, int depth) {
        Value obj = Value::object();
        ++i;
        skip(i);
        if (i < n && s[i] != '}') {
            while (i < n) {
                if (s[i] != '"') return std::nullopt;
                ++i;
                auto key = string(i);
                if (!key) return std::nullopt;
                skip(i);
                if (i >= n || s[i] != ':') return std::nullopt;
                ++i;
                skip(i);
                auto v = value(i, depth);
                if (!v) return std::nullopt;
                obj.set(std::move(*key), std::move(*v));
                skip(i);
                if (i < n && s[i] == '}') break;
                if (i >= n || s[i] != ',') return std::nullopt;
                ++i;
                skip(i);
            }
        }
        if (i >= n || s[i] != '}') return std::nullopt;
        ++i;
        return obj;
    }

    std::optional<Value> array(size_t& i, int depth) {
        Value arr = Value::array();
        ++i;
        skip(i);
        if (i < n && s[i] != ']') {
            while (i < n) {
                auto v = value(i, depth);
                if (!v) return std::nullopt;
                arr.a.push_back(std::move(*v));
                skip(i);
                if (i < n && s[i] == ']') break;
                if (i >= n || s[i] != ',') return std::nullopt;
                ++i;
                skip(i);
            }
        }
        if (i >= n || s[i] != ']') return std::nullopt;
        ++i;
        return arr;
    }
};

void quote(std::string& out, const std::u32string& s) {
    out.push_back('"');
    char buf[16];
    for (char32_t c : s) {
        switch (c) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            default: break;
        }
        if (c >= 0x20 && c < 0x7f) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x10000) {
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
            out += buf;
        } else {
            unsigned v = static_cast<unsigned>(c) - 0x10000;
            std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", 0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff));
            out += buf;
        }
    }
    out.push_back('"');
}

void dump(std::string& out, const Value& v, int indent, int level) {
    switch (v.type) {
        case Value::Type::Null: out += "null"; return;
        case Value::Type::Bool: out += v.b ? "true" : "false"; return;
        case Value::Type::Int: out += v.i; return;
        case Value::Type::Float:
            if (std::isnan(v.f)) out += "NaN";
            else if (std::isinf(v.f)) out += v.f > 0 ? "Infinity" : "-Infinity";
            else out += float_repr(v.f);
            return;
        case Value::Type::Str: quote(out, v.s); return;
        case Value::Type::Array:
        case Value::Type::Object: break;
    }
    bool arr = v.type == Value::Type::Array;
    size_t count = arr ? v.a.size() : v.o.size();
    if (!count) {
        out += arr ? "[]" : "{}";
        return;
    }
    out.push_back(arr ? '[' : '{');
    std::string sep = ", ", inner, outer;
    if (indent >= 0) {
        inner = "\n" + std::string(static_cast<size_t>(indent * (level + 1)), ' ');
        outer = "\n" + std::string(static_cast<size_t>(indent * level), ' ');
        sep = "," + inner;
        out += inner;
    }
    for (size_t k = 0; k < count; ++k) {
        if (k) out += sep;
        if (arr) {
            dump(out, v.a[k], indent, level + 1);
        } else {
            quote(out, v.o[k].first);
            out += ": ";
            dump(out, v.o[k].second, indent, level + 1);
        }
    }
    out += outer;
    out.push_back(arr ? ']' : '}');
}

}  // namespace

Value Value::boolean(bool v) {
    Value x;
    x.type = Type::Bool;
    x.b = v;
    return x;
}

Value Value::integer(long long v) {
    Value x;
    x.type = Type::Int;
    x.i = std::to_string(v);
    return x;
}

Value Value::number(double v) {
    Value x;
    x.type = Type::Float;
    x.f = v;
    return x;
}

Value Value::string(std::string_view ascii) {
    Value x;
    x.type = Type::Str;
    x.s = widen(ascii);
    return x;
}

Value Value::object() {
    Value x;
    x.type = Type::Object;
    return x;
}

Value Value::array() {
    Value x;
    x.type = Type::Array;
    return x;
}

const Value* Value::get(std::u32string_view key) const {
    for (auto& [k, v] : o)
        if (k == key) return &v;
    return nullptr;
}

void Value::set(std::u32string key, Value v) {
    for (auto& kv : o)
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    o.emplace_back(std::move(key), std::move(v));
}

std::optional<Value> loads(const std::u32string& text) {
    if (!text.empty() && text[0] == 0xfeff) return std::nullopt;  // "Unexpected UTF-8 BOM"
    Parser p{text, text.size()};
    size_t i = 0;
    p.skip(i);
    auto v = p.value(i, 0);
    if (!v || !p.ok) return std::nullopt;
    p.skip(i);
    if (i != text.size()) return std::nullopt;  // "Extra data"
    return v;
}

std::string dumps(const Value& v, int indent) {
    std::string out;
    dump(out, v, indent, 0);
    return out;
}

std::string float_repr(double d) {
    if (d == 0) return std::signbit(d) ? "-0.0" : "0.0";
    // The shortest digits that read back as d (as Python's repr), from the
    // scientific form d.ddde[+-]X.
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    std::string out;
    size_t at = 0;
    if (sci[0] == '-') {
        out.push_back('-');
        at = 1;
    }
    size_t e = sci.find('e');
    std::string digits;
    for (size_t k = at; k < e; ++k)
        if (sci[k] != '.') digits.push_back(sci[k]);
    int exp = std::atoi(sci.c_str() + e + 1);
    int decpt = exp + 1;
    int nd = static_cast<int>(digits.size());
    if (decpt <= -4 || decpt > 16) {
        out += digits.substr(0, 1);
        if (nd > 1) out += "." + digits.substr(1);
        char eb[16];
        std::snprintf(eb, sizeof eb, "e%+.02d", exp);
        out += eb;
    } else if (decpt <= 0) {
        out += "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
    } else if (decpt >= nd) {
        out += digits + std::string(static_cast<size_t>(decpt - nd), '0') + ".0";
    } else {
        out += digits.substr(0, static_cast<size_t>(decpt)) + "." + digits.substr(static_cast<size_t>(decpt));
    }
    return out;
}

std::optional<std::u32string> decode_utf8(std::string_view b, bool surrogateescape) {
    std::u32string out;
    size_t i = 0, n = b.size();
    auto cont = [&](size_t k) { return k < n && (static_cast<unsigned char>(b[k]) & 0xc0) == 0x80; };
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(b[i]);
        if (c < 0x80) {
            out.push_back(c);
            ++i;
            continue;
        }
        size_t len = 0;
        char32_t cp = 0;
        unsigned char c1 = i + 1 < n ? static_cast<unsigned char>(b[i + 1]) : 0;
        if (c >= 0xc2 && c <= 0xdf && cont(i + 1)) {
            len = 2;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef && cont(i + 1) && cont(i + 2) && !(c == 0xe0 && c1 < 0xa0) &&
                   !(c == 0xed && c1 > 0x9f)) {
            len = 3;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4 && cont(i + 1) && cont(i + 2) && cont(i + 3) && !(c == 0xf0 && c1 < 0x90) &&
                   !(c == 0xf4 && c1 > 0x8f)) {
            len = 4;
            cp = c & 0x07;
        }
        if (!len) {
            if (!surrogateescape) return std::nullopt;
            out.push_back(0xdc00 + c);
            ++i;
            continue;
        }
        for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(b[i + k]) & 0x3f);
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::u32string widen(std::string_view ascii) {
    std::u32string out;
    for (char c : ascii) out.push_back(static_cast<unsigned char>(c));
    return out;
}

bool is_space(char32_t c) {
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

}  // namespace pp::pyjson

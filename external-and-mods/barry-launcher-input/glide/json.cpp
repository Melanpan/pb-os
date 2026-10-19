#include "json.h"

#include "unicode.h"

#include <cstdlib>
#include <cstring>

namespace barryglide::json {

const Value *Value::get(std::string_view key) const
{
    if (type != Object)
        return nullptr;
    for (const auto &kv : obj)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

namespace {

struct Parser {
    std::string_view s;
    size_t i = 0;
    int depth = 0;
    std::string err;

    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }
    bool fail(const char *what)
    {
        if (err.empty())
            err = std::string(what) + " at " + std::to_string(i);
        return false;
    }
    bool hex4(unsigned &v)
    {
        if (i + 4 > s.size())
            return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= unsigned(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool string(std::string &out)
    {
        ++i;  // "
        out.clear();
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') {
                ++i;
                std::u32string check;
                return uni::decode(out, check) || fail("bad UTF-8");
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in string");
            if (c != '\\') {
                out += c;
                ++i;
                continue;
            }
            if (++i >= s.size())
                break;
            const char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned v;
                if (!hex4(v))
                    return fail("bad \\u escape");
                if (v >= 0xD800 && v <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    const size_t save = i;
                    i += 2;
                    unsigned w;
                    if (hex4(w) && w >= 0xDC00 && w <= 0xDFFF) {
                        v = 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00);
                    } else {
                        i = save;
                    }
                }
                if (v >= 0xD800 && v <= 0xDFFF)
                    return fail("lone surrogate");  // Python keeps it; Barry never sends one
                uni::append(out, char32_t(v));
                break;
            }
            default:
                return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Value &v)
    {
        const size_t start = i;
        bool isFloat = false;
        if (s[i] == '-')
            ++i;
        if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9'))
            return fail("bad number");
        if (s[i] == '0')
            ++i;
        else
            while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                ++i;
        if (i < s.size() && s[i] == '.') {
            isFloat = true;
            ++i;
            if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9'))
                return fail("bad number");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            isFloat = true;
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                ++i;
            if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9'))
                return fail("bad number");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                ++i;
        }
        const std::string text(s.substr(start, i - start));
        v.type = isFloat ? Value::Float : Value::Int;
        v.num = std::strtod(text.c_str(), nullptr);
        return true;
    }
    bool literal(const char *word)
    {
        const size_t n = std::strlen(word);
        if (s.substr(i, n) != word)
            return fail("bad literal");
        i += n;
        return true;
    }
    bool value(Value &v)
    {
        ws();
        if (i >= s.size())
            return fail("unexpected end");
        if (++depth > 200)
            return fail("too deep");
        bool ok = true;
        const char c = s[i];
        if (c == '{') {
            v.type = Value::Object;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
            } else {
                for (;;) {
                    ws();
                    if (i >= s.size() || s[i] != '"') {
                        ok = fail("expected key");
                        break;
                    }
                    std::string key;
                    if (!string(key)) {
                        ok = false;
                        break;
                    }
                    ws();
                    if (i >= s.size() || s[i] != ':') {
                        ok = fail("expected :");
                        break;
                    }
                    ++i;
                    Value item;
                    if (!value(item)) {
                        ok = false;
                        break;
                    }
                    bool replaced = false;
                    for (auto &kv : v.obj)
                        if (kv.first == key) {
                            kv.second = std::move(item);
                            replaced = true;
                            break;
                        }
                    if (!replaced)
                        v.obj.emplace_back(std::move(key), std::move(item));
                    ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == '}') {
                        ++i;
                        break;
                    }
                    ok = fail("expected , or }");
                    break;
                }
            }
        } else if (c == '[') {
            v.type = Value::Array;
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
            } else {
                for (;;) {
                    Value item;
                    if (!value(item)) {
                        ok = false;
                        break;
                    }
                    v.arr.push_back(std::move(item));
                    ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == ']') {
                        ++i;
                        break;
                    }
                    ok = fail("expected , or ]");
                    break;
                }
            }
        } else if (c == '"') {
            v.type = Value::String;
            ok = string(v.str);
        } else if (c == 't') {
            v.type = Value::Bool, v.b = true;
            ok = literal("true");
        } else if (c == 'f') {
            v.type = Value::Bool;
            ok = literal("false");
        } else if (c == 'n') {
            ok = literal("null");
        } else {
            ok = number(v);
        }
        --depth;
        return ok;
    }
};

}  // namespace

bool parse(std::string_view text, Value &out, std::string *error)
{
    Parser p;
    p.s = text;
    out = Value();
    bool ok = p.value(out);
    if (ok) {
        p.ws();
        if (p.i != text.size())
            ok = p.fail("trailing data");
    }
    if (!ok && error)
        *error = p.err;
    return ok;
}

std::string quote(std::string_view s)
{
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char *hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

}  // namespace barryglide::json

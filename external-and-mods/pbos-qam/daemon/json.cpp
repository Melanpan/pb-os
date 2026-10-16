#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pj {

namespace {

const Value kNull;
const std::string kEmpty;
const Value::Array kNoItems;
const Value::Object kNoMembers;

void put_number(std::string& out, double d) {
    if (!std::isfinite(d)) {
        out += "null";
        return;
    }
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d));
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    // The shortest text that reads back the same.
    for (int prec = 1; prec < 17; ++prec) {
        char tmp[40];
        std::snprintf(tmp, sizeof tmp, "%.*g", prec, d);
        if (std::strtod(tmp, nullptr) == d) {
            out += tmp;
            return;
        }
    }
    out += buf;
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    std::optional<Value> run() {
        auto v = value(0);
        ws();
        if (!v || i_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    bool lit(std::string_view w) {
        if (s_.substr(i_, w.size()) != w) return false;
        i_ += w.size();
        return true;
    }

    std::optional<Value> value(int depth) {
        if (depth > 200) return std::nullopt;
        ws();
        if (i_ >= s_.size()) return std::nullopt;
        char c = s_[i_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') {
            auto str = string();
            if (!str) return std::nullopt;
            return Value(std::move(*str));
        }
        if (lit("true")) return Value(true);
        if (lit("false")) return Value(false);
        if (lit("null")) return Value();
        // Python's json writes these for floats it cannot represent.
        if (lit("NaN")) return Value(NAN);
        if (lit("Infinity")) return Value(INFINITY);
        if (lit("-Infinity")) return Value(-INFINITY);
        return number();
    }

    std::optional<Value> number() {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                                  s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+'))
            ++i_;
        if (i_ == start) return std::nullopt;
        std::string tmp(s_.substr(start, i_ - start));
        char* end = nullptr;
        double d = std::strtod(tmp.c_str(), &end);
        if (!end || *end) return std::nullopt;
        return Value(d);
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }

    bool hex4(unsigned& cp) {
        if (i_ + 4 > s_.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= h - '0';
            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
            else return false;
        }
        return true;
    }

    std::optional<std::string> string() {
        ++i_;  // the opening quote
        std::string out;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return std::nullopt;
            char e = s_[i_++];
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
                    unsigned cp;
                    if (!hex4(cp)) return std::nullopt;
                    if (cp >= 0xd800 && cp < 0xdc00 && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        size_t keep = i_;
                        i_ += 2;
                        unsigned lo;
                        if (hex4(lo) && lo >= 0xdc00 && lo < 0xe000) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        } else {
                            i_ = keep;
                        }
                    }
                    utf8(out, cp);
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> array(int depth) {
        ++i_;
        Value::Array out;
        ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return Value(std::move(out));
        }
        while (true) {
            auto v = value(depth + 1);
            if (!v) return std::nullopt;
            out.push_back(std::move(*v));
            ws();
            if (i_ >= s_.size()) return std::nullopt;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return Value(std::move(out)); }
            return std::nullopt;
        }
    }

    std::optional<Value> object(int depth) {
        ++i_;
        Value out = Value::object();
        ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return out;
        }
        while (true) {
            ws();
            if (i_ >= s_.size() || s_[i_] != '"') return std::nullopt;
            auto key = string();
            if (!key) return std::nullopt;
            ws();
            if (i_ >= s_.size() || s_[i_] != ':') return std::nullopt;
            ++i_;
            auto v = value(depth + 1);
            if (!v) return std::nullopt;
            out[*key] = std::move(*v);  // a repeated key keeps the last, as Python
            ws();
            if (i_ >= s_.size()) return std::nullopt;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return out; }
            return std::nullopt;
        }
    }

    std::string_view s_;
    size_t i_ = 0;
};

}  // namespace

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    // U+2028/2029 are line ends to JavaScript: escaped, the text is safe to
    // put straight into a script.
    std::string safe;
    safe.reserve(out.size() + 2);
    for (size_t i = 0; i < out.size(); ++i) {
        if (i + 2 < out.size() && static_cast<unsigned char>(out[i]) == 0xe2 &&
            static_cast<unsigned char>(out[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(out[i + 2]) == 0xa8 || static_cast<unsigned char>(out[i + 2]) == 0xa9)) {
            safe += static_cast<unsigned char>(out[i + 2]) == 0xa8 ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            safe += out[i];
        }
    }
    return safe + "\"";
}

bool Value::truthy() const {
    switch (type()) {
        case Type::Null: return false;
        case Type::Bool: return std::get<bool>(v_);
        case Type::Number: return std::get<double>(v_) != 0;
        case Type::String: return !std::get<std::string>(v_).empty();
        case Type::Array: return !std::get<Array>(v_).empty();
        case Type::Object: return !std::get<Object>(v_).empty();
    }
    return false;
}

bool Value::as_bool(bool def) const {
    if (is_null()) return def;
    return truthy();
}

double Value::as_double(double def) const {
    if (is_number()) return std::get<double>(v_);
    if (is_bool()) return std::get<bool>(v_) ? 1 : 0;
    if (is_string()) {
        const auto& s = std::get<std::string>(v_);
        char* end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        if (end && end != s.c_str() && *end == 0) return d;
    }
    return def;
}

long long Value::as_int(long long def) const {
    double d = as_double(NAN);
    if (std::isnan(d)) return def;
    return static_cast<long long>(d);  // int() truncates, as Python
}

std::string Value::str(std::string_view def) const {
    switch (type()) {
        case Type::String: return std::get<std::string>(v_);
        case Type::Bool: return std::get<bool>(v_) ? "True" : "False";
        case Type::Number: {
            std::string out;
            put_number(out, std::get<double>(v_));
            return out;
        }
        default: return std::string(def);
    }
}

const std::string& Value::as_string() const {
    return is_string() ? std::get<std::string>(v_) : kEmpty;
}

const Value::Array& Value::items() const { return is_array() ? std::get<Array>(v_) : kNoItems; }

Value::Array& Value::items() {
    if (!is_array()) v_ = Array{};
    return std::get<Array>(v_);
}

const Value::Object& Value::members() const { return is_object() ? std::get<Object>(v_) : kNoMembers; }

Value::Object& Value::members() {
    if (!is_object()) v_ = Object{};
    return std::get<Object>(v_);
}

size_t Value::size() const {
    if (is_array()) return std::get<Array>(v_).size();
    if (is_object()) return std::get<Object>(v_).size();
    return 0;
}

const Value* Value::find(std::string_view key) const {
    if (!is_object()) return nullptr;
    for (const auto& [k, v] : std::get<Object>(v_))
        if (k == key) return &v;
    return nullptr;
}

bool Value::has(std::string_view key) const { return find(key) != nullptr; }

const Value& Value::operator[](std::string_view key) const {
    const Value* v = find(key);
    return v ? *v : kNull;
}

Value& Value::operator[](std::string_view key) {
    auto& obj = members();
    for (auto& [k, v] : obj)
        if (k == key) return v;
    obj.emplace_back(std::string(key), Value());
    return obj.back().second;
}

const Value& Value::at(size_t i) const {
    const auto& a = items();
    return i < a.size() ? a[i] : kNull;
}

void Value::erase(std::string_view key) {
    if (!is_object()) return;
    auto& obj = std::get<Object>(v_);
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it->first == key) {
            obj.erase(it);
            return;
        }
    }
}

void Value::push(Value v) { items().push_back(std::move(v)); }

void Value::update(const Value& other) {
    for (const auto& [k, v] : other.members()) (*this)[k] = v;
}

void Value::dump_to(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * d), ' ');
    };
    switch (type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += std::get<bool>(v_) ? "true" : "false"; break;
        case Type::Number: put_number(out, std::get<double>(v_)); break;
        case Type::String: out += quote(std::get<std::string>(v_)); break;
        case Type::Array: {
            const auto& a = std::get<Array>(v_);
            if (a.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out += indent < 0 ? "," : ",";
                newline(depth + 1);
                a[i].dump_to(out, indent, depth + 1);
            }
            newline(depth);
            out += ']';
            break;
        }
        case Type::Object: {
            const auto& o = std::get<Object>(v_);
            if (o.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            for (size_t i = 0; i < o.size(); ++i) {
                if (i) out += ',';
                newline(depth + 1);
                out += quote(o[i].first);
                out += indent < 0 ? ":" : ": ";
                o[i].second.dump_to(out, indent, depth + 1);
            }
            newline(depth);
            out += '}';
            break;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

std::optional<Value> Value::parse(std::string_view text) { return Parser(text).run(); }

}  // namespace pj

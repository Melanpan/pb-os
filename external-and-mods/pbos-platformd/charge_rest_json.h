// charge-rest's JSON, as Python's json module reads and writes it, so the
// settings file and the CLI's answers stay byte for byte what the Python
// sm8550-charge-rest wrote: ints apart from floats (4.0 stays 4.0), floats
// as Python's repr, ensure_ascii escapes, NaN/Infinity, and dict semantics
// for repeated keys (the last value, at the first key's place).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pp::pyjson {

struct Value {
    enum class Type { Null, Bool, Int, Float, Str, Array, Object };
    Type type = Type::Null;
    bool b = false;
    std::string i;      // Int: its decimal text, as str(int) writes it (any size)
    double f = 0;
    std::u32string s;   // Str: code points (lone surrogates kept, as Python's str)
    std::vector<Value> a;
    std::vector<std::pair<std::u32string, Value>> o;

    static Value boolean(bool v);
    static Value integer(long long v);
    static Value number(double v);
    static Value string(std::string_view ascii);
    static Value object();
    static Value array();

    const Value* get(std::u32string_view key) const;
    // dict[key] = v: replaced in place, or added at the end.
    void set(std::u32string key, Value v);
};

// Thrown where Python would raise something other than ValueError (deep
// nesting: RecursionError).
struct NotValueError {
    std::string what;
};

// json.loads(text): nullopt where it raises ValueError (JSONDecodeError, or
// an int literal over Python's 4300-digit limit).
std::optional<Value> loads(const std::u32string& text);
// json.dumps(v) (indent < 0) or the text of json.dump(v, fh, indent=N).
std::string dumps(const Value& v, int indent = -1);
// repr(float).
std::string float_repr(double d);
// UTF-8 to code points: strict (nullopt on any error: open(encoding="utf-8")
// .read()), or with each bad byte as U+DC80..U+DCFF (sys.argv's
// surrogateescape).
std::optional<std::u32string> decode_utf8(std::string_view bytes, bool surrogateescape);
std::u32string widen(std::string_view ascii);
// Python's str.isspace() for one code point.
bool is_space(char32_t c);

}  // namespace pp::pyjson

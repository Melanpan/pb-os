// barryglide: a small JSON reader and writer, for keyboard.json and the
// equivalence harness. Objects keep their keys in order, as Python's json
// does (a repeated key: first place, last value), and a number remembers
// whether it was written as an integer.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace barryglide::json {

struct Value {
    enum Type { Null, Bool, Int, Float, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;  // Int and Float
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool isNumber() const { return type == Int || type == Float; }
    const Value *get(std::string_view key) const;
};

// False on bad JSON (out untouched past what was read).
bool parse(std::string_view text, Value &out, std::string *error = nullptr);

std::string quote(std::string_view s);  // a JSON string, UTF-8 kept as is

}  // namespace barryglide::json

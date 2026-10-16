// A small JSON value: what pbos-qamd sends to and gets from Steam's
// frontend, the helpers' answers and the state files. Objects keep their
// keys in the order they were added, as Python's dicts do, so files written
// here read like the ones the Decky plugins wrote.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pj {

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    Value(int n) : v_(static_cast<double>(n)) {}
    Value(long n) : v_(static_cast<double>(n)) {}
    Value(long long n) : v_(static_cast<double>(n)) {}
    Value(unsigned n) : v_(static_cast<double>(n)) {}
    Value(unsigned long n) : v_(static_cast<double>(n)) {}
    Value(unsigned long long n) : v_(static_cast<double>(n)) {}
    Value(double d) : v_(d) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(Array a) : v_(std::move(a)) {}
    Value(Object o) : v_(std::move(o)) {}

    static Value object() { return Value(Object{}); }
    static Value array() { return Value(Array{}); }

    Type type() const { return static_cast<Type>(v_.index()); }
    bool is_null() const { return type() == Type::Null; }
    bool is_bool() const { return type() == Type::Bool; }
    bool is_number() const { return type() == Type::Number; }
    bool is_string() const { return type() == Type::String; }
    bool is_array() const { return type() == Type::Array; }
    bool is_object() const { return type() == Type::Object; }

    // Python's truthiness, for the switches the frontend sends.
    bool truthy() const;
    bool as_bool(bool def = false) const;
    double as_double(double def = 0) const;
    long long as_int(long long def = 0) const;
    // Strings as they are; numbers and booleans as Python's str() would
    // write them; anything else gives def.
    std::string str(std::string_view def = "") const;
    const std::string& as_string() const;

    const Array& items() const;
    Array& items();
    const Object& members() const;
    Object& members();
    size_t size() const;

    bool has(std::string_view key) const;
    const Value* find(std::string_view key) const;
    const Value& operator[](std::string_view key) const;
    const Value& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    // Adds the key (null) when absent; a null value becomes an object first.
    // So a read of a value that is returned or saved goes through a const
    // reference (std::as_const), or the key would be added.
    Value& operator[](std::string_view key);
    Value& operator[](const char* key) { return (*this)[std::string_view(key)]; }
    const Value& at(size_t i) const;
    void set(std::string_view key, Value v) { (*this)[key] = std::move(v); }
    void erase(std::string_view key);
    void push(Value v);
    // Keys and values of other over this one (Python's dict.update).
    void update(const Value& other);

    // indent < 0: compact, as JSON.stringify; else pretty with that indent.
    std::string dump(int indent = -1) const;
    static std::optional<Value> parse(std::string_view text);

    bool operator==(const Value& o) const { return v_ == o.v_; }
    bool operator!=(const Value& o) const { return !(*this == o); }

private:
    void dump_to(std::string& out, int indent, int depth) const;
    std::variant<std::monostate, bool, double, std::string, Array, Object> v_;
};

// The JSON text of a string, quotes included.
std::string quote(std::string_view s);

}  // namespace pj

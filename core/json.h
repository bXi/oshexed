#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hx::json {

// A small JSON reader for HX Edit's preset files (.hlx). Objects keep their members in file order.

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    bool integer = false;           // a Number written without fraction or exponent
    std::string s;                  // String (UTF-8)
    std::vector<std::string> keys;  // Object: member names, parallel to items
    std::vector<Value> items;       // Array elements, or Object member values

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isBool() const { return type == Type::Bool; }

    // Object member by name; nullptr if absent or not an object.
    const Value* get(std::string_view key) const;
    double asNumber(double def = 0) const;   // Number or Bool
    std::int64_t asInt(std::int64_t def = 0) const;
    bool asBool(bool def = false) const;     // Bool, or a Number != 0
    std::string asString() const;            // String, else ""

    // Building values (for writing .hlx files).
    static Value object();
    static Value array();
    static Value boolean(bool v);
    static Value integerNumber(std::int64_t v);
    static Value real(double v);  // written with a fraction ("1.0"), at float precision
    static Value string(std::string v);
    // Adds or replaces an object member; returns it.
    Value& set(std::string key, Value v);
    Value& push(Value v);  // appends to an array; returns it
};

// The whole text as one value (trailing whitespace allowed); nullopt with a message on bad input.
std::optional<Value> parse(std::string_view text, std::string* error = nullptr);
// As text: objects in member order, two-space indent, "\n" line ends. Numbers that aren't finite become 0.
std::string write(const Value& v);

} // namespace hx::json

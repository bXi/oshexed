#pragma once

#include "core/bytes.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hx::mp {

// Read-only msgpack decoder for the firmware's preset records (NOTES.md "Records 8-15").
// Line 6 uses integer map keys, NUL-terminated strings, and str blobs that hold more msgpack.

enum class Type { Nil, Bool, Int, Float, Str, Bin, Array, Map, Ext };

struct Value {
    Type type = Type::Nil;
    bool b = false;
    std::int64_t i = 0;     // Int (uint64 above INT64_MAX is clamped; not seen in presets)
    double f = 0;           // Float (float32 widened)
    std::string s;          // Str, Bin, Ext payload bytes
    std::vector<Value> items;  // Array elements; Map as key, value, key, value, ...
    // The format byte it was read with (0 = built here). encode() reuses it while the value still fits, so an
    // unchanged tree is written back byte for byte (the unit writes some ints wider than needed, e.g. d1 00 00).
    std::uint8_t fmt = 0;

    bool isNil() const { return type == Type::Nil; }
    bool isMap() const { return type == Type::Map; }
    bool isArray() const { return type == Type::Array; }
    std::size_t size() const { return type == Type::Map ? items.size() / 2 : items.size(); }

    // Map lookup by integer key; nullptr if absent or not a map.
    const Value* get(std::int64_t key) const;
    // Array element; nullptr if out of range or not an array.
    const Value* at(std::size_t index) const;

    std::int64_t asInt(std::int64_t def = 0) const;
    double asNumber(double def = 0) const;  // Int, Float or Bool
    bool asBool(bool def = false) const;
    // Str/Bin as UTF-8 text, trailing NULs removed.
    std::string asText() const;
    Bytes bytes() const { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }
};

// One value starting at `offset`; `end` (if given) receives the offset after it. Throws FormatError.
Value parse(Bytes data, std::size_t offset = 0, std::size_t* end = nullptr);

// Every value in `data`, back to back (the preset records hold several top-level values).
std::vector<Value> parseAll(Bytes data);

// Building values (requests to the unit). Floats are written as float32, as the unit's own messages are.
Value nil();
Value boolean(bool b);
Value integer(std::int64_t i);
Value real(double f);
Value str(std::string s);  // as stored by the unit: NUL-terminated
Value array(std::vector<Value> items = {});
Value map(std::vector<std::pair<std::int64_t, Value>> members = {});  // integer keys, in order
// The msgpack bytes of a value (smallest encodings, as the unit uses).
std::vector<std::uint8_t> encode(const Value& v);
// Short readable text for logs, like ../tools/hxcap.py prints it: {98: 26, 109: 'name'}; long strings / bins as
// "<n bytes>"; cut at maxLen characters.
std::string toText(const Value& v, std::size_t maxLen = 300);

} // namespace hx::mp

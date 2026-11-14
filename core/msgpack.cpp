#include "core/msgpack.h"

#include "core/text.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <limits>

namespace hx::mp {

namespace {

constexpr int kMaxDepth = 64;

struct Reader {
    Bytes d;
    std::size_t p;

    std::uint8_t u8() {
        need(d, p, 1, "msgpack");
        return d[p++];
    }
    std::uint64_t be(int n) {
        need(d, p, std::size_t(n), "msgpack");
        std::uint64_t v = 0;
        for (int k = 0; k < n; ++k) v = v << 8 | d[p + std::size_t(k)];
        p += std::size_t(n);
        return v;
    }
    void raw(Value& v, Type t, std::size_t n) {
        need(d, p, n, "msgpack string");
        v.type = t;
        v.s.assign(reinterpret_cast<const char*>(d.data() + p), n);
        p += n;
    }
    // Each element takes at least one byte, so a count beyond the remaining data is corrupt.
    std::size_t count(std::uint64_t n, std::size_t perItem) {
        if (n > (d.size() - p) / perItem) throw FormatError("msgpack: container count runs past the data");
        return std::size_t(n);
    }
    void array(Value& v, std::size_t n, int depth) {
        v.type = Type::Array;
        v.items.resize(count(n, 1));
        for (Value& x : v.items) value(x, depth + 1);
    }
    void map(Value& v, std::size_t n, int depth) {
        v.type = Type::Map;
        v.items.resize(count(n, 2) * 2);
        for (Value& x : v.items) value(x, depth + 1);
    }
    void integer(Value& v, std::int64_t x) {
        v.type = Type::Int;
        v.i = x;
    }
    void uinteger(Value& v, std::uint64_t x) {
        integer(v, x > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ? std::numeric_limits<std::int64_t>::max()
                                                                               : std::int64_t(x));
    }

    void value(Value& v, int depth) {
        if (depth > kMaxDepth) throw FormatError("msgpack: nested too deep");
        const std::uint8_t t = u8();
        v.fmt = t;
        if (t <= 0x7f) return integer(v, t);
        if (t >= 0xe0) return integer(v, std::int8_t(t));
        if ((t & 0xf0) == 0x80) return map(v, t & 0x0f, depth);
        if ((t & 0xf0) == 0x90) return array(v, t & 0x0f, depth);
        if ((t & 0xe0) == 0xa0) return raw(v, Type::Str, t & 0x1f);
        switch (t) {
        case 0xc0: v.type = Type::Nil; return;
        case 0xc2: v.type = Type::Bool; v.b = false; return;
        case 0xc3: v.type = Type::Bool; v.b = true; return;
        case 0xc4: return raw(v, Type::Bin, std::size_t(be(1)));
        case 0xc5: return raw(v, Type::Bin, std::size_t(be(2)));
        case 0xc6: return raw(v, Type::Bin, std::size_t(be(4)));
        case 0xc7: { const std::size_t n = std::size_t(be(1)); u8(); return raw(v, Type::Ext, n); }
        case 0xc8: { const std::size_t n = std::size_t(be(2)); u8(); return raw(v, Type::Ext, n); }
        case 0xc9: { const std::size_t n = std::size_t(be(4)); u8(); return raw(v, Type::Ext, n); }
        case 0xca: v.type = Type::Float; v.f = double(std::bit_cast<float>(std::uint32_t(be(4)))); return;
        case 0xcb: v.type = Type::Float; v.f = std::bit_cast<double>(be(8)); return;
        case 0xcc: return uinteger(v, be(1));
        case 0xcd: return uinteger(v, be(2));
        case 0xce: return uinteger(v, be(4));
        case 0xcf: return uinteger(v, be(8));
        case 0xd0: return integer(v, std::int8_t(be(1)));
        case 0xd1: return integer(v, std::int16_t(be(2)));
        case 0xd2: return integer(v, std::int32_t(be(4)));
        case 0xd3: return integer(v, std::int64_t(be(8)));
        case 0xd4: u8(); return raw(v, Type::Ext, 1);
        case 0xd5: u8(); return raw(v, Type::Ext, 2);
        case 0xd6: u8(); return raw(v, Type::Ext, 4);
        case 0xd7: u8(); return raw(v, Type::Ext, 8);
        case 0xd8: u8(); return raw(v, Type::Ext, 16);
        case 0xd9: return raw(v, Type::Str, std::size_t(be(1)));
        case 0xda: return raw(v, Type::Str, std::size_t(be(2)));
        case 0xdb: return raw(v, Type::Str, std::size_t(be(4)));
        case 0xdc: return array(v, std::size_t(be(2)), depth);
        case 0xdd: return array(v, std::size_t(be(4)), depth);
        case 0xde: return map(v, std::size_t(be(2)), depth);
        case 0xdf: return map(v, std::size_t(be(4)), depth);
        default: throw FormatError("msgpack: unknown type byte " + std::to_string(t));
        }
    }
};

} // namespace

const Value* Value::get(std::int64_t key) const {
    if (type != Type::Map) return nullptr;
    for (std::size_t k = 0; k + 1 < items.size(); k += 2)
        if (items[k].type == Type::Int && items[k].i == key) return &items[k + 1];
    return nullptr;
}

const Value* Value::at(std::size_t index) const {
    return type == Type::Array && index < items.size() ? &items[index] : nullptr;
}

std::int64_t Value::asInt(std::int64_t def) const {
    switch (type) {
    case Type::Int: return i;
    case Type::Bool: return b ? 1 : 0;
    case Type::Float: return std::int64_t(f);
    default: return def;
    }
}

double Value::asNumber(double def) const {
    switch (type) {
    case Type::Int: return double(i);
    case Type::Float: return f;
    case Type::Bool: return b ? 1 : 0;
    default: return def;
    }
}

bool Value::asBool(bool def) const {
    switch (type) {
    case Type::Bool: return b;
    case Type::Int: return i != 0;
    default: return def;
    }
}

std::string Value::asText() const {
    if (type != Type::Str && type != Type::Bin) return {};
    return cString(bytes());
}

Value parse(Bytes data, std::size_t offset, std::size_t* end) {
    Reader r{data, offset};
    Value v;
    r.value(v, 0);
    if (end) *end = r.p;
    return v;
}

std::vector<Value> parseAll(Bytes data) {
    std::vector<Value> out;
    std::size_t p = 0;
    while (p < data.size()) out.push_back(parse(data, p, &p));
    return out;
}

Value nil() { return Value{}; }

Value boolean(bool b) {
    Value v;
    v.type = Type::Bool;
    v.b = b;
    return v;
}

Value integer(std::int64_t i) {
    Value v;
    v.type = Type::Int;
    v.i = i;
    return v;
}

Value real(double f) {
    Value v;
    v.type = Type::Float;
    v.f = f;
    return v;
}

Value str(std::string s) {
    Value v;
    v.type = Type::Str;
    v.s = std::move(s);
    v.s.push_back('\0');
    return v;
}

Value array(std::vector<Value> items) {
    Value v;
    v.type = Type::Array;
    v.items = std::move(items);
    return v;
}

Value map(std::vector<std::pair<std::int64_t, Value>> members) {
    Value v;
    v.type = Type::Map;
    for (auto& [k, x] : members) {
        v.items.push_back(integer(k));
        v.items.push_back(std::move(x));
    }
    return v;
}

namespace {

void putBe(std::vector<std::uint8_t>& o, std::uint64_t x, int n) {
    for (int k = n - 1; k >= 0; --k) o.push_back(std::uint8_t(x >> (8 * k)));
}

void encodeInt(std::vector<std::uint8_t>& o, std::int64_t i) {
    if (i >= 0) {
        if (i < 128) o.push_back(std::uint8_t(i));
        else if (i <= 0xff) o.push_back(0xcc), putBe(o, std::uint64_t(i), 1);
        else if (i <= 0xffff) o.push_back(0xcd), putBe(o, std::uint64_t(i), 2);
        else if (i <= 0xffffffffLL) o.push_back(0xce), putBe(o, std::uint64_t(i), 4);
        else o.push_back(0xcf), putBe(o, std::uint64_t(i), 8);
    } else {
        if (i >= -32) o.push_back(std::uint8_t(i));
        else if (i >= -128) o.push_back(0xd0), putBe(o, std::uint64_t(i), 1);
        else if (i >= -32768) o.push_back(0xd1), putBe(o, std::uint64_t(i), 2);
        else if (i >= -2147483648LL) o.push_back(0xd2), putBe(o, std::uint64_t(i), 4);
        else o.push_back(0xd3), putBe(o, std::uint64_t(i), 8);
    }
}

void encodeLen(std::vector<std::uint8_t>& o, std::size_t n, std::uint8_t fix, std::size_t fixMax, std::uint8_t c8,
               std::uint8_t c16, std::uint8_t c32) {
    if (n <= fixMax && fix) o.push_back(std::uint8_t(fix | n));
    else if (n <= 0xff && c8) o.push_back(c8), putBe(o, n, 1);
    else if (n <= 0xffff) o.push_back(c16), putBe(o, n, 2);
    else o.push_back(c32), putBe(o, n, 4);
}

// An int in the format it was read with, when that format still holds it (fixints excepted: they're the shortest
// anyway); false = let encodeInt pick.
bool encodeIntAs(std::vector<std::uint8_t>& o, std::int64_t i, std::uint8_t fmt) {
    auto fits = [&](std::int64_t lo, std::int64_t hi) { return i >= lo && i <= hi; };
    int n = 0;
    switch (fmt) {
    case 0xcc: n = fits(0, 0xff) ? 1 : 0; break;
    case 0xcd: n = fits(0, 0xffff) ? 2 : 0; break;
    case 0xce: n = fits(0, 0xffffffffLL) ? 4 : 0; break;
    case 0xcf: n = i >= 0 ? 8 : 0; break;
    case 0xd0: n = fits(-128, 127) ? 1 : 0; break;
    case 0xd1: n = fits(-32768, 32767) ? 2 : 0; break;
    case 0xd2: n = fits(-2147483648LL, 2147483647LL) ? 4 : 0; break;
    case 0xd3: n = 8; break;
    default: return false;
    }
    if (!n) return false;
    o.push_back(fmt);
    putBe(o, std::uint64_t(i), n);
    return true;
}

// A length in the width it was read with, if it still fits (fix forms: only when the count still fits them).
void encodeLenAs(std::vector<std::uint8_t>& o, std::size_t n, std::uint8_t fmt, std::uint8_t fix, std::size_t fixMax,
                 std::uint8_t c8, std::uint8_t c16, std::uint8_t c32) {
    if (fmt == c8 && c8 && n <= 0xff) return o.push_back(c8), putBe(o, n, 1);
    if (fmt == c16 && n <= 0xffff) return o.push_back(c16), putBe(o, n, 2);
    if (fmt == c32) return o.push_back(c32), putBe(o, n, 4);
    encodeLen(o, n, fix, fixMax, c8, c16, c32);
}

void encodeValue(std::vector<std::uint8_t>& o, const Value& v) {
    switch (v.type) {
    case Type::Nil: o.push_back(0xc0); break;
    case Type::Bool: o.push_back(v.b ? 0xc3 : 0xc2); break;
    case Type::Int:
        if (!encodeIntAs(o, v.i, v.fmt)) encodeInt(o, v.i);
        break;
    case Type::Float:
        if (v.fmt == 0xcb) {
            o.push_back(0xcb);
            putBe(o, std::bit_cast<std::uint64_t>(v.f), 8);
        } else {
            o.push_back(0xca);
            putBe(o, std::bit_cast<std::uint32_t>(float(v.f)), 4);
        }
        break;
    case Type::Str:
        encodeLenAs(o, v.s.size(), v.fmt, 0xa0, 31, 0xd9, 0xda, 0xdb);
        o.insert(o.end(), v.s.begin(), v.s.end());
        break;
    case Type::Bin:
    case Type::Ext:  // not used in requests: written as bin
        encodeLen(o, v.s.size(), 0, 0, 0xc4, 0xc5, 0xc6);
        o.insert(o.end(), v.s.begin(), v.s.end());
        break;
    case Type::Array:
        encodeLenAs(o, v.items.size(), v.fmt, 0x90, 15, 0, 0xdc, 0xdd);
        for (const Value& x : v.items) encodeValue(o, x);
        break;
    case Type::Map:
        encodeLenAs(o, v.items.size() / 2, v.fmt, 0x80, 15, 0, 0xde, 0xdf);
        for (const Value& x : v.items) encodeValue(o, x);
        break;
    }
}

} // namespace

std::vector<std::uint8_t> encode(const Value& v) {
    std::vector<std::uint8_t> out;
    encodeValue(out, v);
    return out;
}

namespace {

void appendText(std::string& o, const Value& v, std::size_t maxLen) {
    if (o.size() > maxLen) return;
    switch (v.type) {
    case Type::Nil: o += "nil"; break;
    case Type::Bool: o += v.b ? "true" : "false"; break;
    case Type::Int: o += std::to_string(v.i); break;
    case Type::Float: {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v.f);
        o += buf;
        break;
    }
    case Type::Str:
    case Type::Bin:
    case Type::Ext: {
        const std::string t = v.asText();
        const bool printable = !t.empty() && t.size() <= 64 &&
                               std::all_of(t.begin(), t.end(), [](char c) { return c >= 32 && c < 127; });
        if (printable || (v.s.empty())) o += "'" + t + "'";
        else o += "<" + std::to_string(v.s.size()) + " bytes>";
        break;
    }
    case Type::Array:
        o += "[";
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            if (i) o += ", ";
            appendText(o, v.items[i], maxLen);
        }
        o += "]";
        break;
    case Type::Map:
        o += "{";
        for (std::size_t i = 0; i + 1 < v.items.size(); i += 2) {
            if (i) o += ", ";
            appendText(o, v.items[i], maxLen);
            o += ": ";
            appendText(o, v.items[i + 1], maxLen);
        }
        o += "}";
        break;
    }
}

} // namespace

std::string toText(const Value& v, std::size_t maxLen) {
    std::string o;
    appendText(o, v, maxLen);
    if (o.size() > maxLen) o = o.substr(0, maxLen) + "...";
    return o;
}

} // namespace hx::mp

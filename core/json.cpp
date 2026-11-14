#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hx::json {

namespace {

constexpr int kMaxDepth = 64;

struct Parser {
    std::string_view t;
    std::size_t i = 0;
    std::string error;

    bool fail(const char* what) {
        if (error.empty()) error = std::string(what) + " at offset " + std::to_string(i);
        return false;
    }
    void space() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
    }
    bool literal(std::string_view word) {
        if (t.substr(i, word.size()) != word) return fail("unknown literal");
        i += word.size();
        return true;
    }
    static void utf8(std::string& out, std::uint32_t c) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xc0 | (c >> 6));
            out += char(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += char(0xe0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        } else {
            out += char(0xf0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3f));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        }
    }
    bool hex4(std::uint32_t& c) {
        if (i + 4 > t.size()) return fail("short \\u escape");
        c = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = t[i++];
            c <<= 4;
            if (h >= '0' && h <= '9') c |= std::uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') c |= std::uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') c |= std::uint32_t(h - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++i;  // opening quote
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size()) break;
            const char e = t[i++];
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
                std::uint32_t c1;
                if (!hex4(c1)) return false;
                // A surrogate pair is two escapes.
                if (c1 >= 0xd800 && c1 < 0xdc00 && t.substr(i, 2) == "\\u") {
                    i += 2;
                    std::uint32_t c2;
                    if (!hex4(c2)) return false;
                    if (c2 >= 0xdc00 && c2 < 0xe000) c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                }
                utf8(out, c1);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Value& v) {
        const std::size_t start = i;
        if (i < t.size() && t[i] == '-') ++i;
        bool fraction = false;
        while (i < t.size()) {
            const char c = t[i];
            if (c >= '0' && c <= '9') ++i;
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                fraction = true;
                ++i;
            } else break;
        }
        const std::string token(t.substr(start, i - start));
        char* end = nullptr;
        v.n = std::strtod(token.c_str(), &end);
        if (token.empty() || end != token.c_str() + token.size() || !std::isfinite(v.n)) return fail("bad number");
        v.type = Type::Number;
        v.integer = !fraction;
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > kMaxDepth) return fail("nested too deep");
        space();
        if (i >= t.size()) return fail("unexpected end");
        const char c = t[i];
        if (c == '{') {
            v.type = Type::Object;
            ++i;
            space();
            if (i < t.size() && t[i] == '}') return ++i, true;
            while (true) {
                space();
                if (i >= t.size() || t[i] != '"') return fail("expected a member name");
                std::string key;
                if (!string(key)) return false;
                space();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                ++i;
                Value member;
                if (!value(member, depth + 1)) return false;
                v.keys.push_back(std::move(key));
                v.items.push_back(std::move(member));
                space();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == '}') return ++i, true;
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Type::Array;
            ++i;
            space();
            if (i < t.size() && t[i] == ']') return ++i, true;
            while (true) {
                Value item;
                if (!value(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                space();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == ']') return ++i, true;
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Type::String;
            return string(v.s);
        }
        if (c == 't') {
            v.type = Type::Bool;
            v.b = true;
            return literal("true");
        }
        if (c == 'f') {
            v.type = Type::Bool;
            return literal("false");
        }
        if (c == 'n') return literal("null");
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return fail("unexpected character");
    }
};

} // namespace

const Value* Value::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (std::size_t k = 0; k < keys.size(); ++k)
        if (keys[k] == key) return &items[k];
    return nullptr;
}

double Value::asNumber(double def) const {
    if (type == Type::Number) return n;
    if (type == Type::Bool) return b ? 1 : 0;
    return def;
}

std::int64_t Value::asInt(std::int64_t def) const {
    if (type == Type::Number) return std::int64_t(std::llround(n));
    if (type == Type::Bool) return b ? 1 : 0;
    return def;
}

bool Value::asBool(bool def) const {
    if (type == Type::Bool) return b;
    if (type == Type::Number) return n != 0;
    return def;
}

std::string Value::asString() const { return type == Type::String ? s : std::string(); }

std::optional<Value> parse(std::string_view text, std::string* error) {
    Parser p{text};
    // A UTF-8 byte order mark is allowed in front.
    if (text.substr(0, 3) == "\xef\xbb\xbf") p.i = 3;
    Value v;
    bool ok = p.value(v, 0);
    if (ok) {
        p.space();
        if (p.i != text.size()) ok = p.fail("text after the value");
    }
    if (!ok) {
        if (error) *error = p.error;
        return std::nullopt;
    }
    return v;
}

Value Value::object() {
    Value v;
    v.type = Type::Object;
    return v;
}

Value Value::array() {
    Value v;
    v.type = Type::Array;
    return v;
}

Value Value::boolean(bool b) {
    Value v;
    v.type = Type::Bool;
    v.b = b;
    return v;
}

Value Value::integerNumber(std::int64_t i) {
    Value v;
    v.type = Type::Number;
    v.n = double(i);
    v.integer = true;
    return v;
}

Value Value::real(double d) {
    Value v;
    v.type = Type::Number;
    v.n = d;
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.type = Type::String;
    v.s = std::move(s);
    return v;
}

Value& Value::set(std::string key, Value v) {
    for (std::size_t k = 0; k < keys.size(); ++k)
        if (keys[k] == key) return items[k] = std::move(v);
    keys.push_back(std::move(key));
    items.push_back(std::move(v));
    return items.back();
}

Value& Value::push(Value v) {
    items.push_back(std::move(v));
    return items.back();
}

namespace {

// The shortest text that reads back as the same float (preset values are 32-bit floats), always with a fraction or
// exponent so it stays a real number.
std::string realText(double d) {
    if (!std::isfinite(d)) return "0.0";
    char buf[40];
    for (int digits = 6; digits <= 9; ++digits) {
        std::snprintf(buf, sizeof buf, "%.*g", digits, d);
        if (float(std::strtod(buf, nullptr)) == float(d)) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof esc, "\\u%04x", unsigned(c));
                out += esc;
            } else {
                out += ch;  // UTF-8 passes through
            }
        }
    }
    out += '"';
}

void writeValue(std::string& out, const Value& v, int depth) {
    const std::string pad(std::size_t(depth + 1) * 2, ' '), end(std::size_t(depth) * 2, ' ');
    switch (v.type) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += v.b ? "true" : "false"; break;
    case Type::Number:
        if (v.integer && std::isfinite(v.n)) out += std::to_string(std::int64_t(std::llround(v.n)));
        else out += realText(v.n);
        break;
    case Type::String: writeString(out, v.s); break;
    case Type::Array:
        if (v.items.empty()) {
            out += "[]";
            break;
        }
        out += "[\n";
        for (std::size_t k = 0; k < v.items.size(); ++k) {
            out += pad;
            writeValue(out, v.items[k], depth + 1);
            out += k + 1 < v.items.size() ? ",\n" : "\n";
        }
        out += end + "]";
        break;
    case Type::Object:
        if (v.keys.empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        for (std::size_t k = 0; k < v.keys.size(); ++k) {
            out += pad;
            writeString(out, v.keys[k]);
            out += ": ";
            writeValue(out, v.items[k], depth + 1);
            out += k + 1 < v.keys.size() ? ",\n" : "\n";
        }
        out += end + "}";
        break;
    }
}

} // namespace

std::string write(const Value& v) {
    std::string out;
    writeValue(out, v, 0);
    out += '\n';
    return out;
}

} // namespace hx::json

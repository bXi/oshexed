#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hx {
    
using Bytes = std::span<const std::uint8_t>;

// Thrown by the checked readers when a table points outside its data. Parsers catch it at the
// top and turn it into an error message, so a damaged or unexpected file never crashes the app.
struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Unchecked: callers check the length first.
inline std::uint32_t readBE32(Bytes b, std::size_t off) {
    return std::uint32_t(b[off]) << 24 | std::uint32_t(b[off + 1]) << 16 | std::uint32_t(b[off + 2]) << 8 |
           std::uint32_t(b[off + 3]);
}

inline std::uint32_t readLE32(Bytes b, std::size_t off) {
    return std::uint32_t(b[off]) | std::uint32_t(b[off + 1]) << 8 | std::uint32_t(b[off + 2]) << 16 |
           std::uint32_t(b[off + 3]) << 24;
}

inline std::uint16_t readLE16(Bytes b, std::size_t off) { return std::uint16_t(b[off] | b[off + 1] << 8); }

// Checked: throw FormatError when [off, off+n) is not inside b.
inline void need(Bytes b, std::size_t off, std::size_t n, const char* what) {
    if (off > b.size() || n > b.size() - off)
        throw FormatError(std::string(what) + ": offset " + std::to_string(off) + " out of range");
}

inline std::uint32_t le32(Bytes b, std::size_t off, const char* what = "read") {
    need(b, off, 4, what);
    return readLE32(b, off);
}

inline Bytes slice(Bytes b, std::size_t off, std::size_t n, const char* what = "slice") {
    need(b, off, n, what);
    return b.subspan(off, n);
}

// Reverses every 32-bit word in place (file BE <-> device LE). A trailing partial word is left alone.
inline void swapWordsInPlace(std::span<std::uint8_t> b) {
    for (std::size_t i = 0; i + 4 <= b.size(); i += 4) {
        std::swap(b[i], b[i + 3]);
        std::swap(b[i + 1], b[i + 2]);
    }
}

// L6IA payloads are stored as big-endian 32-bit words; swapping each word gives device-native
// (LE) data (NOTES.md "Inner: L6IA image").
inline std::vector<std::uint8_t> swapWords(Bytes b) {
    std::vector<std::uint8_t> out(b.begin(), b.end());
    swapWordsInPlace(out);
    return out;
}

} // namespace hx

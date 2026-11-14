#pragma once

// Builds synthetic binary data for the tests. No firmware bytes in the repo.

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace test {

struct Buf {
    std::vector<std::uint8_t> b;

    std::size_t size() const { return b.size(); }
    void le32(std::uint32_t v) {
        for (int s = 0; s < 32; s += 8) b.push_back(std::uint8_t(v >> s));
    }
    void le16(std::uint16_t v) {
        b.push_back(std::uint8_t(v));
        b.push_back(std::uint8_t(v >> 8));
    }
    void be32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) b.push_back(std::uint8_t(v >> s));
    }
    void bytes(std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }
    void zeros(std::size_t n) { b.insert(b.end(), n, 0); }
    void padTo(std::size_t n) {
        if (b.size() < n) zeros(n - b.size());
    }
    void putLE32(std::size_t off, std::uint32_t v) {
        for (int k = 0; k < 4; ++k) b[off + k] = std::uint8_t(v >> (8 * k));
    }
    void putBE32(std::size_t off, std::uint32_t v) {
        for (int k = 0; k < 4; ++k) b[off + k] = std::uint8_t(v >> (24 - 8 * k));
    }
};

} // namespace test

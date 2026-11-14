#pragma once

#include "core/bytes.h"

#include <array>
#include <cstdint>
#include <string>

namespace hx {

// zlib CRC32 (same as Python's zlib.crc32).
std::uint32_t crc32(Bytes b);

std::array<std::uint8_t, 16> md5(Bytes b);

// SHA-256 (FIPS 180-4). Fingerprints of record files and, later, of the stock bytes a mod expects.
std::array<std::uint8_t, 32> sha256(Bytes b);
std::string toHex(Bytes b);

// Record hash8 (NOTES.md "Inner: L6IA image"): sum1 = sum of LE u32 words, sum2 = sum of the
// running sum1, both mod 2^32. Feed the parts in order with add(); each part except the last must be a
// whole number of words.
struct Fletcher {
    std::uint32_t sum1 = 0, sum2 = 0;
    void add(Bytes words);
};

} // namespace hx

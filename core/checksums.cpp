#include "core/checksums.h"

#include <miniz.h>

#include <cstring>

namespace hx {

std::uint32_t crc32(Bytes b) {
    // mz_crc32 takes a size_t length, so one call covers any size.
    return std::uint32_t(mz_crc32(MZ_CRC32_INIT, b.data(), b.size()));
}

void Fletcher::add(Bytes words) {
    std::uint32_t s1 = sum1, s2 = sum2;
    const std::size_t n = words.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        s1 += readLE32(words, i * 4);
        s2 += s1;
    }
    sum1 = s1;
    sum2 = s2;
}

// MD5 per RFC 1321.
namespace {

constexpr std::uint32_t kK[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline std::uint32_t rotl(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void md5Block(std::uint32_t h[4], const std::uint8_t* p) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = std::uint32_t(p[i * 4]) | std::uint32_t(p[i * 4 + 1]) << 8 | std::uint32_t(p[i * 4 + 2]) << 16 |
               std::uint32_t(p[i * 4 + 3]) << 24;
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t t = d;
        d = c;
        c = b;
        b = b + rotl(a + f + kK[i] + m[g], kS[i]);
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

} // namespace

std::array<std::uint8_t, 16> md5(Bytes b) {
    std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::size_t i = 0;
    for (; i + 64 <= b.size(); i += 64) md5Block(h, b.data() + i);

    // Tail: the remaining bytes, 0x80, zeros, then the bit length as LE u64.
    std::uint8_t tail[128] = {};
    const std::size_t rest = b.size() - i;
    if (rest) std::memcpy(tail, b.data() + i, rest);
    tail[rest] = 0x80;
    const std::size_t tailLen = rest < 56 ? 64 : 128;
    const std::uint64_t bits = std::uint64_t(b.size()) * 8;
    for (int k = 0; k < 8; ++k) tail[tailLen - 8 + k] = std::uint8_t(bits >> (8 * k));
    md5Block(h, tail);
    if (tailLen == 128) md5Block(h, tail + 64);

    std::array<std::uint8_t, 16> out{};
    for (int k = 0; k < 16; ++k) out[k] = std::uint8_t(h[k / 4] >> (8 * (k % 4)));
    return out;
}

namespace {

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(std::uint32_t h[8], const std::uint8_t *p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = std::uint32_t(p[4 * i]) << 24 | std::uint32_t(p[4 * i + 1]) << 16 | std::uint32_t(p[4 * i + 2]) << 8 |
               std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
        const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

} // namespace

std::array<std::uint8_t, 32> sha256(Bytes b) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t i = 0;
    for (; i + 64 <= b.size(); i += 64) sha256Block(h, b.data() + i);
    std::uint8_t tail[128] = {};
    const std::size_t rest = b.size() - i;
    for (std::size_t k = 0; k < rest; ++k) tail[k] = b[i + k];
    tail[rest] = 0x80;
    const std::size_t n = rest < 56 ? 64 : 128;
    const std::uint64_t bits = std::uint64_t(b.size()) * 8;
    for (int k = 0; k < 8; ++k) tail[n - 1 - k] = std::uint8_t(bits >> (8 * k));
    sha256Block(h, tail);
    if (n == 128) sha256Block(h, tail + 64);
    std::array<std::uint8_t, 32> out{};
    for (int k = 0; k < 8; ++k)
        for (int j = 0; j < 4; ++j) out[4 * k + j] = std::uint8_t(h[k] >> (24 - 8 * j));
    return out;
}

std::string toHex(Bytes b) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (std::uint8_t c : b) {
        s += digits[c >> 4];
        s += digits[c & 15];
    }
    return s;
}

} // namespace hx

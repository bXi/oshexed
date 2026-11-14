#include "test.h"

#include "core/checksums.h"

#include <string>
#include <string_view>

namespace {

hx::Bytes text(std::string_view s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }

std::string hexOf(const std::array<std::uint8_t, 16>& d) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::uint8_t c : d) {
        s += digits[c >> 4];
        s += digits[c & 15];
    }
    return s;
}

} // namespace

TEST("checksums: md5 test vectors (RFC 1321)") {
    CHECK_EQ(hexOf(hx::md5(text(""))), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(hexOf(hx::md5(text("abc"))), std::string("900150983cd24fb0d6963f7d28e17f72"));
    // 80 bytes: the padding spills into a second block.
    CHECK_EQ(hexOf(hx::md5(text("12345678901234567890123456789012345678901234567890123456789012345678901234567890"))),
             std::string("57edf4a22be3c955ac49da2e2107b67a"));
    // 56 bytes: the edge where the length no longer fits the first tail block.
    CHECK_EQ(hexOf(hx::md5(text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
             std::string("8215ef0796a20bcaaae116d3876c664a"));
}

TEST("checksums: crc32 check value") {
    CHECK_EQ(hx::crc32(text("123456789")), 0xcbf43926u);
}

TEST("checksums: fletcher sums words and running sums") {
    const std::uint8_t a[] = {1, 0, 0, 0, 2, 0, 0, 0};
    const std::uint8_t b[] = {3, 0, 0, 0, 9};  // trailing partial word ignored
    hx::Fletcher f;
    f.add(a);
    f.add(b);
    CHECK_EQ(f.sum1, 6u);           // 1 + 2 + 3
    CHECK_EQ(f.sum2, 10u);          // 1 + 3 + 6
}

TEST("checksums: sha256 test vectors (FIPS 180-4)") {
    CHECK_EQ(hx::toHex(hx::sha256(text(""))), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(hx::toHex(hx::sha256(text("abc"))), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    // 56 bytes: the length spills into a second tail block.
    CHECK_EQ(hx::toHex(hx::sha256(text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // Exactly one block, then 1000 bytes (several blocks plus a tail).
    CHECK_EQ(hx::toHex(hx::sha256(text(std::string(64, 'a')))),
             std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
    CHECK_EQ(hx::toHex(hx::sha256(text(std::string(1000, 'a')))),
             std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

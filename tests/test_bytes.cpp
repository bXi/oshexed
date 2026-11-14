#include "test.h"

#include "core/bytes.h"

TEST("bytes: BE and LE reads") {
    const std::uint8_t b[] = {0x12, 0x34, 0x56, 0x78, 0x9a};
    CHECK_EQ(hx::readBE32(b, 0), 0x12345678u);
    CHECK_EQ(hx::readLE32(b, 0), 0x78563412u);
    CHECK_EQ(hx::readBE32(b, 1), 0x3456789au);
}

TEST("bytes: word swap keeps a trailing partial word") {
    const std::uint8_t b[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const std::vector<std::uint8_t> want = {4, 3, 2, 1, 8, 7, 6, 5, 9, 10};
    CHECK(hx::swapWords(b) == want);
}

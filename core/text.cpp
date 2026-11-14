#include "core/text.h"

#include <algorithm>

namespace hx {

std::string latin1ToUtf8(Bytes b) {
    std::string out;
    out.reserve(b.size());
    for (std::uint8_t c : b) {
        if (c < 0x80) {
            out += char(c);
        } else {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string cString(Bytes b) {
    const auto end = std::find(b.begin(), b.end(), std::uint8_t(0));
    return latin1ToUtf8(b.first(std::size_t(end - b.begin())));
}

} // namespace hx

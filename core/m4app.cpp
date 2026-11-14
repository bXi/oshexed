#include "core/m4app.h"

namespace hx {

namespace {

constexpr std::uint32_t kAppMagic = 0x230B0C5E;
constexpr std::size_t kChainStart = 0x34;

} // namespace

Bytes AppImage::at(std::uint32_t va, std::size_t n) const {
    for (const AppSegment& s : segments)
        if (va >= s.addr && va - s.addr <= s.data.size() && n <= s.data.size() - (va - s.addr))
            return s.data.subspan(va - s.addr, n);
    throw FormatError("address " + std::to_string(va) + " not in the app image");
}

std::optional<AppImage> parseApp(Bytes payload, std::string* error) {
    try {
        if (le32(payload, 0, "app header") != kAppMagic) throw FormatError("not an MCU app (bad magic)");
        const std::uint32_t total = le32(payload, 4, "app header");
        if (total > payload.size()) throw FormatError("app size runs past the record");
        AppImage app;
        app.entry = le32(payload, 0x14, "app header");
        std::size_t off = kChainStart;
        while (off < total) {
            const std::uint32_t len = le32(payload, off, "segment");
            const std::uint32_t addr = le32(payload, off + 4, "segment");
            const std::uint32_t next = le32(payload, off + 8, "segment");
            app.segments.push_back({addr, slice(payload, off + 12, len, "segment data")});
            if (next <= off) throw FormatError("bad segment chain at " + std::to_string(off));
            off = next;
        }
        return app;
    } catch (const FormatError& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

} // namespace hx

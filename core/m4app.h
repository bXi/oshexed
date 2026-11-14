#pragma once

#include "core/bytes.h"

#include <optional>
#include <string>
#include <vector>

namespace hx {

// The M4 app record (rec 0) as loadable segments (NOTES.md "MCU app format"; tools/app2elf.py).
struct AppSegment {
    std::uint32_t addr = 0;
    Bytes data;
};

struct AppImage {
    std::uint32_t entry = 0;
    std::vector<AppSegment> segments;

    // The n bytes at virtual address va, or FormatError if they don't sit inside one segment.
    Bytes at(std::uint32_t va, std::size_t n) const;
    std::uint32_t u32(std::uint32_t va) const { return readLE32(at(va, 4), 0); }
};

// `payload` must outlive the result.
std::optional<AppImage> parseApp(Bytes payload, std::string* error = nullptr);

} // namespace hx

#pragma once

#include "core/bytes.h"

#include <filesystem>
#include <string>

namespace hx {

// Firmware strings are 8-bit (the Python tools read them as latin-1). RmlUi wants UTF-8.
std::string latin1ToUtf8(Bytes b);

// Bytes up to the first NUL (or the end), as UTF-8.
std::string cString(Bytes b);

// Paths are UTF-8 everywhere in the app (SDL hands them over that way). On Windows a plain
// std::string path would be read in the ANSI code page, so go through u8string.
inline std::filesystem::path pathFromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
inline std::string utf8FromPath(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace hx

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hx {

// One HTTP(S) GET, the whole body; false with a one-line reason (no connection, status != 200, too big).
// Windows: WinHTTP (ships with Windows, no library of ours). Elsewhere: not yet (returns false).
// maxBytes guards against a wrong URL serving something huge.
bool httpGet(const std::string& url, std::vector<std::uint8_t>& body, std::string* error = nullptr,
             std::size_t maxBytes = 64u * 1024u * 1024u);

} // namespace hx

#pragma once

#include "core/bytes.h"

#include <functional>
#include <vector>

namespace hx {

// zlib stream of `data`, as Python's zlib.compress(data, level). Line 6's .hxf data chunk is exactly this at level 9,
// so a repack of unchanged records gives back the stock file byte for byte. Throws std::runtime_error on failure.
// progress (optional): the share of the input done so far (0..1), after each 1 MB piece. Feeding zlib in pieces
// without flushes gives the same stream as one call.
std::vector<std::uint8_t> zlibCompress(Bytes data, int level = 9, const std::function<void(double)>& progress = {});

} // namespace hx

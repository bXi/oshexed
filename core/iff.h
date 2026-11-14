#pragma once

#include "core/bytes.h"

#include <optional>
#include <string>
#include <vector>

namespace hx {

// One chunk inside the .hxf's outer FORM.
struct IffChunk {
    std::string id;     // 4 chars, e.g. "HEAD", "dinf", "data"
    std::size_t offset; // of the chunk body, from the start of the file
    Bytes body;
};

struct IffForm {
    std::string type;   // "L6FF" for a .hxf
    std::vector<IffChunk> chunks;
};

// Splits a big-endian IFF FORM into its chunks. The .hxf has no pad byte after odd-length chunks
// (NOTES.md "Outer: IFF FORM / L6FF"), so none is skipped. Returns nothing and fills `error`
// when the data is not a well-formed FORM.
std::optional<IffForm> parseIff(Bytes data, std::string* error = nullptr);

} // namespace hx

#pragma once

#include <cstddef>

namespace editor {

// ui/ and assets/ as a zip inside the executable (OSHEXED_EMBED_ASSETS, as lumiSudoku does it), or empty when the
// build copies the folders next to it instead.
struct EmbeddedAssets {
    const unsigned char *data = nullptr;
    std::size_t size = 0;
};

EmbeddedAssets embeddedAssets();

} // namespace editor

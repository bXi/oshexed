#pragma once

// The unit's screen graphics, read from the user's firmware (NOTES.md "Records 8-15", port of
// tools/fwedit.py gfx_tables / to_rgb).
//   rec 8: header [magic 0x5b21255e, 1, size, nameTable, count, imageRecords]; names {offset, length};
//          image record 48 bytes [0, end, idx, idx, 0, width, height, bpp 16, keyColour, dataIdx, glyphs, glyphTable]
//   rec 9: header [magic, 1, count, table, ...]; table {size, offset}; pixels little-endian xRGB1555
// Fonts are records with bpp != 16 and are skipped here.

#include "core/bytes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hx {

struct AppImage;

struct GfxImage {
    std::uint32_t index = 0;  // record number in rec 8 (what the M4 code uses as an image id)
    std::string name;         // "Block_Amp", "Icon_..."
    std::uint16_t width = 0, height = 0;
    std::size_t offset = 0;   // of the pixels in rec 9
};

class Graphics {
public:
    // rec9 must outlive the result.
    static std::optional<Graphics> parse(Bytes rec8, Bytes rec9, std::string *error = nullptr);

    const std::vector<GfxImage> &images() const { return images_; }
    const GfxImage *byIndex(std::uint32_t index) const;
    const GfxImage *byName(std::string_view name) const;

    // 8-bit RGB, rows top to bottom, same rounding as fwedit.py (c * 255 / 31).
    std::vector<std::uint8_t> rgb(const GfxImage &img) const;
    // RGBA for drawing on a dark background: the images are drawn for the unit's black screen, so alpha = the
    // brightest channel (black vanishes, colours keep their look) and RGB stays as is (premultiplied).
    // crop: pixels cut off each edge (block icons carry a 2 px frame ring); the result is
    // (width - 2 * crop) x (height - 2 * crop), or empty when nothing would be left.
    std::vector<std::uint8_t> rgbaOnDark(const GfxImage &img, int crop = 0) const;

private:
    Bytes data_;
    std::vector<GfxImage> images_;
};

// Block icons per browser category (M4 app table, v3.80 LT at 0x280d8a1c; NOTES.md "Block icons and colours"):
// 19 categories x 6 rec 8 image ids.
enum class IconState { Normal, Move, Sel, Off, MoveOff, SelOff };
// Our mod builds move the table and add a 20th row (the Custom category, a copy of the Amp row): it is read through
// the code's pointer to it.
struct CategoryIcons {
    std::array<std::array<std::uint32_t, 6>, 20> ids{};
    int count = 19;
    std::uint32_t id(int category, IconState s) const {
        return category >= 0 && category < count ? ids[std::size_t(category)][std::size_t(s)] : ids[0][0];
    }
};
// nullopt (and *error) when the table isn't where v3.80 LT has it or doesn't point at images.
std::optional<CategoryIcons> readCategoryIcons(const AppImage &app, const Graphics &gfx, std::string *error = nullptr);

} // namespace hx

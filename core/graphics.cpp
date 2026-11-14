#include "core/graphics.h"

#include "core/m4app.h"

#include <algorithm>

namespace hx {

namespace {

constexpr std::uint32_t kRec8Magic = 0x5b21255e;
constexpr std::uint32_t kIconTable = 0x280d8a1c;  // v3.80 LT, 19 x 6 u32
constexpr std::uint32_t kIconLit = 0x280d42fc;    // a code literal holding the table's address (catpatch TABLES)

} // namespace

std::optional<Graphics> Graphics::parse(Bytes rec8, Bytes rec9, std::string *error) {
    try {
        if (le32(rec8, 0, "rec 8 header") != kRec8Magic) throw FormatError("rec 8: unexpected magic");
        const std::uint32_t nameTable = le32(rec8, 12), count = le32(rec8, 16), records = le32(rec8, 20);
        const std::uint32_t n9 = le32(rec9, 8, "rec 9 header"), t9 = le32(rec9, 12);
        need(rec9, t9, std::size_t(n9) * 8, "rec 9 table");
        Graphics g;
        g.data_ = rec9;
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint32_t nameOff = le32(rec8, nameTable + k * 8), nameLen = le32(rec8, nameTable + k * 8 + 4);
            const Bytes rec = slice(rec8, records + std::size_t(k) * 48, 48, "rec 8 image record");
            const std::uint32_t w = readLE32(rec, 20), h = readLE32(rec, 24), bpp = readLE32(rec, 28),
                                di = readLE32(rec, 36);
            if (bpp != 16 || di >= n9 || w == 0 || h == 0 || w > 0xffff || h > 0xffff) continue;
            const std::uint32_t size = readLE32(rec9, t9 + di * 8), off = readLE32(rec9, t9 + di * 8 + 4);
            if (size != w * h * 2 || off > rec9.size() || size > rec9.size() - off) continue;
            const Bytes name = slice(rec8, nameOff, nameLen, "rec 8 name");
            g.images_.push_back({k, std::string(name.begin(), name.end()), std::uint16_t(w), std::uint16_t(h), off});
        }
        return g;
    } catch (const FormatError &e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

const GfxImage *Graphics::byIndex(std::uint32_t index) const {
    const auto it = std::lower_bound(images_.begin(), images_.end(), index,
                                     [](const GfxImage &g, std::uint32_t i) { return g.index < i; });
    return it != images_.end() && it->index == index ? &*it : nullptr;
}

const GfxImage *Graphics::byName(std::string_view name) const {
    for (const GfxImage &g : images_)
        if (g.name == name) return &g;
    return nullptr;
}

std::vector<std::uint8_t> Graphics::rgb(const GfxImage &img) const {
    const std::size_t n = std::size_t(img.width) * img.height;
    std::vector<std::uint8_t> out(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint16_t p = readLE16(data_, img.offset + i * 2);
        out[i * 3] = std::uint8_t((p >> 10 & 31) * 255 / 31);
        out[i * 3 + 1] = std::uint8_t((p >> 5 & 31) * 255 / 31);
        out[i * 3 + 2] = std::uint8_t((p & 31) * 255 / 31);
    }
    return out;
}

std::vector<std::uint8_t> Graphics::rgbaOnDark(const GfxImage &img, int crop) const {
    const int w = img.width - 2 * crop, h = img.height - 2 * crop;
    if (crop < 0 || w <= 0 || h <= 0) return {};
    const std::vector<std::uint8_t> c = rgb(img);
    std::vector<std::uint8_t> out(std::size_t(w) * std::size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint8_t *p = &c[(std::size_t(y + crop) * img.width + std::size_t(x + crop)) * 3];
            std::uint8_t *q = &out[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4];
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
            q[3] = std::max({p[0], p[1], p[2]});
        }
    return out;
}

std::optional<CategoryIcons> readCategoryIcons(const AppImage &app, const Graphics &gfx, std::string *error) {
    try {
        CategoryIcons icons;
        const std::uint32_t table = app.u32(kIconLit);
        icons.count = table == kIconTable ? 19 : 20;
        const Bytes t = app.at(table, std::size_t(icons.count) * 6 * 4);
        for (std::size_t c = 0; c < std::size_t(icons.count); ++c)
            for (std::size_t s = 0; s < 6; ++s) {
                const std::uint32_t id = readLE32(t, (c * 6 + s) * 4);
                const GfxImage *img = gfx.byIndex(id);
                if (!img || img->name.rfind("Block_", 0) != 0)
                    throw FormatError("icon table entry " + std::to_string(c) + "." + std::to_string(s) +
                                      " is not a block image");
                icons.ids[c][s] = id;
            }
        return icons;
    } catch (const FormatError &e) {
        if (error) *error = std::string("block icon table: ") + e.what();
        return std::nullopt;
    }
}

} // namespace hx

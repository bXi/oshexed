#include "test.h"

#include "builder.h"
#include "core/graphics.h"

#include <string>

namespace {

// rec 8 with three records (two images, one font) and rec 9 with their pixel blocks.
struct GfxFixture {
    test::Buf rec8, rec9;

    GfxFixture() {
        const char *names[] = {"Block_A", "Font_X", "Block_B"};
        // rec 8: header 0x18 bytes, name table at 0x18, strings after it, image records at 0x60.
        rec8.le32(0x5b21255e);
        rec8.le32(1);
        rec8.le32(0);
        rec8.le32(0x18);   // name table
        rec8.le32(3);      // count
        rec8.le32(0x60);   // image records
        std::size_t str = 0x30;
        for (const char *n : names) {
            rec8.le32(std::uint32_t(str));
            rec8.le32(std::uint32_t(std::string(n).size()));
            str += 8;
        }
        rec8.padTo(0x30);
        for (const char *n : names) {
            rec8.bytes(n);
            rec8.padTo(rec8.size() + (8 - std::string(n).size()));
        }
        rec8.padTo(0x60);
        auto image = [&](std::uint32_t w, std::uint32_t h, std::uint32_t bpp, std::uint32_t di) {
            for (std::uint32_t v : {0u, 0u, 0u, 0u, 0u, w, h, bpp, 0x1f1f1fu, di, 0u, 0u}) rec8.le32(v);
        };
        image(2, 1, 16, 0);  // Block_A: 2x1
        image(1, 1, 1, 1);   // Font_X: skipped (bpp != 16)
        image(1, 2, 16, 2);  // Block_B: 1x2

        // rec 9: header, table of 3 {size, offset} at 0x18, pixels from 0x30.
        rec9.le32(0x2a5f3b5e);
        rec9.le32(1);
        rec9.le32(3);
        rec9.le32(0x18);
        rec9.padTo(0x18);
        for (std::uint32_t v : {4u, 0x30u, 2u, 0x34u, 4u, 0x36u}) rec9.le32(v);
        rec9.padTo(0x30);
        rec9.le16(0x7c00);  // Block_A: red, black
        rec9.le16(0x0000);
        rec9.le16(0x7fff);  // font junk
        rec9.le16(0x001f);  // Block_B: blue, white
        rec9.le16(0x7fff);
    }
};

} // namespace

TEST("graphics: images, names and font skipping") {
    GfxFixture f;
    std::string err;
    const std::optional<hx::Graphics> g = hx::Graphics::parse(f.rec8.b, f.rec9.b, &err);
    CHECK(g.has_value());
    if (!g) return;
    CHECK_EQ(g->images().size(), std::size_t(2));
    const hx::GfxImage *b = g->byName("Block_B");
    CHECK(b != nullptr && b->index == 2 && b->width == 1 && b->height == 2);
    CHECK(g->byIndex(1) == nullptr);  // the font
    CHECK(g->byIndex(0) != nullptr && g->byIndex(0)->name == "Block_A");
}

TEST("graphics: xRGB1555 to RGB and RGBA on dark") {
    GfxFixture f;
    const std::optional<hx::Graphics> g = hx::Graphics::parse(f.rec8.b, f.rec9.b);
    CHECK(g.has_value());
    if (!g) return;
    const std::vector<std::uint8_t> rgb = g->rgb(*g->byIndex(0));
    const std::vector<std::uint8_t> want = {255, 0, 0, 0, 0, 0};
    CHECK(rgb == want);
    const std::vector<std::uint8_t> rgba = g->rgbaOnDark(*g->byIndex(2));
    const std::vector<std::uint8_t> wantA = {0, 0, 255, 255, 255, 255, 255, 255};
    CHECK(rgba == wantA);
    // Black on the unit's screen = fully transparent here.
    CHECK_EQ(int(g->rgbaOnDark(*g->byIndex(0))[7]), 0);
}

TEST("graphics: bad magic is an error, not a crash") {
    GfxFixture f;
    f.rec8.putLE32(0, 0);
    std::string err;
    CHECK(!hx::Graphics::parse(f.rec8.b, f.rec9.b, &err).has_value());
    CHECK(!err.empty());
}

TEST("graphics: crop cuts the frame ring off") {
    GfxFixture f;
    const std::optional<hx::Graphics> g = hx::Graphics::parse(f.rec8.b, f.rec9.b);
    CHECK(g.has_value());
    if (!g) return;
    // Block_A is 2x1: no crop keeps it whole, any crop leaves nothing.
    CHECK_EQ(g->rgbaOnDark(*g->byIndex(0), 0).size(), std::size_t(8));
    CHECK(g->rgbaOnDark(*g->byIndex(0), 1).empty());
}

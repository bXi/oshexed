#include "test.h"

#include "core/modeltables.h"
#include "fixtures.h"

using test::MetaFixture;

namespace {

// The v3.80 LT table addresses (core/modeltables.cpp) and the code literals that point at them, in one synthetic
// segment.
constexpr std::uint32_t kBase = 0x280e0000;
constexpr std::uint32_t kCost = 0x2813bba4, kCatNSub = 0x2813ae18, kCatPtrs = 0x2813abb8, kCatCnts = 0x2813ace8;
constexpr std::uint32_t kLabel = 0x280ff490;
constexpr std::uint32_t kCostLit = 0x2813b708, kCostEndLit = 0x2813b750;
constexpr std::uint32_t kNSubLit = 0x2812d908, kPtrsLit = 0x2812d7ec, kCntsLit = 0x2812d918, kLabelLit = 0x280fef60;
constexpr std::uint32_t kList = 0x28131000;

struct AppFixture {
    test::Buf seg;
    AppFixture() {
        seg.zeros(0x60000);
        put(kCostLit, kCost);
        put(kCostEndLit, kCost + 12 * 0x337);
        put(kNSubLit, kCatNSub);
        put(kPtrsLit, kCatPtrs);
        put(kCntsLit, kCatCnts);
        put(kLabelLit, kLabel);
        put(0x2812d7b4, 0x2813ae64);  // sub-list names
    }
    void put(std::uint32_t va, std::uint32_t v) { seg.putLE32(va - kBase, v); }
    void entry(std::uint32_t va, std::uint32_t cat, std::uint32_t sym, std::uint32_t cab, std::uint16_t no,
               const char* name) {
        put(va, cat);
        put(va + 4, sym);
        put(va + 8, cab);
        seg.b[va - kBase + 12] = std::uint8_t(no);
        seg.b[va - kBase + 13] = std::uint8_t(no >> 8);
        for (std::size_t i = 0; name[i]; ++i) seg.b[va - kBase + 14 + i] = std::uint8_t(name[i]);
    }
    hx::AppImage image() const {
        hx::AppImage app;
        app.segments.push_back({kBase, seg.b});
        return app;
    }
};

} // namespace

TEST("modeltables: browser lists, costs and labels") {
    AppFixture f;
    // Category 12: two sub-lists. Sub 0 = AmpA, sub 1 = AmpB linked to a cab.
    f.put(kCatNSub + 12 * 4, 2);
    f.put(kCatPtrs + 12 * 16, kList);
    f.put(kCatCnts + 12 * 16, 1);
    f.put(kCatPtrs + 12 * 16 + 4, kList + 48);
    f.put(kCatCnts + 12 * 16 + 4, 1);
    f.entry(kList, 17, MetaFixture::kAmpA, hx::kNoCab, 349, "Amp A");
    f.entry(kList + 48, 17, MetaFixture::kAmpB, 0x1234, 350, "Amp \xE9");  // latin-1 e-acute
    f.put(kCost, MetaFixture::kAmpA);
    f.put(kCost + 8, 1600);

    const hx::AppImage app = f.image();
    std::string error;
    auto tables = hx::readModelTables(app, &error);
    if (!tables) std::printf("  %s\n", error.c_str());
    REQUIRE(tables.has_value());
    REQUIRE(tables->lists.size() == 2);
    CHECK_EQ(tables->costs.size(), std::size_t(0x337));
    const hx::CostEntry* cost = tables->costBySymId(MetaFixture::kAmpA);
    REQUIRE(cost != nullptr);
    CHECK_EQ(cost->cost, 1600u);

    const hx::BrowserList& l0 = tables->lists[0];
    CHECK_EQ(l0.category, 12);
    CHECK_EQ(l0.va, kList);
    REQUIRE(l0.entries.size() == 1);
    CHECK_EQ(l0.entries[0].name, std::string("Amp A"));
    CHECK_EQ(int(l0.entries[0].modelNo), 349);
    CHECK_EQ(tables->lists[1].entries[0].name, std::string("Amp \xC3\xA9"));  // as UTF-8

    const std::vector<std::uint8_t> metaData = MetaFixture::build();
    const auto meta = hx::DspMeta::parse(metaData);
    REQUIRE(meta.has_value());
    hx::labelLists(*tables, *meta);
    CHECK_EQ(tables->lists[0].label, std::string("amp / guitar"));
    CHECK_EQ(tables->lists[1].label, std::string("amp / bass + cab"));
}

TEST("modeltables: a bad sub-list count is an error, not a crash") {
    AppFixture f;
    f.put(kCatNSub + 3 * 4, 99);
    std::string error;
    CHECK(!hx::readModelTables(f.image(), &error));
    CHECK(!error.empty());
}

TEST("modeltables: duplicate labels get a number") {
    AppFixture f;
    f.put(kCatNSub + 12 * 4, 2);
    f.put(kCatPtrs + 12 * 16, kList);
    f.put(kCatCnts + 12 * 16, 1);
    f.put(kCatPtrs + 12 * 16 + 4, kList);
    f.put(kCatCnts + 12 * 16 + 4, 1);
    f.entry(kList, 17, MetaFixture::kAmpA, hx::kNoCab, 349, "Amp A");
    auto tables = hx::readModelTables(f.image());
    REQUIRE(tables.has_value());
    const std::vector<std::uint8_t> metaData = MetaFixture::build();
    const auto meta = hx::DspMeta::parse(metaData);
    REQUIRE(meta.has_value());
    hx::labelLists(*tables, *meta);
    CHECK_EQ(tables->lists[1].label, std::string("amp / guitar (2)"));
}

// A mod build: cost table moved and grown, category tables moved with a 20th category (Custom), read through the
// literals (newamp.py build_m4, catpatch.py).
TEST("modeltables: moved tables of a mod build, Custom category") {
    AppFixture f;
    constexpr std::uint32_t cost = 0x280e489c, nsub = 0x280e7430, ptrs = 0x280e71b0, cnts = 0x280e72f0, label = 0x280e7ca0;
    f.put(kCostLit, cost);
    f.put(kCostEndLit, cost + 12 * (0x337 + 2));
    f.put(kNSubLit, nsub);
    f.put(kPtrsLit, ptrs);
    f.put(kCntsLit, cnts);
    f.put(kLabelLit, label);
    f.put(cost + 12 * 0x337, 0x1111);           // a new model at the end of the cost table
    f.put(cost + 12 * 0x337 + 4, 383);
    f.put(cost + 12 * 0x337 + 8, 6000);
    f.put(nsub + 19 * 4, 1);
    f.put(ptrs + 19 * 16, kList);
    f.put(cnts + 19 * 16, 1);
    f.entry(kList, 19, 0x1111, hx::kNoCab, 383, "NAM");
    const char* custom = "Custom";
    for (std::size_t i = 0; custom[i]; ++i) {
        f.seg.b[label + 19 * 44 - kBase + i] = std::uint8_t(custom[i]);
        f.seg.b[label + 19 * 44 + 17 - kBase + i] = std::uint8_t(custom[i]);
    }
    f.put(label + 19 * 44 + 28, 0x00e5ff);
    std::string error;
    auto t = hx::readModelTables(f.image(), &error);
    if (!t) std::printf("  %s\n", error.c_str());
    REQUIRE(t.has_value());
    CHECK_EQ(t->categories, 20);
    CHECK_EQ(t->costs.size(), std::size_t(0x337 + 2));
    REQUIRE(t->costBySymId(0x1111) != nullptr);
    CHECK_EQ(int(t->costBySymId(0x1111)->modelNo), 383);
    REQUIRE(t->lists.size() == 1);
    CHECK_EQ(t->lists[0].category, 19);
    CHECK_EQ(t->lists[0].entries[0].name, std::string("NAM"));
    REQUIRE(t->labels.size() == 20);
    CHECK_EQ(t->labels[19].name, std::string("Custom"));
    CHECK_EQ(t->labels[19].rgb, 0x00e5ffu);
}

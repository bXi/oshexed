#include "test.h"

#include "core/dspmeta.h"
#include "fixtures.h"

#include <bit>

using test::MetaFixture;

TEST("dspmeta: symbols, entries, knobs and attributes") {
    const std::vector<std::uint8_t> data = MetaFixture::build();
    std::string error;
    const auto meta = hx::DspMeta::parse(data, &error);
    if (!meta) std::printf("  %s\n", error.c_str());
    REQUIRE(meta.has_value());
    CHECK_EQ(meta->symbolCount(), std::size_t(MetaFixture::kCount));
    CHECK_EQ(meta->symbol(MetaFixture::kDrive), std::string("Drive"));
    CHECK_EQ(meta->symbol(999), std::string("sym999"));
    REQUIRE(meta->entries().size() == 2);

    const hx::MetaEntry* a = meta->bySymId(MetaFixture::kAmpA);
    REQUIRE(a != nullptr);
    CHECK_EQ(a->rec5, 17u);
    CHECK_EQ(meta->attr(a->modelAttrs, "category"), std::string("amp"));
    CHECK_EQ(meta->displayName(*a), std::string("Amp A Deluxe"));
    REQUIRE(a->knobs.size() == 1);
    const hx::MetaParam& k = a->knobs[0];
    CHECK_EQ(k.type, 3u);
    CHECK_EQ(meta->displayName(k), std::string("Gain"));
    CHECK_EQ(hx::formatParamValue(k.type, k.max), std::string("1"));
    CHECK_EQ(hx::formatParamValue(k.type, k.def), std::string("0.5"));
    REQUIRE(a->states.size() == 1);
    CHECK_EQ(a->states[0].number, 4u);
    CHECK_EQ(meta->displayName(a->states[0]), std::string("Drive"));

    const hx::MetaEntry* b = meta->bySymId(MetaFixture::kAmpB);
    REQUIRE(b != nullptr);
    CHECK_EQ(meta->displayName(*b), std::string("HD2_AmpB"));  // no displayName attr: falls back to the symbol
    CHECK(meta->bySymId(MetaFixture::kGain) == nullptr);
}

TEST("dspmeta: rejects bad magic and out-of-range tables") {
    std::vector<std::uint8_t> data = MetaFixture::build();
    std::string error;
    std::vector<std::uint8_t> bad = data;
    bad[0] ^= 1;
    CHECK(!hx::DspMeta::parse(bad, &error));

    bad = data;
    bad[28] = 0xFF;  // entry count far past the end
    CHECK(!hx::DspMeta::parse(bad, &error));
    CHECK(!error.empty());
}

TEST("dspmeta: value formatting") {
    CHECK_EQ(hx::formatParamValue(3, std::bit_cast<std::uint32_t>(-18.0f)), std::string("-18"));
    CHECK_EQ(hx::formatParamValue(3, std::bit_cast<std::uint32_t>(0.125f)), std::string("0.125"));
    CHECK_EQ(hx::formatParamValue(2, 7), std::string("7"));
}

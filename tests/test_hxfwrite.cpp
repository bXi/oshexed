#include "test.h"

#include "fixtures.h"
#include "hxffixture.h"
#include "core/hxf.h"
#include "core/hxfwrite.h"

#include <algorithm>

using namespace hxtest;

namespace {

// rec 5 with `n` modules; module i holds i + 1 words of value i.
std::vector<std::uint8_t> makeRec5(std::uint32_t n) {
    hx::DspModules m;
    m.header = {0x5e, 0x7e, 0x26, 0x28, 4, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0};
    for (std::uint32_t i = 0; i < n; ++i) {
        test::Buf b;
        for (std::uint32_t k = 0; k <= i; ++k) b.le32(i);
        m.modules.push_back(b.b);
    }
    return hx::joinDspModules(m);
}

// Records 0..5 as the mod builder needs them: app, app copy, boot, UI-less filler, rec 4 metadata, rec 5 modules.
std::vector<std::uint8_t> makeFirmware() {
    const std::vector<std::uint8_t> app = makeApp();
    test::Buf img;
    img.bytes("L6IA");
    for (std::uint32_t w : {2u, 1u, 1u, 0x40u, 0x10u}) img.be32(w);
    appendRecord(img, 0, 2, app);
    appendRecord(img, 1, 3, app);
    appendRecord(img, 2, 6, {9, 9, 9, 9});
    appendRecord(img, 3, 6, {3, 3, 3, 3});
    appendRecord(img, 4, 6, test::MetaFixture::build());
    appendRecord(img, 5, 6, makeRec5(20));
    return makeHxf(img.b);
}

} // namespace

TEST("hxfwrite: rec 5 split and join give back the same bytes") {
    const std::vector<std::uint8_t> rec5 = makeRec5(5);
    const auto m = hx::splitDspModules(rec5);
    REQUIRE(m.has_value());
    REQUIRE(m->modules.size() == 5);
    CHECK_EQ(m->modules[3].size(), std::size_t(16));
    CHECK(hx::joinDspModules(*m) == rec5);

    hx::DspModules grown = *m;
    grown.modules.push_back({1, 2, 3, 4});
    const auto again = hx::splitDspModules(hx::joinDspModules(grown));
    REQUIRE(again.has_value());
    CHECK_EQ(again->modules.size(), std::size_t(6));
    CHECK(again->modules.back() == std::vector<std::uint8_t>({1, 2, 3, 4}));

    grown.modules.back().push_back(5);  // not whole words
    bool threw = false;
    try {
        hx::joinDspModules(grown);
    } catch (const hx::FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("hxfwrite: parts named after rec 4, unchanged pack gives the same image") {
    const auto img = hx::loadHxf(makeFirmware());
    REQUIRE(img != nullptr);
    std::string error;
    const auto parts = hx::explodeParts(*img, &error);
    if (!parts) std::printf("  %s\n", error.c_str());
    REQUIRE(parts.has_value());
    CHECK(parts->appShared);
    REQUIRE(parts->moduleFiles.size() == 20);
    CHECK_EQ(parts->moduleFiles[0], std::string("dsp/modules/000_res.bin"));
    CHECK_EQ(parts->moduleFiles[17], std::string("dsp/modules/017_HD2_AmpA.bin"));
    CHECK_EQ(parts->moduleFiles[18], std::string("dsp/modules/018_HD2_AmpB.bin"));

    const hx::PackedHxf out = hx::packHxf(*img, hx::assembleParts(*parts));
    CHECK(out.changed.empty());
    const auto back = hx::loadHxf(out.file);
    REQUIRE(back != nullptr);
    CHECK(back->md5Ok);
    CHECK(back->image == img->image);  // the test file is deflated by miniz, so only the image must match
    CHECK_EQ(back->fileSize, out.file.size());
    CHECK_EQ(std::size_t(back->formSize), out.file.size() - 8);
}

TEST("hxfwrite: an edited app and a grown rec 5 pass every check") {
    const auto img = hx::loadHxf(makeFirmware());
    REQUIRE(img != nullptr);
    auto parts = hx::explodeParts(*img);
    REQUIRE(parts.has_value());
    parts->app.insert(parts->app.end(), {0xAA, 0xBB, 0xCC, 0xDD, 0xEE});  // inner CRC and size now stale, not words
    parts->modules.modules.push_back({7, 7, 7, 7});

    const hx::PackedHxf out = hx::packHxf(*img, hx::assembleParts(*parts));
    CHECK((out.changed == std::vector<std::uint32_t>{0, 1, 5}));
    const auto back = hx::loadHxf(out.file);
    REQUIRE(back != nullptr);
    CHECK(back->md5Ok);
    for (const hx::HxfRecord& r : back->records) CHECK(hx::checkRecord(r).all());
    const hx::HxfRecord* r0 = back->record(0);
    REQUIRE(r0 != nullptr);
    CHECK_EQ(r0->size % 4, 0u);
    CHECK_EQ(hx::readLE32(r0->payload, 4), r0->size);
    const auto reparts = hx::explodeParts(*back);
    REQUIRE(reparts.has_value());
    CHECK_EQ(reparts->modules.modules.size(), std::size_t(21));
}

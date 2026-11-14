#include "test.h"

#include "hxffixture.h"
#include "core/checksums.h"
#include "core/hxf.h"
#include "core/m4app.h"

#include <algorithm>

using namespace hxtest;

TEST("hxf: loads a synthetic file, payloads native, all checks pass") {
    const std::vector<std::uint8_t> app = makeApp();
    const std::vector<std::uint8_t> res = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::uint8_t> file = makeHxf(makeImage(app, res).b);

    std::string error;
    const auto img = hx::loadHxf(file, &error);
    if (!img) std::printf("  %s\n", error.c_str());
    REQUIRE(img != nullptr);
    CHECK(img->md5Ok);
    CHECK_EQ(img->productId, 0x00210004u);
    CHECK_EQ(img->l6iaHeader[4], 0x40u);
    REQUIRE(img->records.size() == 2);

    const hx::HxfRecord& r0 = img->records[0];
    CHECK_EQ(r0.offset, std::size_t(0x18));
    CHECK_EQ(r0.type, 2u);
    CHECK(std::equal(r0.payload.begin(), r0.payload.end(), app.begin(), app.end()));
    const hx::RecordChecks c0 = hx::checkRecord(r0);
    CHECK(c0.crc);
    CHECK(c0.words);
    CHECK(c0.hash8);
    CHECK(c0.inner == hx::InnerKind::App);
    CHECK(c0.innerOk);

    const hx::HxfRecord* r1 = img->record(1);
    REQUIRE(r1 != nullptr);
    CHECK(std::equal(r1->payload.begin(), r1->payload.end(), res.begin(), res.end()));
    CHECK(hx::checkRecord(*r1).all());
    CHECK(hx::checkRecord(*r1).inner == hx::InnerKind::None);

    std::string appError;
    const auto parsed = hx::parseApp(r0.payload, &appError);
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->entry, 0x100000e5u);
    REQUIRE(parsed->segments.size() == 1);
    CHECK_EQ(parsed->u32(0x28000004), 0x55667788u);
}

TEST("hxf: a changed payload byte fails crc and hash8, and the MD5") {
    const std::vector<std::uint8_t> app = makeApp();
    const std::vector<std::uint8_t> res = {1, 2, 3, 4, 5, 6, 7, 8};
    test::Buf image = makeImage(app, res);
    const std::vector<std::uint8_t> good = image.b;
    image.b[image.size() - 1] ^= 0xFF;  // last byte of record 1's payload
    std::vector<std::uint8_t> file = makeHxf(image.b);
    // Keep the HEAD digest of the unchanged image, so the MD5 check has something to catch.
    const auto digest = hx::md5(good);
    std::copy(digest.begin(), digest.end(), file.begin() + 20 + 0x14);

    const auto img = hx::loadHxf(file);
    REQUIRE(img != nullptr);
    CHECK(!img->md5Ok);
    const hx::RecordChecks c = hx::checkRecord(img->records[1]);
    CHECK(!c.crc);
    CHECK(!c.hash8);
    CHECK(c.words);
    CHECK(hx::checkRecord(img->records[0]).all());
}

TEST("hxf: rejects non-L6FF and a truncated record") {
    std::string error;
    test::Buf f;
    f.bytes("FORM");
    f.be32(4);
    f.bytes("AIFF");
    CHECK(!hx::loadHxf(f.b, &error));

    test::Buf image = makeImage(makeApp(), {1, 2, 3, 4});
    image.b.resize(image.size() - 2);
    CHECK(!hx::loadHxf(makeHxf(image.b), &error));
    CHECK(!error.empty());
}

TEST("m4app: address lookup stays inside a segment") {
    const std::vector<std::uint8_t> app = makeApp();
    const auto parsed = hx::parseApp(app);
    REQUIRE(parsed.has_value());
    bool threw = false;
    try {
        parsed->u32(0x28000006);  // 2 bytes past the end
    } catch (const hx::FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

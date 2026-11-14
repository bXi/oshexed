#include "test.h"

#include "core/buildtag.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::uint64_t bits(const std::vector<int>& slots) {
    std::uint64_t b = 0;
    for (int s : slots) b |= std::uint64_t(1) << s;
    return b;
}

struct Vector {
    bool dirty;
    std::vector<int> mods;
    const char* tag;
};

// MODTAG.md "Test vectors" (commit 3fa9c1e); same as ../tools/fwtag.py VECTORS.
std::vector<int> all61() {
    std::vector<int> v;
    for (int i = 0; i < 61; ++i) v.push_back(i);
    return v;
}

} // namespace

TEST("buildtag: MODTAG.md vectors encode and decode") {
    const Vector vectors[] = {
        {false, {0, 1, 2, 3, 7}, "zuLwklb7n9Uli6iBT"},
        {true, {0, 3}, "waG2ASdUy3dZTFaVC"},
        {false, {}, "zuLwklb7n9Uli6qgV"},
        {false, all61(), "zuLwkoFF0hri67D0L"},
    };
    for (const Vector& v : vectors) {
        CHECK(hx::encodeModTag("3fa9c1e", bits(v.mods), v.dirty) == v.tag);
        const auto t = hx::decodeModTag(v.tag);
        CHECK(t.has_value());
        CHECK(t->layout == 1);
        CHECK(t->dirty == v.dirty);
        CHECK(t->commit == "3fa9c1e");
        CHECK(t->mods == v.mods);
        CHECK(!t->preGit());
    }
}

TEST("buildtag: damaged tags are rejected") {
    CHECK(!hx::decodeModTag("zuLwklB7n9Uli6iBT"));  // one character changed
    CHECK(!hx::decodeModTag("zuLwklb7n9Uli6iB"));   // too short
    CHECK(!hx::decodeModTag("zuLwklb7n9Uli6iB-"));  // not in the alphabet
    CHECK(!hx::decodeModTag("99999999999999999"));  // over 101 bits
}

TEST("buildtag: a unit's answer to command 200") {
    const hx::FirmwareIdentity ours = hx::identifyUnit(std::string("Mod %d.%02d.%d:%cwx6OXRqHGzRW8NjUE "));
    CHECK(ours.kind == hx::FirmwareIdentity::Kind::Ours);
    CHECK(ours.tag == "wx6OXRqHGzRW8NjUE");
    CHECK(ours.build && ours.build->mods == std::vector<int>({0, 1}));
    CHECK(hx::identifyUnit(std::string("Mod %d.%02d.%d:%cwx6OXRqHGzRW8NjUe ")).kind == hx::FirmwareIdentity::Kind::Damaged);
    CHECK(hx::identifyUnit(std::string("something else")).kind == hx::FirmwareIdentity::Kind::Damaged);
    CHECK(hx::identifyUnit(std::nullopt).kind == hx::FirmwareIdentity::Kind::Other);
}

TEST("buildtag: pre-git build") {
    // ../tools/fwtag.py encode("0000000", 0b11, True)
    CHECK(hx::encodeModTag("0000000", 0b11, true) == "wx6OXRqHGzRW8NjUE");
    const auto t = hx::decodeModTag("wx6OXRqHGzRW8NjUE");
    CHECK(t && t->preGit() && t->dirty && t->mods == std::vector<int>({0, 1}));
}

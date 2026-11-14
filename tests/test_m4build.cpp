#include "test.h"

#include "core/m4build.h"
#include "core/modspec.h"

#include <string>

namespace {

hx::mods::Package pkg(const std::string& id, int slot, const std::string& moduleJson, std::vector<std::string> needs = {}) {
    hx::mods::Package p;
    p.id = id;
    p.slot = slot;
    p.name = id;
    p.needs = std::move(needs);
    p.module = *hx::json::parse(moduleJson);
    return p;
}

bool refused(std::vector<const hx::mods::Package*> ps) {
    try {
        hx::mods::mergePackages(ps);
    } catch (const hx::mods::BuildError&) {
        return true;
    }
    return false;
}

} // namespace

TEST("m4build: BL / B.W encode and decode, both directions") {
    for (bool link : {true, false})
        for (auto [site, target] : {std::pair<std::uint32_t, std::uint32_t>{0x280c9aba, 0x280e8298},
                                    {0x2811d346, 0x2810a000}, {0x28000000, 0x28000004}}) {
            const auto b = hx::mods::encodeBranch(site, target, link);
            const auto back = hx::mods::branchTarget(b.data(), site, link);
            REQUIRE(back.has_value());
            CHECK_EQ(*back, target);
            CHECK(!hx::mods::branchTarget(b.data(), site, !link).has_value());
        }
    // BL from 0 to 8 (offset 4): halfwords f000 f802
    const auto b = hx::mods::encodeBranch(0, 8, true);
    CHECK_EQ(int(b[0]), 0x00);
    CHECK_EQ(int(b[1]), 0xf0);
    CHECK_EQ(int(b[2]), 0x02);
    CHECK_EQ(int(b[3]), 0xf8);
    bool threw = false;
    try {
        hx::mods::encodeBranch(0, 3, true);  // odd offset
    } catch (const hx::mods::BuildError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("m4build: stock check windows are 16-byte aligned, at least 16 bytes") {
    CHECK((hx::mods::stockWindow(0x280e0796, 4) == std::pair<std::uint32_t, std::uint32_t>{0x280e0790, 16}));
    CHECK((hx::mods::stockWindow(0x280e079e, 4) == std::pair<std::uint32_t, std::uint32_t>{0x280e0790, 32}));
    CHECK((hx::mods::stockWindow(0x280c4684, 36) == std::pair<std::uint32_t, std::uint32_t>{0x280c4680, 48}));
}

TEST("modspec: order, mod bits, requires, collisions") {
    const auto base = pkg("base", -1, R"({"spec": {}, "code": [], "layout": {"catpatch:table:names": 100}})");
    const auto nam = pkg("nam", 0,
                         R"({"spec": {"models": [{"graph": "graphs/a.json", "sym": "HD2_A", "name": "A", "modelNo": 383,
                             "like": "HD2_L"}]}, "code": [], "layout": {"code:a.c": "0x200"}})",
                         {"base"});
    const auto midi = pkg("midiguitar", 1, R"({"spec": {}, "code": [], "layout": {"code:b.c": 300}})", {"base"});

    const hx::mods::BuildSpec s = hx::mods::mergePackages({&midi, &nam, &base});
    REQUIRE(s.packages.size() == 3);
    CHECK_EQ(s.packages[0]->id, std::string("base"));
    CHECK_EQ(s.packages[1]->id, std::string("nam"));
    CHECK_EQ(s.modBits, std::uint64_t(3));
    REQUIRE(s.models.size() == 1);
    CHECK_EQ(s.models[0].after, std::string("HD2_L"));  // defaults to like
    CHECK_EQ(s.layout.at("code:a.c"), 0x200u);

    CHECK(refused({&nam}));  // needs base
    const auto clash = pkg("other", 2, R"({"spec": {}, "code": [], "layout": {"code:a.c": 512}})", {"base"});
    CHECK(refused({&base, &nam, &clash}));  // same allocation twice
    const auto sameNo = pkg("other", 2,
                            R"({"spec": {"models": [{"graph": "g", "sym": "HD2_B", "name": "B", "modelNo": 383,
                                "like": "HD2_L"}]}, "code": []})",
                            {"base"});
    CHECK(refused({&base, &nam, &sameNo}));  // same model number
}

TEST("modspec: numbers as JSON numbers or text") {
    const auto v = hx::json::parse(R"([672039576, "0x280c9aba", "-4", "12x"])");
    REQUIRE(v.has_value());
    CHECK_EQ(hx::mods::number(&v->items[0], "n"), std::int64_t(672039576));
    CHECK_EQ(hx::mods::number(&v->items[1], "n"), std::int64_t(0x280c9aba));
    CHECK_EQ(hx::mods::number(&v->items[2], "n"), std::int64_t(-4));
    bool threw = false;
    try {
        hx::mods::number(&v->items[3], "n");
    } catch (const hx::mods::BuildError&) {
        threw = true;
    }
    CHECK(threw);
}

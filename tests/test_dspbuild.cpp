#include "test.h"

#include "core/dspbuild.h"
#include "core/json.h"
#include "core/modbuild.h"
#include "core/text.h"

#include <cstdint>
#include <vector>

using hx::mods::dsp::Words;

namespace {

hx::json::Value js(const char* text) { return *hx::json::parse(text); }

} // namespace

TEST("dspbuild: wval / jval keep words, floats where they look like floats") {
    for (std::uint32_t w : {0u, 1u, 0x3f800000u, 0xc2f00000u, 0x80000000u, 0xffffffffu, 0x7fc00000u, 0x4b7fffffu})
        CHECK_EQ(hx::mods::dsp::jval(hx::mods::dsp::wval(w)), w);
    CHECK(!hx::mods::dsp::wval(0x3f800000u).integer);   // 1.0
    CHECK(hx::mods::dsp::wval(0xffffffffu).integer);    // -1
    CHECK_EQ(hx::mods::dsp::wval(0xffffffffu).asInt(), std::int64_t(-1));
    CHECK_EQ(hx::mods::dsp::jval(js("-120.0")), 0xc2f00000u);
    CHECK_EQ(hx::mods::dsp::jval(js("440.0")), 0x43dc0000u);
    CHECK_EQ(hx::mods::dsp::jval(js("3265265664")), 3265265664u);
}

TEST("dspbuild: graph difference patch (graphdiff.patch)") {
    const auto s = js(R"({"a": 1, "b": [1, 2, 3], "c": {"x": 1.5}})");
    const auto d = js(R"({"$dict": {"a": {"$val": 7}, "b": {"$list": {"1": {"$val": 9}, "3": {"$val": [4]}}, "$len": 4},
                          "d": {"$val": null}}, "$keys": ["a", "b", "c", "d"]})");
    const auto p = hx::mods::dsp::patch(&s, &d);
    CHECK_EQ(hx::json::write(p), hx::json::write(js(R"({"a": 7, "b": [1, 9, 3, [4]], "c": {"x": 1.5}, "d": null})")));
    // shorter list, unchanged parts kept
    const auto d2 = js(R"({"$dict": {"b": {"$list": {}, "$len": 2}}})");
    CHECK_EQ(hx::json::write(hx::mods::dsp::patch(&s, &d2)), hx::json::write(js(R"({"a": 1, "b": [1, 2], "c": {"x": 1.5}})")));
}

TEST("dspbuild: a module comes back word for word through the graph JSON") {
    // header, cells [0] / [5], one kernel-block entry, a control section with one record whose data is a path ref,
    // one bind, two relocs, one node-map pair (laid out as mkchain.build lays it out)
    Words w{3, 2, 16, 1, 59, 2, 20, 35, 63, 1, 55, 1};
    w.insert(w.end(), {1, 0, 1, 5});            // cells at 12
    w.insert(w.end(), {0, 0x600005, 0, 0});     // kernel block at 16
    Words ctrl(24, 0);                          // control section at 20
    ctrl[0] = 24;
    ctrl[2] = ctrl[4] = ctrl[6] = ctrl[8] = ctrl[10] = 35;
    ctrl[13] = ctrl[18] = 1;
    ctrl.insert(ctrl.end(), {34, 0, 0, 1, 0, 0, 1, 1, 0, 0});
    ctrl.push_back(12);                         // the ref (path cell)
    w.insert(w.end(), ctrl.begin(), ctrl.end());
    w.insert(w.end(), {12, 2, 14, 1});          // bind at 55
    w.insert(w.end(), {1, 54, 1, 55});          // relocs at 59
    w.insert(w.end(), {12, 12});                // node map at 63
    REQUIRE(w.size() == 65);
    CHECK(hx::mods::dsp::rebuildModule(w) == w);

    // modcheck: this module has no sub-sections, so its node-map entry's control path [0] points at none
    const std::vector<std::string> p = hx::mods::checkModule(w);
    REQUIRE(p.size() == 1);
    CHECK(p[0].find("node map 0") == 0);
    Words bad = w;
    bad[1] |= 1;  // the loader rejects bit 0
    CHECK(hx::mods::checkModule(bad).size() == 2);
    CHECK(hx::mods::checkModule(Words{4, 0, 0}).size() == 1);
}

TEST("dspbuild: core repack shares leaf lists and is stable") {
    Words C(18, 0);
    C[1] = 2, C[2] = 18;
    C.insert(C.end(), {7, 8});                                   // small array at 18
    const Words root{1, 2, 1, 20, 0, 0, 0};
    std::copy(root.begin(), root.end(), C.begin() + 8);
    Words n0{0, 0, 0, 0, 0, 0, 0x600000, 1, 40, 1, 40, 0, 0, 1, 2, 3, 4, 5, 6, 0};  // root node at 20
    C.insert(C.end(), n0.begin(), n0.end());
    C.push_back(0);                                              // buffer list [0] at 40
    C[0xF] = 1, C[0x10] = 41;
    C.insert(C.end(), {3, 4, 2, 48, 1, 88, 0});                  // kernel table at 41
    C.insert(C.end(), {0, 2, 92, 0, 0, 0, 0x80, 1, 40, 1, 40, 1, 95, 0, 0, 0, 0, 0, 0, 0});
    C.insert(C.end(), {1, 0, 0, 0, 0, 0, 0x81, 1, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    C.insert(C.end(), {1, 2, 3, 4});                             // records at 88
    C.insert(C.end(), {2, 97, 0});                               // triple at 92
    C.insert(C.end(), {1, 0x3f800000});                          // params at 95
    C.insert(C.end(), {0, 0});                                   // triple data at 97
    REQUIRE(C.size() == 99);
    REQUIRE(hx::mods::dsp::coreBoundsOk(C));
    const Words A = hx::mods::dsp::repackCore(C);
    CHECK(hx::mods::dsp::coreBoundsOk(A));
    CHECK(A.size() < C.size() + 2);       // the [0] buffer list is stored once
    CHECK(hx::mods::dsp::repackCore(A) == A);
    CHECK_EQ(A[1], 2u);
    CHECK_EQ(A[0xF], 1u);
}

TEST("modbuild: output next to the stock file, build tag commit") {
    hx::mods::Stock s;
    s.ok = true;
    s.model = "helix-lt";
    s.version = "3.80.0";
    const std::string out = hx::mods::defaultOutPath(s, hx::utf8FromPath(hx::pathFromUtf8("fw") / "stock.hxf"));
    CHECK_EQ(out, hx::utf8FromPath(hx::pathFromUtf8("fw") / "Helix LT 3.80.0 + mods.hxf"));
    CHECK_EQ(hx::mods::toolCommit().size(), std::size_t(7));
    if (hx::mods::toolCommit() == "0000000") {
        CHECK(hx::mods::toolDirty());
    }
}

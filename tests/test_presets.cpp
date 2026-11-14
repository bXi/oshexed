#include "test.h"

#include "builder.h"
#include "core/firmware.h"
#include "core/hlx.h"
#include "core/msgpack.h"
#include "core/presets.h"
#include "core/presetwriter.h"

#include <cstring>

#include <bit>
#include <cmath>

namespace {

// Minimal msgpack writer for the fixtures.
struct Mp {
    test::Buf o;
    Mp& map(int n) { o.b.push_back(std::uint8_t(0x80 | n)); return *this; }
    Mp& arr(int n) {
        if (n < 16) o.b.push_back(std::uint8_t(0x90 | n));
        else { o.b.push_back(0xdc); o.b.push_back(std::uint8_t(n >> 8)); o.b.push_back(std::uint8_t(n)); }
        return *this;
    }
    Mp& i(int v) {
        if (v >= 0 && v < 128) o.b.push_back(std::uint8_t(v));
        else { o.b.push_back(0xcd); o.b.push_back(std::uint8_t(v >> 8)); o.b.push_back(std::uint8_t(v)); }
        return *this;
    }
    Mp& f(float v) { o.b.push_back(0xca); o.be32(std::bit_cast<std::uint32_t>(v)); return *this; }
    Mp& b(bool v) { o.b.push_back(v ? 0xc3 : 0xc2); return *this; }
    Mp& nil() { o.b.push_back(0xc0); return *this; }
    Mp& str(std::string_view s) {  // NUL-terminated, as the firmware stores them
        const std::size_t n = s.size() + 1;
        o.b.push_back(0xda);
        o.b.push_back(std::uint8_t(n >> 8));
        o.b.push_back(std::uint8_t(n));
        o.bytes(s);
        o.b.push_back(0);
        return *this;
    }
    Mp& raw(const std::vector<std::uint8_t>& bytes) {  // a str holding nested msgpack
        o.b.push_back(0xda);
        o.b.push_back(std::uint8_t(bytes.size() >> 8));
        o.b.push_back(std::uint8_t(bytes.size()));
        o.b.insert(o.b.end(), bytes.begin(), bytes.end());
        return *this;
    }
};

// A preset body: path 0 with a block (model 349, enabled, values 0.5, true, 3) in lane A slot 2
// and a bypassed block (model 12) in lane B slot 0; path 1 empty.
std::vector<std::uint8_t> presetBlob() {
    Mp m;
    m.o.bytes("\xa9l6-helix");
    m.o.b.push_back(0);
    m.o.b.push_back(0xda);
    m.o.b.push_back(0x00);
    m.o.b.push_back(0x30);
    m.o.zeros(48);
    m.map(6).i(0).map(2).i(21).i(2).i(22).arr(20);
    for (int s = 0; s < 20; ++s) {
        if (s == 3) {  // lane A slot 2 (slots 1..8 are lane A)
            m.map(2).i(19).i(6).i(20).map(4);
            m.i(24).map(2).i(25).i(349).i(26);
            m.o.b.push_back(0xff);  // cab model -1 (negative fixint)
            m.i(10).b(true);
            m.i(11).map(1).i(4).arr(3).f(0.5f).b(true).i(3);
            m.i(12).map(1).i(4).arr(2).f(1.5f).i(4);  // second param set (a linked cab's knobs)
        } else if (s == 8) {  // lane A slot 7: looper, model 153, bypassed, 1 value
            m.map(2).i(19).i(7).i(20).map(4);
            m.i(8).i(153).i(9).i(22).i(10).b(false).i(7).map(1).i(4).arr(1).f(0.25f);
        } else if (s == 0) {  // input A: Multi (1), gate on at -60 dB, decay 0.1
            m.map(2).i(19).i(0).i(20).map(2);
            m.i(5).i(1).i(7).map(1).i(4).arr(3).b(true).f(-60.f).f(0.1f);
        } else if (s == 9) {  // output A: Path 2A (2), pan 0.25, gain -3 dB
            m.map(2).i(19).i(1).i(20).map(2);
            m.i(6).i(2).i(7).map(1).i(4).arr(2).f(0.25f).f(-3.f);
        } else if (s == 10) {  // split at stored position 2 (gap 1), on; input B Aux (3)
            m.map(2).i(19).i(2).i(20).map(3);
            m.i(14).map(1).i(5).i(3);
            m.i(15).map(4).i(8).i(257).i(13).i(2).i(10).b(true).i(7).map(1).i(4).arr(3).f(0.25f).f(0.75f).b(false);
            m.i(18).b(true);
        } else if (s == 19) {  // join at stored position 9 (gap 8), off; output B Path 2B (3)
            m.map(2).i(19).i(3).i(20).map(3);
            m.i(16).map(2).i(6).i(3).i(7).map(1).i(4).arr(2).f(0.5f).f(0.f);
            m.i(17).map(3).i(8).i(151).i(13).i(9).i(10).b(false);
            m.i(18).b(false);
        } else if (s == 11) {  // lane B slot 0
            m.map(2).i(19).i(6).i(20).map(2);
            m.i(24).map(1).i(25).i(12);
            m.i(10).b(false);
        } else {
            m.map(2).i(19).i(8).i(20).nil();
        }
    }
    // Snapshots (key 10): 2 of them (key 8 = 1, meaning unknown). The block in lane A slot 2 (global slot 3) is on in
    // the first and off in the second; controller 0 has 0.2 / 0.8.
    m.i(10).map(2).i(8).i(1).i(10).arr(2);
    for (int s = 0; s < 2; ++s) {
        m.map(5).i(4).str(s ? "S TWO" : "S ONE").i(5).f(100.f).i(12).i(3);
        m.i(3).arr(40);
        for (int g = 0; g < 40; ++g) m.arr(2).nil().b(g == 3 ? s == 0 : true);
        m.i(2).arr(1).arr(3).b(false).i(0).f(s ? 0.8f : 0.2f);
    }
    // Controllers (key 4, by source): Snapshot (19) drives knob 0 of global slot 3.
    m.i(4).arr(20);
    for (int src = 0; src < 20; ++src) {
        if (src != 19) {
            m.nil();
            continue;
        }
        m.arr(1).map(2).i(0).i(0).i(1).map(5).i(0).i(19).i(2).f(0.f).i(3).f(1.f).i(5).i(3).i(6).map(1).i(29).i(0);
    }
    // Footswitches (key 3): FS8 (index 7) toggles global slot 3; 11.6 / 11.7 = 0x00ff80 / true; momentary, LED
    // colour 8 (Blue).
    m.i(3).map(1).i(8).arr(13);
    for (int fs = 0; fs < 13; ++fs) {
        if (fs != 7) {
            m.nil();
            continue;
        }
        m.arr(1).map(3).i(11).map(5).i(0).i(1).i(5).str("Comp").i(6).i(0xff80).i(7).b(true).i(8).i(3);
        m.i(12).b(true).i(16).i(8);
    }
    // Globals (key 5, by id): tempo 100, guitar pad 1.
    m.i(5).map(2).i(16).f(100.f).i(134).i(1);
    // Command Center (key 2): slot 8 (FS3) = CC Toggle on CC 73, dim 0, lit 127; the other 8 slots empty.
    m.i(2).map(2).i(0).arr(9);
    for (int s = 0; s < 9; ++s) m.i(s == 8 ? 2 : 0);
    m.i(1).arr(9);
    for (int s = 0; s < 9; ++s) {
        m.arr(7);
        if (s == 8) m.i(2).i(0).i(73).i(0).i(127).i(0).i(0);
        else for (int k = 0; k < 7; ++k) m.i(0);
    }
    return m.o.b;
}

} // namespace

TEST("msgpack: scalars, containers and int-key lookup") {
    Mp m;
    m.map(3).i(1).str("abc").i(200).arr(2).f(1.5f).b(false).i(7).nil();
    const hx::mp::Value v = hx::mp::parse(m.o.b);
    REQUIRE(v.isMap());
    CHECK_EQ(v.size(), std::size_t(3));
    REQUIRE(v.get(1) != nullptr);
    CHECK_EQ(v.get(1)->asText(), std::string("abc"));  // trailing NUL dropped
    const hx::mp::Value* a = v.get(200);
    REQUIRE(a != nullptr && a->isArray());
    CHECK_EQ(a->at(0)->asNumber(), 1.5);
    CHECK(!a->at(1)->asBool(true));
    CHECK(a->at(2) == nullptr);
    CHECK(v.get(7)->isNil());
    CHECK(v.get(99) == nullptr);
}

TEST("msgpack: negative fixint, int16 and a stream of values") {
    const std::uint8_t data[] = {0xff, 0xd1, 0xff, 0x38, 0x05};
    const std::vector<hx::mp::Value> all = hx::mp::parseAll(data);
    REQUIRE(all.size() == 3);
    CHECK_EQ(all[0].asInt(), std::int64_t(-1));
    CHECK_EQ(all[1].asInt(), std::int64_t(-200));
    CHECK_EQ(all[2].asInt(), std::int64_t(5));
}

TEST("msgpack: truncated and oversized data throw") {
    const std::uint8_t cut[] = {0xda, 0x00, 0x10, 'a'};
    bool threw = false;
    try { hx::mp::parse(cut); } catch (const hx::FormatError&) { threw = true; }
    CHECK(threw);
    const std::uint8_t huge[] = {0xdd, 0xff, 0xff, 0xff, 0xff};
    threw = false;
    try { hx::mp::parse(huge); } catch (const hx::FormatError&) { threw = true; }
    CHECK(threw);
}

TEST("presets: a preset body maps slots to lanes") {
    std::string error;
    const auto p = hx::parsePresetBlob(presetBlob(), &error);
    if (!p) std::printf("  %s\n", error.c_str());
    REQUIRE(p.has_value());
    CHECK(p->stored);
    const hx::PresetPath& path = p->paths[0];
    REQUIRE(path.a.slots[2].has_value());
    const hx::PresetBlock& b = *path.a.slots[2];
    CHECK_EQ(int(b.modelNo), 349);
    CHECK(b.enabled);
    CHECK_EQ(b.cabModelNo, -1);
    REQUIRE(b.values.size() == 3);
    CHECK(b.values[0].kind == hx::PresetValue::Kind::Float);
    CHECK_EQ(b.values[0].v, 0.5);
    CHECK(b.values[1].kind == hx::PresetValue::Kind::Bool);
    CHECK(b.values[2].kind == hx::PresetValue::Kind::Int);
    REQUIRE(b.cabValues.size() == 2);
    CHECK_EQ(b.cabValues[0].v, 1.5);
    CHECK_EQ(b.cabValues[1].v, 4.0);
    REQUIRE(path.b.slots[0].has_value());
    CHECK_EQ(int(path.b.slots[0]->modelNo), 12);
    CHECK(!path.b.slots[0]->enabled);
    CHECK(!path.a.slots[0].has_value());
    CHECK(p->paths[1].a.empty() && p->paths[1].b.empty());

    REQUIRE(path.a.slots[7].has_value());
    const hx::PresetBlock& looper = *path.a.slots[7];
    CHECK_EQ(int(looper.slotType), 7);
    CHECK_EQ(int(looper.modelNo), 153);
    CHECK(!looper.enabled);
    REQUIRE(looper.values.size() == 1);
    CHECK_EQ(looper.values[0].v, 0.25);

    const hx::PresetRouting& r = path.routing;
    CHECK_EQ(r.mode, 2);
    CHECK(r.splitOn);
    CHECK(!r.joinOn);
    CHECK_EQ(r.splitPos, 1);
    CHECK_EQ(r.joinPos, 8);
    CHECK_EQ(int(r.splitModel), 257);
    CHECK(r.splitEnabled);
    REQUIRE(r.splitValues.size() == 3);
    CHECK_EQ(r.splitValues[1].v, 0.75);
    CHECK(r.splitValues[2].kind == hx::PresetValue::Kind::Bool);
    CHECK(!r.joinEnabled);
    CHECK(r.joinValues.empty());
    CHECK(r.hasOwnOutputB());
    CHECK(!r.hasOwnInputB());
    CHECK_EQ(p->paths[1].routing.mode, 0);
}

TEST("presets: inputs and outputs of both lanes") {
    const auto p = hx::parsePresetBlob(presetBlob());
    REQUIRE(p.has_value());
    const hx::PresetPath& path = p->paths[0];
    CHECK_EQ(path.inputs[0].source, 1);
    REQUIRE(path.inputs[0].values.size() == 3);
    CHECK(path.inputs[0].values[0].kind == hx::PresetValue::Kind::Bool);
    CHECK_EQ(path.inputs[0].values[0].v, 1.0);
    CHECK_EQ(path.inputs[0].values[1].v, -60.0);
    CHECK_EQ(path.inputs[1].source, 3);
    CHECK(path.inputs[1].values.empty());
    CHECK_EQ(path.outputs[0].dest, hx::kOutPath2A);
    REQUIRE(path.outputs[0].values.size() == 2);
    CHECK_EQ(path.outputs[0].values[0].v, 0.25);
    CHECK_EQ(path.outputs[0].values[1].v, -3.0);
    CHECK_EQ(path.outputs[1].dest, hx::kOutPath2B);
    // Port tables: the values HX Edit's lists use.
    CHECK_EQ(hx::ioInputs().size(), std::size_t(17));
    CHECK_EQ(hx::ioOutputs().size(), std::size_t(13));
    CHECK_EQ(std::string(hx::ioOutputs()[hx::kOutPath2B].label), std::string("Path 2B"));
    // An empty path: nothing set.
    CHECK_EQ(p->paths[1].inputs[0].source, 0);
    CHECK_EQ(p->paths[1].outputs[0].dest, 0);

    REQUIRE(p->globals.size() == 2);
    REQUIRE(p->global(hx::globalid::Tempo) != nullptr);
    CHECK_EQ(p->global(hx::globalid::Tempo)->v, 100.0);
    CHECK_EQ(p->global(hx::globalid::GuitarPad)->v, 1.0);
    CHECK(p->global(hx::globalid::DtSelect) == nullptr);

    REQUIRE(p->commands.size() == 9);
    CHECK_EQ(p->commands[0].type, 0);
    CHECK_EQ(p->commands[8].type, 2);
    CHECK_EQ(p->commands[8].fields[1], 73);
    CHECK_EQ(p->commands[8].fields[3], 127);
    CHECK_EQ(hx::commandTypeName(2), std::string("CC Toggle"));
    CHECK_EQ(hx::commandFieldNames(2).size(), std::size_t(4));
    CHECK_EQ(hx::commandTypeName(99), std::string("Type 99"));
    // HX Edit's value names (its HelixControls.json): base channel, note names, MMC, looper, wait time
    CHECK_EQ(hx::commandFieldText(1, 0, 0), std::string("Base"));
    CHECK_EQ(hx::commandFieldText(5, 1, 48), std::string("C3"));
    CHECK_EQ(hx::commandFieldText(5, 3, 1), std::string("Momentary"));
    CHECK_EQ(hx::commandFieldText(6, 0, 1), std::string("Play"));
    CHECK_EQ(hx::commandFieldText(4, 1, 0), std::string("Off"));
    CHECK_EQ(hx::commandFieldText(18, 3, 2), std::string("100 ms"));
    CHECK_EQ(hx::commandFieldRange(15, 1).second, 10);  // HX Snapshot press: None, Next, Prev, SNAPSHOT 1-8

    // Adding a command to FS5 (slot 10) also gives the switch a Command Center function; None takes both away.
    hx::Preset q = *p;
    q.setCommand(10, 1);
    REQUIRE(q.commands.size() == std::size_t(hx::kCommandSlots));
    CHECK_EQ(q.commands[10].type, 1);
    REQUIRE(q.footswitches[4].size() == 1);
    CHECK_EQ(q.footswitches[4][0].kind, 3);
    CHECK_EQ(q.footswitches[4][0].label, std::string("CC Value"));
    q.setCommandField(10, 1, 300);
    CHECK_EQ(q.commands[10].fields[1], 127);
    q.setCommand(10, 0);
    CHECK_EQ(q.commands[10].type, 0);
    CHECK(q.footswitches[4].empty());
}

TEST("command center: defaults and values per snapshot (capture command-center3)") {
    hx::Preset p;
    p.snapshots.resize(hx::kSnapshots);
    p.setCommand(9, 15);  // FS4 HX Snapshot: press = Next in every snapshot
    CHECK_EQ(p.commands[9].fields[1], 1);
    for (const hx::PresetSnapshot &s : p.snapshots) {
        REQUIRE(s.commands.size() == 1);
        CHECK_EQ(s.commands[0].slot, 9);
        CHECK_EQ(s.commands[0].fields[1], 1);
    }
    p.setCommand(19, 3);  // EXP 1 MIDI CC: max 127, no snapshot rows (pedals hold one value)
    CHECK_EQ(p.commands[19].fields[3], 127);
    CHECK_EQ(p.snapshots[0].commands.size(), std::size_t(1));
    p.setCommand(2, 17);  // instant Looper: Play (1); None isn't offered
    CHECK_EQ(p.commands[2].fields[0], 1);
    CHECK_EQ(hx::commandFieldRange(17, 0).first, 1);
    CHECK_EQ(hx::commandFieldText(17, 0, 5), std::string("Overdub"));
    CHECK_EQ(hx::commandFieldText(12, 3, 1), std::string("A"));  // Hotkey key (Clean_Dirt w_d_w123.hlx: "A" = 1)
    CHECK_EQ(hx::commandFieldText(13, 3, 37), std::string("Enter"));
    CHECK_EQ(hx::commandFieldText(12, 3, 99), std::string("Power"));
    CHECK_EQ(hx::commandFieldText(7, 0, 1), std::string("2 (Ring)"));

    p.setCommandField(9, 1, 5);  // snapshot 1: press = SNAPSHOT 3
    p.recallSnapshot(1);
    CHECK_EQ(p.commands[9].fields[1], 1);
    p.setCommandField(9, 1, 7);
    p.recallSnapshot(0);
    CHECK_EQ(p.commands[9].fields[1], 5);
    p.copySnapshot(1, 0);
    CHECK_EQ(p.commands[9].fields[1], 7);
}

TEST("hlx: Command Center with per-snapshot values, in and out") {
    // Shapes as in HX Edit's export (capture3.hlx): a Hotkey on FS2 (slot 7), an instant Bank/Program, EXP 1 MIDI CC.
    const char *text = R"({"data":{"tone":{
        "commandFS2":{"@command":12,"@fs_label":"Hotkey","@fs_ledcolor":462860,"@fs_momentary":false,"@fs_primary":true,
                      "@fs_enabled":false,"@fs_customcolor":5,"@keystroke":12,"@modifier1":1,"@modifier2":2,"@modifier3":3},
        "commandInst1":{"@command":19,"@channel":3,"@bank0":6,"@bank32":8,"@program":22,"@waittime":3},
        "commandExpPedal1":{"@command":3,"@channel":0,"@cc":7,"@min":3,"@max":126},
        "global":{"@current_snapshot":0},
        "snapshot0":{"@name":"A","commands":{
            "commandFS2":{"@fs_enabled":false,"@fs_momentary":0,"@keystroke":12,"@modifier1":1,"@modifier2":2,"@modifier3":3},
            "commandInst1":{"@fs_enabled":false,"@channel":3,"@bank0":6,"@bank32":8,"@program":22,"@waittime":3}}},
        "snapshot1":{"@name":"B","commands":{
            "commandFS2":{"@fs_enabled":false,"@fs_momentary":0,"@keystroke":0,"@modifier1":1,"@modifier2":2,"@modifier3":0},
            "commandInst1":{"@fs_enabled":false,"@channel":0,"@bank0":0,"@bank32":0,"@program":0,"@waittime":0}}}}}})";
    hx::Firmware fw;
    std::string err;
    auto p = hx::importHlx(text, fw, &err);
    REQUIRE(p.has_value());
    REQUIRE(p->commands.size() == std::size_t(hx::kCommandSlots));
    CHECK_EQ(p->commands[7].type, 12);
    CHECK_EQ(p->commands[7].fields[3], 12);  // key
    CHECK_EQ(p->commands[0].fields[3], 22);  // program
    CHECK_EQ(p->commands[19].fields[3], 126);
    REQUIRE(p->footswitches[1].size() == 1);
    CHECK_EQ(p->footswitches[1][0].kind, 3);
    CHECK_EQ(p->footswitches[1][0].ledColour, 5);
    REQUIRE(p->snapshots.size() == 2);
    CHECK_EQ(p->snapshots[1].commands.size(), std::size_t(2));  // FS2 and Inst 1; the pedal has one value
    p->recallSnapshot(1);
    CHECK_EQ(p->commands[7].fields[3], 0);
    CHECK_EQ(p->commands[7].fields[1], 2);
    CHECK_EQ(p->commands[19].fields[3], 126);

    // Out and back in: the same commands and values per snapshot.
    p->recallSnapshot(0);
    const std::string out = hx::exportHlx(*p, fw);
    auto q = hx::importHlx(out, fw, &err);
    REQUIRE(q.has_value());
    CHECK_EQ(q->commands[7].fields[3], 12);
    CHECK_EQ(q->commands[0].fields[0], 3);
    CHECK_EQ(q->footswitches[1].size(), std::size_t(1));
    q->recallSnapshot(1);
    CHECK_EQ(q->commands[7].fields[3], 0);
    CHECK_EQ(q->commands[0].fields[0], 0);
    CHECK(out.find("\"@fs_customcolor\"") != std::string::npos);
}

TEST("presetwriter: written back it reads the same; a block moved to the other path takes its states along") {
    auto p = hx::parsePresetBlob(presetBlob());
    REQUIRE(p.has_value());
    std::string error;
    auto out = hx::writePresetBlob(*p, {}, &error);
    REQUIRE(out.has_value());
    // The header: magic, str16 of 48 bytes, word 0 = the map at 0x3d, words 10 / 11 = the end (no key 12).
    CHECK(std::memcmp(out->data(), "\xa9l6-helix", 10) == 0);
    CHECK_EQ(hx::readLE32(*out, 13), std::uint32_t(0x3d));
    CHECK_EQ(hx::readLE32(*out, 13 + 40), std::uint32_t(out->size()));
    CHECK_EQ(hx::readLE32(*out, 13 + 44), std::uint32_t(out->size()));
    auto q = hx::parsePresetBlob(*out);
    REQUIRE(q.has_value());
    REQUIRE(q->paths[0].a.slots[2].has_value());
    CHECK_EQ(q->paths[0].a.slots[2]->modelNo, std::uint16_t(349));
    CHECK_EQ(q->paths[0].a.slots[2]->values.size(), std::size_t(3));
    CHECK(std::abs(q->paths[0].a.slots[2]->values[0].v - 0.5) < 1e-6);
    CHECK_EQ(q->paths[0].a.slots[2]->cabValues.size(), std::size_t(2));
    REQUIRE(q->paths[0].a.slots[7].has_value());
    CHECK_EQ(q->paths[0].a.slots[7]->slotType, std::uint8_t(7));
    CHECK_EQ(q->paths[0].a.slots[7]->modelNo, std::uint16_t(153));
    REQUIRE(q->paths[0].b.slots[0].has_value());
    CHECK(!q->paths[0].b.slots[0]->enabled);
    CHECK_EQ(q->paths[0].inputs[0].source, 1);
    CHECK_EQ(q->paths[0].outputs[0].dest, 2);
    CHECK_EQ(q->paths[0].inputs[1].source, 3);
    CHECK_EQ(q->paths[0].routing.splitPos, p->paths[0].routing.splitPos);
    CHECK_EQ(q->paths[0].routing.splitModel, std::uint16_t(257));
    CHECK_EQ(q->snapshots.size(), std::size_t(2));
    CHECK_EQ(q->snapshots[1].name, std::string("S TWO"));
    REQUIRE(q->controllers.size() == 1);
    CHECK_EQ(q->controllers[0].target.block, 3);
    CHECK(std::abs(q->controllers[0].snapValues[1].v - 0.8) < 1e-6);
    REQUIRE(q->footswitches[7].size() == 1);
    CHECK_EQ(q->footswitches[7][0].label, std::string("Comp"));
    CHECK_EQ(q->commands[8].type, 2);
    CHECK_EQ(q->commands[8].fields[1], 73);
    // Written twice, the same bytes (nothing drifts).
    auto again = hx::writePresetBlob(*q, {}, &error);
    REQUIRE(again.has_value());
    CHECK(*again == *out);

    // Move the block of global slot 3 to path 2 lane B slot 5 (global 20 + 11 + 5 = 36) and rename a snapshot.
    auto moved = p->paths[0].a.take(2);
    REQUIRE(moved.has_value());
    p->paths[1].b.insert(5, std::move(*moved));
    p->snapshots[0].name = "FIRST";
    out = hx::writePresetBlob(*p, {}, &error);
    REQUIRE(out.has_value());
    q = hx::parsePresetBlob(*out);
    REQUIRE(q.has_value());
    CHECK(!q->paths[0].a.slots[2].has_value());
    REQUIRE(q->paths[1].b.slots[5].has_value());
    const hx::PresetBlock& b = *q->paths[1].b.slots[5];
    CHECK_EQ(b.uid, 36);
    CHECK_EQ(b.modelNo, std::uint16_t(349));
    CHECK(b.snapOn[0]);
    CHECK(!b.snapOn[1]);
    REQUIRE(q->controllers.size() == 1);
    CHECK_EQ(q->controllers[0].target.block, 36);
    REQUIRE(q->footswitches[7].size() == 1);
    CHECK_EQ(q->footswitches[7][0].target.block, 36);
    CHECK_EQ(q->snapshots[0].name, std::string("FIRST"));
}

TEST("msgpack: an unchanged tree encodes back byte for byte (wide ints, str16 kept)") {
    const std::vector<std::uint8_t> in = {0x83, 0x01, 0xd1, 0x00, 0x00, 0x02, 0xda, 0x00, 0x02, 'h', 0,
                                          0x03, 0xdc, 0x00, 0x01, 0xcd, 0x00, 0x05};
    CHECK(hx::mp::encode(hx::mp::parse(in)) == in);
    hx::mp::Value v = hx::mp::parse(in);
    v.items[1].i = 70000;  // no longer fits int16: the shortest form instead (uint32)
    const std::vector<std::uint8_t> out = hx::mp::encode(v);
    CHECK_EQ(out[2], std::uint8_t(0xce));
}

TEST("presets: snapshots, controllers and footswitches follow the block") {
    auto p = hx::parsePresetBlob(presetBlob());
    REQUIRE(p.has_value());
    REQUIRE(p->snapshots.size() == 2);
    CHECK_EQ(p->snapshots[0].name, std::string("S ONE"));
    CHECK_EQ(p->snapshots[1].name, std::string("S TWO"));
    CHECK_EQ(p->snapshots[0].colour, 3);
    CHECK_EQ(p->snapshots[0].tempo, 100.0);
    CHECK_EQ(p->snapshot, 0);  // key 10.8 is not the snapshot in use

    hx::PresetBlock& b = *p->paths[0].a.slots[2];
    CHECK_EQ(b.uid, 3);  // global slot: path 0, slot list index 3
    CHECK(b.snapOn[0]);
    CHECK(!b.snapOn[1]);

    REQUIRE(p->controllers.size() == 1);
    const hx::ControllerAssign& c = p->controllers[0];
    CHECK_EQ(c.source, hx::controller::Snapshot);
    CHECK_EQ(c.target.block, 3);
    CHECK_EQ(c.param, 0);
    CHECK(std::abs(c.snapValues[0].v - 0.2) < 1e-6);
    CHECK(std::abs(c.snapValues[1].v - 0.8) < 1e-6);
    CHECK(c.snapValues[2].kind == hx::PresetValue::Kind::None);

    REQUIRE(p->footswitches[7].size() == 1);
    const hx::FootswitchAssign& f = p->footswitches[7][0];
    CHECK_EQ(f.kind, 1);
    CHECK_EQ(f.label, std::string("Comp"));
    CHECK(f.customColour);
    CHECK_EQ(f.colour, std::uint32_t(0xff80));
    CHECK(f.momentary);
    CHECK_EQ(f.ledColour, 8);
    CHECK_EQ(f.target.block, 3);
    CHECK_EQ(hx::footswitchName(7), std::string("FS8"));
    CHECK_EQ(hx::controllerName(hx::controller::footswitch(7)), std::string("FS8"));

    p->recallSnapshot(1);
    CHECK(!p->paths[0].a.slots[2]->enabled);
    CHECK(std::abs(p->paths[0].a.slots[2]->values[0].v - 0.8) < 1e-6);
    p->recallSnapshot(0);
    CHECK(p->paths[0].a.slots[2]->enabled);
    CHECK(std::abs(p->paths[0].a.slots[2]->values[0].v - 0.2) < 1e-6);

    // Copy snapshot 2 onto 1 (the one in use): its state comes in at once, the name stays.
    {
        hx::Preset q = *p;
        q.snapshots[1].colour = 7;
        q.copySnapshot(1, 0);
        CHECK(!q.paths[0].a.slots[2]->enabled);
        CHECK(std::abs(q.paths[0].a.slots[2]->values[0].v - 0.8) < 1e-6);
        CHECK_EQ(q.snapshots[0].colour, 7);
        CHECK_EQ(q.snapshots[0].name, std::string("S ONE"));
    }

    // Moving the block keeps its assignments; removing it drops them.
    auto moved = p->paths[0].a.take(2);
    REQUIRE(moved.has_value());
    p->paths[1].b.insert(5, std::move(*moved));
    hx::Preset::Place where;
    CHECK(p->findBlock(3, &where) != nullptr);
    CHECK_EQ(where.dsp, 1);
    CHECK_EQ(where.lane, 1);
    CHECK_EQ(where.slot, 5);
    p->pruneAssignments();
    CHECK_EQ(p->controllers.size(), std::size_t(1));
    CHECK_EQ(p->freeControllerId(), 1);
    p->paths[1].b.take(5);
    p->pruneAssignments();
    CHECK(p->controllers.empty());
    CHECK(p->footswitches[7].empty());
    CHECK_EQ(p->freeControllerId(), 0);
}

TEST("presets: library from rec 11/12/13 shapes") {
    // rec 13: [[nil, nil, nil, <blob [{0: "SET A"}, {1: "SET B"}]>], 0]
    Mp names13;
    names13.arr(2).map(1).i(0).str("SET A").map(1).i(1).str("SET B");
    Mp r13;
    r13.arr(4).nil().nil().nil().raw(names13.o.b).i(0);
    // rec 12: [[<blob [{0: "First"}, {1: {109: "Second"}}]>], 0]
    Mp names12;
    names12.arr(2).map(1).i(0).str("First").map(1).i(1).map(1).i(109).str("Second");
    Mp r12;
    r12.arr(1).raw(names12.o.b).i(0);
    // rec 11: [[<preset>, nil], 0]
    Mp r11;
    r11.arr(2).raw(presetBlob()).nil().i(0);

    std::vector<std::string> warnings;
    const hx::PresetLibrary lib = hx::readPresets(r11.o.b, r12.o.b, r13.o.b, &warnings);
    for (const std::string& w : warnings) std::printf("  warning: %s\n", w.c_str());
    CHECK(warnings.empty());
    CHECK_EQ(lib.setlists[0], std::string("SET A"));
    CHECK_EQ(lib.setlists[1], std::string("SET B"));
    CHECK_EQ(lib.setlists[2], std::string("Setlist 3"));
    CHECK_EQ(lib.presets[0].name, std::string("First"));
    CHECK(lib.presets[0].stored);
    CHECK_EQ(lib.presets[1].name, std::string("Second"));
    CHECK(!lib.presets[1].stored);
}

TEST("presets: numbering") {
    CHECK_EQ(hx::presetNumber(0), std::string("01A"));
    CHECK_EQ(hx::presetNumber(5), std::string("02B"));
    CHECK_EQ(hx::presetNumber(127), std::string("32D"));
}

namespace {

hx::PresetBlock blk(int modelNo) {
    hx::PresetBlock b;
    b.modelNo = std::uint16_t(modelNo);
    return b;
}

// Lane contents as model numbers, 0 for empty: "1 2 0 3 ...".
std::string laneStr(const hx::PresetLane& l) {
    std::string s;
    for (const auto& b : l.slots) s += (s.empty() ? "" : " ") + std::to_string(b ? b->modelNo : 0);
    return s;
}

} // namespace

TEST("lane: insert into empty, shift right, shift left, full") {
    hx::PresetLane l;
    CHECK_EQ(l.insert(2, blk(1)), 2);
    CHECK_EQ(laneStr(l), std::string("0 0 1 0 0 0 0 0"));
    CHECK_EQ(l.insert(2, blk(2)), 2);  // occupied: 1 moves right
    CHECK_EQ(laneStr(l), std::string("0 0 2 1 0 0 0 0"));

    hx::PresetLane r;
    for (int i = 0; i < 8; ++i)
        if (i != 1) r.insert(i, blk(10 + i));
    CHECK_EQ(r.insert(5, blk(99)), 5);  // nothing free on the right: 12..15 move left into slot 1
    CHECK_EQ(laneStr(r), std::string("10 12 13 14 15 99 16 17"));
    CHECK_EQ(r.insert(0, blk(5)), -1);  // full
    CHECK_EQ(r.insert(8, blk(5)), -1);  // out of range
}

TEST("lane: take leaves a gap") {
    hx::PresetLane l;
    l.insert(0, blk(1));
    l.insert(1, blk(2));
    const auto b = l.take(0);
    REQUIRE(b.has_value());
    CHECK_EQ(int(b->modelNo), 1);
    CHECK_EQ(laneStr(l), std::string("0 2 0 0 0 0 0 0"));
    CHECK(!l.take(0).has_value());
}

TEST("routing: normalize after lane B edits") {
    hx::PresetPath p;
    p.b.insert(3, blk(7));
    hx::normalizeRouting(p);  // serial path gained a B block: split before it, join at the end
    CHECK_EQ(p.routing.mode, 1);
    CHECK(p.routing.splitOn && p.routing.joinOn);
    CHECK_EQ(p.routing.splitPos, 3);
    CHECK_EQ(p.routing.joinPos, 8);

    p.routing.mode = 2;  // an existing parallel layout is left alone
    p.b.insert(0, blk(8));
    hx::normalizeRouting(p);
    CHECK_EQ(p.routing.mode, 2);
    CHECK_EQ(p.routing.splitPos, 3);

    p.b.take(0);
    p.b.take(3);
    hx::normalizeRouting(p);  // lane B empty: back to serial
    CHECK_EQ(p.routing.mode, 0);
    CHECK(!p.routing.splitOn && !p.routing.joinOn);
}

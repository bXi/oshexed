// oshexed --info  FILE.hxf          same output as tools/hxf.py info
// oshexed --lists FILE.hxf          same output as tools/models.py DIR lists
// oshexed --model FILE.hxf NAME     a model's browser entry, cost and rec 4 knobs
// oshexed --presets FILE.hxf        setlists and stored presets; same output as tools/oracle/presets_ref.py
// oshexed --snapshots FILE.hxf      snapshots, controller assignments, footswitches; same as tools/oracle/snapshots_ref.py
// oshexed --hlx FILE.hxf PRESET.hlx an HX Edit preset file as the editor imports it (presets + snapshots format)
// oshexed --parts FILE.hxf        the parts the mod builder edits (path size sha256); same as tools/oracle/parts_ref.py
// oshexed --repack FILE.hxf [-o OUT.hxf]  split into parts and pack again; exit 0 when the file comes back identical
// oshexed --m4-build STOCK.hxf PKG.ohem... [-o OUT]  a mod build: changed parts with SHA-256 (-o: the M4 app)
// oshexed --mod-build STOCK.hxf PKG.ohem... [-o OUT.hxf]  the same, packed (= modpkg.py apply + fwedit.py build)
// oshexed --gfx FILE.hxf          every image: index, name, size, CRC32 of its RGB; same as tools/oracle/gfx_ref.py
// oshexed --flash-plan FILE.hxf     the writes a full flash sends; same output as tools/hxflash.py plan
// oshexed --flash-sim FILE.hxf [--check-slots]
//                                   full flash against a fake unit (every hash checked, no USB); same
//                                   last line as tools/hxflash.py sim
// oshexed --flash FILE.hxf [--fast] [--check-slots] [--keep-presets | --rebuild-presets] [--simulate] [--yes]
//                                   REAL UNIT, writes: the records of FILE (all, or with --fast those that
//                                   differ from what this program last wrote). Asks first unless --yes.
//                                   --simulate: the same against the fake unit, nothing sent.
// oshexed --unit-state              what this program last wrote to each slot
// oshexed --assume FILE.hxf         record that the unit holds FILE (e.g. after the Line 6 Updater)
// oshexed --device-info             REAL UNIT, read-only: product, version, build (editor service 5), then a
//                                   clean close; the unit keeps running (tools/hxflash.py query)

#include "cli/hxed_dump.h"
#include "core/appdata.h"
#include "core/buildtag.h"
#include "core/checksums.h"
#include "core/modstore.h"
#include "core/device/editorclient.h"
#include "core/device/fakeunit.h"
#include "core/device/updater.h"
#include "core/device/usbio.h"
#include "core/firmware.h"
#include "core/hlx.h"
#include "core/dspbuild.h"
#include "core/modbuild.h"
#include "core/hxfwrite.h"
#include "core/m4build.h"
#include "core/modspec.h"
#include "core/ir.h"
#include "core/nam.h"
#include "core/presetwriter.h"
#include "core/text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string hex(hx::Bytes b) {
    std::string s;
    char buf[3];
    for (std::uint8_t c : b) {
        std::snprintf(buf, sizeof buf, "%02x", c);
        s += buf;
    }
    return s;
}

const char* flag(bool ok) { return ok ? "ok" : "BAD"; }

int cmdInfo(const char* path) {
    std::string error;
    const std::unique_ptr<hx::HxfImage> img = hx::loadHxfFile(path, &error);
    if (!img) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    // Stock or one of our builds (MODTAG.md), first: it's what this file is.
    if (const hx::HxfRecord* app = img->record(0)) {
        std::string appError;
        if (const std::optional<hx::AppImage> a = hx::parseApp(app->payload, &appError)) {
            const hx::FirmwareIdentity id = hx::identifyFirmware(img->imageSha256, *a);
            std::printf("build: %s\n", id.summary.c_str());
            if (id.build) std::printf("  tag %s: layout %d, commit %s%s, mod slots", id.tag.c_str(), id.build->layout,
                                      id.build->commit.c_str(), id.build->dirty ? " (dirty)" : "");
            if (id.build) {
                for (int s : id.build->mods) std::printf(" %d", s);
                std::printf("\n");
            }
        } else {
            std::printf("build: unknown (record 0: %s)\n", appError.c_str());
        }
    }
    std::printf("file %s: %zu bytes, FORM size %#x\n", path, img->fileSize, img->formSize);
    for (const hx::HxfChunk& c : img->chunks) {
        if (c.id == "data")
            std::printf("  chunk %s @%#zx len %#zx: zlib -> %zu bytes\n", c.id.c_str(), c.offset, c.length, img->image.size());
        else
            std::printf("  chunk %s @%#zx len %#zx: %s\n", c.id.c_str(), c.offset, c.length, hex(c.body).c_str());
        if (c.id == "HEAD") std::printf("    HEAD MD5 of image: %s\n", img->md5Ok ? "OK" : "BAD");
    }
    std::printf("L6IA header:");
    for (std::uint32_t w : img->l6iaHeader) std::printf(" %08x", w);
    std::printf("\n%3s %9s %4s %10s %8s  crc words hash8 inner  name\n", "#", "offset", "type", "size", "crc");
    int bad = 0;
    for (const hx::HxfRecord& r : img->records) {
        const hx::RecordChecks c = hx::checkRecord(r);
        bad += !c.all();
        std::string inner = "-";
        if (c.inner != hx::InnerKind::None)
            inner = std::string(c.inner == hx::InnerKind::App ? "app:" : "ui:") + flag(c.innerOk);
        std::printf("%3u %9zx %4u %10u %08x  %3s %5s %5s %6s  %s\n", r.index, r.offset, r.type, r.size, r.crc,
                    flag(c.crc), flag(c.words), flag(c.hash8), inner.c_str(), hx::recordName(r.index));
    }
    if (bad == 0) std::printf("all checks pass\n");
    else std::printf("%d record(s) failing checks\n", bad);
    return 0;
}

std::unique_ptr<hx::Firmware> load(const char* path) {
    std::string error;
    std::unique_ptr<hx::Firmware> fw = hx::loadFirmware(path, &error);
    if (!fw) std::fprintf(stderr, "%s\n", error.c_str());
    else
        for (const std::string& w : fw->warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    return fw;
}

int cmdLists(const char* path) {
    const auto fw = load(path);
    if (!fw) return 1;
    for (const hx::BrowserList& l : fw->tables.lists)
        std::printf("cat %2d sub %d  @%#x  %3zu entries  first '%s'\n", l.category, l.sub, l.va, l.entries.size(),
                    l.entries.empty() ? "" : l.entries[0].name.c_str());
    return 0;
}

bool containsNoCase(const std::string& hay, const char* needle) {
    const std::size_t n = std::strlen(needle);
    for (std::size_t i = 0; i + n <= hay.size(); ++i) {
        std::size_t k = 0;
        while (k < n && std::tolower((unsigned char)hay[i + k]) == std::tolower((unsigned char)needle[k])) ++k;
        if (k == n) return true;
    }
    return false;
}

int cmdModel(const char* path, const char* query) {
    const auto fw = load(path);
    if (!fw) return 1;
    const hx::DspMeta& meta = fw->meta;
    // Browser entries whose on-screen name or rec 4 symbol (HD2_...) contains the text; exact matches first.
    const auto same = [&](const std::string& s) { return s.size() == std::strlen(query) && containsNoCase(s, query); };
    std::vector<std::pair<const hx::BrowserList*, const hx::BrowserEntry*>> exact, partial;
    for (const hx::BrowserList& l : fw->tables.lists)
        for (const hx::BrowserEntry& e : l.entries) {
            const std::string sym = meta.symbol(e.symId);
            if (same(e.name) || same(sym)) exact.emplace_back(&l, &e);
            else if (containsNoCase(e.name, query) || containsNoCase(sym, query)) partial.emplace_back(&l, &e);
        }
    exact.insert(exact.end(), partial.begin(), partial.end());
    int shown = 0;
    for (const auto& [lp, ep] : exact) {
        {
            const hx::BrowserList& l = *lp;
            const hx::BrowserEntry& e = *ep;
            ++shown;
            const hx::CostEntry* cost = fw->tables.costBySymId(e.symId);
            std::printf("%s  [%s]  modelNo %u  symId %#x %s  cost %s\n", e.name.c_str(), l.label.c_str(), e.modelNo,
                        e.symId, meta.symbol(e.symId).c_str(), cost ? std::to_string(cost->cost).c_str() : "-");
            const hx::MetaEntry* m = meta.bySymId(e.symId);
            if (!m) continue;
            std::printf("  rec 4 entry %u, rec 5 resource %u\n", m->index, m->rec5);
            for (const hx::MetaParam& k : m->knobs)
                std::printf("  %2u %-20s %-7s min %s max %s default %s\n", k.number, meta.displayName(k).c_str(),
                            hx::paramTypeName(k.type), hx::formatParamValue(k.type, k.min).c_str(),
                            hx::formatParamValue(k.type, k.max).c_str(), hx::formatParamValue(k.type, k.def).c_str());
        }
    }
    if (!shown) std::printf("no browser entry matches '%s'\n", query);
    return 0;
}

// One line per stored preset: slot, name, then per path its routing (mode, split pos/on, join
// pos/on) and per lane the 8 slots as modelNo(value count), '-' for empty, '!' suffix when
// bypassed, 'L' prefix for a looper slot; then the cost of each path.
void printPresetLine(const hx::Firmware& fw, const hx::Preset& p, const std::string& label) {
    std::printf("%s %s", label.c_str(), p.name.c_str());
    for (int dsp = 0; dsp < 2; ++dsp) {
        const hx::PresetPath& path = p.paths[std::size_t(dsp)];
        const hx::PresetRouting& r = path.routing;
        std::printf(" | r%d s%d%s j%d%s", r.mode, r.splitPos, r.splitOn ? "+" : "", r.joinPos, r.joinOn ? "+" : "");
        for (const hx::PresetLane* lane : {&path.a, &path.b}) {
            std::printf(" |");
            for (const auto& b : lane->slots) {
                if (!b) std::printf(" -");
                else
                    std::printf(" %s%u(%zu)%s", b->slotType == 7 ? "L" : "", unsigned(b->modelNo), b->values.size(),
                                b->enabled ? "" : "!");
            }
        }
    }
    std::printf(" | cost %u %u\n", fw.pathCost(p.paths[0]), fw.pathCost(p.paths[1]));
}

std::string slotLabel(std::size_t s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04zu", s);
    return buf;
}

int cmdPresets(const char* path) {
    const auto fw = load(path);
    if (!fw) return 1;
    const hx::PresetLibrary& lib = fw->presets;
    for (std::size_t i = 0; i < lib.setlists.size(); ++i) std::printf("setlist %zu %s\n", i, lib.setlists[i].c_str());
    for (std::size_t s = 0; s < lib.presets.size(); ++s)
        if (lib.presets[s].stored) printPresetLine(*fw, lib.presets[s], slotLabel(s));
    return 0;
}

std::string presetValue(const hx::PresetValue& v) {
    char buf[32];
    switch (v.kind) {
    case hx::PresetValue::Kind::None: return "-";
    case hx::PresetValue::Kind::Bool: return v.v != 0 ? "true" : "false";
    case hx::PresetValue::Kind::Int: std::snprintf(buf, sizeof buf, "%lld", (long long)v.v); return buf;
    case hx::PresetValue::Kind::Float: std::snprintf(buf, sizeof buf, "%.6g", v.v); return buf;
    }
    return "-";
}

// A target as its global slot. Blocks read from a preset carry their global slot as uid.
int targetSlot(const hx::PresetTarget& t) { return t.block >= 0 ? t.block : t.slot; }

// Per stored preset: each snapshot (name, colour, tempo, then per global slot 0-39 the
// block's on/off as 1 / 0, '.' where there is no block); each controller assignment (id, source, slot, knob,
// min, max, value in each snapshot); each footswitch function (switch index, kind, slot, knob, colour, label).
// Same output as tools/oracle/snapshots_ref.py.
void printSnapshotLines(const hx::Preset& p, const std::string& label) {
    const char* s = label.c_str();
    for (std::size_t i = 0; i < p.snapshots.size(); ++i) {
        const hx::PresetSnapshot& sn = p.snapshots[i];
        std::string on;
        for (int g = 0; g < 2 * hx::kSlotsPerPath; ++g) {
            const hx::PresetBlock* b = p.findBlock(g);
            on += b ? (b->snapOn[i] ? '1' : '0') : '.';
        }
        std::printf("%s s%zu %s | c%d t%.6g | %s\n", s, i, sn.name.c_str(), sn.colour, sn.tempo, on.c_str());
    }
    for (const hx::ControllerAssign& c : p.controllers) {
        std::printf("%s ctl %d src %d slot %d knob %d min %s max %s snap", s, c.id, c.source, targetSlot(c.target), c.param,
                    presetValue(c.min).c_str(), presetValue(c.max).c_str());
        for (const hx::PresetValue& v : c.snapValues) std::printf(" %s", presetValue(v).c_str());
        std::printf("\n");
    }
    for (std::size_t i = 0; i < p.footswitches.size(); ++i)
        for (const hx::FootswitchAssign& f : p.footswitches[i]) {
            char colour[16] = "-";
            if (f.customColour) std::snprintf(colour, sizeof colour, "%06x", unsigned(f.colour));
            std::printf("%s fs %zu k%d slot %d knob %d %s %s\n", s, i, f.kind, targetSlot(f.target), f.param, colour,
                        f.label.c_str());
        }
}

int cmdSnapshots(const char* path) {
    const auto fw = load(path);
    if (!fw) return 1;
    for (std::size_t s = 0; s < fw->presets.presets.size(); ++s)
        if (fw->presets.presets[s].stored) printSnapshotLines(fw->presets.presets[s], slotLabel(s));
    return 0;
}

// An HX Edit preset file as the editor reads it: warnings, then the same lines as `presets` and `snapshots`
// (label "hlx"). Blocks imported from a file also carry their global slot as uid.
int cmdHlx(const char* path, const char* hlxPath) {
    const auto fw = load(path);
    if (!fw) return 1;
    std::ifstream in(hx::pathFromUtf8(hlxPath), std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", hlxPath);
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    std::vector<std::string> warnings;
    const std::optional<hx::Preset> p = hx::importHlx(text, *fw, &error, &warnings);
    for (const std::string& w : warnings) std::printf("warning: %s\n", w.c_str());
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("snapshot in use: %d\n", p->snapshot);
    printPresetLine(*fw, *p, "hlx");
    printSnapshotLines(*p, "hlx");
    return 0;
}

// Connects with the editor protocol (read-only: names and the current preset), prints what the unit says,
// disconnects. The same requests HX Edit makes at startup.
int cmdUnit() {
    std::string error;
    std::unique_ptr<hx::LinkIo> io = hx::openUsbLink(&error);
    if (!io) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("USB: %s\n", io->description().c_str());
    try {
        hx::EditorClient c(*io);
        c.log = [](const std::string& s) { std::printf("  %s\n", s.c_str()); };
        c.open();
        {
            std::optional<std::string> reply;
            try {
                const hx::EditorClient::Reply r =
                    c.call(hx::EditorChannel::Edit, hx::editor::kBuildTag, hx::mp::map(), 2.0);
                if (r.status == 0 && r.result.type == hx::mp::Type::Str) reply = r.result.s;
                else std::printf("command %d: status %d\n", hx::editor::kBuildTag, r.status);
            } catch (const hx::LinkError& e) {
                std::printf("command %d: %s\n", hx::editor::kBuildTag, e.what());
            }
            std::printf("build: %s\n", hx::identifyUnit(reply).summary.c_str());
        }
        const auto setlists = hx::editor::setlistNames(
            c.call(hx::EditorChannel::Library, hx::editor::kSetlistNames, hx::mp::nil()).result);
        for (std::size_t i = 0; i < setlists.size(); ++i) std::printf("setlist %zu: %s\n", i, setlists[i].c_str());
        const auto info = hx::editor::currentInfo(
            c.call(hx::EditorChannel::Edit, hx::editor::kCurrentInfo, hx::mp::nil()).result);
        std::printf("current: setlist %d preset %s \"%s\" snapshot %d\n", info.setlist,
                    hx::presetNumber(info.preset).c_str(), info.name.c_str(), info.snapshot + 1);
        const hx::mp::Value body = c.call(hx::EditorChannel::Edit, hx::editor::kCurrentPreset, hx::mp::nil()).result;
        std::string perr;
        if (const auto p = hx::parsePresetBlob(hx::Bytes(reinterpret_cast<const std::uint8_t*>(body.s.data()), body.s.size()), &perr)) {
            for (int dsp = 0; dsp < 2; ++dsp) {
                std::printf("path %d:", dsp + 1);
                for (const hx::PresetLane* lane : {&p->paths[std::size_t(dsp)].a, &p->paths[std::size_t(dsp)].b})
                    for (const auto& b : lane->slots)
                        if (b) std::printf(" %u%s", unsigned(b->modelNo), b->enabled ? "" : "(off)");
                std::printf("\n");
            }
        } else {
            std::printf("preset body: %s (%zu bytes)\n", perr.c_str(), body.s.size());
        }
        c.close();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}

// oshexed --settings ID...: REAL UNIT, read-only: global settings by id as the unit stores them (cmd 24), e.g. to
// match a value with what the unit's Global Settings screen shows.
int cmdSettings(int argc, char** argv) {
    std::string error;
    std::unique_ptr<hx::LinkIo> io = hx::openUsbLink(&error);
    if (!io) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    try {
        hx::EditorClient c(*io);
        c.open();
        for (int i = 2; i < argc; ++i) {
            const int id = std::atoi(argv[i]);
            const hx::mp::Value r = c.call(hx::EditorChannel::Edit, hx::editor::kGlobal, hx::mp::map({{118, hx::mp::integer(id)}})).result;
            const hx::mp::Value* v = r.get(119);
            if (!v || v->isNil()) std::printf("%d: -\n", id);
            else if (v->type == hx::mp::Type::Bool) std::printf("%d: %s\n", id, v->b ? "true" : "false");
            else if (v->type == hx::mp::Type::Int) std::printf("%d: %lld\n", id, static_cast<long long>(v->i));
            else std::printf("%d: %g\n", id, v->asNumber());
        }
        c.close();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}

// oshexed --ir-read SLOT [-o FILE]: REAL UNIT, read-only probe of the IR read-back the firmware has (library command
// 11 = read a slot's samples, streamed, as HX Edit's IR export; 12 = a slot's info; handlers registered at
// 0x280f0870.., hf_usbLibrarianServiceCommands_280ef1b4 / _280f02d8). SLOT is 0-based. Prints what comes back.
int cmdIrRead(int argc, char** argv) {
    const int slot = std::atoi(argv[2]);
    const char* out = argc >= 5 && std::strcmp(argv[3], "-o") == 0 ? argv[4] : nullptr;
    std::string error;
    std::unique_ptr<hx::LinkIo> io = hx::openUsbLink(&error);
    if (!io) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    try {
        hx::EditorClient c(*io);
        c.log = [](const std::string& s) { std::printf("  %s\n", s.c_str()); };
        c.open();
        const auto args = hx::mp::map({{112, hx::mp::integer(slot)}});
        const hx::EditorClient::Reply info = c.call(hx::EditorChannel::Library, 12, args);
        std::printf("lib 12 (slot info): status %d %s\n", info.status, hx::mp::toText(info.result, 400).c_str());
        const hx::EditorClient::Reply r = c.call(hx::EditorChannel::Library, 11, args, 10.0);
        std::printf("lib 11 (read): status %d %s\n", r.status, hx::mp::toText(r.result, 400).c_str());
        // the result is the samples themselves (8192 bytes = 2048 LE float32, the slot's data after its header)
        const hx::mp::Value* data = !r.result.s.empty() ? &r.result : r.result.get(110);
        if (data && !data->s.empty()) {
            const std::string& s = data->s;
            std::printf("data: %zu bytes", s.size());
            for (std::size_t k = 0; k + 4 <= s.size() && k < 32; k += 4) {
                float f;
                std::memcpy(&f, s.data() + k, 4);
                std::printf(" %g", f);
            }
            std::printf("\n");
            if (out) {
                std::ofstream f(hx::pathFromUtf8(out), std::ios::binary);
                f.write(s.data(), std::streamsize(s.size()));
                std::printf("wrote %s\n", out);
            }
        }
        c.poll(2.0);  // the rest of a streamed answer may come as notices
        for (const hx::EditorNotice& n : c.takeNotices())
            std::printf("notice %d: %s\n", n.event, hx::mp::toText(n.payload, 200).c_str());
        c.close();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}

// A preset as an HX Edit file on stdout: a factory slot (0-1023), or a .hlx read and written again (to compare with
// the original). Warnings go to stderr.
int cmdExportHlx(const char* path, const char* what) {
    const auto fw = load(path);
    if (!fw) return 1;
    std::optional<hx::Preset> p;
    char* end = nullptr;
    const long slot = std::strtol(what, &end, 10);
    if (end != what && *end == 0) {
        if (slot < 0 || std::size_t(slot) >= fw->presets.presets.size() || !fw->presets.presets[std::size_t(slot)].stored) {
            std::fprintf(stderr, "slot %ld has no stored preset\n", slot);
            return 1;
        }
        p = fw->presets.presets[std::size_t(slot)];
    } else {
        std::ifstream in(hx::pathFromUtf8(what), std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", what);
            return 1;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        std::vector<std::string> warnings;
        p = hx::importHlx(text, *fw, &error, &warnings);
        for (const std::string& w : warnings) std::fprintf(stderr, "import warning: %s\n", w.c_str());
        if (!p) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    }
    std::vector<std::string> warnings;
    const std::string out = hx::exportHlx(*p, *fw, &warnings);
    for (const std::string& w : warnings) std::fprintf(stderr, "export warning: %s\n", w.c_str());
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}


int cmdGfx(const char* path) {
    const std::unique_ptr<hx::Firmware> fw = load(path);
    if (!fw) return 1;
    if (!fw->gfx) {
        std::fprintf(stderr, "no graphics\n");
        return 1;
    }
    for (const hx::GfxImage& g : fw->gfx->images()) {
        const std::vector<std::uint8_t> rgb = fw->gfx->rgb(g);
        std::printf("%3u %-28s %3ux%-3u %08x\n", unsigned(g.index), g.name.c_str(), unsigned(g.width), unsigned(g.height),
                    unsigned(hx::crc32(rgb)));
    }
    return 0;
}

std::unique_ptr<hx::HxfImage> loadOrSay(const char* path) {
    std::string error;
    std::unique_ptr<hx::HxfImage> img = hx::loadHxfFile(path, &error);
    if (!img) std::fprintf(stderr, "%s\n", error.c_str());
    return img;
}

int cmdFlashPlan(const char* path) {
    const std::unique_ptr<hx::HxfImage> img = loadOrSay(path);
    if (!img) return 1;
    try {
        std::size_t writes = 0, total = 0;
        hx::forEachFlashWrite(hx::recordFiles(*img), [&](const std::string& name, hx::Bytes b) {
            ++writes;
            total += b.size();
            if (name.rfind("chunk", 0) != 0)
                std::printf("%-12s %6zu B  %s\n", name.c_str(), b.size(), hex(b.first(std::min<std::size_t>(32, b.size()))).c_str());
        });
        std::printf("%zu writes, %zu bytes\n", writes, total);
    } catch (const hx::FormatError& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}

int cmdFlashSim(const char* path, bool checkSlots) {
    const std::unique_ptr<hx::HxfImage> img = loadOrSay(path);
    if (!img) return 1;
    try {
        const std::vector<hx::RecordFile> files = hx::recordFiles(*img);
        hx::FakeClock clock;
        hx::FakeUnit unit(clock);
        hx::Link link(unit, clock.clock());
        hx::Flasher flasher(link);
        hx::FlashOptions opt;
        opt.checkSlots = checkSlots;
        flasher.flash(files, opt);
        std::size_t bytes = 0;
        for (const hx::RecordFile& f : files) {
            const auto it = unit.records().find(f.index);
            if (it == unit.records().end() || it->second != f.data) {
                std::fprintf(stderr, "record %u differs on the fake unit\n", unsigned(f.index));
                return 1;
            }
            bytes += f.data.size();
        }
        std::printf("sim OK: %zu records, %zu bytes, every hash checked\n", files.size(), bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sim FAILED: %s\n", e.what());
        return 1;
    }
    return 0;
}

int cmdDeviceInfo() {
    std::string error;
    std::unique_ptr<hx::LinkIo> io = hx::openUsbLink(&error);
    if (!io) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    auto print = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
    print("USB: " + io->description());
    hx::Link link(*io);
    link.log = print;
    link.trace = print;
    try {
        hx::Flasher flasher(link);
        flasher.log = print;
        flasher.open(/*withInfo=*/true, /*handover=*/false);
        link.close();
    } catch (const hx::LinkError& e) {
        link.closeQuietly();
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    std::printf("done; link closed%s\n", link.unitClosed() ? "" : " (no close reply from the unit)");
    return 0;
}

// Shared with the app (its data folder, core/appdata.h).
std::string unitStatePath() {
    const std::string dir = hx::appDataDir();
    if (dir.empty()) return "unit_state.txt";
    return (std::filesystem::path(dir) / "unit_state.txt").string();
}

bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 3; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

std::string baseName(const char* path) { return hx::pathFromUtf8(path).filename().string(); }

int cmdUnitState() {
    const hx::UnitState state(unitStatePath());
    if (state.empty()) std::printf("no record of the unit's contents: the next --fast flash writes everything\n");
    for (std::uint32_t ri = 0; ri < 16; ++ri) {
        const std::string name = state.fileName(ri);
        std::printf("  %2u  %s\n", unsigned(ri), name.empty() ? "?  (unknown: will be written)" : name.c_str());
    }
    return 0;
}

int cmdAssume(const char* path) {
    const std::unique_ptr<hx::HxfImage> img = loadOrSay(path);
    if (!img) return 1;
    hx::UnitState state(unitStatePath());
    std::vector<hx::RecordFile> files;
    try {
        files = hx::recordFiles(*img);
    } catch (const hx::FormatError& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    state.assume(files, baseName(path));
    std::printf("recorded: the unit holds %s in all %zu slots\n", baseName(path).c_str(), files.size());
    return 0;
}

int cmdFlash(int argc, char** argv) {
    const char* path = argv[2];
    const bool simulate = hasFlag(argc, argv, "--simulate");
    const std::unique_ptr<hx::HxfImage> img = loadOrSay(path);
    if (!img) return 1;
    std::vector<hx::RecordFile> files;
    try {
        files = hx::recordFiles(*img);  // checks sizes and hashes before anything touches the unit
    } catch (const hx::FormatError& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    hx::UnitState state(unitStatePath());
    if (hasFlag(argc, argv, "--keep-presets") && hasFlag(argc, argv, "--rebuild-presets")) {
        std::fprintf(stderr, "--keep-presets and --rebuild-presets together\n");
        return 1;
    }
    const hx::PresetsMode pm = hasFlag(argc, argv, "--keep-presets")      ? hx::PresetsMode::Keep
                               : hasFlag(argc, argv, "--rebuild-presets") ? hx::PresetsMode::Rebuild
                                                                          : hx::PresetsMode::Auto;
    const hx::PresetsChoice presets = hx::presetsDecision(pm, files, state);
    hx::FlashOptions opt;
    opt.checkSlots = hasFlag(argc, argv, "--check-slots");
    opt.keepPresets = presets.keep;
    opt.sourceName = baseName(path);
    if (hasFlag(argc, argv, "--fast"))
        for (const hx::RecordFile& f : files)
            if (state.same(f)) opt.skip.insert(f.index);

    std::size_t bytes = 0, count = 0;
    std::printf("%s%s\n", simulate ? "REHEARSAL against the fake unit (nothing is sent): " : "", path);
    for (const hx::RecordFile& f : files) {
        const bool skip = opt.skip.count(f.index) != 0;
        std::printf("  record %2u  %-14s %10zu B  %s\n", unsigned(f.index), hx::recordName(f.index), f.data.size(),
                    skip ? "skip (already on the unit)" : "write");
        if (!skip) bytes += f.data.size(), ++count;
    }
    std::printf("presets: %s (%s)\n", presets.keep ? "kept" : "rebuilt", presets.reason.c_str());
    if (count == 0) {
        std::printf("nothing to write\n");
        return 0;
    }
    if (!simulate && !hasFlag(argc, argv, "--yes")) {
        std::printf("\nThis writes %zu records (%.1f MB) to the unit. If it stops halfway, the unit needs recovery:\n"
                    "power off, hold Mode + Tap while switching on, flash a stock .hxf.\n"
                    "Close HX Edit and the Line 6 Updater first. Type yes to write: ",
                    count, double(bytes) / 1e6);
        std::fflush(stdout);
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "yes") {
            std::printf("cancelled; nothing sent\n");
            return 1;
        }
    }

    hx::FakeClock clock;
    std::unique_ptr<hx::LinkIo> io;
    std::string error;
    if (simulate) io = std::make_unique<hx::FakeUnit>(clock);
    else io = hx::openUsbLink(&error);
    if (!io) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    hx::Link link(*io, simulate ? clock.clock() : hx::Clock(hx::steadySeconds));
    hx::Flasher flasher(link, simulate ? nullptr : &state);
    auto print = [](const std::string& s) { std::printf("\r%-70s\n", s.c_str()); };
    link.log = print;
    flasher.log = print;
    std::size_t written = 0, recordBase = 0;
    flasher.onRecord = [&](std::uint32_t, hx::RecordStep st) {
        if (st == hx::RecordStep::Prepare) recordBase = written;
    };
    flasher.onRecordBytes = [&](std::uint32_t ri, std::size_t done, std::size_t total) {
        written = recordBase + done;
        std::printf("\r  %5.1f%%  record %u: %zu / %zu B   ", 100.0 * double(written) / double(bytes), unsigned(ri), done,
                    total);
        std::fflush(stdout);
    };
    try {
        flasher.flash(files, opt);
    } catch (const std::exception& e) {
        link.closeQuietly();
        std::fprintf(stderr, "\n\nFLASH FAILED: %s\n", e.what());
        if (flasher.writing())
            std::fprintf(stderr, "Writing had started: the unit needs a full update (power off, Mode + Tap at power-on,\n"
                                 "then flash a stock .hxf here or with the Line 6 Updater).\n");
        else
            std::fprintf(stderr, "Nothing was written. Reboot the unit normally.\n");
        return 1;
    }
    return 0;
}

// The preset writer's self-check on the firmware's own presets: each one written back unchanged must come out byte
// for byte as stored (core/presetwriter.h). Prints the first differences.
// The parts the mod builder edits, as fwedit.py explode writes them: one line "path size sha256" each.
// Same output as tools/oracle/parts_ref.py.
int cmdParts(const char* path) {
    std::string error;
    const std::unique_ptr<hx::HxfImage> img = hx::loadHxfFile(path, &error);
    const std::optional<hx::FirmwareParts> p = img ? hx::explodeParts(*img, &error) : std::nullopt;
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const auto line = [](const std::string& name, const std::vector<std::uint8_t>& b) {
        const auto h = hx::sha256(b);
        std::printf("%s %zu %s\n", name.c_str(), b.size(), hx::toHex(h).c_str());
    };
    line("main_app/m4_app.bin", p->app);
    if (!p->appShared) line("main_app/m4_app_copy.bin", p->appCopy);
    line("dsp/boot_ldr.bin", p->bootLdr);
    line("dsp/module_meta.bin", p->moduleMeta);
    for (std::size_t i = 0; i < p->modules.modules.size(); ++i) line(p->moduleFiles[i], p->modules.modules[i]);
    return 0;
}

// Explode the parts, put them back and pack: an unchanged file must come back byte for byte (stage B check).
int cmdRepack(int argc, char** argv) {
    const char* path = argv[2];
    const char* outPath = argc >= 5 && std::strcmp(argv[3], "-o") == 0 ? argv[4] : nullptr;
    std::string error;
    const std::unique_ptr<hx::HxfImage> img = hx::loadHxfFile(path, &error);
    const std::optional<hx::FirmwareParts> p = img ? hx::explodeParts(*img, &error) : std::nullopt;
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const hx::PackedHxf out = hx::packHxf(*img, hx::assembleParts(*p));
    std::ifstream in(hx::pathFromUtf8(path), std::ios::binary);
    const std::vector<std::uint8_t> orig((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string changed;
    for (std::uint32_t r : out.changed) changed += (changed.empty() ? "" : ", ") + std::to_string(r);
    std::printf("packed %zu bytes, image %zu bytes, %s\n", out.file.size(), out.imageSize,
                changed.empty() ? "no records modified" : ("modified records: " + changed).c_str());
    std::size_t at = 0;
    while (at < orig.size() && at < out.file.size() && orig[at] == out.file[at]) ++at;
    const bool same = orig == out.file;
    if (same) std::printf("identical to %s\n", path);
    else std::printf("DIFFERS from %s at byte %#zx (sizes %zu / %zu)\n", path, at, orig.size(), out.file.size());
    if (outPath && !hx::writeFile(outPath, out.file, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (outPath) std::printf("wrote %s\n", outPath);
    return same ? 0 : 1;
}

std::vector<std::uint8_t> readAll(const std::string& path) {
    std::ifstream in(hx::pathFromUtf8(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A mod build from STOCK + packages (stages C + D; same as tools/modpkg.py apply + fwedit.py build).
// oshexed --m4-build STOCK.hxf PKG.ohem... [-o m4_app.bin] [--commit 7HEX] [--dirty 0|1]   the parts, -o = the M4 app
// oshexed --mod-build STOCK.hxf PKG.ohem... [-o OUT.hxf] [--commit 7HEX] [--dirty 0|1]     the parts and the .hxf
int cmdModBuild(int argc, char** argv, bool whole) {
    std::string error, outPath, commit = "0000000";
    bool dirty = true;
    std::vector<std::string> files;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--commit" && i + 1 < argc) commit = argv[++i];
        else if (a == "--dirty" && i + 1 < argc) dirty = std::string(argv[++i]) != "0";
        else files.push_back(a);
    }
    const std::unique_ptr<hx::Firmware> fw = hx::loadFirmware(argv[2], &error);
    if (!fw) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const hx::mods::Stock stock = hx::mods::stockOf(*fw);
    const std::optional<hx::FirmwareParts> parts = stock.ok && fw->hxf ? hx::explodeParts(*fw->hxf, &error) : std::nullopt;
    if (!parts) {
        std::fprintf(stderr, "%s\n", stock.ok ? error.c_str() : stock.why.c_str());
        return 1;
    }
    std::vector<hx::mods::Package> pkgs;
    for (const std::string& f : files) {
        const std::vector<std::uint8_t> bytes = readAll(f);
        const std::string name = hx::utf8FromPath(hx::pathFromUtf8(f).filename());
        std::optional<hx::mods::Package> p = hx::mods::readPackage(bytes, hx::mods::idFromFileName(name), &error);
        if (!p) {
            std::fprintf(stderr, "%s: %s\n", f.c_str(), error.c_str());
            return 1;
        }
        if (p->fwImageSha256 != stock.imageSha256) {
            std::fprintf(stderr, "%s: made for another firmware image\n", f.c_str());
            return 1;
        }
        pkgs.push_back(std::move(*p));
    }
    std::vector<const hx::mods::Package*> chosen;
    for (const hx::mods::Package& p : pkgs) chosen.push_back(&p);
    try {
        const hx::mods::BuildSpec spec = hx::mods::mergePackages(chosen);
        const hx::mods::ModBuild b = hx::mods::buildMods(*parts, spec, commit, dirty);
        for (const std::string& l : b.log) std::printf("%s\n", l.c_str());
        const auto line = [](const std::string& name, const std::vector<std::uint8_t>& d) {
            std::printf("%s %zu %s\n", name.c_str(), d.size(), hx::toHex(hx::sha256(d)).c_str());
        };
        line("main_app/m4_app.bin", b.parts.app);
        line("dsp/boot_ldr.bin", b.parts.bootLdr);
        line("dsp/module_meta.bin", b.parts.moduleMeta);
        for (std::size_t i = 0; i < b.parts.modules.modules.size(); ++i)
            if (i == 0 || i >= parts->modules.modules.size() || b.parts.modules.modules[i] != parts->modules.modules[i])
                line(b.parts.moduleFiles[i], b.parts.modules.modules[i]);
        if (!whole) {
            if (!outPath.empty() && !hx::writeFile(outPath, b.parts.app, &error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            return 0;
        }
        const hx::PackedHxf out = hx::packHxf(*fw->hxf, hx::assembleParts(b.parts));
        std::string changed;
        for (std::uint32_t r : out.changed) changed += (changed.empty() ? "" : ", ") + std::to_string(r);
        std::printf("hxf %zu %s (image %zu bytes, modified records: %s)\n", out.file.size(),
                    hx::toHex(hx::sha256(out.file)).c_str(), out.imageSize, changed.c_str());
        if (!outPath.empty()) {
            if (!hx::writeFile(outPath, out.file, &error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("wrote %s\n", outPath.c_str());
        }
    } catch (const hx::mods::BuildError& e) {
        std::fprintf(stderr, "build refused: %s\n", e.what());
        return 1;
    } catch (const hx::FormatError& e) {
        std::fprintf(stderr, "build refused: %s\n", e.what());
        return 1;
    }
    return 0;
}

// oshexed --build STOCK.hxf --mods ID,ID... [-o OUT.hxf]: the user-facing build. Packages by id from
// the download cache (missing ones downloaded, size and SHA-256 checked), the always-in base added, "requires" followed; .ohem paths
// are taken as they are. Writes "<Unit> <version> + mods.hxf" next to the stock file unless -o says otherwise.
int cmdBuild(int argc, char** argv) {
    std::string error, outPath;
    std::vector<std::string> mods;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (a == "--mods" && i + 1 < argc) {
            std::string list = argv[++i];
            for (std::size_t p = 0; p <= list.size();) {
                const std::size_t c = std::min(list.find(',', p), list.size());
                if (c > p) mods.push_back(list.substr(p, c - p));
                p = c + 1;
            }
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    const std::unique_ptr<hx::Firmware> fw = hx::loadFirmware(argv[2], &error);
    if (!fw) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const hx::mods::Stock stock = hx::mods::stockOf(*fw);
    if (!stock.ok || !fw->hxf) {
        std::fprintf(stderr, "%s\n", stock.why.c_str());
        return 1;
    }
    const hx::mods::Store store;
    std::set<std::string> ids;
    std::vector<hx::mods::Package> files;
    for (const std::string& m : mods) {
        if (m.size() > 5 && m.compare(m.size() - 5, 5, ".ohem") == 0) {
            const std::vector<std::uint8_t> bytes = readAll(m);
            const std::string name = hx::utf8FromPath(hx::pathFromUtf8(m).filename());
            std::optional<hx::mods::Package> p = hx::mods::readPackage(bytes, hx::mods::idFromFileName(name), &error);
            if (!p) {
                std::fprintf(stderr, "%s: %s\n", m.c_str(), error.c_str());
                return 1;
            }
            files.push_back(std::move(*p));
        } else {
            ids.insert(m);
        }
    }
    std::vector<hx::mods::Package> pkgs;
    if (files.empty() || !ids.empty()) {
        std::optional<std::vector<hx::mods::Package>> cached = hx::mods::choosePackages(store, stock, ids, &error, true);  // missing ones downloaded
        if (!cached) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        pkgs = std::move(*cached);
    }
    for (hx::mods::Package& p : files) {
        bool have = false;
        for (const hx::mods::Package& q : pkgs) have = have || q.id == p.id;
        if (!have) pkgs.push_back(std::move(p));
    }
    if (outPath.empty()) outPath = hx::mods::defaultOutPath(stock, fw->path);
    try {
        const hx::mods::BuildReport r = hx::mods::buildFirmware(*fw->hxf, stock, fw->path, pkgs, outPath);
        for (const std::string& l : hx::mods::reportLines(r)) std::printf("%s\n", l.c_str());
    } catch (const hx::mods::BuildError& e) {
        std::fprintf(stderr, "build refused: %s\n", e.what());
        return 1;
    }
    return 0;
}

int cmdPresetRoundtrip(const char* path) {
    const auto fw = load(path);
    if (!fw) return 1;
    int same = 0, differ = 0, failed = 0;
    const auto& lib = fw->presets.presets;
    for (std::size_t s = 0; s < lib.size(); ++s) {
        const hx::Preset& p = lib[s];
        if (!p.raw) continue;
        std::string error;
        const auto out = hx::writePresetBlob(p, {}, &error);
        if (!out) {
            if (++failed <= 5) std::printf("%04zu %s: %s\n", s, p.name.c_str(), error.c_str());
            continue;
        }
        if (*out == *p.raw) {
            ++same;
            continue;
        }
        if (++differ <= 5) {
            const std::vector<std::uint8_t>& a = *p.raw;
            std::size_t i = 0;
            while (i < a.size() && i < out->size() && a[i] == (*out)[i]) ++i;
            std::printf("%04zu %s: differs at %#zx (stored %zu bytes, written %zu)\n", s, p.name.c_str(), i, a.size(),
                        out->size());
            auto hex = [](const std::vector<std::uint8_t>& b, std::size_t at) {
                std::string h;
                for (std::size_t k = at > 8 ? at - 8 : 0; k < b.size() && k < at + 16; ++k) {
                    char buf[4];
                    std::snprintf(buf, sizeof buf, "%02x", b[k]);
                    h += buf;
                }
                return h;
            };
            std::printf("  stored:  %s\n  written: %s\n", hex(a, i).c_str(), hex(*out, i).c_str());
        }
    }
    std::printf("presets written back identical: %d, different: %d, failed: %d\n", same, differ, failed);
    return differ || failed ? 1 : 0;
}

// NAM capture -> the IR-slot .wav (= nam2wav.py; check with `nam2wav.py check FILE.nam OUT.wav`).
int cmdConvert(int argc, char** argv) {
    const char* in = argv[2];
    std::string out;
    for (int i = 3; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "-o") == 0) out = argv[i + 1];
    const std::filesystem::path inPath = hx::pathFromUtf8(in);
    if (out.empty()) {
        std::filesystem::path p = inPath;
        p.replace_extension();
        const std::u8string s = p.u8string() + u8" NAM.wav";
        out.assign(s.begin(), s.end());
    }
    std::ifstream f(inPath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        std::fprintf(stderr, "can't read %s\n", in);
        return 1;
    }
    std::string error;
    const std::optional<hx::NamIr> ir = hx::namToIr(text, &error);
    if (!ir) {
        std::fprintf(stderr, "%s: %s\n", in, error.c_str());
        return 1;
    }
    const std::vector<std::uint8_t> wav = hx::irWav(ir->samples);
    std::ofstream o(hx::pathFromUtf8(out), std::ios::binary);
    o.write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
    if (!o) {
        std::fprintf(stderr, "can't write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s: %d weights / 2^%d, level %g dBu, loudness %.2f dB%s\n", out.c_str(), ir->weights,
                ir->exponent, ir->levelDbu, ir->loudness,
                ir->flushed ? (", " + std::to_string(ir->flushed) + " denormal weights set to 0").c_str() : "");
    if (const std::string hint = hx::namGearHint(ir->gearType); !hint.empty()) std::printf("%s\n", hint.c_str());
    return 0;
}

// The module server's offer for a stock firmware (../mod.md "Distribution"): every module of the matching manifest,
// whether it is cached; --fetch [ID...] downloads (and checks) the given ones, or all, into the cache. A dropped
// package: --import FILE.ohem. Reads from the network, writes only the cache folder.
int cmdModules(int argc, char** argv) {
    std::string error;
    const std::unique_ptr<hx::Firmware> fw = hx::loadFirmware(argv[2], &error);
    if (!fw) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const hx::mods::Stock stock = hx::mods::stockOf(*fw);
    if (!stock.ok) {
        std::fprintf(stderr, "%s\n", stock.why.c_str());
        return 1;
    }
    const hx::mods::Store store;
    std::printf("stock %s %s, image %s\ncache %s\n", stock.model.c_str(), stock.version.c_str(), stock.imageSha256.c_str(),
                store.folder(stock.model, stock.version).c_str());
    if (argc >= 5 && std::strcmp(argv[3], "--import") == 0) {
        const std::optional<hx::mods::Package> p = store.import(argv[4], stock, &error);
        if (!p) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("imported %s: %s %s (slot %d)\n", p->id.c_str(), p->name.c_str(), p->version.c_str(), p->slot);
        return 0;
    }
    bool fromCache = false;
    const std::optional<hx::mods::Manifest> m = store.manifest(stock.model, stock.version, &fromCache, &error);
    if (!m) {
        std::fprintf(stderr, "no modules for %s %s: %s\n", stock.model.c_str(), stock.version.c_str(), error.c_str());
        return 1;
    }
    std::printf("manifest %s%s, %s\n", (store.baseUrl() + stock.model + "/" + stock.version + "/manifest.json").c_str(),
                fromCache ? " (cached copy: server not reached)" : "", m->isSigned ? "signed" : "NOT signed");
    if (m->stockSha256 != stock.imageSha256) {
        std::fprintf(stderr, "the manifest is for another %s %s image (%s): no modules offered\n", m->model.c_str(),
                     m->version.c_str(), m->stockSha256.c_str());
        return 1;
    }
    const bool fetch = argc >= 4 && std::strcmp(argv[3], "--fetch") == 0;
    std::vector<std::string> wanted;
    for (int i = 4; fetch && i < argc; ++i) wanted.push_back(argv[i]);
    int failed = 0;
    for (const hx::mods::ModuleRef& r : m->modules) {
        const bool cached = store.cached(*m, r).has_value();
        std::string needs;
        for (const std::string& q : r.needs) needs += (needs.empty() ? "" : ",") + q;
        std::printf("  %-12s v%-4s slot %-3s %7llu B  %-8s %s%s%s\n", r.id.c_str(), r.version.c_str(),
                    r.slot < 0 ? "-" : std::to_string(r.slot).c_str(), (unsigned long long)r.size,
                    cached ? "cached" : "", r.name.c_str(), needs.empty() ? "" : ("  requires " + needs).c_str(),
                    r.always ? "  (always)" : "");
        if (fetch && !cached && (wanted.empty() || std::find(wanted.begin(), wanted.end(), r.id) != wanted.end())) {
            if (const std::optional<hx::mods::Package> p = store.fetch(*m, r, &error))
                std::printf("    fetched, checked (%zu files)\n", p->files.size() + 1);
            else {
                std::printf("    FAILED: %s\n", error.c_str());
                ++failed;
            }
        }
    }
    for (const hx::mods::Package& p : store.extras(&*m, stock.model, stock.version))
        std::printf("  %-12s v%-4s slot %-3d  imported   %s\n", p.id.c_str(), p.version.c_str(), p.slot, p.name.c_str());
    return failed ? 1 : 0;
}

} // namespace

int hxedDumpMain(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "modules") == 0) return cmdModules(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], "convert") == 0) return cmdConvert(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], "parts") == 0) return cmdParts(argv[2]);
    if (argc >= 4 && std::strcmp(argv[1], "m4-build") == 0) return cmdModBuild(argc, argv, false);
    if (argc >= 4 && std::strcmp(argv[1], "mod-build") == 0) return cmdModBuild(argc, argv, true);
    if (argc >= 3 && std::strcmp(argv[1], "build") == 0) return cmdBuild(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], "repack") == 0) return cmdRepack(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], "preset-roundtrip") == 0) return cmdPresetRoundtrip(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "info") == 0) return cmdInfo(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "lists") == 0) return cmdLists(argv[2]);
    if (argc >= 4 && std::strcmp(argv[1], "model") == 0) return cmdModel(argv[2], argv[3]);
    if (argc >= 3 && std::strcmp(argv[1], "presets") == 0) return cmdPresets(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "snapshots") == 0) return cmdSnapshots(argv[2]);
    if (argc >= 4 && std::strcmp(argv[1], "hlx") == 0) return cmdHlx(argv[2], argv[3]);
    if (argc >= 4 && std::strcmp(argv[1], "export-hlx") == 0) return cmdExportHlx(argv[2], argv[3]);
    if (argc >= 2 && std::strcmp(argv[1], "device-info") == 0) return cmdDeviceInfo();
    if (argc >= 2 && std::strcmp(argv[1], "unit") == 0) return cmdUnit();
    if (argc >= 3 && std::strcmp(argv[1], "settings") == 0) return cmdSettings(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], "ir-read") == 0) return cmdIrRead(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "unit-state") == 0) return cmdUnitState();
    if (argc >= 3 && std::strcmp(argv[1], "assume") == 0) return cmdAssume(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "flash") == 0) return cmdFlash(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], "gfx") == 0) return cmdGfx(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "flash-plan") == 0) return cmdFlashPlan(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "flash-sim") == 0)
        return cmdFlashSim(argv[2], argc >= 4 && std::strcmp(argv[3], "--check-slots") == 0);
    std::fprintf(stderr,
                 "usage: oshexed [FILE.hxf | PRESET.hlx]   (the editor)\n"
                 "       oshexed --info|--lists|--presets|--snapshots|--flash-plan|--gfx FILE.hxf\n"
                 "       oshexed --modules STOCK.hxf [--fetch [ID...] | --import FILE.ohem]   (mod packages)\n"
                 "       oshexed --build STOCK.hxf --mods ID,ID|PKG.ohem... [-o OUT.hxf]\n"
                 "                 (modded firmware from downloaded packages; default \"<Unit> <version> + mods.hxf\")\n"
                 "       oshexed --flash FILE.hxf [--fast] [--check-slots] [--keep-presets|--rebuild-presets]\n"
                 "                 [--simulate] [--yes]   (WRITES to the unit; asks first)\n"
                 "       oshexed --flash-sim FILE.hxf [--check-slots]\n"
                 "       oshexed --unit-state | --assume FILE.hxf\n"
                 "       oshexed --device-info   (reads from the connected unit, writes nothing)\n"
                 "       oshexed --unit   (editor protocol, read only: setlists and the current preset)\n"
                 "       oshexed --settings ID...   (read only: global settings as the unit stores them)\n"
                 "       oshexed --ir-read SLOT [-o FILE]   (read only probe: an IR slot's info and samples, 0-based)\n"
                 "       oshexed --convert FILE.nam [-o OUT.wav]   (NAM capture -> IR-slot .wav, as nam2wav.py)\n"
                 "       oshexed --model FILE.hxf TEXT   (models whose name or symbol contains TEXT; exact matches first)\n"
                 "       oshexed --hlx FILE.hxf PRESET.hlx   (an HX Edit preset as the editor reads it)\n"
                 "       oshexed --export-hlx FILE.hxf SLOT|PRESET.hlx   (a preset as an HX Edit file, on stdout)\n"
                 "   developer checks (tools/oracle/compare.py):\n"
                 "       oshexed --parts FILE.hxf   (the records the mod builder edits, as fwedit.py explode splits them)\n"
                 "       oshexed --repack FILE.hxf [-o OUT.hxf]   (split + pack again; must give the same file)\n"
                 "       oshexed --mod-build STOCK.hxf PKG.ohem... [-o OUT.hxf] [--commit 7HEX] [--dirty 0|1]\n"
                 "       oshexed --m4-build STOCK.hxf PKG.ohem... [-o m4_app.bin] [--commit 7HEX] [--dirty 0|1]\n"
                 "                 (a mod build: changed parts with SHA-256; m4-build -o writes only the M4 app)\n"
                 "       oshexed --preset-roundtrip FILE.hxf   (the preset writer's self-check on the factory presets)\n");
    return 2;
}

bool isCliCommand(int argc, char** argv) {
    if (argc < 2 || std::strncmp(argv[1], "--", 2) != 0) return false;
    return std::strcmp(argv[1], "--lumi-licenses") != 0;  // the engine's own: shows the licences window
}

int runCli(int argc, char** argv) {
    std::string command = std::string(argv[1]).substr(2);
    int first = 2;  // argv index of the command's own arguments
    if (command == "dump" && argc >= 3) {
        command = argv[2];
        first = 3;
    }
    std::vector<char*> args = {argv[0], command.data()};
    for (int i = first; i < argc; ++i) args.push_back(argv[i]);
    const int code = hxedDumpMain(int(args.size()), args.data());
    std::fflush(stdout);
    std::fflush(stderr);
    return code;
}

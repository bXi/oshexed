#pragma once

// The DSP side of a mod build (port of newamp.py build_dsp / dsp_replay, ampgraph.py decompile / kernel_from_json /
// module_from_json / model_records, graphdiff.py rebuild, corepack.py, mkchain.py parse / build / Prog / Rec4,
// harness raise_placeholder). Every new model's graph is rebuilt on the user's machine from their own stock model
// plus the package's difference; our DSP code (LDR blocks, module trailer) comes prebuilt from the package.

#include "core/hxfwrite.h"
#include "core/json.h"
#include "core/m4build.h"
#include "core/modspec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hx::mods {

struct DspResult {
    FirmwareParts parts;           // rec 2 / 4 / 5 changed; the app is still stock
    std::vector<M4Model> models;   // spec.models' rec 4 registrations, for buildM4
    std::vector<std::string> log;
};
// Throws BuildError.
DspResult buildDsp(const FirmwareParts& stock, const BuildSpec& spec);

// newamp.py check: the finished build against the stock parts, emulating the patched M4 lookups. Every stock model
// still found by the info lookup and the cost search with its own values, the new ones too; model counts patched;
// no duplicate symbol ids / model numbers; every stock browser list unchanged apart from the added models; no stock
// rec 4 model changed; each new model in an allowed-model list; each new DSP module passes the loader's rules
// (modcheck). Returns the problems (none = OK).
std::vector<std::string> checkBuild(const FirmwareParts& stock, const FirmwareParts& built, const std::vector<M4Model>& models);
// modcheck.py check_words: one DSP module against the rules the DSP loader enforces.
std::vector<std::string> checkModule(const std::vector<std::uint32_t>& words);

// The whole build: DSP, then the M4 app (buildM4), then checkBuild (a failed check throws BuildError: nothing is
// saved). parts.app is the tagged app.
struct ModBuild {
    FirmwareParts parts;
    std::string tag;
    std::vector<std::string> log;            // dspLog, then m4Log, then "checks: OK"
    std::vector<std::string> dspLog, m4Log;  // the two stages' own lines (the build report sorts them)
    std::vector<M4Model> models;             // the new models' registrations (model number, cost, ...)
};
// progress (optional): its own 0..1 (DSP, M4 app, checks).
ModBuild buildMods(const FirmwareParts& stock, const BuildSpec& spec, const std::string& commit = "0000000",
                   bool dirty = true, const BuildProgress& progress = {});

// Pieces, for tests.
namespace dsp {
using Words = std::vector<std::uint32_t>;
// corepack.parse + pack (leaf lists shared by content) + bounds_ok.
Words repackCore(const Words& core);
bool coreBoundsOk(const Words& core);
// mkchain.parse -> ampgraph module_to_json -> module_from_json (-> mkchain.build): stock modules come back unchanged.
Words rebuildModule(const Words& module);
// graphdiff.patch: s with the difference d applied.
json::Value patch(const json::Value* s, const json::Value* d);
// ampgraph.wval / jval: raw word <-> JSON value (float when it looks like a float32).
json::Value wval(std::uint32_t w);
std::uint32_t jval(const json::Value& v);
} // namespace dsp

} // namespace hx::mods

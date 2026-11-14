#pragma once

// The M4 app side of a mod build (port of newamp.py build_m4 / place_code / place_hook, catpatch.py apply in its
// package form, mkchain.py m4_compact_info and fwtag.py tag). Addresses are Helix LT 3.80.0's; every place that
// changes stock code is checked against the user's stock app first (by window hash or by the value it holds).

#include "core/modspec.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hx::mods {

// One new model as the M4 tables need it: its rec 4 registration (the DSP stage makes it) plus the spec.
struct M4Model {
    std::string sym, name;
    std::uint32_t sid = 0;      // rec 4 symbol id
    std::uint32_t index = 0;    // rec 4 entry index
    std::uint32_t knobs = 0, states = 0;
    std::uint32_t modelNo = 0, cost = 0;
    std::uint32_t afterSid = 0;              // stock symbol id of "after" (browser position)
    std::array<std::uint32_t, 14> like{};    // the "like" model's stock rec 4 entry
};

struct M4Result {
    std::vector<std::uint8_t> app;
    std::string tag;             // the build tag written into the version string
    std::uint32_t costTable = 0; // where the cost table moved
    std::vector<std::string> log;
};

// models: spec.models' registrations, in the same order. commit / dirty go into the build tag (MODTAG.md).
// Throws BuildError.
M4Result buildM4(const std::vector<std::uint8_t>& stockApp, const BuildSpec& spec, const std::vector<M4Model>& models,
                 const std::string& commit = "0000000", bool dirty = true);

// The 16-byte-aligned window (>= 16 bytes) a stock check hashes (newamp.window).
std::pair<std::uint32_t, std::uint32_t> stockWindow(std::uint32_t at, std::uint32_t n);

// Thumb-2 BL (link) or B.W at site to target; and the reverse (nullopt: not that instruction).
std::array<std::uint8_t, 4> encodeBranch(std::uint32_t site, std::uint32_t target, bool link);
std::optional<std::uint32_t> branchTarget(const std::uint8_t* four, std::uint32_t site, bool link);

} // namespace hx::mods

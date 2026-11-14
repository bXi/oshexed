#pragma once

// The chosen mod packages merged into one build description (what tools/modpkg.py `merge` makes for newamp).
// Order is fixed: the base first, then the slots ascending; within a package, its own order.

#include "core/json.h"
#include "core/modpkg.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hx::mods {

// Any reason a build can't go ahead (a failed stock check, packages that collide, ...). Nothing is written then.
struct BuildError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// How far a build is: the share done (0..1) and what it's doing now ("Packing the firmware"). Called on the thread
// doing the build.
using BuildProgress = std::function<void(double done, const std::string& step)>;

// A place our code is hooked in (newamp place_hook). Stock code is checked by the SHA-256 of its 16-byte window
// (Patch) or by the address it calls / holds (Bl, Bw, Literal): never by its bytes.
struct Hook {
    enum class Kind { Patch, Bl, Bw, Literal };
    Kind kind = Kind::Patch;
    std::string symbol;   // our function; "" = the blob's first byte (origin)
    std::uint32_t site = 0;
    bool link = false;    // Patch: BL instead of B.W
    std::uint32_t len = 0;       // Patch: stock bytes replaced (BL / B.W + NOPs)
    std::string sha256;          // Patch: stock window hash
    std::uint32_t expect = 0;    // Bl / Bw: stock call target; Literal: stock value
};

struct CodePart {
    std::string name;  // "cat_rows.c"
    std::vector<std::uint8_t> blob;
    std::uint32_t origin = 0;
    std::map<std::string, std::uint32_t> symbols;
    std::vector<Hook> hooks;
    std::vector<std::pair<std::string, std::string>> words;  // (data symbol, model sym): set to its rec 4 entry index
};

// Custom browser category (catpatch.py, package form).
struct CodePatch {
    std::uint32_t at = 0, len = 0;
    std::string sha256;
    std::vector<std::uint8_t> bytes;  // ours (assembled on our side)
};
struct MovedTable {
    std::string name;  // "names", "listptr", ...: also its allocation name "catpatch:table:<name>"
    std::uint32_t va = 0, entrySize = 0;
    std::vector<std::pair<std::uint32_t, std::int32_t>> lits;  // (literal VA, offset of its value from the table)
};
struct CustomSub {
    std::string name;
    std::vector<std::string> models;  // syms; those not in the build are left out
};
struct CustomCategory {
    std::string name, shortName;
    int labelLike = 12;
    std::vector<CustomSub> subs;
    std::optional<std::uint32_t> rgb, rgbDim;
    std::vector<CodePatch> codePatches;
    std::vector<MovedTable> tables;
    std::uint32_t ramArray = 0, ramPaths = 0;
    std::vector<std::pair<std::uint32_t, std::int32_t>> ramLits;
};

struct ModelSpec {
    std::string sym, name, like, after;  // after: list position anchor (default: like)
    std::string graph;                   // path inside its package
    std::uint32_t modelNo = 0;
    std::optional<std::uint32_t> cost;   // else the graph's m4.cost
    const Package* package = nullptr;
};

struct BuildSpec {
    std::vector<const Package*> packages;  // in build order
    std::vector<ModelSpec> models;
    std::optional<CustomCategory> custom;
    std::vector<CodePart> code;
    std::map<std::string, std::uint32_t> layout;  // fixed allocations: name -> address
    std::uint64_t modBits = 0;                    // MODTAG slots of the packages
    // DSP parts, read by the DSP stage (with the package that holds their blobs).
    const json::Value* namblock = nullptr;
    const json::Value* polymidi = nullptr;
    const Package* namblockPackage = nullptr;
    const Package* polymidiPackage = nullptr;
    std::vector<const json::Value*> harness;
};

// Checks "requires", duplicate parts and layout claims; reads every section. Throws BuildError.
BuildSpec mergePackages(std::vector<const Package*> packages);

// A number written as JSON number or as text ("0x280c9aba", "1234"), as Python's int(str(x), 0).
std::int64_t number(const json::Value* v, const char* what);

} // namespace hx::mods

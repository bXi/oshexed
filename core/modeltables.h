#pragma once

#include "core/dspmeta.h"
#include "core/m4app.h"

#include <optional>
#include <string>
#include <vector>

namespace hx {

// M4 app tables for the model browser and DSP costs (NOTES.md "Model registration chain" and
// "Model tables"; tools/models.py). The addresses are for v3.80 Helix LT only.

constexpr std::uint32_t kNoCab = 0xFFFFFFFF;

struct BrowserEntry {
    std::uint32_t category = 0;  // word 0: really the block kind (= preset key 9: 1 effect, 8 delay, 17 amp, 19 IR, ...)
    std::uint32_t symId = 0;
    std::uint32_t cabSymId = kNoCab;  // amp+cab lists: the linked cab
    std::uint16_t modelNo = 0;
    std::string name;
};

struct BrowserList {
    int category = 0;
    int sub = 0;
    std::uint32_t va = 0;
    std::vector<BrowserEntry> entries;
    std::string label;  // from rec 4: "<category> / <subcategory>", filled by labelLists()
    std::string subName;  // the firmware's own sub-list name (M4 sub-list name table), "" if none
};

struct CostEntry {
    std::uint32_t symId = 0;
    std::uint16_t modelNo = 0;
    std::uint32_t cost = 0;
};

// A browser category's label (M4 label table, 44 B entries: name, short name, colours).
struct CategoryLabel {
    std::string name, shortName;
    std::uint32_t rgb = 0, rgbDim = 0;
};

// Our mod builds move these tables (cost table grown, category tables with a 20th entry "Custom"): they're read
// through the code's own pointers to them, so stock and mod builds read the same way.
struct ModelTables {
    std::vector<BrowserList> lists;
    std::vector<CostEntry> costs;
    int categories = 19;                 // 20 in our builds: 19 = the Custom category
    std::vector<CategoryLabel> labels;   // one per category
    const CostEntry* costBySymId(std::uint32_t symId) const;
};

std::optional<ModelTables> readModelTables(const AppImage& app, std::string* error = nullptr);

// Names each list after the most common rec 4 category and subcategory of its models. Lists
// that would get the same label are told apart: "+ cab" when they link cabs, else a number.
void labelLists(ModelTables& tables, const DspMeta& meta);

} // namespace hx

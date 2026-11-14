#include "core/modeltables.h"

#include "core/text.h"

#include <map>

namespace hx {

namespace {

// v3.80 LT addresses (tools/models.py).
constexpr std::uint32_t kNCost = 0x337;
constexpr std::uint32_t kCatNSub = 0x2813ae18;
constexpr int kNCat = 19;
// Literals in the code that hold the tables' addresses (newamp COST_LITS / COST_END_LIT, catpatch TABLES): a mod
// build re-points them at the moved tables.
constexpr std::uint32_t kCostLit = 0x2813b708, kCostEndLit = 0x2813b750;
constexpr std::uint32_t kNSubLit = 0x2812d908, kPtrsLit = 0x2812d7ec, kCntsLit = 0x2812d918, kLabelLit = 0x280fef60,
                        kSubNamesLit = 0x2812d7b4;
constexpr std::uint32_t kMaxAdded = 64;    // new models a build may add (sanity limit)
constexpr std::uint32_t kMaxSubs = 4;     // 4 pointer/count slots per category
constexpr std::uint32_t kMaxEntries = 1024;  // sanity limit; the largest stock list has 94
constexpr std::size_t kEntrySize = 48;

std::string mostCommon(const std::map<std::string, int>& counts) {
    std::string best;
    int n = 0;
    for (const auto& [k, v] : counts)
        if (v > n) {
            best = k;
            n = v;
        }
    return best;
}

} // namespace

const CostEntry* ModelTables::costBySymId(std::uint32_t symId) const {
    for (const CostEntry& c : costs)
        if (c.symId == symId) return &c;
    return nullptr;
}

std::optional<ModelTables> readModelTables(const AppImage& app, std::string* error) {
    try {
        ModelTables t;
        const std::uint32_t cost = app.u32(kCostLit), costEnd = app.u32(kCostEndLit);
        if (costEnd < cost || (costEnd - cost) % 12 || (costEnd - cost) / 12 < kNCost || (costEnd - cost) / 12 > kNCost + kMaxAdded)
            throw FormatError("cost table pointers");
        const std::uint32_t nCost = (costEnd - cost) / 12;
        t.costs.reserve(nCost);
        for (std::uint32_t i = 0; i < nCost; ++i) {
            const Bytes r = app.at(cost + i * 12, 12);
            t.costs.push_back({readLE32(r, 0), readLE16(r, 4), readLE32(r, 8)});
        }
        const std::uint32_t nsubVa = app.u32(kNSubLit), ptrs = app.u32(kPtrsLit), cnts = app.u32(kCntsLit);
        const std::uint32_t labels = app.u32(kLabelLit), subnames = app.u32(kSubNamesLit);
        t.categories = nsubVa == kCatNSub ? kNCat : kNCat + 1;  // moved: our Custom category is the 20th
        for (int c = 0; c < t.categories; ++c) {
            const Bytes l = app.at(labels + std::uint32_t(c) * 44, 44);
            t.labels.push_back({cString(l.subspan(0, 17)), cString(l.subspan(17, 11)), readLE32(l, 28), readLE32(l, 32)});
        }
        for (int c = 0; c < t.categories; ++c) {
            const std::uint32_t nSub = app.u32(nsubVa + std::uint32_t(c) * 4);
            if (nSub > kMaxSubs) throw FormatError("browser category " + std::to_string(c) + ": bad sub-list count");
            for (std::uint32_t s = 0; s < nSub; ++s) {
                BrowserList list;
                list.category = c;
                list.sub = int(s);
                list.va = app.u32(ptrs + std::uint32_t(c) * 16 + s * 4);
                list.subName = cString(app.at(subnames + std::uint32_t(c) * 104 + s * 26, 26));
                const std::uint32_t n = app.u32(cnts + std::uint32_t(c) * 16 + s * 4);
                if (n > kMaxEntries) throw FormatError("browser list too long; is this v3.80 LT firmware?");
                for (std::uint32_t k = 0; k < n; ++k) {
                    const Bytes r = app.at(list.va + k * kEntrySize, kEntrySize);
                    list.entries.push_back(
                        {readLE32(r, 0), readLE32(r, 4), readLE32(r, 8), readLE16(r, 12), cString(r.subspan(14, 26))});
                }
                t.lists.push_back(std::move(list));
            }
        }
        return t;
    } catch (const FormatError& e) {
        if (error) *error = std::string(e.what()) + " (the browser tables are mapped for v3.80 LT only)";
        return std::nullopt;
    }
}

void labelLists(ModelTables& tables, const DspMeta& meta) {
    std::map<std::string, int> seen;
    for (BrowserList& list : tables.lists) {
        std::map<std::string, int> cats, subs;
        bool cabs = false;
        for (const BrowserEntry& e : list.entries) {
            if (e.cabSymId != kNoCab) cabs = true;
            const MetaEntry* m = meta.bySymId(e.symId);
            if (!m) continue;
            ++cats[meta.attr(m->modelAttrs, "category")];
            ++subs[meta.attr(m->modelAttrs, "subcategory")];
        }
        std::string label = mostCommon(cats);
        if (label.empty()) label = "category " + std::to_string(list.category);
        if (const std::string sub = mostCommon(subs); !sub.empty()) label += " / " + sub;
        if (cabs) label += " + cab";
        if (const int n = ++seen[label]; n > 1) label += " (" + std::to_string(n) + ")";
        list.label = std::move(label);
    }
}

} // namespace hx

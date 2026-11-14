#pragma once

#include "core/bytes.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hx {

// rec 4, DSP module metadata (NOTES.md "Model tables (rec 4 + M4)"; tools/rec4.py).

enum class ParamType : std::uint32_t { Switch = 1, List = 2, Float = 3, Command = 6 };
const char* paramTypeName(std::uint32_t type);

struct MetaAttr {
    std::uint32_t nameSym = 0;
    std::uint32_t type = 0;     // 7 = value is a symbol
    std::uint32_t value = 0;
};

// One knob, state param or output param. min/max/def are raw words: floats when type == 3.
// State params have no range (min = max = def = 0).
struct MetaParam {
    std::uint32_t nameSym = 0;
    std::uint32_t type = 0;
    std::uint32_t number = 0;   // knobNo for knobs, idx for state/output params
    std::uint32_t min = 0, max = 0, def = 0;
    std::vector<MetaAttr> attrs;
};

struct MetaEntry {
    std::uint32_t index = 0;    // position in the entry table
    std::uint32_t symId = 0;    // symbol of the HD2_* name
    std::uint32_t rec5 = 0;     // rec 5 resource (the DSP module)
    std::vector<MetaAttr> modelAttrs;  // category, subcategory, displayName
    std::vector<MetaParam> knobs, states, outputs;
};

class DspMeta {
public:
    static std::optional<DspMeta> parse(Bytes payload, std::string* error = nullptr);

    std::size_t symbolCount() const { return symbols_.size(); }
    // "sym<N>" for an index outside the table, as rec4.py prints it.
    std::string symbol(std::uint32_t index) const;
    const std::vector<MetaEntry>& entries() const { return entries_; }
    const MetaEntry* bySymId(std::uint32_t symId) const;

    // The value of the attribute called `name`, as text (symbol name or number); "" if absent.
    std::string attr(const std::vector<MetaAttr>& attrs, std::string_view name) const;
    std::string displayName(const MetaEntry& e) const;
    std::string displayName(const MetaParam& p) const;

private:
    std::vector<std::string> symbols_;
    std::vector<MetaEntry> entries_;
    std::unordered_map<std::uint32_t, std::size_t> bySym_;
};

// A param value as text: floats with up to 6 significant digits (rec4.py's %.6g), others as integers.
std::string formatParamValue(std::uint32_t type, std::uint32_t raw);

} // namespace hx

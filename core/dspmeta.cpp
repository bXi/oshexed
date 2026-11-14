#include "core/dspmeta.h"

#include "core/text.h"

#include <bit>
#include <cstdio>

namespace hx {

namespace {

constexpr std::uint32_t kMetaMagic = 0x4023245E;
constexpr std::uint32_t kAttrSymbol = 7;

std::vector<MetaAttr> readAttrs(Bytes m, std::uint32_t count, std::uint32_t off) {
    std::vector<MetaAttr> out;
    out.reserve(count);
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::size_t o = std::size_t(off) + std::size_t(k) * 12;
        out.push_back({le32(m, o, "attribute"), le32(m, o + 4, "attribute"), le32(m, o + 8, "attribute")});
    }
    return out;
}

// stride/fields: knobs 0x20 [nAttr, attrOff, nameSym, type, knobNo, min, max, default],
// states 0x14 [.. idx], outputs 0x1c [.. idx, min, max].
std::vector<MetaParam> readParams(Bytes m, std::uint32_t count, std::uint32_t off, std::size_t stride, int words) {
    std::vector<MetaParam> out;
    out.reserve(count);
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::size_t o = std::size_t(off) + k * stride;
        need(m, o, std::size_t(words) * 4, "param");
        MetaParam p;
        p.nameSym = readLE32(m, o + 8);
        p.type = readLE32(m, o + 12);
        p.number = readLE32(m, o + 16);
        if (words >= 7) {
            p.min = readLE32(m, o + 20);
            p.max = readLE32(m, o + 24);
        }
        if (words >= 8) p.def = readLE32(m, o + 28);
        p.attrs = readAttrs(m, readLE32(m, o), readLE32(m, o + 4));
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace

const char* paramTypeName(std::uint32_t type) {
    switch (type) {
    case 1: return "switch";
    case 2: return "list";
    case 3: return "float";
    case 6: return "command";
    default: return "?";
    }
}

std::string formatParamValue(std::uint32_t type, std::uint32_t raw) {
    if (type == std::uint32_t(ParamType::Float)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.6g", double(std::bit_cast<float>(raw)));
        return buf;
    }
    return std::to_string(raw);
}

std::optional<DspMeta> DspMeta::parse(Bytes m, std::string* error) {
    try {
        if (le32(m, 0, "rec 4 header") != kMetaMagic) throw FormatError("rec 4: bad magic");
        need(m, 0, 40, "rec 4 header");
        const std::uint32_t symOff = readLE32(m, 12), entOff = readLE32(m, 24), nEnt = readLE32(m, 28);

        DspMeta meta;
        const std::uint32_t nSym = le32(m, symOff, "symbol table");
        const std::size_t table = std::size_t(symOff) + 8;
        const std::size_t pool = table + std::size_t(nSym) * 12;
        need(m, table, std::size_t(nSym) * 12, "symbol table");
        meta.symbols_.reserve(nSym);
        for (std::uint32_t i = 0; i < nSym; ++i) {
            const std::uint32_t o = readLE32(m, table + i * 12), n = readLE32(m, table + i * 12 + 8);
            meta.symbols_.push_back(latin1ToUtf8(slice(m, pool + o, n, "symbol")));
        }

        meta.entries_.reserve(nEnt);
        for (std::uint32_t i = 0; i < nEnt; ++i) {
            const std::size_t o = std::size_t(entOff) + std::size_t(i) * 0x38;
            need(m, o, 0x38, "rec 4 entry");
            std::uint32_t w[14];
            for (int k = 0; k < 14; ++k) w[k] = readLE32(m, o + std::size_t(k) * 4);
            MetaEntry e;
            e.index = i;
            e.symId = w[2];
            e.rec5 = w[13];
            e.modelAttrs = readAttrs(m, w[0], w[1]);
            e.states = readParams(m, w[6], w[10], 0x14, 5);
            e.knobs = readParams(m, w[7], w[11], 0x20, 8);
            e.outputs = readParams(m, w[8], w[12], 0x1c, 7);
            meta.bySym_.emplace(e.symId, meta.entries_.size());  // first match wins, like the device's scan
            meta.entries_.push_back(std::move(e));
        }
        return meta;
    } catch (const FormatError& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::string DspMeta::symbol(std::uint32_t index) const {
    return index < symbols_.size() ? symbols_[index] : "sym" + std::to_string(index);
}

const MetaEntry* DspMeta::bySymId(std::uint32_t symId) const {
    const auto it = bySym_.find(symId);
    return it == bySym_.end() ? nullptr : &entries_[it->second];
}

std::string DspMeta::attr(const std::vector<MetaAttr>& attrs, std::string_view name) const {
    for (const MetaAttr& a : attrs)
        if (symbol(a.nameSym) == name) return a.type == kAttrSymbol ? symbol(a.value) : std::to_string(a.value);
    return {};
}

std::string DspMeta::displayName(const MetaEntry& e) const {
    std::string n = attr(e.modelAttrs, "displayName");
    return n.empty() ? symbol(e.symId) : n;
}

std::string DspMeta::displayName(const MetaParam& p) const {
    std::string n = attr(p.attrs, "displayName");
    return n.empty() ? symbol(p.nameSym) : n;
}

} // namespace hx

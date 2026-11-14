#include "core/m4build.h"

#include "core/buildtag.h"
#include "core/bytes.h"
#include "core/checksums.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace hx::mods {

namespace {

// Helix LT 3.80.0 M4 app (NOTES.md "Growing the M4 model tables", mkchain.py / newamp.py constants).
constexpr std::uint32_t kAppSeg = 0x14308;  // file offset of VA 0x28000000
constexpr std::uint32_t kN = 0x337;         // stock entries in the info and cost tables
constexpr std::uint32_t kCost = 0x2813BBA4, kCostEnd = kCost + 12 * kN;
constexpr std::uint32_t kCostLits[] = {0x2813B708, 0x2813B754, 0x2813B76C, 0x2813B860, 0x2813B918};
constexpr std::uint32_t kCostEndLit = 0x2813B750;
constexpr std::uint32_t kInfo = 0x280E0828, kInfoPtrLit = 0x280E0824;
constexpr std::uint32_t kCatPtrs = 0x2813ABB8, kCatCnts = 0x2813ACE8, kCatSubs = 0x2813AE18;
constexpr int kNcat = 19;
constexpr std::uint32_t kTagVa = 0x280C4684, kTagRoom = 36;
constexpr const char* kTagWindow = "218d057810cef19d37c11a16833c612c6ea09769ce430c286ecba1b09effc795";

// Model-count instructions (movw rd, #count or #count - 1): site, register, minus one, stock window hash.
struct CountPatch {
    std::uint32_t va;
    int rd;
    bool last;
    const char* sha256;
};
constexpr CountPatch kCountPatches[] = {
    {0x280E0784, 12, false, "bee19ada660e538e174aa3d880b23d3ff12306513102b1eae24a1fe007a20675"},  // info lookup loop
    {0x2813B740, 1, true, "045ad251db6cb874271c376944fdb85dd4eaceec4bbacd44de98b8a42d59d560"},    // cost sort
    {0x2813B758, 0, false, "87c2c5cbd22aa61ba63b7df4f986dbfe6d12eb4c114a76f4bbe480d4501bcd74"},   // cost count
    {0x2813B7CA, 12, true, "282d3d5ba3e6c8f1c985870b8e5f42ed931087db65b5901a2d5e3c0fedb387d4"},   // bsearch -> cost
    {0x2813B872, 12, true, "d5453ccb0cf9d96511a2648e748313dfd987dccd8ea46ef75383cca97c670d2d"},   // bsearch -> modelNo
};

// The info lookup (FUN_280e077c) rewritten for 20-byte entries (mkchain.INFO_FN_PATCH; our instructions).
struct InfoPatch {
    std::uint32_t va;
    std::vector<std::uint8_t> bytes;
    const char* sha256;
};
const InfoPatch kInfoPatches[] = {
    {0x280E0796, {0x03, 0xf1, 0x05, 0x03}, "f07e4289c456acd2b796bf8bb27efc12b32eb884343840c61af9e4703ead1e9c"},  // add.w r3, r3, #5
    {0x280E079C, {0x14, 0x23}, "f07e4289c456acd2b796bf8bb27efc12b32eb884343840c61af9e4703ead1e9c"},              // movs r3, #20
    {0x280E07A4, {0x6b, 0x68}, "a73c8c6e2710f877f59fff71bd4879a2a7ed530d8bb660ddbaaa2ccea8ef612e"},              // ldr r3, [r5, #4]
    {0x280E07A8, {0xab, 0x68}, "a73c8c6e2710f877f59fff71bd4879a2a7ed530d8bb660ddbaaa2ccea8ef612e"},              // ldr r3, [r5, #8]
    {0x280E07AC, {0x2b, 0x7c}, "a73c8c6e2710f877f59fff71bd4879a2a7ed530d8bb660ddbaaa2ccea8ef612e"},              // ldrb r3, [r5, #16]
    {0x280E07B0, {0x6b, 0x7c}, "c6aa8a3a7200f41188bfdc71a3544be7891531b6d12a460095de39505d718008"},              // ldrb r3, [r5, #17]
    {0x280E07B4, {0xab, 0x7c, 0x00, 0xbf}, "c6aa8a3a7200f41188bfdc71a3544be7891531b6d12a460095de39505d718008"},  // ldrb r3, [r5, #18]; nop
    {0x280E07BA, {0xeb, 0x68}, "c6aa8a3a7200f41188bfdc71a3544be7891531b6d12a460095de39505d718008"},              // ldr r3, [r5, #12]
    {0x280E07BE, {0x2b, 0x7c}, "c6aa8a3a7200f41188bfdc71a3544be7891531b6d12a460095de39505d718008"},              // ldrb r3, [r5, #16]
    {0x280E07D6, {0x6b, 0x7c}, "d100f4b120617395dcb5e6f8e088122864d488754c8ce721e5b3edcb842463d0"},              // ldrb r3, [r5, #17]
};

std::string hex32(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%#x", unsigned(v));
    return b;
}

// Names are latin-1 on the unit (Python's .encode("latin1")); JSON gives UTF-8.
std::string latin1(const std::string& utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const unsigned char c = utf8[i];
        if (c < 0x80) out += char(c);
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size() && c <= 0xC3) out += char(((c & 0x1F) << 6) | (utf8[++i] & 0x3F));
        else throw BuildError("name \"" + utf8 + "\" has characters the unit can't show");
    }
    return out;
}

std::vector<std::uint8_t> padded(const std::string& s, std::size_t n) {
    std::vector<std::uint8_t> out(s.begin(), s.end());
    out.resize(n, 0);
    return out;
}

struct App {
    std::vector<std::uint8_t> b;
    std::size_t off(std::uint32_t va, std::size_t n) const {
        const std::int64_t o = std::int64_t(kAppSeg) + std::int64_t(va) - 0x28000000;
        if (o < 0 || std::uint64_t(o) + n > b.size()) throw BuildError("M4 address " + hex32(va) + " outside the app");
        return std::size_t(o);
    }
    std::uint32_t u32(std::uint32_t va) const { return readLE32(b, off(va, 4)); }
    std::vector<std::uint8_t> raw(std::uint32_t va, std::size_t n) const {
        const std::size_t o = off(va, n);
        return {b.begin() + o, b.begin() + o + n};
    }
    void put(std::uint32_t va, const std::vector<std::uint8_t>& d) { std::copy(d.begin(), d.end(), b.begin() + off(va, d.size())); }
    void put32(std::uint32_t va, std::uint32_t v) { put(va, {std::uint8_t(v), std::uint8_t(v >> 8), std::uint8_t(v >> 16), std::uint8_t(v >> 24)}); }
};

void le32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(std::uint8_t(v >> (8 * k)));
}
void setLE32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + k] = std::uint8_t(v >> (8 * k));
}

struct Region {
    std::uint32_t start, end;
};

class Builder {
public:
    Builder(const std::vector<std::uint8_t>& stock, const BuildSpec& spec, const std::vector<M4Model>& models)
        : stock_{stock}, a_{stock}, spec_(spec), models_(models) {}

    M4Result run(const std::string& commit, bool dirty);

private:
    App stock_, a_;
    const BuildSpec& spec_;
    const std::vector<M4Model>& models_;
    std::vector<Region> regions_;
    M4Result res_;

    void log(std::string s) { res_.log.push_back(std::move(s)); }
    // newamp.expect_hash: the stock window around [at, at + n), always from the untouched stock app.
    void expectStock(std::uint32_t at, std::uint32_t n, const std::string& sha, const std::string& what) const {
        const auto [lo, len] = stockWindow(at, n);
        if (toHex(sha256(stock_.raw(lo, len))) != sha) throw BuildError(what + " at " + hex32(at) + ": not the stock code");
    }
    std::uint32_t take(std::uint32_t size, const std::string& name, std::uint32_t align);
    void tables();
    void custom();
    void placeCode(const CodePart& c);
    void hook(const Hook& h, std::uint32_t target);
    void tag(const std::string& commit, bool dirty);
};

std::uint32_t Builder::take(std::uint32_t size, const std::string& name, std::uint32_t align) {
    const auto up = [align](std::uint32_t v) { return (v + align - 1) & ~(align - 1); };
    const auto fixed = spec_.layout.find(name);
    std::uint32_t at = 0;
    Region* reg = nullptr;
    if (fixed != spec_.layout.end()) {
        at = fixed->second;
        for (Region& r : regions_)
            if (r.start <= at && std::uint64_t(at) + size <= r.end) {
                reg = &r;
                break;
            }
        if (!reg || at < up(reg->start))
            throw BuildError("fixed layout: " + name + " at " + hex32(at) + " (" + std::to_string(size) + " bytes) is not free");
    } else {
        for (Region& r : regions_)
            if (std::uint64_t(up(r.start)) + size <= r.end) {
                reg = &r;
                break;
            }
        if (!reg) throw BuildError("out of M4 space for " + name);
        at = up(reg->start);
    }
    reg->start = at + size;
    return at;
}

// newamp.build_m4: info table compacted and grown, cost table moved and grown, counts patched, browser lists grown.
void Builder::tables() {
    const std::uint32_t n = kN + std::uint32_t(models_.size());
    if (n >= 0x800) throw BuildError("too many models");
    for (const CountPatch& p : kCountPatches) expectStock(p.va, 4, p.sha256, "model count");
    for (std::uint32_t va : kCostLits)
        if (a_.u32(va) != kCost) throw BuildError("cost table literal " + hex32(va) + " is not stock");
    if (a_.u32(kCostEndLit) != kCostEnd) throw BuildError("cost table end literal is not stock");
    for (std::size_t off = 0; off + 4 <= a_.b.size(); off += 4) {  // nothing else may point into the cost table
        const std::uint32_t v = readLE32(a_.b, off);
        const std::int64_t va = std::int64_t(off) - kAppSeg + 0x28000000;
        if (v >= kCost && v < kCostEnd && std::find(std::begin(kCostLits), std::end(kCostLits), va) == std::end(kCostLits))
            throw BuildError("unexpected cost-table reference at " + hex32(std::uint32_t(va)));
    }
    std::set<std::uint32_t> usedNos;
    for (std::uint32_t i = 0; i < kN; ++i) usedNos.insert(readLE16(a_.b, a_.off(kCost + 12 * i + 4, 2)));
    for (const M4Model& m : models_)
        if (m.modelNo >= 0x341 || usedNos.count(m.modelNo))
            throw BuildError(m.sym + ": model number " + std::to_string(m.modelNo) + " is not free");
    const std::vector<std::uint8_t> cost = a_.raw(kCost, 12 * kN);

    // info: 44 -> 20 bytes per entry (symId, @0x18, @0x1c, @0x28, b@0x15, b@0x16, b@0x24, pad), then the new ones
    if (a_.u32(kInfoPtrLit) != kInfo) throw BuildError("info table literal is not stock");
    for (const InfoPatch& p : kInfoPatches) expectStock(p.va, std::uint32_t(p.bytes.size()), p.sha256, "info lookup");
    std::vector<std::uint8_t> compact;
    for (std::uint32_t i = 0; i < kN; ++i) {
        const std::vector<std::uint8_t> e = a_.raw(kInfo + 44 * i, 44);
        for (std::size_t at : {0x00, 0x18, 0x1C, 0x28}) le32(compact, readLE32(e, at));
        compact.insert(compact.end(), {e[0x15], e[0x16], e[0x24], 0});
    }
    const std::uint32_t freeEnd = kInfo + 44 * kN;
    std::vector<std::uint8_t> infoArea = compact;
    infoArea.resize(freeEnd - kInfo, 0);
    a_.put(kInfo, infoArea);
    for (const InfoPatch& p : kInfoPatches) a_.put(p.va, p.bytes);
    for (std::size_t j = 0; j < models_.size(); ++j) {
        const M4Model& m = models_[j];
        std::vector<std::uint8_t> f;
        for (std::uint32_t i = 0; i < kN && f.empty(); ++i)
            if (readLE32(compact, 20 * i) == m.like[2]) f.assign(compact.begin() + 20 * i, compact.begin() + 20 * i + 20);
        if (f.empty()) throw BuildError(m.sym + ": its \"like\" model has no info entry");
        setLE32(f, 0, m.sid);
        setLE32(f, 4, m.knobs);
        setLE32(f, 8, m.states);
        setLE32(f, 12, m.like[8]);  // outputs come from "like"
        a_.put(kInfo + 20 * (kN + std::uint32_t(j)), f);
    }
    std::uint32_t free = (kInfo + 20 * n + 3) & ~3u;

    // cost: moved behind the info table and grown
    std::vector<std::uint8_t> newCost = cost;
    for (const M4Model& m : models_) {
        le32(newCost, m.sid);
        newCost.insert(newCost.end(), {std::uint8_t(m.modelNo), std::uint8_t(m.modelNo >> 8), 0, 0});
        le32(newCost, m.cost);
    }
    const std::uint32_t cva = free;
    a_.put(cva, newCost);
    free += std::uint32_t(newCost.size());
    for (std::uint32_t va : kCostLits) a_.put32(va, cva);
    a_.put32(kCostEndLit, cva + std::uint32_t(newCost.size()));
    for (const CountPatch& p : kCountPatches) {
        const std::uint32_t v = p.last ? n - 1 : n;
        a_.put(p.va, {0x40, 0xF2, std::uint8_t(v & 0xFF), std::uint8_t(((v >> 8) & 7) << 4 | p.rd)});  // movw rd, #v
    }
    a_.put(kCost, std::vector<std::uint8_t>(12 * kN, 0));  // the old table's spot is free now
    regions_ = {{free, freeEnd}, {kCost, kCostEnd}};
    res_.costTable = cva;
    log("M4: " + std::to_string(n) + " table entries, cost table at " + hex32(cva));

    // browser lists: each new model after its anchor in every list that holds its "like" (Custom ones: custom())
    std::set<std::string> customSyms;
    if (spec_.custom)
        for (const CustomSub& s : spec_.custom->subs) customSyms.insert(s.models.begin(), s.models.end());
    for (int cat = 0; cat < kNcat; ++cat)
        for (std::uint32_t sub = 0; sub < a_.u32(kCatSubs + 4 * cat); ++sub) {
            const std::uint32_t pva = kCatPtrs + 16 * cat + 4 * sub, nva = kCatCnts + 16 * cat + 4 * sub;
            if (!a_.u32(pva)) continue;
            const std::uint32_t p = a_.u32(pva), cnt = a_.u32(nva);
            std::vector<std::vector<std::uint8_t>> list;
            std::vector<std::uint32_t> sids;
            for (std::uint32_t k = 0; k < cnt; ++k) {
                list.push_back(a_.raw(p + 48 * k, 48));
                sids.push_back(readLE32(list.back(), 4));
            }
            std::vector<const M4Model*> adds;
            for (const M4Model& m : models_)
                if (std::find(sids.begin(), sids.end(), m.like[2]) != sids.end() && !customSyms.count(m.sym)) adds.push_back(&m);
            if (adds.empty()) continue;
            std::set<std::uint32_t> addSids;
            for (const M4Model* m : adds) addSids.insert(m->sid);
            for (const M4Model* m : adds) {
                std::vector<std::uint8_t> src = list[std::find(sids.begin(), sids.end(), m->like[2]) - sids.begin()];
                setLE32(src, 4, m->sid);
                if (readLE32(src, 8) == 0xFFFFFFFF) {  // Amp+Cab entries keep the cab's modelNo
                    src[12] = std::uint8_t(m->modelNo);
                    src[13] = std::uint8_t(m->modelNo >> 8);
                }
                const std::vector<std::uint8_t> name = padded(latin1(m->name), 26);
                std::copy(name.begin(), name.end(), src.begin() + 14);
                const bool anchorIn = std::find(sids.begin(), sids.end(), m->afterSid) != sids.end();
                const std::uint32_t anchor = anchorIn ? m->afterSid : m->like[2];
                std::size_t k = sids.size() - (std::find(sids.rbegin(), sids.rend(), anchor) - sids.rbegin());
                while (k < sids.size() && addSids.count(sids[k])) ++k;
                list.insert(list.begin() + std::ptrdiff_t(k), src);
                sids.insert(sids.begin() + std::ptrdiff_t(k), m->sid);
            }
            std::vector<std::uint8_t> blob;
            for (const auto& e : list) blob.insert(blob.end(), e.begin(), e.end());
            Region* reg = nullptr;
            for (Region& r : regions_)
                if (std::uint64_t(r.start) + blob.size() <= r.end) {
                    reg = &r;
                    break;
                }
            if (!reg) throw BuildError("out of M4 space for the grown browser lists");
            a_.put(reg->start, blob);
            a_.put32(pva, reg->start);
            a_.put32(nva, std::uint32_t(list.size()));
            log("browser list " + std::to_string(cat) + "/" + std::to_string(sub) + ": " + std::to_string(cnt) + " -> " +
                std::to_string(list.size()) + " at " + hex32(reg->start));
            reg->start += std::uint32_t(blob.size());
        }
}

// catpatch.apply, package form: category 19 "Custom" with our models.
void Builder::custom() {
    const CustomCategory& cc = *spec_.custom;
    for (const CodePatch& p : cc.codePatches) {
        expectStock(p.at, p.len, p.sha256, "Custom category patch");
        a_.put(p.at, p.bytes);
    }
    const auto table = [&](const char* name) -> const MovedTable& {
        for (const MovedTable& t : cc.tables)
            if (t.name == name) return t;
        throw BuildError(std::string("custom_category: no table ") + name);
    };
    const MovedTable &tNsub = table("nsub"), &tPtr = table("listptr"), &tCnt = table("counts"), &tLabel = table("label"),
                     &tIcons = table("icons");
    (void)table("names");  // all seven must be there
    (void)table("subnames");

    // each model's entry copied from its "like" model's plain (not Amp+Cab) browser entry
    std::map<std::uint32_t, std::vector<std::uint8_t>> templ;
    for (int cat = 0; cat < kNcat; ++cat)
        for (std::uint32_t sub = 0; sub < a_.u32(tNsub.va + 4 * cat); ++sub) {
            const std::uint32_t p = a_.u32(tPtr.va + 16 * cat + 4 * sub), n = a_.u32(tCnt.va + 16 * cat + 4 * sub);
            for (std::uint32_t k = 0; p && k < n; ++k) {
                const std::vector<std::uint8_t> e = a_.raw(p + 48 * k, 48);
                if (readLE32(e, 8) == 0xFFFFFFFF) templ.emplace(readLE32(e, 4), e);
            }
        }
    struct List {
        std::uint32_t at, count;
        std::string name;
    };
    std::vector<List> lists;
    for (const CustomSub& s : cc.subs) {
        std::vector<std::uint8_t> blob;
        for (const std::string& sym : s.models) {
            const M4Model* m = nullptr;
            for (const M4Model& x : models_)
                if (x.sym == sym) m = &x;
            if (!m) continue;  // that mod isn't in this build
            const auto t = templ.find(m->like[2]);
            if (t == templ.end()) throw BuildError(sym + ": its \"like\" model has no browser entry to copy");
            std::vector<std::uint8_t> e = t->second;
            setLE32(e, 4, m->sid);
            e[12] = std::uint8_t(m->modelNo);
            e[13] = std::uint8_t(m->modelNo >> 8);
            const std::vector<std::uint8_t> name = padded(latin1(m->name), 26);
            std::copy(name.begin(), name.end(), e.begin() + 14);
            std::fill(e.begin() + 0x28, e.begin() + 0x30, 0);  // filled at boot (availability, DSP cost)
            blob.insert(blob.end(), e.begin(), e.end());
        }
        if (blob.empty()) continue;  // no model of this sub-list in the build: left out
        const std::uint32_t at = take(std::uint32_t(blob.size()), "catpatch:list:" + s.name, 4);
        a_.put(at, blob);
        lists.push_back({at, std::uint32_t(blob.size() / 48), s.name});
    }
    if (lists.size() > 4) throw BuildError("Custom category: more than 4 sub-lists");

    const std::string name = latin1(cc.name);
    std::map<std::string, std::vector<std::uint8_t>> extra;
    extra["names"] = padded(name.substr(0, 26), 26);
    std::vector<std::uint8_t> ptrs, cnts, subnames;
    for (const List& l : lists) {
        le32(ptrs, l.at);
        le32(cnts, l.count);
        const std::vector<std::uint8_t> nm = padded(latin1(l.name).substr(0, 26), 26);
        subnames.insert(subnames.end(), nm.begin(), nm.end());
    }
    ptrs.resize(16, 0);
    cnts.resize(16, 0);
    subnames.resize(104, 0);
    extra["listptr"] = ptrs;
    extra["counts"] = cnts;
    extra["nsub"] = {};
    le32(extra["nsub"], std::uint32_t(lists.size()));
    extra["subnames"] = subnames;
    std::vector<std::uint8_t> lab = a_.raw(tLabel.va + 44 * std::uint32_t(cc.labelLike), 44);
    const std::vector<std::uint8_t> full = padded(name.substr(0, 17), 17), shortName = padded(latin1(cc.shortName).substr(0, 11), 11);
    std::copy(full.begin(), full.end(), lab.begin());
    std::copy(shortName.begin(), shortName.end(), lab.begin() + 17);
    if (cc.rgb) {
        setLE32(lab, 28, *cc.rgb);
        setLE32(lab, 32, cc.rgbDim.value_or(*cc.rgb));
    }
    extra["label"] = lab;
    extra["icons"] = a_.raw(tIcons.va + 24 * std::uint32_t(cc.labelLike), 24);

    std::string moved;
    for (const MovedTable& t : cc.tables) {  // each table copied with a 20th entry, its literals re-pointed
        const auto x = extra.find(t.name);
        if (x == extra.end()) throw BuildError("custom_category: unknown table " + t.name);
        std::vector<std::uint8_t> nt = a_.raw(t.va, t.entrySize * kNcat);
        nt.insert(nt.end(), x->second.begin(), x->second.end());
        const std::uint32_t at = take(std::uint32_t(nt.size()), "catpatch:table:" + t.name, 4);
        a_.put(at, nt);
        for (const auto& [lit, off] : t.lits) {
            if (a_.u32(lit) != t.va + std::uint32_t(off))
                throw BuildError("Custom category: literal " + hex32(lit) + " is not stock");
            a_.put32(lit, at + std::uint32_t(off));
        }
        moved += (moved.empty() ? "" : ", ") + t.name + " " + hex32(at);
    }
    // availability array (RAM, rebuilt at boot): paths x 20 words, plus the word before it
    const std::uint32_t size = 4 + 4 * cc.ramPaths * (kNcat + 1);
    const std::uint32_t arr = take(size, "catpatch:ram_array", 4) + 4;
    a_.put(arr - 4, std::vector<std::uint8_t>(size, 0));
    for (const auto& [lit, off] : cc.ramLits) {
        if (a_.u32(lit) != cc.ramArray + std::uint32_t(off)) throw BuildError("Custom category: RAM literal " + hex32(lit) + " is not stock");
        a_.put32(lit, arr + std::uint32_t(off));
    }
    log("custom category 19 '" + cc.name + "': " + std::to_string(lists.size()) + " sub-list(s); tables " + moved +
        "; RAM array " + hex32(arr) + "; " + std::to_string(cc.codePatches.size()) + " code patches");
}

// newamp.place_code, prebuilt form: our blob at its fixed origin, build-time words, then its hooks.
void Builder::placeCode(const CodePart& c) {
    std::vector<std::uint8_t> blob = c.blob;
    if (take(std::uint32_t(blob.size()), "code:" + c.name, 8) != c.origin)
        throw BuildError("prebuilt code " + c.name + ": placed off its origin " + hex32(c.origin));
    for (const auto& [symbol, model] : c.words) {
        const M4Model* m = nullptr;
        for (const M4Model& x : models_)
            if (x.sym == model) m = &x;
        if (!m) throw BuildError(c.name + " needs model " + model + ", which isn't in this build");
        const std::uint32_t at = c.symbols.at(symbol) - c.origin;
        if (at + 4 > blob.size()) throw BuildError(c.name + ": word " + symbol + " outside the blob");
        setLE32(blob, at, m->index);
    }
    a_.put(c.origin, blob);
    log("code: " + c.name + " " + std::to_string(blob.size()) + " bytes at " + hex32(c.origin));
    for (const Hook& h : c.hooks) hook(h, h.symbol.empty() ? c.origin : c.symbols.at(h.symbol));
}

void Builder::hook(const Hook& h, std::uint32_t target) {
    if (h.kind == Hook::Kind::Patch) {
        if (h.len < 4 || h.len % 2) throw BuildError("patch at " + hex32(h.site) + ": at least 4 bytes, whole halfwords");
        expectStock(h.site, h.len, h.sha256, "patch");
        const std::array<std::uint8_t, 4> br = encodeBranch(h.site, target, h.link);
        std::vector<std::uint8_t> out(br.begin(), br.end());
        for (std::uint32_t k = 4; k < h.len; k += 2) out.insert(out.end(), {0x00, 0xbf});  // nop
        a_.put(h.site, out);
        log("  patch " + hex32(h.site) + " -> " + (h.link ? "bl " : "b.w ") + hex32(target));
    } else if (h.kind == Hook::Kind::Bl || h.kind == Hook::Kind::Bw) {
        const bool link = h.kind == Hook::Kind::Bl;
        const std::vector<std::uint8_t> cur = a_.raw(h.site, 4);
        const std::optional<std::uint32_t> got = branchTarget(cur.data(), h.site, link);
        if (!got || *got != h.expect)
            throw BuildError(std::string(link ? "bl" : "b.w") + " at " + hex32(h.site) + " doesn't go to " + hex32(h.expect));
        const std::array<std::uint8_t, 4> br = encodeBranch(h.site, target, link);
        a_.put(h.site, {br.begin(), br.end()});
        log(std::string("  ") + (link ? "bl " : "bw ") + hex32(h.site) + ": " + hex32(h.expect) + " -> " + hex32(target));
    } else {
        if (a_.u32(h.site) != h.expect) throw BuildError("hook literal " + hex32(h.site) + " is not " + hex32(h.expect));
        a_.put32(h.site, target | 1);
        log("  literal " + hex32(h.site) + ": " + hex32(h.expect) + " -> " + hex32(target | 1));
    }
}

// fwtag.tag: "Firmware Version %01d.%02d.%01d%c" -> "Mod %d.%02d.%d:%c<tag> " (MODTAG.md).
void Builder::tag(const std::string& commit, bool dirty) {
    expectStock(kTagVa, kTagRoom, kTagWindow, "version string");
    res_.tag = encodeModTag(commit, spec_.modBits, dirty);
    const std::string s = "Mod %d.%02d.%d:%c" + res_.tag + " ";
    if (s.size() + 1 != kTagRoom) throw BuildError("build tag doesn't fit");
    a_.put(kTagVa, padded(s, kTagRoom));
    log("mod tag " + res_.tag);
}

M4Result Builder::run(const std::string& commit, bool dirty) {
    if (models_.size() != spec_.models.size()) throw BuildError("model registrations don't match the spec");
    tables();
    if (spec_.custom) custom();  // first, so its tables don't depend on the mods' code
    for (const CodePart& c : spec_.code) placeCode(c);
    std::string left;
    for (const Region& r : regions_) left += (left.empty() ? "" : ", ") + std::to_string(r.end - r.start);
    log("M4 free space left: " + left + " bytes");
    tag(commit, dirty);
    res_.app = std::move(a_.b);
    return std::move(res_);
}

} // namespace

std::pair<std::uint32_t, std::uint32_t> stockWindow(std::uint32_t at, std::uint32_t n) {
    const std::uint32_t lo = at & ~0xFu, hi = (at + n + 15) & ~0xFu;
    return {lo, hi - lo};
}

std::array<std::uint8_t, 4> encodeBranch(std::uint32_t site, std::uint32_t target, bool link) {
    const std::int64_t off = std::int64_t(target) - (std::int64_t(site) + 4);
    if (off < -(std::int64_t(1) << 24) || off >= (std::int64_t(1) << 24) || (off & 1))
        throw BuildError("branch out of range " + hex32(site) + " -> " + hex32(target));
    const std::uint32_t s = off < 0 ? 1 : 0;
    const std::uint32_t o = std::uint32_t(off) & ((1u << 25) - 1);
    const std::uint32_t i1 = (o >> 23) & 1, i2 = (o >> 22) & 1;
    const std::uint32_t j1 = 1 - (i1 ^ s), j2 = 1 - (i2 ^ s);
    const std::uint32_t h1 = 0xf000 | (s << 10) | ((o >> 12) & 0x3ff);
    const std::uint32_t h2 = (link ? 0xd000u : 0x9000u) | (j1 << 13) | (j2 << 11) | ((o >> 1) & 0x7ff);
    return {std::uint8_t(h1), std::uint8_t(h1 >> 8), std::uint8_t(h2), std::uint8_t(h2 >> 8)};
}

std::optional<std::uint32_t> branchTarget(const std::uint8_t* b, std::uint32_t site, bool link) {
    const std::uint32_t h1 = b[0] | b[1] << 8, h2 = b[2] | b[3] << 8;
    if ((h1 & 0xf800) != 0xf000 || (h2 & 0xd000) != (link ? 0xd000u : 0x9000u)) return std::nullopt;
    const std::uint32_t s = (h1 >> 10) & 1, j1 = (h2 >> 13) & 1, j2 = (h2 >> 11) & 1;
    const std::uint32_t i1 = 1 - (j1 ^ s), i2 = 1 - (j2 ^ s);
    std::int64_t off = std::int64_t((s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3ff) << 12) | ((h2 & 0x7ff) << 1));
    if (s) off -= std::int64_t(1) << 25;
    return std::uint32_t(std::int64_t(site) + 4 + off);
}

M4Result buildM4(const std::vector<std::uint8_t>& stockApp, const BuildSpec& spec, const std::vector<M4Model>& models,
                 const std::string& commit, bool dirty) {
    Builder b(stockApp, spec, models);
    return b.run(commit, dirty);
}

} // namespace hx::mods

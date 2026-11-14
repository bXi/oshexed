#include "core/dspbuild.h"

#include "core/bytes.h"
#include "core/checksums.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace hx::mods {

using json::Value;
using dsp::Words;

namespace {

// ---- small helpers -------------------------------------------------------------------------------------------

std::string hex32(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

Words u32s(const std::vector<std::uint8_t>& b, const std::string& what) {
    if (b.size() % 4) throw BuildError(what + ": not a whole number of words");
    Words w(b.size() / 4);
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = readLE32(b, 4 * i);
    return w;
}

std::vector<std::uint8_t> packWords(const Words& w) {
    std::vector<std::uint8_t> out(4 * w.size());
    for (std::size_t i = 0; i < w.size(); ++i)
        for (int k = 0; k < 4; ++k) out[4 * i + k] = std::uint8_t(w[i] >> (8 * k));
    return out;
}

void putLE32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + k] = std::uint8_t(v >> (8 * k));
}
void addLE32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(std::uint8_t(v >> (8 * k)));
}

// w[at, at + n), checked (stock data that points outside itself stops the build). Empty lists may point anywhere.
Words sub(const Words& w, std::uint64_t at, std::uint64_t n, const char* what) {
    if (!n) return {};
    if (at > w.size() || n > w.size() - at) throw BuildError(std::string(what) + ": points outside its data");
    return Words(w.begin() + std::ptrdiff_t(at), w.begin() + std::ptrdiff_t(at + n));
}
std::uint32_t word(const Words& w, std::uint64_t i, const char* what) {
    if (i >= w.size()) throw BuildError(std::string(what) + ": points outside its data");
    return w[std::size_t(i)];
}

// Symbol names are latin-1 in the firmware, UTF-8 in JSON.
std::string toUtf8(const std::string& latin1) {
    std::string out;
    for (unsigned char c : latin1) {
        if (c < 0x80) out += char(c);
        else out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}
std::string toLatin1(const std::string& utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const unsigned char c = utf8[i];
        if (c < 0x80) out += char(c);
        else if ((c & 0xE0) == 0xC0 && c <= 0xC3 && i + 1 < utf8.size()) out += char(((c & 0x1F) << 6) | (utf8[++i] & 0x3F));
        else throw BuildError("\"" + utf8 + "\" has characters the unit can't store");
    }
    return out;
}

// ---- JSON ------------------------------------------------------------------------------------------------------

Value I(std::int64_t v) { return Value::integerNumber(v); }
Value Str(const std::string& s) { return Value::string(s); }
Value wordsJson(const Words& w) {
    Value a = Value::array();
    for (std::uint32_t x : w) a.push(I(x));
    return a;
}

const Value& mem(const Value& o, const char* key, const char* what) {
    const Value* v = o.isObject() ? o.get(key) : nullptr;
    if (!v) throw BuildError(std::string(what) + ": no \"" + key + "\"");
    return *v;
}
const std::vector<Value>& items(const Value& v, const char* what) {
    if (!v.isArray()) throw BuildError(std::string(what) + ": not a list");
    return v.items;
}
std::uint32_t raw(const Value& v, const char* what) {
    if (!v.isNumber() || !v.integer) throw BuildError(std::string(what) + ": not a whole number");
    return std::uint32_t(std::uint64_t(v.asInt()) & 0xFFFFFFFFu);
}
Words rawList(const Value& v, const char* what) {
    Words out;
    for (const Value& x : items(v, what)) out.push_back(raw(x, what));
    return out;
}
const Value& at(const Value& v, std::size_t i, const char* what) {
    const auto& it = items(v, what);
    if (i >= it.size()) throw BuildError(std::string(what) + ": too short");
    return it[i];
}

// ---- rec 4 (mkchain.Rec4) ----------------------------------------------------------------------------------

using Attr = std::array<std::uint32_t, 3>;
using Entry = std::array<std::uint32_t, 14>;

class Rec4 {
public:
    explicit Rec4(std::vector<std::uint8_t> meta) : m(std::move(meta)) {
        if (m.size() < 40) throw BuildError("rec 4 too short");
        for (int i = 0; i < 10; ++i) h.push_back(u32(4 * i));
        const std::uint32_t so = h[3], ssize = h[5];
        const std::uint32_t nsym = u32(so);
        if (u32(so + 4) != ssize) throw BuildError("rec 4 symbol section size");
        const std::uint64_t poolAt = std::uint64_t(so) + 8 + std::uint64_t(nsym) * 12;
        if (poolAt > std::uint64_t(so) + ssize || std::uint64_t(so) + ssize > m.size()) throw BuildError("rec 4 symbols");
        pool.assign(m.begin() + std::ptrdiff_t(poolAt), m.begin() + std::ptrdiff_t(so + ssize));
        for (std::uint32_t i = 0; i < nsym; ++i) syms.push_back({u32(so + 8 + 12 * i), u32(so + 12 + 12 * i), u32(so + 16 + 12 * i)});
        for (std::uint32_t i = 0; i < h[7]; ++i) {
            Entry e;
            for (int k = 0; k < 14; ++k) e[k] = u32(std::uint64_t(h[6]) + 0x38 * i + 4 * k);
            ents.push_back(e);
        }
        for (std::uint32_t i = 0; i < nsym; ++i) ids.emplace(name(i), i);
    }

    std::uint32_t u32(std::uint64_t off) const {
        if (off + 4 > m.size()) throw BuildError("rec 4: read outside the record");
        return readLE32(m, std::size_t(off));
    }
    std::string name(std::uint32_t i) const {
        if (i >= syms.size()) throw BuildError("rec 4: symbol " + std::to_string(i) + " doesn't exist");
        const auto& s = syms[i];
        if (std::uint64_t(s[0]) + s[2] > pool.size()) throw BuildError("rec 4: symbol outside the pool");
        return std::string(pool.begin() + s[0], pool.begin() + s[0] + s[2]);
    }
    // The symbol's index; added (zlib CRC32 of the name, pool + NUL) when new. s is latin-1.
    std::uint32_t sym(const std::string& s) {
        const auto it = ids.find(s);
        if (it != ids.end()) return it->second;
        const Bytes b(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
        syms.push_back({std::uint32_t(pool.size()), crc32(b), std::uint32_t(s.size())});
        pool.insert(pool.end(), s.begin(), s.end());
        pool.push_back(0);
        const std::uint32_t i = std::uint32_t(syms.size() - 1);
        ids[s] = i;
        return i;
    }
    const Entry& entry(const std::string& symName) const {
        for (const Entry& e : ents)
            if (name(e[2]) == symName) return e;
        throw BuildError("model " + toUtf8(symName) + " isn't in this firmware");
    }
    std::uint32_t append(const std::vector<std::uint8_t>& d) {
        while (m.size() % 4) m.push_back(0);
        const std::uint32_t o = std::uint32_t(m.size());
        m.insert(m.end(), d.begin(), d.end());
        return o;
    }
    std::vector<Attr> attrs(std::uint32_t n, std::uint32_t off) const {
        std::vector<Attr> out;
        for (std::uint32_t k = 0; k < n; ++k)
            out.push_back({u32(std::uint64_t(off) + 12 * k), u32(std::uint64_t(off) + 12 * k + 4), u32(std::uint64_t(off) + 12 * k + 8)});
        return out;
    }
    std::uint32_t packAttrs(const std::vector<Attr>& a) {
        if (a.empty()) return 0;
        std::vector<std::uint8_t> d;
        for (const Attr& x : a)
            for (std::uint32_t v : x) addLE32(d, v);
        return append(d);
    }
    // Symbols rewritten in place (into the old entry table's room), the entry table moved to the end.
    std::vector<std::uint8_t> finish() {
        const std::uint32_t so = h[3], eo = h[6], oldNe = h[7];
        if (std::uint64_t(eo) + std::uint64_t(oldNe) * 0x38 != h[8]) throw BuildError("rec 4: global param list must follow the entry table");
        const std::uint32_t size = 8 + 12 * std::uint32_t(syms.size()) + std::uint32_t(pool.size());
        std::vector<std::uint8_t> sec;
        addLE32(sec, std::uint32_t(syms.size()));
        addLE32(sec, size);
        for (const auto& s : syms)
            for (std::uint32_t v : s) addLE32(sec, v);
        sec.insert(sec.end(), pool.begin(), pool.end());
        if (std::uint64_t(so) + sec.size() > std::uint64_t(eo) + std::uint64_t(oldNe) * 0x38)
            throw BuildError("rec 4: symbols don't fit in the freed entry-table space");
        std::copy(sec.begin(), sec.end(), m.begin() + so);
        std::vector<std::uint8_t> et;
        for (const Entry& e : ents)
            for (std::uint32_t v : e) addLE32(et, v);
        const std::uint32_t eo2 = append(et);
        h[2] = std::uint32_t(m.size());
        h[5] = size;
        h[6] = eo2;
        h[7] = std::uint32_t(ents.size());
        for (int i = 0; i < 10; ++i) putLE32(m, 4 * i, h[i]);
        return m;
    }

    std::vector<std::uint8_t> m;
    Words h;
    std::vector<std::uint8_t> pool;
    std::vector<std::array<std::uint32_t, 3>> syms;
    std::vector<Entry> ents;
    std::unordered_map<std::string, std::uint32_t> ids;  // name -> first index
};

// ---- the kernel core (corepack.py) ----------------------------------------------------------------------------

constexpr std::uint32_t kCoreMax = 0x7A612;  // DSP core buffer capacity in words
constexpr std::uint32_t kCoreHdr = 0x12;

struct Tri {
    std::uint32_t a = 0;
    Words d;
    std::uint32_t x = 0;
};
struct Node {
    Words w;  // 20 words
    std::vector<Tri> tri;
    Words in, out, par;
};
struct Kernel {
    Words e;  // 7 words
    std::vector<Node> nodes;
    Words recs;
};
struct Core {
    Words hdr, small;
    Kernel root;
    std::vector<Kernel> kernels;
};

Kernel coreEntry(const Words& C, const Words& e) {
    Kernel k;
    k.e = e;
    for (std::uint32_t i = 0; i < e[2]; ++i) {
        Node nd;
        nd.w = sub(C, std::uint64_t(e[3]) + 20 * i, 20, "core node");
        for (std::uint32_t t = 0; t < nd.w[1] / 2; ++t) {
            const Words abx = sub(C, std::uint64_t(nd.w[2]) + 3 * t, 3, "core triple");
            nd.tri.push_back({abx[0], sub(C, abx[1], abx[0] & ~1u, "core triple data"), abx[2]});
        }
        nd.in = sub(C, nd.w[8], nd.w[7], "core buffer list");
        nd.out = sub(C, nd.w[10], nd.w[9], "core buffer list");
        nd.par = sub(C, nd.w[12], 2 * std::uint64_t(nd.w[11]), "core param list");
        k.nodes.push_back(std::move(nd));
    }
    k.recs = sub(C, e[5], 4 * std::uint64_t(e[4]), "core records");
    return k;
}

Core parseCore(const Words& C) {
    if (C.size() < kCoreHdr) throw BuildError("core too short");
    Core S;
    S.hdr = sub(C, 0, kCoreHdr, "core");
    S.small = sub(C, C[2], C[1], "core small array");
    S.root = coreEntry(C, sub(C, 8, 7, "core"));
    for (std::uint32_t k = 0; k < C[0xF]; ++k) S.kernels.push_back(coreEntry(C, sub(C, std::uint64_t(C[0x10]) + 7 * k, 7, "core kernel table")));
    return S;
}

Words packCore(const Core& S) {
    Words out = S.hdr;
    std::map<Words, std::uint32_t> shared;
    const auto put = [&out](const Words& w) {
        const std::uint32_t o = std::uint32_t(out.size());
        out.insert(out.end(), w.begin(), w.end());
        return o;
    };
    const auto leaf = [&](const Words& w) {
        const auto it = shared.find(w);
        if (it != shared.end()) return it->second;
        const std::uint32_t o = put(w);
        shared.emplace(w, o);
        return o;
    };
    out[1] = std::uint32_t(S.small.size());
    out[2] = put(S.small);
    std::vector<const Kernel*> ents{&S.root};
    for (const Kernel& k : S.kernels) ents.push_back(&k);
    std::vector<std::uint32_t> arr(ents.size(), 0);
    arr[0] = put(Words(20 * S.root.nodes.size(), 0));
    const std::uint32_t tab = put(Words(7 * S.kernels.size(), 0));
    const std::uint32_t zero = tab;  // target for zero-length pointers
    for (std::size_t k = 0; k < ents.size(); ++k) {
        const Kernel& E = *ents[k];
        if (k) arr[k] = put(Words(20 * E.nodes.size(), 0));
        for (std::size_t i = 0; i < E.nodes.size(); ++i) {
            const Node& nd = E.nodes[i];
            Words w = nd.w;
            if (!nd.tri.empty()) {
                Words tl;
                for (const Tri& t : nd.tri) {
                    const std::uint32_t d = t.d.empty() ? zero : put(t.d);
                    tl.insert(tl.end(), {t.a, d, t.x});
                }
                w[2] = put(tl);
            } else {
                w[2] = zero;
            }
            w[8] = nd.in.empty() ? zero : leaf(nd.in);
            w[10] = nd.out.empty() ? zero : leaf(nd.out);
            w[12] = nd.par.empty() ? zero : leaf(nd.par);
            std::copy(w.begin(), w.end(), out.begin() + arr[k] + 20 * i);
        }
        Words e = E.e;
        e[3] = E.nodes.empty() ? zero : arr[k];
        e[5] = E.recs.empty() ? zero : leaf(E.recs);
        std::copy(e.begin(), e.end(), out.begin() + (k == 0 ? 8 : tab + 7 * (k - 1)));
    }
    out[0xF] = std::uint32_t(S.kernels.size());
    out[0x10] = tab;
    return out;
}

// ---- modules (mkchain.parse / build / Prog) --------------------------------------------------------------------

using Path = Words;
using Refs = std::vector<std::pair<std::uint32_t, Path>>;  // in insertion order, as Python's dict

void refSet(Refs& r, std::uint32_t k, const Path& v) {
    for (auto& [key, val] : r)
        if (key == k) {
            val = v;
            return;
        }
    r.emplace_back(k, v);
}
const Path* refGet(const Refs& r, std::uint32_t k) {
    for (const auto& [key, val] : r)
        if (key == k) return &val;
    return nullptr;
}

struct KTree {
    std::uint32_t idx = 0, kid = 0;
    std::vector<KTree> children;
};
struct Bind {
    Path path;
    std::uint32_t port = 0;
    Words vals;
    std::uint32_t fl = 0;
};
struct Module {
    KTree kroot;
    Words ctrl;
    Refs refs;
    std::vector<Bind> binds;
    std::vector<std::pair<Path, Path>> nmap;
};

KTree ktree(const Words& w, std::uint64_t e, int depth) {
    if (depth > 32) throw BuildError("module kernel tree too deep");
    KTree t;
    t.idx = word(w, e, "module kernel tree");
    t.kid = word(w, e + 1, "module kernel tree");
    const std::uint32_t list = word(w, e + 2, "module kernel tree"), n = word(w, e + 3, "module kernel tree");
    for (std::uint32_t i = 0; i < n; ++i) t.children.push_back(ktree(w, std::uint64_t(list) + 4 * i, depth + 1));
    return t;
}

Module parseModule(const Words& w) {
    const Words hdr = sub(w, 0, 12, "module header");
    const std::uint32_t kb = hdr[2], ro = hdr[4], rc = hdr[5], co = hdr[6], cl = hdr[7], no = hdr[8], nc = hdr[9],
                        bo = hdr[10], bc = hdr[11];
    const auto cell = [&w](std::uint64_t o) { return sub(w, o + 1, word(w, o, "module cell"), "module cell"); };
    Module m;
    m.kroot = ktree(w, kb, 0);
    std::set<std::uint64_t> bindObjs;
    for (std::uint32_t i = 0; i < rc; ++i) {
        const std::uint32_t t = word(w, std::uint64_t(ro) + 2 * i, "module reloc"), idx = word(w, std::uint64_t(ro) + 2 * i + 1, "module reloc");
        if (t != 1) throw BuildError("module: reloc of type " + std::to_string(t));
        if (idx >= co && idx < std::uint64_t(co) + cl) refSet(m.refs, idx - co, cell(word(w, idx, "module reloc")));
        else bindObjs.insert(idx);
    }
    for (std::uint32_t i = 0; i < bc; ++i) {
        const Words b = sub(w, std::uint64_t(bo) + 4 * i, 4, "module bind");
        if (!bindObjs.count(std::uint64_t(bo) + 4 * i)) throw BuildError("module: bind without reloc");
        m.binds.push_back({cell(b[0]), b[1], cell(b[2]), b[3]});
    }
    m.ctrl = sub(w, co, cl, "module control section");
    for (const auto& [local, path] : m.refs) m.ctrl[local] = 0;  // filled with the path cell by build
    for (std::uint32_t i = 0; i < nc; ++i) {
        const Words p = sub(w, std::uint64_t(no) + 2 * i, 2, "module node map");
        m.nmap.emplace_back(cell(p[0]), cell(p[1]));
    }
    return m;
}

Words buildModule(const Module& m) {
    Words cells;
    std::map<Path, std::uint32_t> pcache;
    const auto pc = [&](const Path& p) {
        const auto it = pcache.find(p);
        if (it != pcache.end()) return it->second;
        const std::uint32_t o = 12 + std::uint32_t(cells.size());
        pcache.emplace(p, o);
        cells.push_back(std::uint32_t(p.size()));
        cells.insert(cells.end(), p.begin(), p.end());
        return o;
    };
    const auto vc = [&](const Words& v) {
        const std::uint32_t o = 12 + std::uint32_t(cells.size());
        cells.push_back(std::uint32_t(v.size()));
        cells.insert(cells.end(), v.begin(), v.end());
        return o;
    };
    Words ctrl = m.ctrl;
    for (const auto& [local, path] : m.refs) {
        if (local >= ctrl.size()) throw BuildError("module: reference outside the control section");
        ctrl[local] = pc(path);
    }
    std::vector<Words> binds;
    for (const Bind& b : m.binds) {
        const std::uint32_t p = pc(b.path);
        binds.push_back({p, b.port, vc(b.vals), b.fl});
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> nmap;
    for (const auto& [a, b] : m.nmap) {
        const std::uint32_t x = pc(a);
        nmap.emplace_back(x, pc(b));
    }
    // kernel block: root, then each child list contiguous (breadth first)
    std::vector<std::array<std::uint32_t, 4>> ents{{m.kroot.idx, m.kroot.kid, 0, std::uint32_t(m.kroot.children.size())}};
    std::deque<std::pair<std::size_t, const KTree*>> todo{{0, &m.kroot}};
    while (!todo.empty()) {
        const auto [i, node] = todo.front();
        todo.pop_front();
        if (node->children.empty()) continue;
        ents[i][2] = std::uint32_t(ents.size());
        for (const KTree& ch : node->children) {
            todo.emplace_back(ents.size(), &ch);
            ents.push_back({ch.idx, ch.kid, 0, std::uint32_t(ch.children.size())});
        }
    }
    const std::uint32_t kb = 12 + std::uint32_t(cells.size());
    Words kblk;
    for (const auto& e : ents) kblk.insert(kblk.end(), {e[0], e[1], e[3] ? kb + 4 * e[2] : 0, e[3]});
    const std::uint32_t co = kb + std::uint32_t(kblk.size());
    const std::uint32_t bo = co + std::uint32_t(ctrl.size());
    std::vector<std::uint32_t> locals;
    for (const auto& [local, path] : m.refs) locals.push_back(local);
    std::sort(locals.begin(), locals.end());
    Words relocs;
    for (std::uint32_t l : locals) relocs.insert(relocs.end(), {1, co + l});
    for (std::size_t i = 0; i < binds.size(); ++i) relocs.insert(relocs.end(), {1, bo + 4 * std::uint32_t(i)});
    const std::uint32_t ro = bo + 4 * std::uint32_t(binds.size());
    const std::uint32_t no = ro + std::uint32_t(relocs.size());
    Words out{3, 2, kb, std::uint32_t(ents.size()), ro, std::uint32_t(relocs.size() / 2), co, std::uint32_t(ctrl.size()), no,
              std::uint32_t(nmap.size()), bo, std::uint32_t(binds.size())};
    out.insert(out.end(), cells.begin(), cells.end());
    out.insert(out.end(), kblk.begin(), kblk.end());
    out.insert(out.end(), ctrl.begin(), ctrl.end());
    for (const Words& b : binds) out.insert(out.end(), b.begin(), b.end());
    out.insert(out.end(), relocs.begin(), relocs.end());
    for (const auto& [a, b] : nmap) out.insert(out.end(), {a, b});
    return out;
}

// modcheck.ctrl_len: words used by the control section at c (offsets inside relative to the top section).
std::uint64_t ctrlLen(const Words& w, std::uint64_t base, std::uint64_t c, int depth = 0) {
    if (depth > 32) throw BuildError("control section nested too deep");
    const Words h = sub(w, c, 24, "control section");
    std::uint64_t used = 24 + std::uint64_t(h[18]) * 10;
    for (std::uint32_t k = 0; k < h[18]; ++k) used += word(w, base + h[0] + 10 * std::uint64_t(k) + 7, "control record");
    used += std::uint64_t(h[19]) * 6;
    for (std::uint32_t k = 0; k < h[19]; ++k) used += word(w, base + h[2] + 6 * std::uint64_t(k) + 4, "control curve");
    used += (std::uint64_t(h[20]) + h[21] + h[22]) * 4 + std::uint64_t(h[23]) * 8;
    for (std::uint32_t k = 0; k < h[23]; ++k) {
        const std::uint32_t off = word(w, base + h[10] + 8 * std::uint64_t(k), "control sub-section");
        if (off) used += ctrlLen(w, base, base + off, depth + 1);
    }
    return used;
}

using Block = std::vector<std::int64_t>;

// mkchain.shift_block: a control section stored in arr (its offsets point at index value - origin), shifted by delta.
Block shiftBlock(const Block& arr, std::int64_t origin, std::int64_t delta) {
    Block out = arr;
    const auto idx = [&arr](std::int64_t j) {
        if (j < 0 || std::uint64_t(j) >= arr.size()) throw BuildError("control section: offset outside its block");
        return std::size_t(j);
    };
    std::function<void(std::int64_t, int)> walk = [&](std::int64_t i, int depth) {
        if (depth > 32) throw BuildError("control section nested too deep");
        std::int64_t h[24];
        for (int k = 0; k < 24; ++k) h[k] = arr[idx(i + k)];
        for (int k : {0, 2, 4, 6, 8, 10}) out[idx(i + k)] = h[k] + delta;
        for (std::int64_t r = 0; r < h[18]; ++r) {
            const std::size_t j = idx(h[0] - origin + 10 * r);
            out[j] = arr[j] + delta;
        }
        for (std::int64_t c = 0; c < h[19]; ++c) {
            const std::size_t j = idx(h[2] - origin + 6 * c);
            out[j] = arr[j] + delta;
        }
        for (std::int64_t s = 0; s < h[23]; ++s) {
            const std::size_t j = idx(h[10] - origin + 8 * s);
            if (arr[j]) {
                walk(arr[j] - origin, depth + 1);
                out[j] = arr[j] + delta;
            }
        }
    };
    walk(0, 0);
    return out;
}

struct Item {
    bool ref = false;
    std::uint32_t v = 0;
    Path path;
};
struct ProgRec {
    Words rec;
    std::vector<Item> data;
};
struct ProgSub {
    Words e;
    Block blk;  // empty = none
    Refs brefs;
};
struct Prog {
    std::vector<ProgRec> recs;
    std::vector<std::pair<Words, Words>> curves;
    std::vector<Words> edges, kA, kB;
    std::vector<ProgSub> subs;
};

Prog parseProg(const Words& ctrl, const Refs& refs) {
    const Words h = sub(ctrl, 0, 24, "control header");
    std::uint64_t used = 24;
    Prog P;
    std::set<std::uint64_t> refpos;
    for (std::uint32_t r = 0; r < h[18]; ++r) {
        ProgRec pr;
        pr.rec = sub(ctrl, std::uint64_t(h[0]) + 10 * r, 10, "control record");
        if (pr.rec[2] != r) throw BuildError("control record out of order");
        for (std::uint32_t j = 0; j < pr.rec[7]; ++j) {
            const std::uint64_t pos = std::uint64_t(pr.rec[0]) + j;
            refpos.insert(pos);
            Item it;
            if (const Path* p = pos <= 0xFFFFFFFFu ? refGet(refs, std::uint32_t(pos)) : nullptr) it.ref = true, it.path = *p;
            else it.v = word(ctrl, pos, "control record data");
            pr.data.push_back(std::move(it));
        }
        used += 10 + pr.rec[7];
        P.recs.push_back(std::move(pr));
    }
    for (std::uint32_t c = 0; c < h[19]; ++c) {
        const Words cv = sub(ctrl, std::uint64_t(h[2]) + 6 * c, 6, "control curve");
        P.curves.emplace_back(cv, sub(ctrl, cv[0], cv[4], "control curve data"));
        used += 6 + std::uint64_t(cv[4]);
    }
    for (std::uint32_t i = 0; i < h[20]; ++i) P.edges.push_back(sub(ctrl, std::uint64_t(h[4]) + 4 * i, 4, "control edge"));
    for (std::uint32_t i = 0; i < h[21]; ++i) P.kA.push_back(sub(ctrl, std::uint64_t(h[6]) + 4 * i, 4, "control knob map"));
    for (std::uint32_t i = 0; i < h[22]; ++i) P.kB.push_back(sub(ctrl, std::uint64_t(h[8]) + 4 * i, 4, "control knob map"));
    used += 4 * (std::uint64_t(h[20]) + h[21] + h[22]) + 8 * std::uint64_t(h[23]);
    for (std::uint32_t s = 0; s < h[23]; ++s) {
        ProgSub ps;
        ps.e = sub(ctrl, std::uint64_t(h[10]) + 8 * s, 8, "control sub-section");
        if (ps.e[0]) {
            const std::uint64_t e0 = ps.e[0];
            const std::uint64_t ln = ctrlLen(ctrl, 0, e0);
            const Words part = sub(ctrl, e0, ln, "control sub-section");
            ps.blk = shiftBlock(Block(part.begin(), part.end()), std::int64_t(e0), -std::int64_t(e0));
            for (const auto& [p, v] : refs)
                if (p >= e0 && p < e0 + ln) ps.brefs.emplace_back(std::uint32_t(p - e0), v);
            for (std::uint64_t k = e0; k < e0 + ln; ++k) refpos.insert(k);
            used += ln;
        }
        P.subs.push_back(std::move(ps));
    }
    if (used != ctrl.size()) throw BuildError("control section has words this parser doesn't place");
    for (const auto& [p, v] : refs)
        if (!refpos.count(p)) throw BuildError("control section: reference outside record data / nested blocks");
    return P;
}

std::pair<Words, Refs> buildProg(const Prog& P) {
    std::uint64_t rdataLen = 0, cdataLen = 0;
    for (const ProgRec& r : P.recs) rdataLen += r.data.size();
    for (const auto& c : P.curves) cdataLen += c.second.size();
    const std::uint64_t nr = P.recs.size(), nc = P.curves.size();
    const std::uint64_t oRec = 24, oRdata = oRec + 10 * nr, oCur = oRdata + rdataLen, oCdata = oCur + 6 * nc;
    const std::uint64_t oEdge = oCdata + cdataLen, oKa = oEdge + 4 * P.edges.size(), oKb = oKa + 4 * P.kA.size();
    const std::uint64_t oSub = oKb + 4 * P.kB.size(), oNest = oSub + 8 * P.subs.size();
    Block h(24, 0);
    h[0] = std::int64_t(oRec), h[2] = std::int64_t(oCur), h[4] = std::int64_t(oEdge), h[6] = std::int64_t(oKa),
    h[8] = std::int64_t(oKb), h[10] = std::int64_t(oSub);
    std::int64_t s5 = 0, s4 = 0;
    for (const ProgRec& r : P.recs) {
        if (r.rec.size() != 10) throw BuildError("control record: not 10 words");
        s5 += r.rec[5], s4 += r.rec[4];
    }
    h[12] = s5;
    h[13] = h[18] = std::int64_t(nr);
    h[14] = s4 + std::int64_t(P.kA.size() + P.kB.size());
    h[15] = std::int64_t(P.edges.size() + P.kA.size() + P.kB.size());
    h[16] = std::int64_t(P.kA.size()), h[17] = std::int64_t(P.kB.size());
    h[19] = std::int64_t(nc), h[20] = std::int64_t(P.edges.size()), h[21] = std::int64_t(P.kA.size()),
    h[22] = std::int64_t(P.kB.size()), h[23] = std::int64_t(P.subs.size());
    Block out = h;
    Refs refs;
    std::int64_t dpos = std::int64_t(oRdata);
    Block rdata;
    for (std::size_t i = 0; i < P.recs.size(); ++i) {
        Words rec = P.recs[i].rec;
        rec[0] = std::uint32_t(dpos), rec[2] = std::uint32_t(i), rec[7] = std::uint32_t(P.recs[i].data.size());
        out.insert(out.end(), rec.begin(), rec.end());
        for (std::size_t j = 0; j < P.recs[i].data.size(); ++j) {
            const Item& x = P.recs[i].data[j];
            if (x.ref) {
                refSet(refs, std::uint32_t(dpos + std::int64_t(j)), x.path);
                rdata.push_back(0);
            } else {
                rdata.push_back(x.v);
            }
        }
        dpos += std::int64_t(P.recs[i].data.size());
    }
    out.insert(out.end(), rdata.begin(), rdata.end());
    std::int64_t cpos = std::int64_t(oCdata);
    Block cdata;
    for (const auto& [cv0, data] : P.curves) {
        if (cv0.size() != 6) throw BuildError("control curve: not 6 words");
        Words cv = cv0;
        cv[0] = std::uint32_t(cpos), cv[4] = std::uint32_t(data.size());
        out.insert(out.end(), cv.begin(), cv.end());
        cdata.insert(cdata.end(), data.begin(), data.end());
        cpos += std::int64_t(data.size());
    }
    out.insert(out.end(), cdata.begin(), cdata.end());
    for (const auto* lst : {&P.edges, &P.kA, &P.kB})
        for (const Words& x : *lst) out.insert(out.end(), x.begin(), x.end());
    std::int64_t npos = std::int64_t(oNest);
    Block nest;
    for (const ProgSub& s : P.subs) {
        Words e = s.e;
        if (!s.blk.empty()) {
            e[0] = std::uint32_t(npos);
            const Block moved = shiftBlock(s.blk, 0, npos);
            nest.insert(nest.end(), moved.begin(), moved.end());
            for (const auto& [p, v] : s.brefs) refSet(refs, std::uint32_t(npos + p), v);
            npos += std::int64_t(s.blk.size());
        }
        out.insert(out.end(), e.begin(), e.end());
    }
    out.insert(out.end(), nest.begin(), nest.end());
    Words w;
    for (std::int64_t v : out) {
        if (v < 0 || v > 0xFFFFFFFFll) throw BuildError("control section: value out of range");
        w.push_back(std::uint32_t(v));
    }
    return {w, refs};
}

// ---- ampgraph JSON ---------------------------------------------------------------------------------------------

Value ktreeJson(const KTree& t) {
    Value o = Value::object();
    o.set("idx", I(t.idx));
    o.set("kid", I(t.kid));
    Value ch = Value::array();
    for (const KTree& c : t.children) ch.push(ktreeJson(c));
    o.set("children", ch);
    return o;
}
KTree ktreeFrom(const Value& v, int depth = 0) {
    if (depth > 32) throw BuildError("graph: kernel tree too deep");
    KTree t;
    t.idx = raw(mem(v, "idx", "kroot"), "kroot idx");
    t.kid = raw(mem(v, "kid", "kroot"), "kroot kid");
    for (const Value& c : items(mem(v, "children", "kroot"), "kroot children")) t.children.push_back(ktreeFrom(c, depth + 1));
    return t;
}

Value moduleToJson(const Words& w) {
    const Module m = parseModule(w);
    const Prog P = parseProg(m.ctrl, m.refs);
    Value o = Value::object();
    o.set("kroot", ktreeJson(m.kroot));
    Value binds = Value::array();
    for (const Bind& b : m.binds) {
        Value vals = Value::array();
        for (std::uint32_t v : b.vals) vals.push(dsp::wval(v));
        Value x = Value::array();
        x.push(wordsJson(b.path));
        x.push(I(b.port));
        x.push(vals);
        x.push(I(b.fl));
        binds.push(x);
    }
    o.set("binds", binds);
    Value nmap = Value::array();
    for (const auto& [a, b] : m.nmap) {
        Value x = Value::array();
        x.push(wordsJson(a));
        x.push(wordsJson(b));
        nmap.push(x);
    }
    o.set("nmap", nmap);
    Value prog = Value::object();
    Value recs = Value::array();
    for (const ProgRec& r : P.recs) {
        Value data = Value::array();
        for (const Item& it : r.data) {
            if (it.ref) {
                Value ref = Value::object();
                ref.set("ref", wordsJson(it.path));
                data.push(ref);
            } else {
                data.push(I(it.v));
            }
        }
        Value x = Value::array();
        x.push(wordsJson(r.rec));
        x.push(data);
        recs.push(x);
    }
    prog.set("recs", recs);
    Value curves = Value::array();
    for (const auto& [cv, data] : P.curves) {
        Value d = Value::array();
        for (std::uint32_t v : data) d.push(dsp::wval(v));
        Value x = Value::array();
        x.push(wordsJson(cv));
        x.push(d);
        curves.push(x);
    }
    prog.set("curves", curves);
    const auto lists = [](const std::vector<Words>& l) {
        Value a = Value::array();
        for (const Words& x : l) a.push(wordsJson(x));
        return a;
    };
    prog.set("edges", lists(P.edges));
    prog.set("kA", lists(P.kA));
    prog.set("kB", lists(P.kB));
    Value subs = Value::array();
    for (const ProgSub& s : P.subs) {
        Value x = Value::array();
        x.push(wordsJson(s.e));
        if (s.blk.empty()) {
            x.push(Value());
            x.push(Value());
        } else {
            Value blk = Value::array();
            for (std::int64_t v : s.blk) blk.push(I(v));
            x.push(blk);
            Value br = Value::object();
            for (const auto& [k, v] : s.brefs) br.set(std::to_string(k), wordsJson(v));
            x.push(br);
        }
        subs.push(x);
    }
    prog.set("subs", subs);
    o.set("prog", prog);
    return o;
}

Words moduleFromJson(const Value& J) {
    const Value& p = mem(J, "prog", "module");
    Prog P;
    for (const Value& r : items(mem(p, "recs", "prog"), "prog recs")) {
        ProgRec pr;
        pr.rec = rawList(at(r, 0, "prog rec"), "prog rec");
        for (const Value& x : items(at(r, 1, "prog rec"), "prog rec data")) {
            Item it;
            if (x.isObject()) it.ref = true, it.path = rawList(mem(x, "ref", "prog rec data"), "prog ref");
            else it.v = raw(x, "prog rec data");
            pr.data.push_back(std::move(it));
        }
        P.recs.push_back(std::move(pr));
    }
    for (const Value& c : items(mem(p, "curves", "prog"), "prog curves")) {
        Words data;
        for (const Value& v : items(at(c, 1, "prog curve"), "prog curve")) data.push_back(dsp::jval(v));
        P.curves.emplace_back(rawList(at(c, 0, "prog curve"), "prog curve"), data);
    }
    for (const Value& x : items(mem(p, "edges", "prog"), "prog edges")) P.edges.push_back(rawList(x, "prog edge"));
    for (const Value& x : items(mem(p, "kA", "prog"), "prog kA")) P.kA.push_back(rawList(x, "prog kA"));
    for (const Value& x : items(mem(p, "kB", "prog"), "prog kB")) P.kB.push_back(rawList(x, "prog kB"));
    for (const Value& s : items(mem(p, "subs", "prog"), "prog subs")) {
        ProgSub ps;
        ps.e = rawList(at(s, 0, "prog sub"), "prog sub");
        if (ps.e.size() != 8) throw BuildError("prog sub: not 8 words");
        const Value& blk = at(s, 1, "prog sub");
        if (blk.isArray())
            for (const Value& v : blk.items) {
                if (!v.isNumber() || !v.integer) throw BuildError("prog sub block: not a whole number");
                ps.blk.push_back(v.asInt());
            }
        const Value& br = at(s, 2, "prog sub");
        if (br.isObject())
            for (std::size_t i = 0; i < br.keys.size(); ++i) {
                char* end = nullptr;
                const long long k = std::strtoll(br.keys[i].c_str(), &end, 10);
                if (*end || br.keys[i].empty() || k < 0) throw BuildError("prog sub refs: bad key");
                ps.brefs.emplace_back(std::uint32_t(k), rawList(br.items[i], "prog sub ref"));
            }
        P.subs.push_back(std::move(ps));
    }
    Module m;
    std::tie(m.ctrl, m.refs) = buildProg(P);
    m.kroot = ktreeFrom(mem(J, "kroot", "module"));
    for (const Value& b : items(mem(J, "binds", "module"), "module binds")) {
        Bind x;
        x.path = rawList(at(b, 0, "bind"), "bind path");
        x.port = raw(at(b, 1, "bind"), "bind port");
        for (const Value& v : items(at(b, 2, "bind"), "bind values")) x.vals.push_back(dsp::jval(v));
        x.fl = raw(at(b, 3, "bind"), "bind flag");
        m.binds.push_back(std::move(x));
    }
    for (const Value& n : items(mem(J, "nmap", "module"), "module nmap"))
        m.nmap.emplace_back(rawList(at(n, 0, "nmap"), "nmap"), rawList(at(n, 1, "nmap"), "nmap"));
    return buildModule(m);
}

Value kernelToJson(const Kernel& E) {
    Value nodes = Value::array();
    for (std::size_t i = 0; i < E.nodes.size(); ++i) {
        const Node& nd = E.nodes[i];
        const Words& w = nd.w;
        if (w[0] != i || w[3] != 0) throw BuildError("core kernel: node " + std::to_string(i) + " out of order");
        const std::uint32_t t = w[6];
        Value n = Value::object();
        char tb[24];
        if (t >> 16 == 0x60) std::snprintf(tb, sizeof tb, "k%u", unsigned(t & 0xFFFF));
        else std::snprintf(tb, sizeof tb, "0x%x", unsigned(t));
        n.set("type", Str(tb));
        n.set("class", I(w[4]));
        n.set("owner", I(w[5]));
        n.set("in", wordsJson(nd.in));
        n.set("out", wordsJson(nd.out));
        Value params = Value::array();
        for (std::size_t j = 0; j + 1 < nd.par.size(); j += 2) {
            Value pr = Value::array();
            pr.push(I(nd.par[j]));
            pr.push(dsp::wval(nd.par[j + 1]));
            params.push(pr);
        }
        n.set("params", params);
        Value tri = Value::array();
        for (const Tri& t3 : nd.tri) {
            Value x = Value::array();
            x.push(I(t3.a));
            x.push(wordsJson(t3.d));
            x.push(I(t3.x));
            tri.push(x);
        }
        n.set("tri", tri);
        n.set("mem", wordsJson(Words(w.begin() + 13, w.begin() + 19)));
        n.set("rec", I(w[19]));
        if (w[1] & 1) n.set("triFlag", I(1));  // node [1] = 2 * triples + 1 on some nodes
        nodes.push(n);
    }
    Value o = Value::object();
    o.set("e0", I(E.e[0]));
    o.set("e1", I(E.e[1]));
    o.set("e6", I(E.e[6]));
    o.set("nodes", nodes);
    Value recs = Value::array();
    for (std::size_t j = 0; j < E.recs.size(); j += 4) recs.push(wordsJson(Words(E.recs.begin() + j, E.recs.begin() + std::min(j + 4, E.recs.size()))));
    o.set("recs", recs);
    return o;
}

Kernel kernelFromJson(const Value& J) {
    Kernel k;
    const auto& nodes = items(mem(J, "nodes", "kernel"), "kernel nodes");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Value& n = nodes[i];
        const std::string t = mem(n, "type", "kernel node").asString();
        std::uint32_t tw = 0;
        char* end = nullptr;
        if (!t.empty() && t[0] == 'k') tw = 0x600000 + std::uint32_t(std::strtoul(t.c_str() + 1, &end, 10));
        else tw = std::uint32_t(std::strtoul(t.c_str(), &end, 16));
        if (t.empty() || !end || *end) throw BuildError("kernel node " + std::to_string(i) + ": bad type \"" + t + "\"");
        Node nd;
        for (const Value& p : items(mem(n, "params", "kernel node"), "kernel params")) {
            nd.par.push_back(raw(at(p, 0, "param"), "param id"));
            nd.par.push_back(dsp::jval(at(p, 1, "param")));
        }
        for (const Value& x : items(mem(n, "tri", "kernel node"), "kernel triples"))
            nd.tri.push_back({raw(at(x, 0, "triple"), "triple"), rawList(at(x, 1, "triple"), "triple data"), raw(at(x, 2, "triple"), "triple")});
        nd.in = rawList(mem(n, "in", "kernel node"), "kernel node in");
        nd.out = rawList(mem(n, "out", "kernel node"), "kernel node out");
        const Words memw = rawList(mem(n, "mem", "kernel node"), "kernel node mem");
        if (memw.size() != 6) throw BuildError("kernel node " + std::to_string(i) + ": mem is not 6 words");
        const std::uint32_t triFlag = n.get("triFlag") ? raw(*n.get("triFlag"), "triFlag") : 0;
        nd.w = {std::uint32_t(i), 2 * std::uint32_t(nd.tri.size()) + triFlag, 0, 0, raw(mem(n, "class", "kernel node"), "class"),
                raw(mem(n, "owner", "kernel node"), "owner"), tw, std::uint32_t(nd.in.size()), 0, std::uint32_t(nd.out.size()), 0,
                std::uint32_t(nd.par.size() / 2), 0};
        nd.w.insert(nd.w.end(), memw.begin(), memw.end());
        nd.w.push_back(raw(mem(n, "rec", "kernel node"), "rec"));
        k.nodes.push_back(std::move(nd));
    }
    const auto& recs = items(mem(J, "recs", "kernel"), "kernel recs");
    for (const Value& r : recs) {
        const Words x = rawList(r, "kernel rec");
        k.recs.insert(k.recs.end(), x.begin(), x.end());
    }
    k.e = {raw(mem(J, "e0", "kernel"), "e0"), raw(mem(J, "e1", "kernel"), "e1"), std::uint32_t(k.nodes.size()), 0,
           std::uint32_t(recs.size()), 0, J.get("e6") ? raw(*J.get("e6"), "e6") : 0};
    return k;
}

// ampgraph.check_kernel: rules the DSP graph builder relies on.
std::vector<std::string> checkKernel(const Value& J) {
    std::vector<std::string> probs;
    const std::uint32_t nb = raw(mem(J, "e1", "kernel"), "e1");
    const std::size_t nrecs = items(mem(J, "recs", "kernel"), "kernel recs").size();
    const auto& nodes = items(mem(J, "nodes", "kernel"), "kernel nodes");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Value& n = nodes[i];
        for (const char* key : {"in", "out"}) {
            const Words l = rawList(mem(n, key, "kernel node"), "buffer list");
            for (std::size_t j = 0; j + 1 < l.size(); ++j)
                if (l[j + 1] != l[j] + 1) {
                    probs.push_back("node " + std::to_string(i) + ": buffer list not consecutive");
                    break;
                }
            if (!l.empty() && *std::max_element(l.begin(), l.end()) >= nb)
                probs.push_back("node " + std::to_string(i) + ": buffer >= e1 (" + std::to_string(nb) + ")");
        }
        if (raw(mem(n, "owner", "kernel node"), "owner") > i) probs.push_back("node " + std::to_string(i) + ": owner after the node");
        if (raw(mem(n, "rec", "kernel node"), "rec") > nrecs) probs.push_back("node " + std::to_string(i) + ": record link past the records");
    }
    return probs;
}

Value attrsJson(const Rec4& r, std::uint32_t n, std::uint32_t off) {
    Value out = Value::array();
    for (const Attr& a : r.attrs(n, off)) {
        Value x = Value::array();
        x.push(Str(toUtf8(r.name(a[0]))));
        x.push(I(a[1]));
        x.push(a[1] == 7 ? Str(toUtf8(r.name(a[2]))) : dsp::wval(a[2]));
        out.push(x);
    }
    return out;
}

Value modelToJson(const Rec4& r, const Entry& e) {
    Value knobs = Value::array();
    for (std::uint32_t k = 0; k < e[7]; ++k) {
        Words w;
        for (int j = 0; j < 8; ++j) w.push_back(r.u32(std::uint64_t(e[11]) + 0x20 * k + 4 * j));
        Value o = Value::object();
        o.set("name", Str(toUtf8(r.name(w[2]))));
        o.set("attrs", attrsJson(r, w[0], w[1]));
        Value ww = wordsJson(Words(w.begin() + 3, w.begin() + 7));
        ww.push(dsp::wval(w[7]));
        o.set("w", ww);
        knobs.push(o);
    }
    Value states = Value::array();
    for (std::uint32_t k = 0; k < e[6]; ++k) {
        Words w;
        for (int j = 0; j < 5; ++j) w.push_back(r.u32(std::uint64_t(e[10]) + 0x14 * k + 4 * j));
        Value o = Value::object();
        o.set("name", Str(toUtf8(r.name(w[2]))));
        o.set("attrs", attrsJson(r, w[0], w[1]));
        o.set("w", wordsJson({w[3], w[4]}));
        states.push(o);
    }
    Value o = Value::object();
    o.set("sym", Str(toUtf8(r.name(e[2]))));
    o.set("attrs", attrsJson(r, e[0], e[1]));
    o.set("entry", wordsJson({e[3], e[4], e[5], e[8], e[9], e[12], e[13]}));
    o.set("knobs", knobs);
    o.set("states", states);
    return o;
}

std::vector<Attr> attrsFrom(Rec4& r, const Value& attrs) {
    std::vector<Attr> out;
    for (const Value& a : items(attrs, "attrs")) {
        const std::uint32_t name = r.sym(toLatin1(at(a, 0, "attr").asString()));
        const std::uint32_t t = raw(at(a, 1, "attr"), "attr type");
        const Value& v = at(a, 2, "attr");
        out.push_back({name, t, t == 7 ? r.sym(toLatin1(v.asString())) : dsp::jval(v)});
    }
    return out;
}

struct ModelRecords {
    std::vector<Words> knobs, states;
    std::vector<Attr> attrs;
};
// ampgraph.model_records: knob / state records and entry attributes as words (symbols added if new).
ModelRecords modelRecords(Rec4& r, const Value& J) {
    ModelRecords out;
    for (const Value& k : items(mem(J, "knobs", "model"), "knobs")) {
        const std::vector<Attr> a = attrsFrom(r, mem(k, "attrs", "knob"));
        const Value& x = mem(k, "w", "knob");
        const std::uint32_t packed = r.packAttrs(a);
        const std::uint32_t name = r.sym(toLatin1(mem(k, "name", "knob").asString()));
        out.knobs.push_back({std::uint32_t(a.size()), packed, name, raw(at(x, 0, "knob w"), "knob w"), raw(at(x, 1, "knob w"), "knob w"),
                             raw(at(x, 2, "knob w"), "knob w"), raw(at(x, 3, "knob w"), "knob w"), dsp::jval(at(x, 4, "knob w"))});
    }
    for (const Value& s : items(mem(J, "states", "model"), "states")) {
        const std::vector<Attr> a = attrsFrom(r, mem(s, "attrs", "state"));
        const Value& x = mem(s, "w", "state");
        const std::uint32_t packed = r.packAttrs(a);
        const std::uint32_t name = r.sym(toLatin1(mem(s, "name", "state").asString()));
        out.states.push_back({std::uint32_t(a.size()), packed, name, raw(at(x, 0, "state w"), "state w"), raw(at(x, 1, "state w"), "state w")});
    }
    out.attrs = attrsFrom(r, mem(J, "attrs", "model"));
    return out;
}

// ampgraph.m4_to_json: on-screen name and DSP cost from the stock M4 tables.
Value m4ToJson(const std::vector<std::uint8_t>& app, std::uint32_t sid) {
    constexpr std::uint32_t kSeg = 0x14308, kN = 0x337, kInfo = 0x280E0828, kCost = 0x2813BBA4;
    const auto off = [&](std::uint32_t va, std::size_t n) {
        const std::uint64_t o = std::uint64_t(kSeg) + va - 0x28000000;
        if (o + n > app.size()) throw BuildError("M4 app too short");
        return std::size_t(o);
    };
    Value o = Value::object();
    o.set("name", Value());
    o.set("cost", Value());
    for (std::uint32_t i = 0; i < kN; ++i)
        if (readLE32(app, off(kInfo + 44 * i, 4)) == sid) {
            const std::size_t p = off(kInfo + 44 * i + 4, 17);
            std::string name;
            for (std::size_t k = 0; k < 17 && app[p + k]; ++k) name += char(app[p + k]);
            o.set("name", Str(toUtf8(name)));
            break;
        }
    for (std::uint32_t i = 0; i < kN; ++i)
        if (readLE32(app, off(kCost + 12 * i, 4)) == sid) {
            o.set("cost", I(readLE32(app, off(kCost + 12 * i + 8, 4))));
            break;
        }
    return o;
}

// ---- rec 2: SHARC LDR stream (sharc_ldr.py) --------------------------------------------------------------------

struct LdrBlock {
    std::size_t off = 0;
    std::uint32_t tag = 0, count = 0, addr = 0;
};

std::uint64_t ldrPayload(std::uint32_t tag, std::uint64_t count) {
    switch (tag) {
    case 0: return 0x180 * 4;
    case 1: case 2: case 7: case 8: return 0;
    case 3: return ((count + 1) >> 1) * 4;
    case 4: case 9: case 10: return count * 4;
    case 5: return ((count & 1) * 2 + ((count * 3) >> 1)) * 4;
    case 6: return count * 8;
    }
    throw BuildError("rec 2: bad LDR tag " + std::to_string(tag));
}

std::vector<LdrBlock> parseLdr(const std::vector<std::uint8_t>& buf, std::uint64_t& end) {
    std::vector<LdrBlock> blocks;
    std::uint64_t p = 0x600;
    while (p + 12 <= buf.size()) {
        const std::uint32_t w0 = readBE32(buf, std::size_t(p)), w1 = readBE32(buf, std::size_t(p + 4)), w2 = readBE32(buf, std::size_t(p + 8));
        const std::uint32_t tag = w0 & 0xFF;
        blocks.push_back({std::size_t(p), tag, w1, w2});
        p += 12 + ldrPayload(tag, w1);
        if (tag == 0) break;
    }
    end = p;
    return blocks;
}

// newamp.dsp_replay: our LDR blocks before FINAL_INIT, the registry record (stock checked by hash), the trailer.
void replay(FirmwareParts& parts, const Value& pre, const Package& pkg, const std::string& moduleSym,
            std::vector<std::string>& log) {
    const auto file = [&pkg](const Value& v, const char* what) -> const std::vector<std::uint8_t>& {
        const std::vector<std::uint8_t>* b = pkg.file(v.asString());
        if (!b) throw BuildError(pkg.name + ": " + what + " " + v.asString() + " missing from the package");
        return *b;
    };
    std::vector<std::uint8_t>& boot = parts.bootLdr;
    std::uint64_t end = 0;
    const std::vector<LdrBlock> blocks = parseLdr(boot, end);
    if (blocks.empty() || end != boot.size() || blocks.back().tag != 0) throw BuildError("rec 2 must end with FINAL_INIT");
    const std::vector<std::uint8_t>& ins = file(mem(pre, "blocks", "DSP part"), "LDR blocks");
    std::vector<std::uint8_t> out(boot.begin(), boot.begin() + std::ptrdiff_t(blocks.back().off));
    out.insert(out.end(), ins.begin(), ins.end());
    out.insert(out.end(), boot.begin() + std::ptrdiff_t(blocks.back().off), boot.end());
    const Value& reg = mem(pre, "registry", "DSP part");
    const std::uint64_t dm = std::uint64_t(number(reg.get("dm"), "registry dm"));
    std::uint64_t end2 = 0;
    const std::vector<LdrBlock> nb = parseLdr(out, end2);
    std::vector<const LdrBlock*> hit;
    for (const LdrBlock& b : nb)
        if (b.tag == 4 && b.addr <= dm && dm < std::uint64_t(b.addr) + b.count) hit.push_back(&b);
    if (hit.size() != 1) throw BuildError("DSP registry record at " + hex32(dm) + ": not in exactly one block");
    const std::uint64_t pos = hit[0]->off + 12 + 4 * (dm - hit[0]->addr);
    if (pos + 32 > out.size()) throw BuildError("DSP registry record outside rec 2");
    const Bytes old(out.data() + pos, 32);
    if (toHex(sha256(old)) != mem(reg, "stock_sha256", "registry").asString())
        throw BuildError("DSP registry record at " + hex32(dm) + " is not stock");
    const auto& words = items(mem(reg, "words", "registry"), "registry words");
    if (words.size() != 8) throw BuildError("registry record: not 8 words");
    for (std::size_t k = 0; k < 8; ++k) {
        const std::uint32_t v = std::uint32_t(number(&words[k], "registry word"));
        for (int b = 0; b < 4; ++b) out[std::size_t(pos + 4 * k + b)] = std::uint8_t(v >> (24 - 8 * b));
    }
    if (out.size() > 0x80000) throw BuildError("rec 2 too big for its partition");
    boot = std::move(out);
    std::string trailer;
    if (!moduleSym.empty() && pre.get("trailer") && pre.get("trailer")->isString()) {
        const std::vector<std::uint8_t>& t = file(*pre.get("trailer"), "trailer");
        const std::string suffix = "_" + moduleSym + ".bin";
        std::vector<std::size_t> found;
        for (std::size_t i = 0; i < parts.moduleFiles.size(); ++i) {
            const std::string& f = parts.moduleFiles[i];
            if (f.size() >= suffix.size() && f.compare(f.size() - suffix.size(), suffix.size(), suffix) == 0) found.push_back(i);
        }
        if (found.size() != 1) throw BuildError("module " + moduleSym + ": not exactly one module file");
        auto& mod = parts.modules.modules[found[0]];
        mod.insert(mod.end(), t.begin(), t.end());
        trailer = ", " + moduleSym + " trailer";
    }
    log.push_back("   DSP part (prebuilt): " + std::to_string(ins.size()) + " bytes of LDR blocks, registry " + hex32(dm) + trailer);
}

// ---- harness (newamp.raise_placeholder) ------------------------------------------------------------------------

const char* const kSlotFields[] = {"settings", "state", "bigbuf", "scratch", "nodes", "recs"};

std::array<std::uint64_t, 6> kernelTotals(const Core& S, std::size_t k) {
    const Kernel& e = S.kernels.at(k);
    std::array<std::uint64_t, 6> t{};
    for (const Node& nd : e.nodes)
        for (int i = 0; i < 6; ++i) t[i] += nd.w[13 + i];
    if (e.recs.size() % 4) throw BuildError("core kernel records: not 4 words each");
    for (std::size_t j = 0; j < e.recs.size(); j += 4)
        for (int i = 0; i < 3; ++i) t[i] += e.recs[j + 1 + i];
    t[3] += 1;
    t[4] += e.e[2];
    t[5] += e.e[4];
    return t;
}

std::string hexList(const std::array<std::uint64_t, 6>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + hex32(v[i]);
    return s + "]";
}

void raisePlaceholder(Core& S, const Value& h, std::vector<std::string>& log) {
    const std::size_t k = std::size_t(number(h.get("kernel"), "harness kernel"));
    const std::size_t n = std::size_t(number(h.get("node"), "harness node"));
    if (k >= S.kernels.size() || n >= S.kernels[k].nodes.size()) throw BuildError("harness: no kernel " + std::to_string(k) + " node " + std::to_string(n));
    std::vector<std::pair<std::string, const Value*>> fields;
    if (const Value* f = h.get("fields"); f && f->isObject()) {
        for (std::size_t i = 0; i < f->keys.size(); ++i) fields.emplace_back(f->keys[i], &f->items[i]);
    } else {
        fields.emplace_back(mem(h, "field", "harness").asString(), h.get("value"));
    }
    const std::array<std::uint64_t, 6> stock = kernelTotals(S, k);
    Words& w = S.kernels[k].nodes[n].w;
    for (const auto& [name, val] : fields) {
        const auto f = std::find_if(std::begin(kSlotFields), std::end(kSlotFields), [&](const char* x) { return name == x; });
        if (f == std::end(kSlotFields)) throw BuildError("harness: unknown field " + name);
        const std::size_t fi = std::size_t(f - std::begin(kSlotFields));
        const std::int64_t v = number(val, "harness value");
        if (v < std::int64_t(w[13 + fi]) || v > 0xFFFFFFFFll)
            throw BuildError("harness " + std::to_string(k) + " node " + std::to_string(n) + ": " + name + " below stock");
        log.push_back("harness kernel " + std::to_string(k) + " node " + std::to_string(n) + ": " + name + " " + hex32(w[13 + fi]) + " -> " + hex32(std::uint64_t(v)));
        w[13 + fi] = std::uint32_t(v);
    }
    const std::array<std::uint64_t, 6> t = kernelTotals(S, k);
    const std::uint32_t slot = 0x600000 + std::uint32_t(number(h.get("slot_kernel"), "harness slot_kernel"));
    std::vector<std::array<std::uint64_t, 6>> hosts;
    for (std::size_t r : {0u, 1u}) {
        if (r >= S.kernels.size()) continue;
        for (const Node& nd : S.kernels[r].nodes) {
            if (nd.w[6] != slot) continue;
            std::array<std::uint64_t, 6> b;
            for (int i = 0; i < 6; ++i) b[i] = nd.w[13 + i];
            bool fits = true;
            for (int i = 0; i < 6; ++i) fits = fits && stock[i] <= b[i];
            if (fits) hosts.push_back(b);
        }
    }
    if (hosts.empty()) throw BuildError("stock harness " + std::to_string(k) + " fits no system slot of type " + hex32(slot));
    for (const auto& b : hosts)
        for (int i = 0; i < 6; ++i)
            if (t[i] > b[i])
                throw BuildError("harness " + std::to_string(k) + " totals " + hexList(t) + " exceed slot " + hexList(b) + ": " + kSlotFields[i]);
    log.push_back("   totals " + hexList(t) + " fit the " + std::to_string(hosts.size()) + " system slots it can go into");
}

// ---- the build -------------------------------------------------------------------------------------------------

Value decompile(const FirmwareParts& stock, const Rec4& r0, const Core& s0, const std::string& sym) {
    const Entry& e = r0.entry(sym);
    if (e[13] >= stock.modules.modules.size()) throw BuildError(toUtf8(sym) + ": its DSP module is missing");
    const Value mod = moduleToJson(u32s(stock.modules.modules[e[13]], "module"));
    const std::uint32_t kid = raw(mem(mem(mod, "kroot", "module"), "kid", "kroot"), "kid");
    Value kernels = Value::object();
    if (kid >> 16 == 0x60) {
        if ((kid & 0xFFFF) >= s0.kernels.size()) throw BuildError(toUtf8(sym) + ": kernel " + std::to_string(kid & 0xFFFF) + " not in the core");
        kernels.set(std::to_string(kid & 0xFFFF), kernelToJson(s0.kernels[kid & 0xFFFF]));
    }
    Value g = Value::object();
    g.set("format", Str("ampgraph-1"));
    g.set("model", modelToJson(r0, e));
    g.set("m4", m4ToJson(stock.app, e[2]));
    g.set("kernels", kernels);
    g.set("module", mod);
    return g;
}

Value loadGraph(const FirmwareParts& stock, const Rec4& r0, const Core& s0, const ModelSpec& m) {
    const std::vector<std::uint8_t>* b = m.package->file(m.graph);
    if (!b) throw BuildError(m.package->name + ": " + m.graph + " missing from the package");
    std::string err;
    const std::optional<Value> j = json::parse(std::string_view(reinterpret_cast<const char*>(b->data()), b->size()), &err);
    if (!j) throw BuildError(m.graph + ": " + err);
    Value g = *j;
    if (const Value* f = g.get("format"); f && f->asString() == "ampgraph-diff-1") {  // derived from the user's stock model
        const Value base = decompile(stock, r0, s0, toLatin1(mem(g, "base", "graph").asString()));
        g = dsp::patch(&base, g.get("diff"));
    }
    if (!g.get("format") || g.get("format")->asString() != "ampgraph-1") throw BuildError(m.graph + ": not an ampgraph-1 graph");
    return g;
}

} // namespace

namespace dsp {

Value wval(std::uint32_t w) {
    if ((w > 0x30000000u && w < 0x50000000u) || (w > 0xB0000000u && w < 0xD0000000u)) {
        float f;
        std::memcpy(&f, &w, 4);
        if (std::isfinite(f)) return Value::real(double(f));
    }
    return I(w >= 0x80000000u ? std::int64_t(w) - (std::int64_t(1) << 32) : std::int64_t(w));
}

std::uint32_t jval(const Value& v) {
    if (!v.isNumber()) throw BuildError("graph: value is not a number");
    if (v.integer) return std::uint32_t(std::uint64_t(v.asInt()) & 0xFFFFFFFFu);
    const float f = float(v.n);
    std::uint32_t w;
    std::memcpy(&w, &f, 4);
    return w;
}

Value patch(const Value* s, const Value* d) {
    if (!d) {
        if (!s) throw BuildError("graph difference: refers to a value that isn't there");
        return *s;
    }
    if (!d->isObject()) throw BuildError("graph difference: bad entry");
    if (const Value* v = d->get("$val")) return *v;
    if (const Value* dd = d->get("$dict")) {
        static const Value empty = Value::object();
        const Value& base = s && s->isObject() ? *s : empty;
        std::vector<std::string> keys = base.keys;
        if (const Value* k = d->get("$keys")) {
            keys.clear();
            for (const Value& x : items(*k, "$keys")) keys.push_back(x.asString());
        }
        Value out = Value::object();
        for (const std::string& key : keys) out.set(key, patch(base.get(key), dd->get(key)));
        return out;
    }
    if (const Value* dl = d->get("$list")) {
        static const Value empty = Value::array();
        const Value& base = s && s->isArray() ? *s : empty;
        const std::int64_t n = number(d->get("$len"), "graph difference $len");
        if (n < 0 || n > 1000000) throw BuildError("graph difference: bad $len");
        Value out = Value::array();
        for (std::int64_t i = 0; i < n; ++i)
            out.push(patch(std::size_t(i) < base.items.size() ? &base.items[std::size_t(i)] : nullptr, dl->get(std::to_string(i))));
        return out;
    }
    throw BuildError("graph difference: unknown entry");
}

Words repackCore(const Words& core) { return packCore(parseCore(core)); }

bool coreBoundsOk(const Words& C) {
    const std::uint64_t n = C.size();
    if (n < kCoreHdr) return false;
    if (std::uint64_t(C[1]) + C[2] > n || std::uint64_t(C[0xF]) * 7 + C[0x10] > n || std::uint64_t(C[0xC]) * 4 + C[0xD] > n) return false;
    std::vector<Words> ents{Words(C.begin() + 8, C.begin() + 15)};
    for (std::uint32_t k = 0; k < C[0xF]; ++k) ents.emplace_back(C.begin() + C[0x10] + 7 * k, C.begin() + C[0x10] + 7 * k + 7);
    for (const Words& e : ents) {
        if (std::uint64_t(e[2]) * 20 + e[3] > n || std::uint64_t(e[4]) * 4 + e[5] > n) return false;
        for (std::uint32_t i = 0; i < e[2]; ++i) {
            const std::uint64_t b = std::uint64_t(e[3]) + 20 * i;
            const auto nd = [&](int j) { return C[std::size_t(b + j)]; };
            if (std::uint64_t(nd(7)) + nd(8) > n || std::uint64_t(nd(9)) + nd(10) > n || 2 * std::uint64_t(nd(11)) + nd(12) > n) return false;
            if (std::uint64_t(nd(1) >> 1) * 3 + nd(2) > n) return false;
            for (std::uint32_t t = 0; t < nd(1) / 2; ++t) {
                const std::uint32_t a = C[std::size_t(nd(2) + 3 * t)], bb = C[std::size_t(nd(2) + 3 * t + 1)];
                if (std::uint64_t(a & ~1u) + bb > n) return false;
            }
        }
    }
    return true;
}

Words rebuildModule(const Words& module) { return moduleFromJson(moduleToJson(module)); }

} // namespace dsp

DspResult buildDsp(const FirmwareParts& stock, const BuildSpec& spec) {
    DspResult res;
    res.parts = stock;
    FirmwareParts& P = res.parts;
    auto& log = res.log;
    const Rec4 r0(stock.moduleMeta);
    if (stock.modules.modules.size() != r0.ents.size() || stock.modules.modules.empty())
        throw BuildError("the firmware's DSP modules don't match its rec 4 (not a stock file?)");
    const Core s0 = parseCore(u32s(stock.modules.modules[0], "core"));

    Rec4 r(stock.moduleMeta);
    Core S = s0;
    struct Added {
        std::uint32_t sid, idx, nk, ns;
        Entry like;
    };
    std::vector<Added> added;
    for (const ModelSpec& m : spec.models) {
        const std::string sym = toLatin1(m.sym), name = toLatin1(m.name);
        if (name.empty() || name.size() > 16) throw BuildError(m.sym + ": name must be 1 to 16 characters");
        if (r.ids.count(sym)) throw BuildError(m.sym + " already exists");
        const Value J = loadGraph(stock, r0, s0, m);
        const Value& ks = mem(J, "kernels", "graph");
        if (!ks.isObject() || ks.items.size() != 1) throw BuildError(m.sym + ": the graph must have exactly one kernel");
        const Value& KJ = ks.items[0];
        const std::vector<std::string> probs = checkKernel(KJ);
        if (!probs.empty()) {
            std::string s = m.sym + ": kernel check failed:";
            for (const std::string& p : probs) s += "\n  " + p;
            throw BuildError(s);
        }
        S.kernels.push_back(kernelFromJson(KJ));
        const std::uint32_t kid = std::uint32_t(S.kernels.size() - 1);
        Value mod = mem(J, "module", "graph");
        for (std::size_t i = 0; i < mod.keys.size(); ++i)
            if (mod.keys[i] == "kroot") mod.items[i].set("kid", I(0x600000 + kid));
        const std::uint32_t res5 = std::uint32_t(P.modules.modules.size());
        char fname[64];
        std::snprintf(fname, sizeof fname, "dsp/modules/%03u_", unsigned(res5));
        P.modules.modules.push_back(packWords(moduleFromJson(mod)));
        P.moduleFiles.push_back(fname + m.sym + ".bin");
        const Entry like = r.entry(toLatin1(m.like));
        const std::uint32_t sid = r.sym(sym);  // first, so the ID doesn't depend on the knob symbols
        ModelRecords mr = modelRecords(r, mem(J, "model", "graph"));
        for (Attr& a : mr.attrs)
            if (r.name(a[0]) == "displayName") a[2] = r.sym(name);
        Entry e = like;
        e[0] = std::uint32_t(mr.attrs.size());
        e[1] = r.packAttrs(mr.attrs);
        e[2] = sid;
        e[3] = std::uint32_t(r.ents.size());
        e[13] = res5;
        std::vector<std::uint8_t> kb, sb;
        for (const Words& k : mr.knobs)
            for (std::uint32_t v : k) addLE32(kb, v);
        for (const Words& s : mr.states)
            for (std::uint32_t v : s) addLE32(sb, v);
        e[7] = std::uint32_t(mr.knobs.size());
        e[11] = r.append(kb);
        e[6] = std::uint32_t(mr.states.size());
        e[10] = r.append(sb);
        r.ents.push_back(e);
        added.push_back({sid, e[3], e[7], e[6], like});

        M4Model mm;
        mm.sym = m.sym;
        mm.name = m.name;
        mm.sid = sid;
        mm.index = e[3];
        mm.knobs = e[7];
        mm.states = e[6];
        mm.modelNo = m.modelNo;
        if (m.cost) mm.cost = *m.cost;
        else if (const Value* c = mem(J, "m4", "graph").get("cost"); c && c->isNumber()) mm.cost = std::uint32_t(c->asInt());
        else throw BuildError(m.sym + ": no DSP cost");
        const auto after = r0.ids.find(toLatin1(m.after));
        if (after == r0.ids.end()) throw BuildError(m.sym + ": \"after\" model " + m.after + " isn't in this firmware");
        mm.afterSid = after->second;
        mm.like = like;
        res.models.push_back(mm);
        log.push_back("   " + m.name + ": symId " + hex32(sid) + ", modelNo " + std::to_string(m.modelNo) + ", rec 4 entry " +
                      std::to_string(e[3]) + ", res " + std::to_string(res5) + ", kernel " + std::to_string(kid) + ", " +
                      std::to_string(e[7]) + " knobs");
    }
    for (const Value* h : spec.harness) raisePlaceholder(S, *h, log);
    const Words core = packCore(S);
    if (!dsp::coreBoundsOk(core) || core.size() > kCoreMax) throw BuildError("DSP core too big (" + hex32(core.size()) + " words, max " + hex32(kCoreMax) + ")");
    std::vector<std::uint8_t> meta = r.finish();

    // allowed-model lists: each new index wherever its "like" is listed (new list copies at the end)
    const Rec4 rf(meta);
    std::vector<std::pair<std::uint64_t, Words>> childLists;  // snapshot first, as newamp does
    for (const Entry& e : rf.ents)
        for (std::uint32_t k = 0; k < e[5]; ++k) {
            const std::uint64_t ro = std::uint64_t(e[9]) + 0x20 * k;
            Words rec;
            for (int j = 0; j < 8; ++j) rec.push_back(rf.u32(ro + 4 * j));
            childLists.emplace_back(ro, rec);
        }
    std::map<std::string, int> grown;
    for (auto& [ro, rec] : childLists) {
        Words L;
        for (std::uint32_t j = 0; j < rec[5]; ++j) {
            if (std::uint64_t(rec[6]) + 4 * j + 4 > meta.size()) throw BuildError("rec 4: allowed-model list outside the record");
            L.push_back(readLE32(meta, rec[6] + 4 * j));
        }
        Words fresh;
        for (const Added& a : added)
            if (std::find(L.begin(), L.end(), a.like[3]) != L.end() && std::find(L.begin(), L.end(), a.idx) == L.end()) fresh.push_back(a.idx);
        if (fresh.empty()) continue;
        L.insert(L.end(), fresh.begin(), fresh.end());
        while (meta.size() % 4) meta.push_back(0);
        rec[5] = std::uint32_t(L.size());
        rec[6] = std::uint32_t(meta.size());
        for (std::uint32_t v : L) addLE32(meta, v);
        for (int j = 0; j < 8; ++j) putLE32(meta, std::size_t(ro + 4 * j), rec[j]);
        ++grown[toUtf8(r.name(rec[2]))];
    }
    putLE32(meta, 8, std::uint32_t(meta.size()));
    P.moduleMeta = std::move(meta);
    P.modules.modules[0] = packWords(core);
    std::string g;
    for (const auto& [k, v] : grown) g += (g.empty() ? "" : ", ") + k + ": " + std::to_string(v);
    log.insert(log.begin(), "DSP: core " + hex32(core.size()) + " words (max " + hex32(kCoreMax) + "); allowed lists grown: {" + g + "}");

    // our DSP code: polymidi first, then the NAM block (newamp's order)
    if (spec.polymidi) {
        const Value* pre = spec.polymidi->get("prebuilt");
        if (!pre) throw BuildError("polymidi: only prebuilt (package) DSP parts can be applied");
        replay(P, *pre, *spec.polymidiPackage, "", log);
    }
    if (spec.namblock) {
        const Value* pre = spec.namblock->get("prebuilt");
        if (!pre) throw BuildError("namblock: only prebuilt (package) DSP parts can be applied");
        const Value* modSym = spec.namblock->get("module");
        replay(P, *pre, *spec.namblockPackage, modSym ? toLatin1(modSym->asString()) : "", log);
    }
    return res;
}

// ---- checks (newamp.check, modcheck.check_words) ---------------------------------------------------------------

namespace {

std::uint64_t ktreeCount(const Words& w, std::uint64_t e, int depth) {
    if (depth > 32) throw BuildError("kernel tree too deep");
    std::uint64_t n = 1;
    const std::uint32_t list = word(w, e + 2, "kernel tree"), count = word(w, e + 3, "kernel tree");
    for (std::uint32_t i = 0; i < count; ++i) n += ktreeCount(w, std::uint64_t(list) + 4 * i, depth + 1);
    return n;
}

// modcheck.totals: recursive (C, nodes, objs, A, links, knobs) of the control section at c; checks sub-section reserves.
std::array<std::uint64_t, 6> ctrlTotals(const Words& w, std::uint64_t base, std::uint64_t c, std::vector<std::string>& p,
                                        const std::string& where, int depth = 0) {
    if (depth > 32) throw BuildError("control section nested too deep");
    const Words h = sub(w, c, 24, "control section");
    std::uint64_t s4 = 0, s5 = 0;
    for (std::uint32_t k = 0; k < h[18]; ++k) {
        const Words r = sub(w, base + h[0] + 10 * std::uint64_t(k), 10, "control record");
        s4 += r[4], s5 += r[5];
    }
    const auto need = [&](bool ok, const std::string& msg) {
        if (!ok) p.push_back(where + ": " + msg);
    };
    need(h[13] == h[18], "h13 != h18");
    need(h[12] == s5, "h12 != sum of record state words");
    need(h[14] == s4 + h[16] + h[17], "h14 != sum of record outputs + h16 + h17");
    need(std::uint64_t(h[15]) == std::uint64_t(h[20]) + h[21] + h[22], "h15 != edges + kmapA + kmapB");
    need(h[16] == h[21] && h[17] == h[22], "h16/h17 != kmap counts");
    std::array<std::uint64_t, 6> t{h[12], h[13], h[23], h[14], h[15], std::uint64_t(h[16]) + h[17]};
    for (std::uint32_t k = 0; k < h[23]; ++k) {
        const Words e = sub(w, base + h[10] + 8 * std::uint64_t(k), 8, "control sub-section");
        if (!e[0]) continue;
        const std::array<std::uint64_t, 6> s = ctrlTotals(w, base, base + e[0], p, where + ".sub" + std::to_string(k), depth + 1);
        bool fits = true;
        for (int j = 0; j < 6; ++j) fits = fits && e[2 + j] >= s[j];
        need(fits, "sub" + std::to_string(k) + " reserve below what it needs");
        for (int j = 0; j < 6; ++j) t[j] += s[j];
    }
    return t;
}

// Model records with their attribute lists resolved (pointers ignored): ampgraph.resolved_model, flattened.
Words resolvedModel(const Rec4& r, const Entry& e) {
    Words out;
    const auto attrs = [&](std::uint32_t n, std::uint32_t off) {
        out.push_back(n);
        for (const Attr& a : r.attrs(n, off)) out.insert(out.end(), a.begin(), a.end());
    };
    out.push_back(e[7]);
    for (std::uint32_t k = 0; k < e[7]; ++k) {
        const std::uint64_t at = std::uint64_t(e[11]) + 0x20 * k;
        attrs(r.u32(at), r.u32(at + 4));
        for (int j = 2; j < 8; ++j) out.push_back(r.u32(at + 4 * j));
    }
    out.push_back(e[6]);
    for (std::uint32_t k = 0; k < e[6]; ++k) {
        const std::uint64_t at = std::uint64_t(e[10]) + 0x14 * k;
        attrs(r.u32(at), r.u32(at + 4));
        for (int j = 2; j < 5; ++j) out.push_back(r.u32(at + 4 * j));
    }
    attrs(e[0], e[1]);
    return out;
}

} // namespace

std::vector<std::string> checkModule(const Words& w) {
    std::vector<std::string> p;
    const std::uint64_t n = w.size();
    if (n < 12 || w[0] != 3) return {"not a DSP module (type 3)"};
    if (n > 0xD2C0) p.push_back(std::to_string(n) + " words > loader limit 0xd2c0");
    if (w[1] & 1) p.push_back("header word 1 bit 0 set (loader rejects)");
    const std::uint32_t kblk = w[2], kcount = w[3], roff = w[4], rcnt = w[5], ctrl = w[6], clen = w[7], nmoff = w[8],
                        nmcnt = w[9], boff = w[10], bcnt = w[11];
    try {
        if (ktreeCount(w, kblk, 0) != kcount) p.push_back("kernel-block entries != header word 3");
    } catch (const BuildError&) {
        p.push_back("kernel-block tree runs off the end");
    }
    if (std::uint64_t(ctrl) + clen != boff) p.push_back("ctrlOff + ctrlLen != bindOff");
    try {
        if (ctrlLen(w, ctrl, ctrl) != clen) p.push_back("control section length != header word 7");
    } catch (const BuildError&) {
        p.push_back("control section runs off the end");
    }
    const auto at = [&](std::uint64_t i) { return i < n ? w[std::size_t(i)] : 0xFFFFFFFFu; };
    if (w[1] & 2)
        for (std::uint32_t i = 0; i < rcnt; ++i) {
            const std::uint32_t t = at(std::uint64_t(roff) + 2 * i), idx = at(std::uint64_t(roff) + 2 * i + 1);
            if (t != 1 && t != 2) p.push_back("reloc " + std::to_string(i) + ": type " + std::to_string(t));
            else if (idx >= n || at(idx) >= n || std::uint64_t(at(idx)) + at(at(idx)) >= n)
                p.push_back("reloc " + std::to_string(i) + ": target out of range");
        }
    const std::uint32_t nsubs = std::uint64_t(ctrl) + 23 < n ? w[ctrl + 23] : 0;
    for (std::uint32_t i = 0; i < nmcnt; ++i) {
        const std::uint32_t a = at(std::uint64_t(nmoff) + 2 * i), b = at(std::uint64_t(nmoff) + 2 * i + 1);
        if (a >= n || b >= n) p.push_back("node map " + std::to_string(i) + ": offset out of range");
        else if (at(b) < 1 || at(std::uint64_t(b) + 1) < 1 || at(std::uint64_t(b) + 1) > nsubs)
            p.push_back("node map " + std::to_string(i) + ": control path has no sub-section");
    }
    for (std::uint32_t i = 0; i < bcnt; ++i)
        if (at(std::uint64_t(boff) + 4 * i + 2) >= n) p.push_back("bind " + std::to_string(i) + ": cell out of range");
    try {
        ctrlTotals(w, ctrl, ctrl, p, "top");
    } catch (const BuildError&) {
        p.push_back("control section runs off the end");
    }
    return p;
}

std::vector<std::string> checkBuild(const FirmwareParts& stock, const FirmwareParts& built, const std::vector<M4Model>& models) {
    constexpr std::uint32_t kSeg = 0x14308, kN = 0x337, kInfo = 0x280E0828, kInfoPtrLit = 0x280E0824, kCost = 0x2813BBA4;
    constexpr std::uint32_t kCostLits[] = {0x2813B708, 0x2813B754, 0x2813B76C, 0x2813B860, 0x2813B918};
    constexpr std::uint32_t kCostEndLit = 0x2813B750;
    constexpr std::uint32_t kCatNSub = 0x2813AE18, kCatPtrs = 0x2813ABB8, kCatCnts = 0x2813ACE8;
    constexpr std::uint32_t kPtrsLit = 0x2812d7ec, kCntsLit = 0x2812d918;
    struct Count {
        std::uint32_t va;
        int rd;
        bool last;
    };
    constexpr Count kCounts[] = {{0x280E0784, 12, false}, {0x2813B740, 1, true}, {0x2813B758, 0, false},
                                 {0x2813B7CA, 12, true}, {0x2813B872, 12, true}};
    std::vector<std::string> bad;
    try {
        const auto off = [&](const std::vector<std::uint8_t>& app, std::uint32_t va, std::size_t len) {
            const std::uint64_t o = std::uint64_t(kSeg) + va - 0x28000000;
            if (va < 0x28000000 || o + len > app.size()) throw BuildError("M4 address " + hex32(va) + " outside the app");
            return std::size_t(o);
        };
        const auto u32 = [&](const std::vector<std::uint8_t>& app, std::uint32_t va) { return readLE32(app, off(app, va, 4)); };
        const auto raw = [&](const std::vector<std::uint8_t>& app, std::uint32_t va, std::size_t len) {
            const std::size_t o = off(app, va, len);
            return std::vector<std::uint8_t>(app.begin() + std::ptrdiff_t(o), app.begin() + std::ptrdiff_t(o + len));
        };
        const std::vector<std::uint8_t>&st = stock.app, &a = built.app;
        const std::uint32_t n = kN + std::uint32_t(models.size());

        for (const Count& c : kCounts) {
            const std::uint32_t v = c.last ? n - 1 : n;
            if (raw(a, c.va, 4) != std::vector<std::uint8_t>{0x40, 0xF2, std::uint8_t(v & 0xFF), std::uint8_t(((v >> 8) & 7) << 4 | c.rd)})
                bad.push_back("count patch " + hex32(c.va));
        }
        // FUN_280e077c as patched: n entries of 20 bytes from the info pointer
        const std::uint32_t base = u32(a, kInfoPtrLit);
        using Info = std::array<std::uint32_t, 6>;
        const auto info = [&](std::uint32_t sid) -> std::optional<Info> {
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::vector<std::uint8_t> e = raw(a, base + 20 * i, 20);
                if (readLE32(e, 0) == sid) return Info{readLE32(e, 4), readLE32(e, 8), readLE32(e, 12), e[16], e[17], e[18]};
            }
            return std::nullopt;
        };
        const std::uint32_t cva = u32(a, kCostLits[0]);
        bool ptrs = u32(a, kCostEndLit) == cva + 12 * n;
        for (std::uint32_t va : kCostLits) ptrs = ptrs && u32(a, va) == cva;
        if (!ptrs) bad.push_back("cost table pointers");
        using Cost = std::array<std::uint32_t, 4>;  // symId, modelNo, 0, cost
        const auto costAt = [&](const std::vector<std::uint8_t>& app, std::uint32_t va) {
            const std::vector<std::uint8_t> e = raw(app, va, 12);
            return Cost{readLE32(e, 0), readLE16(e, 4), readLE16(e, 6), readLE32(e, 8)};
        };
        std::vector<Cost> T;
        for (std::uint32_t i = 0; i < n; ++i) T.push_back(costAt(a, cva + 12 * i));
        std::stable_sort(T.begin(), T.end(), [](const Cost& x, const Cost& y) { return x[0] < y[0]; });
        // appDspUsageTable_2813b868 as patched: binary search with hi = n - 1
        const auto bsearch = [&](std::uint32_t sid) -> std::optional<Cost> {
            std::uint32_t lo = 0, hi = n - 1;
            for (int guard = 0; guard < 64; ++guard) {
                const std::uint32_t mid = (lo + hi) / 2;
                if (hi - lo == 1) {
                    if (T[hi][0] == sid) return T[hi];
                    if (T[lo][0] == sid) return T[lo];
                    return std::nullopt;
                }
                if (T[mid][0] == sid) return T[mid];
                if (lo == hi) return std::nullopt;
                if (sid <= T[mid][0]) hi = mid;
                else lo = mid;
            }
            return std::nullopt;
        };
        for (std::uint32_t i = 0; i < kN; ++i) {
            const std::vector<std::uint8_t> e = raw(st, kInfo + 44 * i, 44);
            const std::uint32_t sid = readLE32(e, 0);
            const Info want{readLE32(e, 0x18), readLE32(e, 0x1C), readLE32(e, 0x28), e[0x15], e[0x16], e[0x24]};
            if (info(sid) != want) bad.push_back("info " + hex32(sid));
            const Cost c = costAt(st, kCost + 12 * i);
            if (bsearch(c[0]) != c) bad.push_back("cost " + hex32(c[0]));
        }
        for (const M4Model& m : models) {
            if (!info(m.sid)) bad.push_back("info missing " + m.sym);
            if (bsearch(m.sid) != Cost{m.sid, m.modelNo, 0, m.cost}) bad.push_back("cost missing " + m.sym);
        }
        std::set<std::uint32_t> sids, nos;
        for (const Cost& c : T) sids.insert(c[0]), nos.insert(c[1]);
        if (sids.size() != n || nos.size() != n) bad.push_back("duplicate symId or modelNo in the cost table");

        // browser lists: every stock one unchanged apart from the added models
        std::set<std::uint32_t> newSids;
        for (const M4Model& m : models) newSids.insert(m.sid);
        const std::uint32_t bp = u32(a, kPtrsLit), bc = u32(a, kCntsLit);
        for (std::uint32_t cat = 0; cat < 19; ++cat)
            for (std::uint32_t s = 0; s < u32(st, kCatNSub + 4 * cat); ++s) {
                const std::uint32_t sp = u32(st, kCatPtrs + 16 * cat + 4 * s);
                if (!sp) continue;
                const std::vector<std::uint8_t> S = raw(st, sp, 48 * std::size_t(u32(st, kCatCnts + 16 * cat + 4 * s)));
                const std::vector<std::uint8_t> L = raw(a, u32(a, bp + 16 * cat + 4 * s), 48 * std::size_t(u32(a, bc + 16 * cat + 4 * s)));
                std::vector<std::uint8_t> rest;
                for (std::size_t k = 0; k + 48 <= L.size(); k += 48)
                    if (!newSids.count(readLE32(L, k + 4))) rest.insert(rest.end(), L.begin() + std::ptrdiff_t(k), L.begin() + std::ptrdiff_t(k + 48));
                if (rest != S) bad.push_back("browser list " + std::to_string(cat) + "/" + std::to_string(s) + " changed");
            }

        // rec 4: no stock model changed; each new one in an allowed-model list; its module passes the loader's rules
        const Rec4 R(built.moduleMeta), R0(stock.moduleMeta);
        std::map<std::uint32_t, const Entry*> bySid;
        for (const Entry& e : R.ents) bySid[e[2]] = &e;
        for (const Entry& e0 : R0.ents) {
            const auto it = bySid.find(e0[2]);
            if (it == bySid.end() || resolvedModel(R, *it->second) != resolvedModel(R0, e0) || (*it->second)[13] != e0[13] ||
                (*it->second)[3] != e0[3])
                bad.push_back("rec 4 " + toUtf8(R0.name(e0[2])) + " changed");
        }
        for (const M4Model& m : models) {
            const auto it = bySid.find(m.sid);
            if (it == bySid.end()) {
                bad.push_back(m.sym + " missing from rec 4");
                continue;
            }
            const std::uint32_t idx = (*it->second)[3];
            bool listed = false;
            for (const Entry& e : R.ents)
                for (std::uint32_t k = 0; k < e[5] && !listed; ++k) {
                    const std::uint64_t ro = std::uint64_t(e[9]) + 0x20 * k;
                    const std::uint32_t cnt = R.u32(ro + 20), lst = R.u32(ro + 24);
                    for (std::uint32_t j = 0; j < cnt && !listed; ++j) listed = R.u32(std::uint64_t(lst) + 4 * j) == idx;
                }
            if (!listed) bad.push_back(m.sym + " in no allowed-model list");
            const std::uint32_t res = (*it->second)[13];
            if (res >= built.modules.modules.size()) {
                bad.push_back(m.sym + ": its DSP module is missing");
                continue;
            }
            const std::string name = res < built.moduleFiles.size() ? built.moduleFiles[res] : m.sym;
            for (const std::string& q : checkModule(u32s(built.modules.modules[res], name))) bad.push_back(name + ": " + q);
        }
    } catch (const BuildError& e) {
        bad.push_back(e.what());
    }
    return bad;
}

ModBuild buildMods(const FirmwareParts& stock, const BuildSpec& spec, const std::string& commit, bool dirty,
                   const BuildProgress& progress) {
    if (progress) progress(0.0, "Building the DSP side (models, modules)");
    DspResult d = buildDsp(stock, spec);
    if (progress) progress(0.6, "Building the M4 app (browser, tables, hooks)");
    M4Result m4 = buildM4(stock.app, spec, d.models, commit, dirty);
    if (progress) progress(0.8, "Checking the build");
    ModBuild out;
    out.parts = std::move(d.parts);
    out.parts.app = std::move(m4.app);
    if (!out.parts.appShared) out.parts.appCopy = out.parts.app;
    out.tag = m4.tag;
    out.dspLog = d.log;
    out.m4Log = m4.log;
    out.log = std::move(d.log);
    out.log.insert(out.log.end(), m4.log.begin(), m4.log.end());
    const std::vector<std::string> bad = checkBuild(stock, out.parts, d.models);
    if (!bad.empty()) {
        std::string s = "the build failed its checks (" + std::to_string(bad.size()) + "):";
        for (std::size_t i = 0; i < bad.size() && i < 12; ++i) s += "\n  " + bad[i];
        throw BuildError(s);
    }
    out.log.push_back("checks: OK");
    out.models = std::move(d.models);
    if (progress) progress(1.0, "Checks passed");
    return out;
}

} // namespace hx::mods

#include "core/modspec.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace hx::mods {

namespace {

const json::Value& need(const json::Value& o, const char* key, const char* what) {
    const json::Value* v = o.get(key);
    if (!v) throw BuildError(std::string(what) + ": no \"" + key + "\"");
    return *v;
}

std::string text(const json::Value& o, const char* key, const char* what) {
    const json::Value& v = need(o, key, what);
    if (!v.isString()) throw BuildError(std::string(what) + ": \"" + key + "\" is not text");
    return v.s;
}

std::vector<std::uint8_t> fromHex(const std::string& h, const char* what) {
    if (h.size() % 2) throw BuildError(std::string(what) + ": odd hex");
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < h.size(); i += 2) {
        char* end = nullptr;
        const std::string two = h.substr(i, 2);
        const long v = std::strtol(two.c_str(), &end, 16);
        if (*end) throw BuildError(std::string(what) + ": bad hex");
        out.push_back(std::uint8_t(v));
    }
    return out;
}

std::vector<std::pair<std::uint32_t, std::int32_t>> litList(const json::Value& v, const char* what) {
    std::vector<std::pair<std::uint32_t, std::int32_t>> out;
    if (!v.isArray()) throw BuildError(std::string(what) + ": not a list");
    for (const json::Value& p : v.items) {
        if (!p.isArray() || p.items.size() != 2) throw BuildError(std::string(what) + ": bad literal entry");
        out.emplace_back(std::uint32_t(number(&p.items[0], what)), std::int32_t(number(&p.items[1], what)));
    }
    return out;
}

Hook readHook(const json::Value& h) {
    Hook k;
    if (const json::Value* s = h.get("symbol")) k.symbol = s->asString();
    if (h.get("patch")) {
        k.kind = Hook::Kind::Patch;
        k.site = std::uint32_t(number(h.get("patch"), "hook patch"));
        k.link = h.get("link") && h.get("link")->asBool();
        if (!h.get("expect_sha256")) throw BuildError("hook patch: no stock hash (only packages with hashes are applied)");
        k.sha256 = h.get("expect_sha256")->asString();
        k.len = std::uint32_t(number(h.get("len"), "hook len"));
    } else if (h.get("bl") || h.get("bw")) {
        k.kind = h.get("bl") ? Hook::Kind::Bl : Hook::Kind::Bw;
        k.site = std::uint32_t(number(h.get(h.get("bl") ? "bl" : "bw"), "hook site"));
        k.expect = std::uint32_t(number(h.get("expect"), "hook expect"));
    } else if (h.get("literal")) {
        k.kind = Hook::Kind::Literal;
        k.site = std::uint32_t(number(h.get("literal"), "hook literal"));
        k.expect = std::uint32_t(number(h.get("expect"), "hook expect"));
    } else {
        throw BuildError("hook of unknown kind");
    }
    return k;
}

CustomCategory readCustom(const json::Value& cc) {
    CustomCategory c;
    c.name = text(cc, "name", "custom_category");
    c.shortName = cc.get("short") ? cc.get("short")->asString() : c.name;
    if (cc.get("label_like")) c.labelLike = int(number(cc.get("label_like"), "label_like"));
    for (const json::Value& s : need(cc, "subs", "custom_category").items) {
        CustomSub sub;
        sub.name = text(s, "name", "custom sub");
        for (const json::Value& m : need(s, "models", "custom sub").items) sub.models.push_back(m.asString());
        c.subs.push_back(std::move(sub));
    }
    const auto hex = [](const json::Value* v) { return std::uint32_t(std::strtoul(v->asString().c_str(), nullptr, 16)); };
    if (cc.get("rgb")) {
        c.rgb = hex(cc.get("rgb"));
        c.rgbDim = cc.get("rgb_dim") ? hex(cc.get("rgb_dim")) : *c.rgb;
    }
    // Packages carry the code patches, table and RAM addresses as data; without them there is nothing to apply.
    const json::Value* patches = cc.get("code_patches");
    const json::Value* tables = cc.get("tables");
    const json::Value* ram = cc.get("ram");
    if (!patches || !tables || !ram) throw BuildError("custom_category: package without code_patches / tables / ram");
    for (const json::Value& p : patches->items) {
        CodePatch cp;
        cp.at = std::uint32_t(number(p.get("at"), "code patch at"));
        cp.len = std::uint32_t(number(p.get("len"), "code patch len"));
        cp.sha256 = text(p, "sha256", "code patch");
        cp.bytes = fromHex(text(p, "bytes", "code patch"), "code patch");
        if (cp.bytes.size() != cp.len) throw BuildError("code patch: bytes and len differ");
        c.codePatches.push_back(std::move(cp));
    }
    for (std::size_t i = 0; i < tables->keys.size(); ++i) {
        const json::Value& t = tables->items[i];
        if (!t.isArray() || t.items.size() != 3) throw BuildError("custom_category table " + tables->keys[i]);
        c.tables.push_back({tables->keys[i], std::uint32_t(number(&t.items[0], "table")),
                            std::uint32_t(number(&t.items[1], "table")), litList(t.items[2], "table literals")});
    }
    c.ramArray = std::uint32_t(number(ram->get("array"), "ram array"));
    c.ramPaths = std::uint32_t(number(ram->get("paths"), "ram paths"));
    c.ramLits = litList(need(*ram, "lits", "ram"), "ram literals");
    return c;
}

} // namespace

std::int64_t number(const json::Value* v, const char* what) {
    if (!v) throw BuildError(std::string(what) + ": missing");
    if (v->isNumber()) return v->asInt();
    if (v->isString() && !v->s.empty()) {
        char* end = nullptr;
        const bool neg = v->s[0] == '-';
        const char* p = v->s.c_str() + (neg ? 1 : 0);
        const bool hexNum = p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
        const long long n = std::strtoll(hexNum ? p + 2 : p, &end, hexNum ? 16 : 10);
        if (end && !*end && end != (hexNum ? p + 2 : p)) return neg ? -n : n;
    }
    throw BuildError(std::string(what) + ": not a number");
}

BuildSpec mergePackages(std::vector<const Package*> packages) {
    std::stable_sort(packages.begin(), packages.end(), [](const Package* a, const Package* b) { return a->slot < b->slot; });
    BuildSpec spec;
    spec.packages = packages;
    std::set<std::string> ids;
    std::set<int> slots;
    for (const Package* p : packages) {
        if (!ids.insert(p->id).second) throw BuildError("package " + p->id + " chosen twice");
        if (p->slot >= 0 && !slots.insert(p->slot).second) throw BuildError("two packages for mod slot " + std::to_string(p->slot));
    }
    for (const Package* p : packages)
        for (const std::string& r : p->needs)
            if (!ids.count(r)) throw BuildError(p->name + " needs package " + r);

    std::set<std::string> parts, syms;
    std::set<std::uint32_t> modelNos;
    for (const Package* p : packages) {
        if (p->slot >= 0) spec.modBits |= std::uint64_t(1) << p->slot;
        const json::Value* s = p->module.get("spec");
        if (s && s->isObject())
            for (std::size_t i = 0; i < s->keys.size(); ++i) {
                const std::string& key = s->keys[i];
                const json::Value& v = s->items[i];
                if (key == "models") {
                    for (const json::Value& m : v.items) {
                        ModelSpec ms;
                        ms.sym = text(m, "sym", "model");
                        ms.name = text(m, "name", "model");
                        ms.like = text(m, "like", "model");
                        ms.after = m.get("after") ? m.get("after")->asString() : ms.like;
                        ms.graph = text(m, "graph", "model");
                        ms.modelNo = std::uint32_t(number(m.get("modelNo"), "modelNo"));
                        if (m.get("cost")) ms.cost = std::uint32_t(number(m.get("cost"), "cost"));
                        ms.package = p;
                        if (!syms.insert(ms.sym).second) throw BuildError("two packages add model " + ms.sym);
                        if (!modelNos.insert(ms.modelNo).second)
                            throw BuildError("two models use model number " + std::to_string(ms.modelNo));
                        spec.models.push_back(std::move(ms));
                    }
                    continue;
                }
                if (!parts.insert(key).second) throw BuildError("two packages define " + key);
                if (key == "custom_category") spec.custom = readCustom(v);
                else if (key == "namblock") spec.namblock = &v, spec.namblockPackage = p;
                else if (key == "polymidi") spec.polymidi = &v, spec.polymidiPackage = p;
                else if (key == "harness")
                    for (const json::Value& h : v.items) spec.harness.push_back(&h);
                else throw BuildError(p->name + ": unknown part \"" + key + "\" (made by a newer tool?)");
            }
        if (const json::Value* code = p->module.get("code"))
            for (const json::Value& c : code->items) {
                CodePart cp;
                cp.name = text(c, "name", "code");
                const std::string blob = text(c, "blob", "code");
                const std::vector<std::uint8_t>* b = p->file(blob);
                if (!b) throw BuildError(p->name + ": " + blob + " missing from the package");
                cp.blob = *b;
                cp.origin = std::uint32_t(number(c.get("origin"), "code origin"));
                if (const json::Value* sy = c.get("symbols"))
                    for (std::size_t i = 0; i < sy->keys.size(); ++i)
                        cp.symbols[sy->keys[i]] = std::uint32_t(number(&sy->items[i], "symbol"));
                for (const json::Value& h : need(c, "hooks", "code").items) {
                    cp.hooks.push_back(readHook(h));
                    if (!cp.hooks.back().symbol.empty() && !cp.symbols.count(cp.hooks.back().symbol))
                        throw BuildError(cp.name + ": hook names unknown symbol " + cp.hooks.back().symbol);
                }
                if (const json::Value* w = c.get("words"))
                    for (const json::Value& wd : w->items) {
                        cp.words.emplace_back(text(wd, "symbol", "word"), text(wd, "model", "word"));
                        if (!cp.symbols.count(cp.words.back().first)) throw BuildError(cp.name + ": word without symbol");
                    }
                spec.code.push_back(std::move(cp));
            }
        if (const json::Value* lay = p->module.get("layout"))
            for (std::size_t i = 0; i < lay->keys.size(); ++i)
                if (!spec.layout.emplace(lay->keys[i], std::uint32_t(number(&lay->items[i], "layout"))).second)
                    throw BuildError("two packages claim " + lay->keys[i] + ": these mods can't be combined (yet)");
    }
    return spec;
}

} // namespace hx::mods

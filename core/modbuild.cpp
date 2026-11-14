#include "core/modbuild.h"

#include "core/appdata.h"
#include "core/checksums.h"
#include "core/dspbuild.h"
#include "core/hxfwrite.h"
#include "core/modspec.h"
#include "core/text.h"
#include "core/unitprofile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

#ifndef OSHEXED_COMMIT
#define OSHEXED_COMMIT "0000000"
#endif
#ifndef OSHEXED_DIRTY
#define OSHEXED_DIRTY 1
#endif

namespace hx::mods {

std::string toolCommit() {
    const std::string c = OSHEXED_COMMIT;
    return c.size() == 7 && c.find_first_not_of("0123456789abcdef") == std::string::npos ? c : "0000000";
}

bool toolDirty() { return OSHEXED_DIRTY != 0 || toolCommit() == "0000000"; }

std::optional<std::vector<Package>> choosePackages(const Store& store, const Stock& stock, const std::set<std::string>& ids,
                                                   std::string* error, bool download) {
    const auto fail = [error](const std::string& why) -> std::optional<std::vector<Package>> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (!stock.ok) return fail(stock.why);
    std::string merr;
    std::optional<Manifest> m = store.manifest(stock.model, stock.version, nullptr, &merr);
    if (m && m->stockSha256 != stock.imageSha256) m.reset();
    std::vector<Package> extras = store.extras(m ? &*m : nullptr, stock.model, stock.version);
    std::erase_if(extras, [&](const Package& p) { return p.fwImageSha256 != stock.imageSha256; });

    std::set<std::string> want = ids;
    if (m)
        for (const ModuleRef& r : m->modules)
            if (r.always) want.insert(r.id);
    const auto needsOf = [&](const std::string& id) -> std::vector<std::string> {
        if (m)
            if (const ModuleRef* r = m->find(id)) return r->needs;
        for (const Package& p : extras)
            if (p.id == id) return p.needs;
        return {};
    };
    for (bool grew = true; grew;) {
        grew = false;
        for (const std::string& id : std::set<std::string>(want))
            for (const std::string& n : needsOf(id)) grew = want.insert(n).second || grew;
    }
    std::vector<Package> out;
    for (const std::string& id : want) {
        if (m)
            if (const ModuleRef* r = m->find(id)) {
                std::string ferr;
                std::optional<Package> p = download ? store.fetch(*m, *r, &ferr) : store.cached(*m, *r);
                if (!p)
                    return fail(download ? r->name + ": " + ferr
                                         : r->name + " isn't downloaded yet (or its download doesn't match the module list)");
                out.push_back(std::move(*p));
                continue;
            }
        bool found = false;
        for (const Package& p : extras)
            if (p.id == id && !found) out.push_back(p), found = true;
        if (!found)
            return fail("package \"" + id + "\" isn't available" + (m ? "" : " (no module list: " + merr + ")"));
    }
    return out;
}

std::string defaultOutPath(const Stock& stock, const std::string& stockPath) {
    const std::filesystem::path dir = pathFromUtf8(stockPath).parent_path();
    const std::string name = std::string(unitName(unitFromId(stock.model))) + " " + stock.version + " + mods.hxf";
    return utf8FromPath(dir / pathFromUtf8(name));
}

BuildReport buildFirmware(const HxfImage& stock, const Stock& stockInfo, const std::string& stockPath,
                          const std::vector<Package>& packages, const std::string& outPath, const std::string& commit,
                          bool dirty, const BuildProgress& progress) {
    // Shares of the whole (rough timings): splitting the base 5%, M4 / DSP 35%, packing (deflate) 55%, writing 5%.
    const auto step = [&](double from, double to) {
        return [&progress, from, to](double done, const std::string& what) {
            if (progress) progress(from + (to - from) * std::clamp(done, 0.0, 1.0), what);
        };
    };
    step(0.0, 0.05)(0.0, "Reading the base firmware");
    if (!stockInfo.ok) throw BuildError(stockInfo.why);
    if (hex(stock.imageSha256) != stockInfo.imageSha256) throw BuildError("the stock file changed since it was opened");
    std::error_code ec;
    if (outPath.empty()) throw BuildError("no output file");
    if (std::filesystem::equivalent(pathFromUtf8(outPath), pathFromUtf8(stockPath), ec))
        throw BuildError("the build would overwrite the stock file: choose another name");
    std::vector<const Package*> chosen;
    for (const Package& p : packages) {
        if (p.fwImageSha256 != stockInfo.imageSha256) throw BuildError(p.name + " is made for another firmware file");
        chosen.push_back(&p);
    }
    std::string err;
    const std::optional<FirmwareParts> parts = explodeParts(stock, &err);
    if (!parts) throw BuildError(err);
    const BuildSpec spec = mergePackages(chosen);
    ModBuild b;
    try {
        b = buildMods(*parts, spec, commit, dirty, step(0.05, 0.40));
    } catch (const FormatError& e) {
        throw BuildError(e.what());
    }
    const RecordMap built = assembleParts(b.parts);
    const auto packing = step(0.40, 0.95);
    const PackedHxf packed = packHxf(stock, built, [&packing](double d) { packing(d, "Packing the firmware"); });
    step(0.95, 1.0)(0.0, "Writing " + utf8FromPath(pathFromUtf8(outPath).filename()));
    if (!writeFile(outPath, packed.file, &err)) throw BuildError("can't write " + outPath + ": " + err);
    step(0.95, 1.0)(1.0, "Done");

    BuildReport r;
    r.outPath = outPath;
    r.tag = b.tag;
    r.size = packed.file.size();
    r.sha256 = toHex(sha256(packed.file));
    r.stockSha256 = stockInfo.imageSha256;
    r.changedRecords = packed.changed;
    const RecordMap before = assembleParts(*parts);
    for (std::uint32_t c : packed.changed) {
        RecordChange rc;
        rc.record = c;
        if (const auto it = before.find(c); it != before.end()) rc.before = it->second.size();
        if (const auto it = built.find(c); it != built.end()) rc.after = it->second.size();
        r.records.push_back(rc);
    }
    r.dspLog = std::move(b.dspLog);
    r.m4Log = std::move(b.m4Log);
    r.log = std::move(b.log);
    for (const Package* p : spec.packages)
        r.packages.push_back(p->name + " " + p->version + (p->slot >= 0 ? " (slot " + std::to_string(p->slot) + ")" : ""));
    for (const ModelSpec& m : spec.models) {
        std::string line = m.name + " (" + m.sym + "): model number " + std::to_string(m.modelNo);
        for (const M4Model& mm : b.models)
            if (mm.sym == m.sym) line += ", DSP cost " + std::to_string(mm.cost);
        r.models.push_back(line);
        r.modelKeys.push_back(m.sym + "=" + m.name);
    }
    r.previous = lastBuild(r.stockSha256);  // before this one is recorded
    recordBuild(r);
    return r;
}

namespace {
std::filesystem::path historyFile() { return pathFromUtf8(appDataDir()) / "builds.txt"; }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (std::size_t next; (next = s.find(sep, at)) != std::string::npos; at = next + 1) out.push_back(s.substr(at, next - at));
    out.push_back(s.substr(at));
    return out;
}

// Every line of the history, oldest first. Lines from before 2026-11-13 have only tag, SHA-256 and path.
std::vector<BuildEntry> readHistory() {
    std::vector<BuildEntry> out;
    if (appDataDir().empty()) return out;
    std::ifstream in(historyFile(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() < 3 || f[0].empty()) continue;
        BuildEntry e{f[0], f[1], f[2], f.size() > 3 ? f[3] : std::string(), {}};
        if (f.size() > 4 && !f[4].empty()) e.models = split(f[4], '|');
        out.push_back(std::move(e));
    }
    return out;
}

std::string bytes(std::size_t n) {
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(std::size_t(i), " ");
    return s + " bytes";
}

std::string recordName(std::uint32_t rec) {
    switch (rec) {
    case 0: return "M4 app";
    case 1: return "M4 app (copy)";
    case 2: return "DSP boot code";
    case 4: return "model data";
    case 5: return "DSP modules";
    default: return "record " + std::to_string(rec);
    }
}

// "sym=name" -> "name (sym)".
std::string modelText(const std::string& key) {
    const std::size_t eq = key.find('=');
    return eq == std::string::npos ? key : key.substr(eq + 1) + " (" + key.substr(0, eq) + ")";
}
} // namespace

void recordBuild(const BuildReport& r) {
    if (appDataDir().empty()) return;
    std::string models;
    for (const std::string& m : r.modelKeys) models += (models.empty() ? "" : "|") + m;
    std::ofstream out(historyFile(), std::ios::app | std::ios::binary);
    out << r.tag << '\t' << r.sha256 << '\t' << r.outPath << '\t' << r.stockSha256 << '\t' << models << '\n';
}

std::string findBuild(const std::string& tag) {
    if (tag.empty()) return {};
    std::string found;
    for (const BuildEntry& e : readHistory()) {
        std::error_code ec;
        if (e.tag == tag && std::filesystem::is_regular_file(pathFromUtf8(e.path), ec)) found = e.path;  // the newest wins
    }
    return found;
}

std::optional<BuildEntry> lastBuild(const std::string& stockSha256) {
    std::optional<BuildEntry> found;
    if (stockSha256.empty()) return found;
    for (BuildEntry& e : readHistory())
        if (e.stockSha256 == stockSha256) found = std::move(e);
    return found;
}

std::vector<ReportItem> reportItems(const BuildReport& r) {
    using K = ReportItem::Kind;
    std::vector<ReportItem> out;
    auto head = [&](std::string t) { out.push_back({K::Heading, std::move(t)}); };
    auto line = [&](std::string t) { out.push_back({K::Line, std::move(t)}); };
    auto warn = [&](std::string t) { out.push_back({K::Warning, std::move(t)}); };
    auto starts = [](const std::string& s, const char* p) { return s.rfind(p, 0) == 0; };
    auto trim = [](const std::string& s) { return s.substr(std::min(s.find_first_not_of(' '), s.size())); };

    head("Build");
    line("Wrote " + r.outPath + " (" + bytes(r.size) + ")");
    line("SHA-256 " + r.sha256);
    line("Build tag " + r.tag + " (the unit shows \"Mod <version>: " + r.tag + "\")");

    head("Modules");
    for (const std::string& p : r.packages) line(p);

    head("Models added");
    if (r.models.empty()) line("none");
    for (const std::string& m : r.models) line(m);

    // Against the last build from the same base: models that went away break presets that use them.
    if (r.previous) {
        const BuildEntry& p = *r.previous;
        head("Compared with the last build");
        line(p.path + " (tag " + p.tag + ")");
        if (p.tag == r.tag) line("Same modules and tool: the same file as that build.");
        bool same = true;
        for (const std::string& m : r.modelKeys)
            if (std::find(p.models.begin(), p.models.end(), m) == p.models.end()) {
                line("New since then: " + modelText(m));
                same = false;
            }
        for (const std::string& m : p.models)
            if (std::find(r.modelKeys.begin(), r.modelKeys.end(), m) == r.modelKeys.end()) {
                warn("Removed since then: " + modelText(m) + ". Presets that use it lose that block on this build.");
                same = false;
            }
        if (same) line("Same models.");
    }

    head("Records changed");
    if (r.records.empty()) line("none");
    for (const RecordChange& c : r.records) {
        std::string t = std::to_string(c.record) + " " + recordName(c.record);
        if (c.before || c.after) {
            const long long d = (long long)c.after - (long long)c.before;
            t += ": " + bytes(c.before) + " -> " + bytes(c.after);
            if (d) t += " (" + std::string(d > 0 ? "+" : "-") + bytes(std::size_t(d > 0 ? d : -d)) + ")";
        }
        line(t);
    }

    head("M4 app changes");
    for (const std::string& l : r.m4Log)
        if (!starts(l, "M4 free space") && !starts(l, "mod tag")) line(trim(l));

    head("DSP changes");
    for (const std::string& l : r.dspLog)
        if (!starts(l, "DSP: core")) line(trim(l));

    head("Memory left");
    for (const std::string& l : r.m4Log)
        if (starts(l, "M4 free space")) line(l);
    for (const std::string& l : r.dspLog)
        if (starts(l, "DSP: core")) line(l);

    head("Checks");
    line("Passed: every stock model, browser list and cost still found as before; the new models are registered and "
         "their DSP modules follow the loader's rules.");
    return out;
}

std::vector<std::string> reportLines(const BuildReport& r) {
    std::vector<std::string> out;
    for (const ReportItem& i : reportItems(r)) switch (i.kind) {
        case ReportItem::Kind::Heading: out.push_back((out.empty() ? "== " : "\n== ") + i.text); break;
        case ReportItem::Kind::Warning: out.push_back("! " + i.text); break;
        default: out.push_back("  " + i.text); break;
        }
    return out;
}

} // namespace hx::mods

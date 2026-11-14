#include "core/modpkg.h"

#include "core/checksums.h"

#include <miniz.h>

#include <algorithm>
#include <cstring>

namespace hx::mods {

namespace {

std::uint16_t u16(Bytes b, std::size_t at) { return std::uint16_t(b[at] | b[at + 1] << 8); }
std::uint32_t u32(Bytes b, std::size_t at) {
    return std::uint32_t(b[at]) | std::uint32_t(b[at + 1]) << 8 | std::uint32_t(b[at + 2]) << 16 |
           std::uint32_t(b[at + 3]) << 24;
}

template <typename T> std::optional<T> fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return std::nullopt;
}

std::vector<std::string> strings(const json::Value* v) {
    std::vector<std::string> out;
    if (v && v->isArray())
        for (const json::Value& s : v->items) out.push_back(s.asString());
    return out;
}

} // namespace

std::optional<std::vector<IndexEntry>> parseIndex(const std::string& text, std::string* error) {
    std::string err;
    const std::optional<json::Value> j = json::parse(text, &err);
    if (!j || !j->isObject()) return fail<std::vector<IndexEntry>>(error, "index.json: " + (err.empty() ? "not an object" : err));
    if (j->get("format") && j->get("format")->asInt() != 1)
        return fail<std::vector<IndexEntry>>(error, "index.json: format " + std::to_string(j->get("format")->asInt()) +
                                                        " is newer than this editor knows");
    std::vector<IndexEntry> out;
    if (const json::Value* builds = j->get("builds"); builds && builds->isArray())
        for (const json::Value& b : builds->items) {
            IndexEntry e;
            if (const json::Value* v = b.get("model")) e.model = v->asString();
            if (const json::Value* v = b.get("version")) e.version = v->asString();
            if (const json::Value* v = b.get("manifest")) e.manifest = v->asString();
            if (!e.model.empty() && !e.version.empty() && !e.manifest.empty()) out.push_back(std::move(e));
        }
    return out;
}

const ModuleRef* Manifest::find(const std::string& id) const {
    for (const ModuleRef& m : modules)
        if (m.id == id) return &m;
    return nullptr;
}

std::optional<Manifest> parseManifest(const std::string& text, std::string* error) {
    std::string err;
    const std::optional<json::Value> j = json::parse(text, &err);
    if (!j || !j->isObject()) return fail<Manifest>(error, "manifest.json: " + (err.empty() ? "not an object" : err));
    if (j->get("format") && j->get("format")->asInt() != 1)
        return fail<Manifest>(error, "manifest.json: format " + std::to_string(j->get("format")->asInt()) +
                                         " is newer than this editor knows");
    Manifest m;
    if (const json::Value* v = j->get("model")) m.model = v->asString();
    if (const json::Value* v = j->get("version")) m.version = v->asString();
    if (const json::Value* v = j->get("stock_image_sha256")) m.stockSha256 = v->asString();
    if (const json::Value* v = j->get("modtag_layout")) m.modtagLayout = int(v->asInt());
    if (const json::Value* v = j->get("signed")) m.isSigned = v->asBool();
    if (const json::Value* mods = j->get("modules"); mods && mods->isArray())
        for (const json::Value& x : mods->items) {
            ModuleRef r;
            if (const json::Value* v = x.get("id")) r.id = v->asString();
            if (const json::Value* v = x.get("file")) r.file = v->asString();
            if (const json::Value* v = x.get("sha256")) r.sha256 = v->asString();
            if (const json::Value* v = x.get("size")) r.size = std::uint64_t(v->asInt());
            if (const json::Value* v = x.get("slot"); v && v->isNumber()) r.slot = int(v->asInt());
            if (const json::Value* v = x.get("name")) r.name = v->asString();
            if (const json::Value* v = x.get("version")) r.version = v->asString();
            r.needs = strings(x.get("requires"));
            if (const json::Value* v = x.get("always")) r.always = v->asBool();
            // A file name with a path in it would write outside the cache folder.
            if (r.id.empty() || r.file.empty() || r.file.find_first_of("/\\") != std::string::npos ||
                r.file.find("..") != std::string::npos)
                return fail<Manifest>(error, "manifest.json: a module entry without a plain id / file name");
            m.modules.push_back(std::move(r));
        }
    return m;
}

// Central directory -> local headers -> data (stored or raw deflate). Zip64 and encryption are not ours to read.
std::optional<std::map<std::string, std::vector<std::uint8_t>>> readZip(Bytes zip, std::string* error) {
    using Files = std::map<std::string, std::vector<std::uint8_t>>;
    if (zip.size() < 22) return fail<Files>(error, "not a zip (too short)");
    std::size_t eocd = std::string::npos;
    for (std::size_t i = zip.size() - 22 + 1; i-- > 0 && zip.size() - i <= 22 + 65535;)
        if (u32(zip, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return fail<Files>(error, "not a zip (no end record)");
    const std::size_t count = u16(zip, eocd + 10), cdSize = u32(zip, eocd + 12), cdAt = u32(zip, eocd + 16);
    if (cdAt + cdSize > eocd) return fail<Files>(error, "zip directory out of range");
    Files files;
    std::size_t p = cdAt;
    for (std::size_t n = 0; n < count; ++n) {
        if (p + 46 > zip.size() || u32(zip, p) != 0x02014b50) return fail<Files>(error, "zip directory damaged");
        const unsigned flags = u16(zip, p + 8), method = u16(zip, p + 10);
        const std::uint32_t crc = u32(zip, p + 16), compSize = u32(zip, p + 20), size = u32(zip, p + 24);
        const std::size_t nameLen = u16(zip, p + 28), extraLen = u16(zip, p + 30), commentLen = u16(zip, p + 32);
        const std::size_t local = u32(zip, p + 42);
        if (p + 46 + nameLen > zip.size()) return fail<Files>(error, "zip directory damaged");
        std::string name(reinterpret_cast<const char*>(zip.data() + p + 46), nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (name.empty() || name.back() == '/') continue;  // a folder
        if (flags & 1) return fail<Files>(error, "zip entry " + name + " is encrypted");
        if (local + 30 > zip.size() || u32(zip, local) != 0x04034b50) return fail<Files>(error, "zip entry " + name + " damaged");
        const std::size_t data = local + 30 + u16(zip, local + 26) + u16(zip, local + 28);
        if (data + compSize > zip.size()) return fail<Files>(error, "zip entry " + name + " runs past the file");
        std::vector<std::uint8_t> out(size);
        if (method == 0) {
            if (compSize != size) return fail<Files>(error, "zip entry " + name + " damaged");
            std::memcpy(out.data(), zip.data() + data, size);
        } else if (method == 8) {
            const std::size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(), zip.data() + data, compSize, 0);
            if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got != size)
                return fail<Files>(error, "zip entry " + name + " doesn't inflate");
        } else {
            return fail<Files>(error, "zip entry " + name + " uses compression " + std::to_string(method));
        }
        if (crc32(Bytes(out.data(), out.size())) != crc) return fail<Files>(error, "zip entry " + name + ": CRC mismatch");
        files[name] = std::move(out);
    }
    return files;
}

const std::vector<std::uint8_t>* Package::file(const std::string& path) const {
    const auto it = files.find(path);
    return it == files.end() ? nullptr : &it->second;
}

std::optional<Package> readPackage(Bytes ohem, const std::string& id, std::string* error) {
    std::string err;
    std::optional<std::map<std::string, std::vector<std::uint8_t>>> files = readZip(ohem, &err);
    if (!files) return fail<Package>(error, id + ": " + err);
    const auto mj = files->find("module.json");
    if (mj == files->end()) return fail<Package>(error, id + ": no module.json in the package");
    std::optional<json::Value> module =
        json::parse(std::string(reinterpret_cast<const char*>(mj->second.data()), mj->second.size()), &err);
    if (!module || !module->isObject()) return fail<Package>(error, id + ": module.json: " + err);
    if (module->get("format") && module->get("format")->asInt() != 1)
        return fail<Package>(error, id + ": package format " + std::to_string(module->get("format")->asInt()) +
                                        " is newer than this editor knows");
    Package p;
    p.id = id;
    if (const json::Value* v = module->get("slot"); v && v->isNumber()) p.slot = int(v->asInt());
    if (const json::Value* v = module->get("name")) p.name = v->asString();
    if (const json::Value* v = module->get("version")) p.version = v->asString();
    p.needs = strings(module->get("requires"));
    if (const json::Value* fw = module->get("firmware")) {
        if (const json::Value* v = fw->get("model")) p.fwModel = v->asString();
        if (const json::Value* v = fw->get("version")) p.fwVersion = v->asString();
        if (const json::Value* v = fw->get("image_sha256")) p.fwImageSha256 = v->asString();
    }
    files->erase(mj);
    p.files = std::move(*files);
    p.module = std::move(*module);
    return p;
}

std::string hex(const std::array<std::uint8_t, 32>& digest) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : digest) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

std::string checkDownload(Bytes ohem, const ModuleRef& ref) {
    if (ohem.size() != ref.size)
        return ref.file + ": " + std::to_string(ohem.size()) + " bytes, the manifest says " + std::to_string(ref.size);
    if (hex(sha256(ohem)) != ref.sha256) return ref.file + ": SHA-256 doesn't match the manifest";
    return {};
}

std::string idFromFileName(const std::string& fileName) {
    std::string stem = fileName;
    if (const auto slash = stem.find_last_of("/\\"); slash != std::string::npos) stem = stem.substr(slash + 1);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos) stem = stem.substr(0, dot);
    if (const auto dash = stem.rfind('-'); dash != std::string::npos && dash > 0) stem = stem.substr(0, dash);
    return stem;
}

} // namespace hx::mods

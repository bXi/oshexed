#include "core/modstore.h"

#include "core/appdata.h"
#include "core/buildtag.h"
#include "core/checksums.h"
#include "core/firmware.h"
#include "core/http.h"
#include "core/text.h"
#include "core/unitprofile.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace hx::mods {

namespace {

namespace fs = std::filesystem;

std::optional<std::vector<std::uint8_t>> readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Beside the target, then renamed over it: a broken download never leaves half a file in the cache.
bool writeAll(const fs::path& p, Bytes data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out.flush()) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string str(const std::vector<std::uint8_t>& b) { return std::string(b.begin(), b.end()); }

} // namespace

namespace {
Stock stockFrom(const HxfImage& img, const FirmwareIdentity& id) {
    Stock s;
    const UnitModel model = unitFromProductId(img.productId);
    if (model == UnitModel::Unknown) {
        s.why = "the file is for a unit this editor doesn't know";
        return s;
    }
    if (id.kind != FirmwareIdentity::Kind::Stock) {
        s.why = "mods are built from the stock firmware file; this one isn't stock (" + id.summary + ")";
        return s;
    }
    s.ok = true;
    s.model = std::string(unitId(model));
    s.version = id.version;
    s.imageSha256 = hex(img.imageSha256);
    return s;
}
} // namespace

Stock stockOf(const Firmware& fw) {
    if (!fw.hxf) {
        Stock s;
        s.why = "no firmware file open";
        return s;
    }
    return stockFrom(*fw.hxf, fw.identity);
}

Stock stockOfFile(const std::string& path) {
    Stock s;
    std::string err;
    const std::unique_ptr<HxfImage> img = loadHxfFile(path, &err);
    if (!img) {
        s.why = err;
        return s;
    }
    const HxfRecord* app = img->record(0);
    const std::optional<AppImage> a = app ? parseApp(app->payload, &err) : std::nullopt;
    if (!a) {
        s.why = app ? "record 0: " + err : "no M4 app (record 0)";
        return s;
    }
    return stockFrom(*img, identifyFirmware(img->imageSha256, *a));
}

Store::Store(std::string cacheRoot, std::string baseUrl) : root_(std::move(cacheRoot)), baseUrl_(std::move(baseUrl)) {
    if (root_.empty()) root_ = utf8FromPath(pathFromUtf8(appDataDir()) / "modules");
    if (!baseUrl_.empty() && baseUrl_.back() != '/') baseUrl_ += '/';
}

std::string Store::folder(const std::string& model, const std::string& version) const {
    return utf8FromPath(pathFromUtf8(root_) / pathFromUtf8(model) / pathFromUtf8(version));
}

std::optional<std::vector<IndexEntry>> Store::fetchIndex(std::string* error) const {
    std::vector<std::uint8_t> body;
    if (!httpGet(baseUrl_ + "index.json", body, error, 1u << 20)) return std::nullopt;
    return parseIndex(str(body), error);
}

std::optional<Manifest> Store::manifest(const std::string& model, const std::string& version, bool* fromCache,
                                        std::string* error) const {
    const fs::path local = pathFromUtf8(folder(model, version)) / "manifest.json";
    if (fromCache) *fromCache = false;
    std::vector<std::uint8_t> body;
    std::string netError;
    if (httpGet(baseUrl_ + model + "/" + version + "/manifest.json", body, &netError, 1u << 20)) {
        std::optional<Manifest> m = parseManifest(str(body), error);
        if (m) writeAll(local, Bytes(body.data(), body.size()));
        return m;
    }
    // Offline (or the server is down): the last manifest we had.
    if (std::optional<std::vector<std::uint8_t>> cachedText = readAll(local)) {
        if (fromCache) *fromCache = true;
        return parseManifest(str(*cachedText), error);
    }
    if (error) *error = netError;
    return std::nullopt;
}

std::optional<Package> Store::cached(const Manifest& m, const ModuleRef& ref) const {
    const std::optional<std::vector<std::uint8_t>> data = readAll(pathFromUtf8(folder(m.model, m.version)) / ref.file);
    if (!data || !checkDownload(Bytes(data->data(), data->size()), ref).empty()) return std::nullopt;
    return readPackage(Bytes(data->data(), data->size()), ref.id);
}

std::optional<Package> Store::fetch(const Manifest& m, const ModuleRef& ref, std::string* error) const {
    if (std::optional<Package> p = cached(m, ref)) return p;
    std::vector<std::uint8_t> body;
    if (!httpGet(baseUrl_ + m.model + "/" + m.version + "/" + ref.file, body, error, std::size_t(ref.size) + 1)) return std::nullopt;
    const Bytes bytes(body.data(), body.size());
    if (const std::string why = checkDownload(bytes, ref); !why.empty()) {
        if (error) *error = why;
        return std::nullopt;
    }
    std::optional<Package> p = readPackage(bytes, ref.id, error);
    if (!p) return std::nullopt;
    if (p->fwImageSha256 != m.stockSha256) {
        if (error) *error = ref.file + " is made for another firmware than its manifest says";
        return std::nullopt;
    }
    writeAll(pathFromUtf8(folder(m.model, m.version)) / ref.file, bytes);
    return p;
}

std::optional<Package> Store::import(const std::string& path, const Stock& stock, std::string* error) const {
    const fs::path src = pathFromUtf8(path);
    const std::optional<std::vector<std::uint8_t>> data = readAll(src);
    if (!data) {
        if (error) *error = "can't read " + path;
        return std::nullopt;
    }
    const std::string fileName = utf8FromPath(src.filename());
    std::optional<Package> p = readPackage(Bytes(data->data(), data->size()), idFromFileName(fileName), error);
    if (!p) return std::nullopt;
    if (!stock.ok || p->fwImageSha256 != stock.imageSha256) {
        if (error)
            *error = fileName + " is made for " + p->fwModel + " " + p->fwVersion +
                     (stock.ok ? ", not for the open stock firmware" : " (open its stock firmware first)");
        return std::nullopt;
    }
    writeAll(pathFromUtf8(folder(stock.model, stock.version)) / pathFromUtf8(fileName), Bytes(data->data(), data->size()));
    return p;
}

std::vector<Package> Store::extras(const Manifest* m, const std::string& model, const std::string& version) const {
    std::vector<Package> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(pathFromUtf8(folder(model, version)), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".ohem") continue;
        const std::string fileName = utf8FromPath(e.path().filename());
        bool listed = false;
        if (m)
            for (const ModuleRef& r : m->modules) listed = listed || r.file == fileName;
        if (listed) continue;
        if (const std::optional<std::vector<std::uint8_t>> data = readAll(e.path()))
            if (std::optional<Package> p = readPackage(Bytes(data->data(), data->size()), idFromFileName(fileName)))
                out.push_back(std::move(*p));
    }
    return out;
}

} // namespace hx::mods

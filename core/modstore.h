#pragma once

// Where mod packages come from and live: the module server (index -> manifest -> .ohem, each package checked against
// the manifest's size and SHA-256) and the local cache, <app data>/modules/<model>/<version>/ (the manifest and the
// packages, so building works offline and a dropped .ohem is kept). ../mod.md "Distribution".

#include "core/modpkg.h"

#include <optional>
#include <string>
#include <vector>

namespace hx {
struct Firmware;
}

namespace hx::mods {

// The stock firmware packages are made for, from the user's .hxf: model id, version and image hash. ok = false (with
// why) when it isn't a stock file of a model we know: modules only ever apply to stock.
struct Stock {
    bool ok = false;
    std::string why;
    std::string model, version, imageSha256;
};
Stock stockOf(const Firmware& fw);
// The same for a file on disk (reads only the image and the M4 app; for the base firmware setting).
Stock stockOfFile(const std::string& path);

class Store {
public:
    // cacheRoot "" = <app data>/modules.
    explicit Store(std::string cacheRoot = {}, std::string baseUrl = kDefaultBaseUrl);

    const std::string& baseUrl() const { return baseUrl_; }
    std::string folder(const std::string& model, const std::string& version) const;

    std::optional<std::vector<IndexEntry>> fetchIndex(std::string* error = nullptr) const;
    // The manifest for a build: downloaded (and cached), or the cached one when the server can't be reached
    // (fromCache says which).
    std::optional<Manifest> manifest(const std::string& model, const std::string& version, bool* fromCache = nullptr,
                                     std::string* error = nullptr) const;

    // A cached package matching the manifest entry (size + SHA-256), or nullopt.
    std::optional<Package> cached(const Manifest& m, const ModuleRef& ref) const;
    // Cached, else downloaded, checked and cached.
    std::optional<Package> fetch(const Manifest& m, const ModuleRef& ref, std::string* error = nullptr) const;
    // A dropped / chosen .ohem: read, checked to be made for `stock`, kept in the cache under its own name.
    std::optional<Package> import(const std::string& path, const Stock& stock, std::string* error = nullptr) const;
    // Packages in the cache for a build that the manifest doesn't list (imported ones).
    std::vector<Package> extras(const Manifest* m, const std::string& model, const std::string& version) const;

private:
    std::string root_, baseUrl_;
};

} // namespace hx::mods

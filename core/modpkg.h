#pragma once

// Mod packages (the firmware project's mod.md "Distribution" and MODFORMAT.md): the index and manifests on the module server, the .ohem
// packages (a zip of module.json, blobs/, graphs/) and the checks before one is used. Reading only; building a
// firmware from packages is core/modbuild (later stages).

#include "core/bytes.h"
#include "core/json.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hx::mods {

constexpr const char* kDefaultBaseUrl = "https://bixxy.net/oshexed/";

// index.json: the builds the server has modules for.
struct IndexEntry {
    std::string model, version;  // "helix-lt", "3.80.0"
    std::string manifest;        // path relative to the base URL
};
std::optional<std::vector<IndexEntry>> parseIndex(const std::string& text, std::string* error = nullptr);

// <model>/<version>/manifest.json: the modules for one stock firmware.
struct ModuleRef {
    std::string id, file, sha256, name, version;
    std::uint64_t size = 0;
    int slot = -1;  // MODTAG slot; -1 for the base (no slot)
    std::vector<std::string> needs;  // "requires" (a C++20 keyword, hence the name)
    bool always = false;  // in every build (the base)
};
struct Manifest {
    std::string model, version, stockSha256;
    int modtagLayout = 0;
    bool isSigned = false;
    std::vector<ModuleRef> modules;
    const ModuleRef* find(const std::string& id) const;
};
std::optional<Manifest> parseManifest(const std::string& text, std::string* error = nullptr);

// The files of a zip (stored or deflated entries), by path; CRCs checked. nullopt with a message on a bad zip.
std::optional<std::map<std::string, std::vector<std::uint8_t>>> readZip(Bytes zip, std::string* error = nullptr);

// One package, read from its .ohem.
struct Package {
    std::string id;  // the manifest's id (or, for a dropped file, its name up to the last '-')
    json::Value module;  // module.json as it is (the builder reads its spec / code / layout)
    std::map<std::string, std::vector<std::uint8_t>> files;  // everything else: blobs/..., graphs/...
    int slot = -1;
    std::string name, version;
    std::vector<std::string> needs;  // module.json "requires"
    std::string fwModel, fwVersion, fwImageSha256;  // module.json "firmware"
    const std::vector<std::uint8_t>* file(const std::string& path) const;
};
std::optional<Package> readPackage(Bytes ohem, const std::string& id, std::string* error = nullptr);

// "" when the downloaded bytes are the manifest's (size and SHA-256), else why not.
std::string checkDownload(Bytes ohem, const ModuleRef& ref);

// "<id>-<version>.ohem" -> id (a dropped package names no id inside).
std::string idFromFileName(const std::string& fileName);

// Hex of a 32-byte digest, lower case.
std::string hex(const std::array<std::uint8_t, 32>& digest);

} // namespace hx::mods

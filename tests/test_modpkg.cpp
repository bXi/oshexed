#include "test.h"

#include "core/checksums.h"
#include "core/modpkg.h"

#include <string>
#include <vector>

namespace {

void put16(std::vector<std::uint8_t>& b, unsigned v) {
    b.push_back(std::uint8_t(v));
    b.push_back(std::uint8_t(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    put16(b, v & 0xffff);
    put16(b, v >> 16);
}

// A zip with stored (uncompressed) entries, as small as the format allows.
std::vector<std::uint8_t> storedZip(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::vector<std::uint8_t> zip, dir;
    for (const auto& [name, data] : entries) {
        const std::uint32_t crc = hx::crc32(hx::Bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
        const std::uint32_t local = std::uint32_t(zip.size());
        put32(zip, 0x04034b50);
        put16(zip, 20), put16(zip, 0), put16(zip, 0), put16(zip, 0), put16(zip, 0);
        put32(zip, crc), put32(zip, std::uint32_t(data.size())), put32(zip, std::uint32_t(data.size()));
        put16(zip, unsigned(name.size())), put16(zip, 0);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), data.begin(), data.end());
        put32(dir, 0x02014b50);
        put16(dir, 20), put16(dir, 20), put16(dir, 0), put16(dir, 0), put16(dir, 0), put16(dir, 0);
        put32(dir, crc), put32(dir, std::uint32_t(data.size())), put32(dir, std::uint32_t(data.size()));
        put16(dir, unsigned(name.size())), put16(dir, 0), put16(dir, 0), put16(dir, 0), put16(dir, 0);
        put32(dir, 0), put32(dir, local);
        dir.insert(dir.end(), name.begin(), name.end());
    }
    const std::uint32_t dirAt = std::uint32_t(zip.size());
    zip.insert(zip.end(), dir.begin(), dir.end());
    put32(zip, 0x06054b50);
    put16(zip, 0), put16(zip, 0), put16(zip, unsigned(entries.size())), put16(zip, unsigned(entries.size()));
    put32(zip, std::uint32_t(dir.size())), put32(zip, dirAt), put16(zip, 0);
    return zip;
}

hx::Bytes bytes(const std::vector<std::uint8_t>& v) { return hx::Bytes(v.data(), v.size()); }

} // namespace

TEST("modpkg: manifest and index as the module server writes them") {
    const std::string manifest = R"({"format": 1, "model": "helix-lt", "version": "3.80.0",
        "stock_image_sha256": "988e", "modtag_layout": 1, "signed": false,
        "modules": [{"id": "base", "file": "base-1.ohem", "sha256": "aa", "size": 5486, "slot": null,
                     "name": "Base", "version": "1", "requires": [], "always": true},
                    {"id": "nam", "file": "nam-1.ohem", "sha256": "bb", "size": 15090, "slot": 0,
                     "name": "NAM block", "version": "1", "requires": ["base"], "always": false}]})";
    const auto m = hx::mods::parseManifest(manifest);
    REQUIRE(m.has_value());
    CHECK(m->model == "helix-lt" && m->version == "3.80.0" && !m->isSigned);
    REQUIRE(m->modules.size() == 2);
    CHECK(m->modules[0].slot == -1 && m->modules[0].always);
    CHECK(m->modules[1].slot == 0 && m->modules[1].needs == std::vector<std::string>{"base"});
    CHECK(m->find("nam") == &m->modules[1]);
    // A file name with a path in it is refused (it would write outside the cache).
    CHECK(!hx::mods::parseManifest(R"({"format":1,"modules":[{"id":"x","file":"../x.ohem"}]})"));
    CHECK(!hx::mods::parseManifest(R"({"format": 2})"));

    const auto idx = hx::mods::parseIndex(
        R"({"format": 1, "builds": [{"model": "helix-lt", "version": "3.80.0", "manifest": "helix-lt/3.80.0/manifest.json"}]})");
    REQUIRE(idx.has_value() && idx->size() == 1);
    CHECK((*idx)[0].manifest == "helix-lt/3.80.0/manifest.json");
}

TEST("modpkg: a package from its zip; damaged zips are refused") {
    const std::vector<std::uint8_t> zip =
        storedZip({{"module.json", R"({"format": 1, "slot": 1, "name": "MIDI Guitar", "version": "1",
                        "requires": ["base"], "firmware": {"model": "helix-lt", "version": "3.80.0", "image_sha256": "988e"}})"},
                   {"blobs/x.bin", std::string("\x01\x02\x03", 3)}});
    std::string error;
    const auto p = hx::mods::readPackage(bytes(zip), "midiguitar", &error);
    REQUIRE(p.has_value());
    CHECK(p->slot == 1 && p->name == "MIDI Guitar" && p->fwImageSha256 == "988e");
    CHECK(p->needs == std::vector<std::string>{"base"});
    REQUIRE(p->file("blobs/x.bin") != nullptr);
    CHECK(p->file("blobs/x.bin")->size() == 3);

    std::vector<std::uint8_t> bad = zip;
    bad[50] ^= 0x01;  // inside module.json's data (it starts at 30 + 11): the CRC catches it
    CHECK(!hx::mods::readPackage(bytes(bad), "x"));
    CHECK(!hx::mods::readZip(bytes(std::vector<std::uint8_t>(10, 0))));

    hx::mods::ModuleRef ref;
    ref.file = "midiguitar-1.ohem";
    ref.size = zip.size();
    ref.sha256 = hx::mods::hex(hx::sha256(bytes(zip)));
    CHECK(hx::mods::checkDownload(bytes(zip), ref).empty());
    ref.size += 1;
    CHECK(!hx::mods::checkDownload(bytes(zip), ref).empty());
}

TEST("modpkg: package id from a dropped file's name") {
    CHECK(hx::mods::idFromFileName("midiguitar-1.ohem") == "midiguitar");
    CHECK(hx::mods::idFromFileName("C:/x/nam-12.ohem") == "nam");
    CHECK(hx::mods::idFromFileName("base.ohem") == "base");
}

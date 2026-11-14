#include "core/buildtag.h"

#include "core/checksums.h"

#include <cstdio>
#include <cstring>

namespace hx {

namespace {

constexpr const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr int kTagLength = 17;

// 128-bit arithmetic for the 101-bit tag value (GCC / Clang; MSVC would need a two-word version).
using u128 = unsigned __int128;

u128 scramble() {
    const u128 s = (u128(0x1B5DE3A7u) << 64) | 0x6C49F2D83E15A9C7ull;  // MODTAG.md: 0x1B5DE3A76C49F2D83E15A9C7
    return s & ((u128(1) << 93) - 1);
}

// CRC-8, polynomial 0x07, init 0, not reflected, over the 93-bit body as 12 big-endian bytes.
std::uint8_t crc8(u128 body) {
    std::uint8_t c = 0;
    for (int i = 11; i >= 0; --i) {
        c ^= std::uint8_t(body >> (8 * i));
        for (int k = 0; k < 8; ++k) c = (c & 0x80) ? std::uint8_t((c << 1) ^ 0x07) : std::uint8_t(c << 1);
    }
    return c;
}

// v3.80 LT M4 app (MODTAG.md "Where it lives"): the version format string's 36-byte slot, then Line 6's build hash.
constexpr std::uint32_t kVersionString = 0x280c4684;
constexpr std::size_t kVersionSlot = 36;

// Stock builds we know: SHA-256 of the inflated image (hashes only: no Line 6 bytes) and Line 6's build hash.
struct KnownBuild {
    const char* imageSha256;
    const char* line6Hash;
    const char* name;
};
// One per model: the version hash is shared, the image isn't. Only the LT is supported for editing / mods
// (core/firmware.cpp); Floor and Rack are recognised as stock.
constexpr KnownBuild kStock[] = {
    {"988e9abec9c1ddaaebbf7d0e989950755efb2040cf8ef62e69b0a81b2657733c", "7d01f5e", "3.80.0"},  // Helix LT
    {"3447c499b53243851da182ece727a7bdbf5e3c8dc9849d238a26ed503ae192cd", "7d01f5e", "3.80.0"},  // Helix Floor
    {"91725e0fa09578a724d9252bcaaf044ec28063f89f736f973f46be1a05788a83", "7d01f5e", "3.80.0"},  // Helix Rack
};

std::string cstr(Bytes b) {
    std::string s;
    for (std::uint8_t c : b) {
        if (c == 0) break;
        s += char(c);
    }
    return s;
}

std::string based(const std::string& version) {
    return version.empty() ? "" : " (based on " + version + ")";
}

} // namespace

// The version format string of one of our builds: "Mod %d.%02d.%d:%c" + 17-character tag + " ". False: not ours.
bool readTag(FirmwareIdentity& id, const std::string& text) {
    const std::string prefix = "Mod %d.%02d.%d:%c";
    if (text.rfind(prefix, 0) != 0 || text.size() < prefix.size() + kTagLength) return false;
    const std::string base = based(id.version);
    id.tag = text.substr(prefix.size(), kTagLength);
    id.build = decodeModTag(id.tag);
    if (!id.build) {
        id.kind = FirmwareIdentity::Kind::Damaged;
        id.summary = "Our build" + base + ", but its tag " + id.tag + " is damaged";
        return true;
    }
    id.kind = FirmwareIdentity::Kind::Ours;
    std::string mods = "Custom category";  // always in (MODTAG.md)
    for (int slot : id.build->mods) mods += ", " + modSlotName(slot);
    id.summary = "Our build" + base + ": " +
                 (id.build->preGit() ? std::string("pre-git build") :
                                       "commit " + id.build->commit + (id.build->dirty ? " (uncommitted changes)" : "")) +
                 "; mods: " + mods;
    return true;
}

std::optional<ModTag> decodeModTag(const std::string& tag) {
    if (tag.size() != std::size_t(kTagLength)) return std::nullopt;
    u128 value = 0;
    for (char ch : tag) {
        const char* p = std::strchr(kAlphabet, ch);
        if (!p || ch == 0) return std::nullopt;
        value = value * 62 + u128(p - kAlphabet);
    }
    if (value >> 101) return std::nullopt;
    const u128 body = (value >> 8) ^ scramble();
    if (crc8(body) != std::uint8_t(value) || (body >> 90) != 1) return std::nullopt;
    ModTag t;
    t.layout = 1;
    t.dirty = (body >> 89) & 1;
    char hex[8];
    std::snprintf(hex, sizeof hex, "%07x", unsigned((body >> 61) & 0xFFFFFFF));
    t.commit = hex;
    for (int n = 0; n < 61; ++n)
        if ((body >> n) & 1) t.mods.push_back(n);
    return t;
}

std::string encodeModTag(const std::string& commitHex, std::uint64_t modBits, bool dirty) {
    const u128 commit = u128(std::stoul(commitHex.substr(0, 7), nullptr, 16) & 0xFFFFFFF);
    const u128 body = (u128(1) << 90) | (u128(dirty) << 89) | (commit << 61) | (u128(modBits) & ((u128(1) << 61) - 1));
    u128 value = ((body ^ scramble()) << 8) | crc8(body);
    std::string out(kTagLength, 'A');
    for (int i = kTagLength - 1; i >= 0; --i) {
        out[std::size_t(i)] = kAlphabet[int(value % 62)];
        value /= 62;
    }
    return out;
}

// MODTAG.md "Mod slot registry". The Custom category is in every build and has no slot.
std::string modSlotName(int slot) {
    switch (slot) {
    case 0: return "NAM block";
    case 1: return "MIDI Guitar";
    default: return "slot " + std::to_string(slot);
    }
}

std::vector<int> knownModSlots() {
    return {0, 1};
}

FirmwareIdentity identifyFirmware(const std::array<std::uint8_t, 32>& imageSha256, const AppImage& app) {
    FirmwareIdentity id;
    std::string text, line6;
    try {
        text = cstr(app.at(kVersionString, kVersionSlot));
        line6 = cstr(app.at(kVersionString + kVersionSlot, 8));
    } catch (const FormatError&) {
        id.summary = "Not a firmware the editor knows (no version string at the v3.80 LT address)";
        return id;
    }
    for (const KnownBuild& k : kStock)
        if (line6 == k.line6Hash) id.version = k.name;
    const std::string hash = toHex(Bytes(imageSha256.data(), imageSha256.size()));
    for (const KnownBuild& k : kStock)
        if (hash == k.imageSha256) {
            id.kind = FirmwareIdentity::Kind::Stock;
            id.version = k.name;
            id.summary = "Stock firmware " + id.version;
            return id;
        }
    if (readTag(id, text)) return id;
    id.summary = "Not stock and not built by these tools" + based(id.version);
    return id;
}

FirmwareIdentity identifyUnit(const std::optional<std::string>& reply) {
    FirmwareIdentity id;
    if (!reply) {
        id.summary = "Stock firmware, or a build from before the tag command (MODTAG.md: command 200)";
        return id;
    }
    if (!readTag(id, *reply)) {
        id.kind = FirmwareIdentity::Kind::Damaged;
        id.summary = "Answers the tag command, but with \"" + *reply + "\"";
    }
    return id;
}

} // namespace hx

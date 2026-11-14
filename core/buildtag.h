#pragma once

// Which firmware this is: stock (by hash) or one of our builds (by the tag on its Global Settings screen). The tag's
// encoding and the mod slot registry are specified in MODTAG.md (in the firmware project, next to mod.md; other
// comments here just say "MODTAG.md"); keep both in step with it.

#include "core/m4app.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hx {

// A decoded build tag (MODTAG.md, layout 1).
struct ModTag {
    int layout = 1;
    bool dirty = false;
    std::string commit;     // 7 hex digits; "0000000" = built before the project was under git
    std::vector<int> mods;  // slots in the build
    bool preGit() const { return commit == "0000000"; }
};

std::optional<ModTag> decodeModTag(const std::string& tag);  // nullopt: not a valid layout-1 tag
std::string encodeModTag(const std::string& commitHex, std::uint64_t modBits, bool dirty);  // for tests / tools

// The registry's name for a slot ("slot N" when this editor doesn't know it: a build from a newer tool).
std::string modSlotName(int slot);
// The slots the registry names (MODTAG.md "Mod slot registry"), in slot order.
std::vector<int> knownModSlots();

struct FirmwareIdentity {
    enum class Kind { Stock, Ours, Damaged, Other };
    Kind kind = Kind::Other;
    std::string version;   // "3.80.0" for the builds we know (from Line 6's build hash next to the string)
    std::string tag;       // the tag text (Ours, Damaged)
    std::optional<ModTag> build;
    std::string summary;   // one line for the UI / --info
};

// A version format string ("Mod %d.%02d.%d:%c<tag> ") into id (kind, tag, build, summary). False: not one of ours.
bool readTag(FirmwareIdentity& id, const std::string& text);
// A connected unit, from its answer to editor command 200 (MODTAG.md "Reading the tag over USB"); nullopt = refused.
FirmwareIdentity identifyUnit(const std::optional<std::string>& reply);
// imageSha256 = HxfImage::imageSha256 (the inflated image as in the file, not the swapped one).
FirmwareIdentity identifyFirmware(const std::array<std::uint8_t, 32>& imageSha256, const AppImage& app);

} // namespace hx

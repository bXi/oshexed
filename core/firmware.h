#pragma once

#include "core/buildtag.h"
#include "core/dspmeta.h"
#include "core/graphics.h"
#include "core/hxf.h"
#include "core/m4app.h"
#include "core/modeltables.h"
#include "core/presets.h"

#include <map>
#include <optional>

#include <memory>
#include <string>
#include <string_view>

namespace hx {

constexpr std::uint32_t kDspBaseCost = 1500;  // every path's fixed share (NOTES.md "DSP usage")

// What the editor knows about a preset model number (the cost table's u16; NOTES.md "Model
// registration chain").
struct ModelRef {
    std::uint16_t modelNo = 0;
    std::uint32_t symId = 0;
    std::uint32_t cost = 0;
    std::string name;           // browser name, else rec 4 displayName, else symbol
    int category = -1;          // browser category (2 = distortion ... 18 = looper), -1 if not listed
    int list = -1, entry = -1;  // position in ModelTables::lists, -1 if not listed
    const MetaEntry* meta = nullptr;
};

// Everything the editor reads from the user's .hxf, parsed once. The app image and tables point
// into `hxf`, so this lives behind a unique_ptr and is never copied.
struct Firmware {
    std::string path;
    std::unique_ptr<HxfImage> hxf;
    AppImage app;
    DspMeta meta;
    ModelTables tables;
    PresetLibrary presets;
    std::optional<Graphics> gfx;          // rec 8/9; missing = a warning, no icons
    std::optional<CategoryIcons> icons;   // block icon per category and state
    std::map<std::uint16_t, ModelRef> models;  // by model number
    std::vector<RecordChecks> checks;   // one per hxf->records entry, computed at load
    FirmwareIdentity identity;          // stock, or one of our builds by its tag (MODTAG.md)
    std::vector<std::string> warnings;  // failed checks; the data still loaded

    const ModelRef* model(std::uint16_t modelNo) const;
    const ModelRef* modelBySym(std::uint32_t symId) const;
    // A rec 4 entry by its symbol name, e.g. "HD2_AppDSPFlowOutput" (the path inputs / outputs have no model number).
    const MetaEntry* metaBySymbol(std::string_view symbol) const;
    // The unit's DSP total for a path: kDspBaseCost + the cost-table entries of its blocks (a linked cab counts too);
    // 9000 = the DSP's budget (90%).
    std::uint32_t pathCost(const PresetPath& path) const;

    Firmware() = default;
    Firmware(const Firmware&) = delete;
    Firmware& operator=(const Firmware&) = delete;
};

std::unique_ptr<Firmware> loadFirmware(const std::string& path, std::string* error = nullptr);

} // namespace hx

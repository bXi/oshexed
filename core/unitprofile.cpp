#include "core/unitprofile.h"

#include "core/presets.h"

#include <algorithm>
#include <iterator>

namespace hx {

namespace {

struct Model {
    UnitModel model;
    std::string_view id, name;
};
constexpr Model kModels[] = {
    {UnitModel::HelixFloor, "helix-floor", "Helix Floor"}, {UnitModel::HelixLt, "helix-lt", "Helix LT"},
    {UnitModel::HelixRack, "helix-rack", "Helix Rack"},    {UnitModel::HxStomp, "hx-stomp", "HX Stomp"},
    {UnitModel::HxStompXl, "hx-stomp-xl", "HX Stomp XL"},  {UnitModel::HxEffects, "hx-effects", "HX Effects"},
};

// The Helix LT, from the Helix LT 3.80 Owner's Manual (Input / Output block tables, Global Settings pages) against
// the Helix (Floor) 3.80 manual. Port names as in ioInputs() / ioOutputs() (our own table).
// Inputs: None, Multi (= Guitar + Variax), Guitar, Variax, Variax Magnetics, Return 1 / 2 / 1/2, USB 3/4 / 5/6 / 7/8;
// no Aux, Mic, Return 3 / 4 / 3/4 or S/PDIF.
constexpr std::string_view kLtNoInputs[] = {"Aux", "Mic", "Return 3", "Return 4", "Return 3/4", "S/PDIF"};
// Outputs: Multi, Path 2A / 2B / 2A+B, 1/4", XLR, Send 1/2, Digital (AES/EBU, L6 LINK), USB 1/2 / 3/4 / 5/6.
constexpr std::string_view kLtNoOutputs[] = {"Send 3/4"};
// Global settings not on the LT's menu: 6 Digital Output type (the LT is AES/EBU only), 19 Stomp Mode Switches,
// 22 Main LCD Brightness, 23 / 24 Scribble LCD Select / Contrast, 70 / 72 EXP 3 Polarity / Pedal Position (no EXP 3
// jack), 84 Lock Snapshot (in no Helix manual), 102 / 126 Pedal Position / Tempo BPM Display (scribble strips). The
// unit still stores them when sent (22: status 0 + notice, no effect; 2026-11-08).
constexpr int kLtNoSettings[] = {6, 19, 22, 23, 24, 70, 72, 84, 102, 126};

template <typename Range, typename T> bool contains(const Range &r, const T &v) {
    return std::find(std::begin(r), std::end(r), v) != std::end(r);
}

} // namespace

std::string_view unitId(UnitModel m) {
    for (const Model &x : kModels)
        if (x.model == m) return x.id;
    return {};
}

std::string_view unitName(UnitModel m) {
    for (const Model &x : kModels)
        if (x.model == m) return x.name;
    return "Unknown unit";
}

UnitModel unitFromId(std::string_view id) {
    for (const Model &x : kModels)
        if (x.id == id) return x.model;
    return UnitModel::Unknown;
}

// The .hxf HEAD product IDs of the 3.80.0 files (the three Helix files differ: own IDs, own image hashes).
UnitModel unitFromProductId(std::uint32_t productId) {
    switch (productId) {
    case 0x00210001: return UnitModel::HelixFloor;
    case 0x00210002: return UnitModel::HelixRack;
    case 0x00210004: return UnitModel::HelixLt;
    default: return UnitModel::Unknown;
    }
}

bool inputAvailable(UnitModel m, int source) {
    const auto ports = ioInputs();
    if (source < 0 || std::size_t(source) >= ports.size()) return false;
    if (m == UnitModel::HelixLt) return !contains(kLtNoInputs, std::string_view(ports[std::size_t(source)].label));
    return true;
}

bool outputAvailable(UnitModel m, int dest) {
    const auto ports = ioOutputs();
    if (dest < 0 || std::size_t(dest) >= ports.size()) return false;
    if (m == UnitModel::HelixLt) return !contains(kLtNoOutputs, std::string_view(ports[std::size_t(dest)].label));
    return true;
}

bool globalSettingAvailable(UnitModel m, int id) {
    if (m == UnitModel::HelixLt) return !contains(kLtNoSettings, id);
    return true;
}

} // namespace hx

#pragma once

// The unit's global settings (its Global Settings menu; NOTES.md "Global settings"): read and set over the editor
// protocol by id (cmd 24 / cmd 25, notice 22). Which ids, how they're grouped and what kind of value they take is
// ours (found in the v3.80 LT M4 app); their labels and option names are read from the user's firmware at runtime
// (appGlobalParamStringsTrio's tables), so the repo holds none of the unit's strings.

#include "core/m4app.h"

#include <string>
#include <vector>

namespace hx {

struct GlobalSetting {
    enum class Kind { Bool, List, Number };
    int id = 0;
    std::string group;                 // the menu page: "Ins/Outs", "Preferences", ...
    std::string label;                 // from the firmware
    Kind kind = Kind::List;
    std::vector<std::string> options;  // Bool / List: the value names (from the firmware; Off / On if it has none)
    std::vector<int> values;           // List: the value each option stands for (empty: its index)
    double lo = 0, hi = 0, step = 1;   // Number
};

// The settings the LT's menu shows, with their labels and options from `app` (the v3.80 LT M4 app). Settings whose
// strings can't be read (another firmware) are left out; `warnings` says so once.
std::vector<GlobalSetting> readGlobalSettings(const AppImage& app, std::vector<std::string>* warnings = nullptr);

// The guitar input's settings a preset stores (key 5): Guitar In-Z (30) and Guitar Pad (134: Global / Off / On, Global =
// the unit's setting 31). Shown with the input block; empty if the firmware can't name them.
std::vector<GlobalSetting> readInputSettings(const AppImage& app);

// The footswitch LED colours by index (preset switch entry key 16, editor cmd 61): 0 "Auto Color" ... 11 "Off".
// Empty if the firmware can't name them.
std::vector<std::string> readLedColourNames(const AppImage& app);

} // namespace hx

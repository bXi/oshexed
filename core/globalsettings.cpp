#include "core/globalsettings.h"

#include <array>
#include <span>

namespace hx {

namespace {

// v3.80 LT M4 app (NOTES.md "Global settings"): appGlobalParamStringsTrio_280945e4 names id N < 137 with the 27-byte
// record N of the array its literal at kLabelArray points to; _28094784 prints a value with the 20-byte record
// `value` of a per-setting table (the address of that table's pointer is in the list below).
constexpr std::uint32_t kLabelArray = 0x28094698;
constexpr std::uint32_t kLabelArray150 = 0x280946a0;  // ids 150..164: their own array (index id - 150)
constexpr std::size_t kLabelSize = 27, kOptionSize = 20;

using K = GlobalSetting::Kind;
struct Def {
    int id;
    const char* group;
    K kind;
    std::uint32_t optionsPtr;  // address of the option table's pointer; 0 = none (Bool: Off / On)
    int count;                 // options used
    double lo = 0, hi = 0, step = 1;
    // Some tables hold more than the LT offers (an unused "Multi" at 0, other models' outputs): the values (= table
    // records) the LT's menu shows, in its order; none = 0 .. count - 1. (Read on the user's LT with
    // `oshexed --settings` next to its screen, 2026-11-13: Multi = 1, Re-amp Src Guitar = 1, Variax = 2, Variax Mags = 3.)
    std::array<int, 8> values{};
    int nValues = 0;
    // Settings whose value names come from a model's parameter data rather than a table: our own words for them
    // (generic input names), with `values`.
    std::array<const char*, 4> names{};
};

// The LT's Global Settings pages. Left out: Variax (its tables don't separate cleanly), the preset-level ids
// (tempo 16, pads, Powercab / DT: they live in each preset), HX Stomp-only ones (Function FS3-5, Tip / Ring).
constexpr Def kDefs[] = {
    {31, "Ins/Outs", K::Bool, 0x28094df8, 2},  // the pad the presets' "Global" choice (setting 134) falls back to
    {0, "Ins/Outs", K::Bool, 0x28094df4, 2},
    {1, "Ins/Outs", K::Bool, 0x280954e0, 2},
    {2, "Ins/Outs", K::Bool, 0x28094df4, 2},
    {3, "Ins/Outs", K::Bool, 0x28094df4, 2},
    // ids 153..164 (LT Ins/Outs page 2; NOTES "Global settings"); choices and ranges from the user's LT screen
    {159, "Ins/Outs", K::List, 0x28095878, 4, 0, 0, 1, {1, 2, 3}, 3},           // USB In 1/2 Destination
    {153, "Ins/Outs", K::Number, 0, 0, -60, 18, 0.1},                          // USB In 1/2 Trim (dB)
    {160, "Ins/Outs", K::List, 0, 0, 0, 0, 1, {1, 2, 3}, 3, {"Guitar", "Variax", "Variax Mags"}},  // Re-amp Src (USB 7)
    {161, "Ins/Outs", K::List, 0, 0, 0, 0, 1, {1, 2, 3}, 3, {"Guitar", "Variax", "Variax Mags"}},  // Re-amp Src (USB 8)
    {156, "Ins/Outs", K::List, 0x2809588c, 6, 0, 0, 1, {1, 2, 3, 4, 5}, 5},    // Volume Knob Controls
    {158, "Ins/Outs", K::List, 0x28095838, 4, 0, 0, 1, {1, 2, 3}, 3},           // Headphones Monitor
    {164, "Ins/Outs", K::Number, 0, 0, -60, 18, 0.1},                          // Digital Out Level (dB)
    {6, "Ins/Outs", K::List, 0x280954f0, 2},
    {7, "Ins/Outs", K::List, 0x280954d0, 4},
    {14, "Preferences", K::List, 0x28095840, 3},
    {15, "Preferences", K::Number, 0, 0, 30, 240, 0.1},
    {65, "Preferences", K::Bool, 0, 0},
    {73, "Preferences", K::List, 0x280954e4, 2},
    {103, "Preferences", K::List, 0x2809582c, 2},
    {27, "Preferences", K::Bool, 0x280954d8, 2},
    {102, "Preferences", K::List, 0x2809584c, 2},
    {126, "Preferences", K::List, 0x280954bc, 2},
    {127, "Preferences", K::List, 0x28095858, 2},
    {128, "Preferences", K::List, 0x28095870, 2},
    {136, "Preferences", K::List, 0x28095148, 2},
    {104, "Preferences", K::List, 0x28095500, 2},
    {18, "Footswitches", K::List, 0x280954d4, 8},
    {19, "Footswitches", K::Bool, 0x280954fc, 2},
    {20, "Footswitches", K::List, 0x280954dc, 3},
    {117, "Footswitches", K::List, 0x280954c4, 2},
    {67, "Footswitches", K::List, 0x280954cc, 2},
    {84, "Footswitches", K::List, 0x28095874, 2},
    {66, "EXP Pedals", K::List, 0x28095170, 3},
    {71, "EXP Pedals", K::List, 0x28095170, 3},
    {72, "EXP Pedals", K::List, 0x28095170, 3},
    {68, "EXP Pedals", K::List, 0x28095174, 2},
    {69, "EXP Pedals", K::List, 0x28095174, 2},
    {70, "EXP Pedals", K::List, 0x28095174, 2},
    {75, "EXP Pedals", K::List, 0x28095848, 2},
    {9, "MIDI/Tempo", K::List, 0x280954c0, 17},
    {10, "MIDI/Tempo", K::Bool, 0x28094df8, 2},
    {11, "MIDI/Tempo", K::Bool, 0x28094df8, 2},
    {12, "MIDI/Tempo", K::List, 0x280954b8, 4},
    {13, "MIDI/Tempo", K::List, 0x28095868, 4},
    {76, "MIDI/Tempo", K::List, 0x28095864, 4},
    {77, "MIDI/Tempo", K::List, 0x280954b8, 4},
    {79, "MIDI/Tempo", K::Bool, 0x28095880, 2},
    {135, "MIDI/Tempo", K::List, 0x28095148, 2},
    // 22 / 23 / 24: not on the LT (core/unitprofile.cpp hides them there).
    {22, "Displays", K::Number, 0, 0, 0, 100, 1},
    {23, "Displays", K::List, 0x280954ec, 14},
    {24, "Displays", K::Number, 0, 0, 0, 100, 1},
    {25, "Displays", K::Bool, 0x280954f8, 2},
    {26, "Displays", K::Bool, 0x28094df8, 2},
};

// Stored in each preset (key 5) rather than the unit: the guitar input's impedance and pad (Global / Off / On).
constexpr Def kInputDefs[] = {
    {30, "Input", K::List, 0x280954e8, 9},
    {134, "Input", K::List, 0x28095850, 3},
};

// A label record: text up to the NUL; a line break shows as a space ("Send/\nReturn 1" -> "Send/Return 1").
std::string text(Bytes b) {
    std::string out;
    for (std::uint8_t c : b) {
        if (c == 0) break;
        if (c == '\n') {
            if (!out.empty() && out.back() != '/') out += ' ';
            continue;
        }
        if (c < 32 || c > 126) return {};  // not a string: another firmware
        out += char(c);
    }
    return out;
}

} // namespace

namespace {

std::vector<GlobalSetting> readDefs(const AppImage& app, std::span<const Def> defs, bool& missing) {
    std::vector<GlobalSetting> out;
    for (const Def& d : defs) {
        try {
            GlobalSetting s;
            s.id = d.id;
            s.group = d.group;
            s.kind = d.kind;
            s.lo = d.lo;
            s.hi = d.hi;
            s.step = d.step;
            const std::uint32_t labels = d.id >= 150 ? app.u32(kLabelArray150) + std::uint32_t(d.id - 150) * kLabelSize
                                                     : app.u32(kLabelArray) + std::uint32_t(d.id) * kLabelSize;
            s.label = text(app.at(labels, kLabelSize));
            for (int k = 0; k < d.nValues; ++k) s.values.push_back(d.values[std::size_t(k)]);
            if (d.names[0]) {
                for (int k = 0; k < d.nValues; ++k) s.options.push_back(d.names[std::size_t(k)]);
            } else if (d.optionsPtr) {
                const std::uint32_t table = app.u32(d.optionsPtr);
                if (d.nValues) {
                    for (int v : s.values) {
                        if (v >= d.count) throw FormatError("option outside its table");
                        s.options.push_back(text(app.at(table + std::uint32_t(v) * kOptionSize, kOptionSize)));
                    }
                } else {
                    for (int k = 0; k < d.count; ++k) s.options.push_back(text(app.at(table + std::uint32_t(k) * kOptionSize, kOptionSize)));
                }
            } else if (d.kind == K::Bool) {
                s.options = {"Off", "On"};
            }
            bool ok = !s.label.empty();
            for (const std::string& o : s.options) ok = ok && !o.empty();
            if (!ok) {
                missing = true;
                continue;
            }
            out.push_back(std::move(s));
        } catch (const FormatError&) {
            missing = true;
        }
    }
    return out;
}

} // namespace

std::vector<GlobalSetting> readGlobalSettings(const AppImage& app, std::vector<std::string>* warnings) {
    bool missing = false;
    std::vector<GlobalSetting> out = readDefs(app, kDefs, missing);
    if (missing && warnings) warnings->push_back("some global settings can't be named from this firmware (made for v3.80 LT)");
    return out;
}

std::vector<GlobalSetting> readInputSettings(const AppImage& app) {
    bool missing = false;
    return readDefs(app, kInputDefs, missing);
}

std::vector<std::string> readLedColourNames(const AppImage& app) {
    // *0x280be328 -> 16-byte records "Auto Color", "White", "Red" ... "Pink", "Off" (v3.80 LT).
    constexpr std::uint32_t kTablePtr = 0x280be328, kRecord = 16;
    constexpr int kCount = 12;
    std::vector<std::string> names;
    try {
        const std::uint32_t table = app.u32(kTablePtr);
        for (int i = 0; i < kCount; ++i) {
            std::string s = text(app.at(table + std::uint32_t(i) * kRecord, kRecord));
            if (s.empty()) return {};  // not this table: another firmware
            names.push_back(std::move(s));
        }
    } catch (const FormatError&) {
        return {};
    }
    return names;
}

} // namespace hx

#pragma once

#include "core/presets.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hx {

struct Firmware;

// Reads a preset file exported by HX Edit (.hlx: JSON, NOTES.md ".hlx files"). Models and knobs are matched by
// their symbols in the user's firmware (rec 4), so a model the firmware doesn't have is skipped with a warning.
// Read: blocks (lane, position, on/off, knobs, linked cab), split / join, routing, inputs / outputs (source, gate,
// destination, pan, level), snapshots (name, colour, tempo, block on/off, knob values), controller assignments,
// footswitch bypass assignments, Command Center (instant, footswitch and EXP pedal commands, with each snapshot's
// values), tempo and input settings. What the model doesn't hold (the toe switch's / Variax knobs' commands, DT /
// Powercab / Variax sections, block flags like @trails) is kept as JSON text and written back by exportHlx.
std::optional<Preset> importHlx(std::string_view text, const Firmware& fw, std::string* error = nullptr,
                                std::vector<std::string>* warnings = nullptr);

// Writes a preset as an HX Edit preset file (.hlx), the layout importHlx reads. Blocks whose model the firmware
// doesn't describe are left out with a warning. The DT / Powercab / Variax sections are only written when the preset
// came from a .hlx (kept text); otherwise HX Edit uses its defaults.
std::string exportHlx(const Preset& preset, const Firmware& fw, std::vector<std::string>* warnings = nullptr);

// HX Edit's routing names (global @topology0 / @topology1) for PresetRouting::mode 0-4.
const char* hlxTopology(int mode);

} // namespace hx

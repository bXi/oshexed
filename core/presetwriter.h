#pragma once

// The editor's preset model back into the unit's l6-helix format (NOTES.md "Preset format"), for everything that sends
// or stores a whole preset: undo / redo and .hlx imports on the unit (cmd 21), library writes (lib 8), backups.
//
// The writer starts from the map the preset was read from (Preset::raw), so every key the model doesn't hold stays as
// it was, and writes the modelled parts over it: the two paths (blocks with their own stored content, which moves
// with them; inputs, outputs, split, merge, routing), the snapshots (names, tempo, colour, block states, controller
// values), controllers, footswitches, Command Center and the global params. Data indexed by global slot (snapshot
// block states, controller / footswitch targets, the selected block) follows each block to its new slot. A preset
// that wasn't read from a blob (an .hlx import) starts from `base`, an empty stored preset.
//
// An unchanged preset comes back byte for byte (msgpack values keep the encoding they were read with).

#include "core/msgpack.h"
#include "core/presets.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hx {

struct PresetWriteOptions {
    // Key 9 for a block that has none (made in the editor); -1 = unknown (the write fails with a reason).
    std::function<int(const PresetBlock&)> blockType;
    // The blob to start from when the preset has no raw (an empty "New Preset" from the library).
    const std::vector<std::uint8_t>* base = nullptr;
};

std::optional<std::vector<std::uint8_t>> writePresetBlob(const Preset& preset, const PresetWriteOptions& options,
                                                         std::string* error);

// A preset map as an l6-helix blob: the magic, the 12-word index of the top-level keys, the map.
std::vector<std::uint8_t> encodePresetBlob(const mp::Value& map);

} // namespace hx

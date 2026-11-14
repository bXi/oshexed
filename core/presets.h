#pragma once

#include "core/bytes.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hx {

// Factory presets and setlists from the firmware (NOTES.md "Preset format" and "Records 8-15"):
// rec 11 = preset bodies, rec 12 = preset names, rec 13 = setlist names (and global defaults).

constexpr int kSetlists = 8;
constexpr int kPresetsPerSetlist = 128;
constexpr int kLaneSlots = 8;        // blocks per path lane on the LT
constexpr int kSlotsPerPath = 20;    // a path's slot list: input A, 8 x lane A, output A, split, 8 x lane B, join
constexpr int kSnapshots = 8;
constexpr int kFootswitchSlots = 13; // FS1..FS12, then the expression pedal's toe switch
constexpr int kToeSwitch = 12;
constexpr int kControllerIds = 64;   // controller assignments per preset (length of each snapshot's value list)

// Global param ids (preset key 5) with known .hlx names.
namespace globalid {
constexpr int Tempo = 16, GuitarInputZ = 30, GuitarPad = 134, PowercabSelect = 380, DtSelect = 381;
} // namespace globalid

// Controller sources. Preset key 4 is a list indexed by them. Footswitch FSn is source n + 5.
namespace controller {
constexpr int None = 0, Exp1 = 1, Exp2 = 2, VariaxVolume = 4, VariaxTone = 5, MidiCc = 18, Snapshot = 19;
constexpr int footswitch(int fsIndex) { return fsIndex + 6; }  // fsIndex 0 = FS1
} // namespace controller
// "EXP 1", "FS 8", "Snapshot", or "Controller N" for sources not identified yet.
std::string controllerName(int source);
// "FS1".."FS12", "EXP toe".
std::string footswitchName(int fsIndex);

struct PresetValue {
    enum class Kind { None, Bool, Int, Float };
    Kind kind = Kind::None;
    double v = 0;
};

struct PresetBlock {
    std::uint8_t slotType = 6;  // 6 = block, 7 = looper (stored with different keys, see presets.cpp)
    std::uint16_t modelNo = 0;
    int cabModelNo = -1;        // linked cab (amp+cab), -1 if none
    bool enabled = true;
    std::uint32_t blockType = 0;  // key 9
    std::vector<PresetValue> values;  // key 11.4, in the model's knob order
    // Key 12.4: the linked model's knobs (cabModelNo: the cab of an amp+cab / preamp+cab, a dual cab's second cab).
    std::vector<PresetValue> cabValues;
    // From an imported .hlx: the block's members this model doesn't hold (@trails, @bypassvolume, @uuid, ...), as a
    // JSON object's text, written back on export. "" otherwise.
    std::string hlxAttrs;
    // Stable within its preset, so controller and footswitch assignments follow the block when it moves.
    // Blocks read from a preset get their global slot (dsp * 20 + slot list index); new ones Preset::newUid().
    int uid = -1;
    // The slot content it was read with (key 20, as msgpack bytes), so writing it back keeps the keys this model
    // doesn't hold; it moves with the block. Null for blocks made here.
    std::shared_ptr<const std::vector<std::uint8_t>> raw;
    // On / off in each snapshot (snapshot key 3). `enabled` is the state of the snapshot in use.
    std::array<bool, kSnapshots> snapOn = {true, true, true, true, true, true, true, true};
};

// What an assignment points at: a block (by uid) or a fixed item of a path (inputs, split, join) by its global
// slot (dsp * 20 + slot list index).
struct PresetTarget {
    int block = -1;  // PresetBlock::uid, or -1
    int slot = -1;   // the fixed item's global slot when block is -1
};

// One knob driven by a controller (preset key 4).
struct ControllerAssign {
    int id = 0;            // 0..63, its value's index in each snapshot (key 0)
    int source = 0;        // controller::*
    int cc = 4;            // key 1.1: the MIDI CC number (used with controller::MidiCc; 4 when unset, as HX Edit sends)
    PresetTarget target;
    int param = -1;        // index into the target's values (key 6.29); -1 if the entry has none
    PresetValue min, max;  // keys 2 / 3: the knob's value at the controller's minimum / maximum
    // Snapshot key 2[id]: the knob's value in each snapshot (used when source is Snapshot).
    std::array<PresetValue, kSnapshots> snapValues;
    std::string hlxAttrs;  // from a .hlx: members not held here (@cc ...), JSON object text
};

// One function of a footswitch (preset key 3.8[switch]); a switch can carry several.
struct FootswitchAssign {
    int kind = 1;               // 11.0: 1 block bypass, 2 a knob (controller), 3 Command Center
    std::string label;          // 11.5, as the scribble strip shows it
    // 11.6 / 11.7: a colour value and flag of unknown meaning (11.6 differs per kind: 462860 on command functions);
    // kept as read. Not the LED: that is ledColour (hx-edit-ops-2).
    bool customColour = false;  // 11.7
    std::uint32_t colour = 0;   // 11.6
    int ledColour = 0;          // entry key 16: index into the unit's LED colour names (0 Auto Color .. 11 Off)
    bool momentary = false;     // entry key 12
    PresetTarget target;        // kinds 1 and 2 (11.8)
    int param = -1;             // kind 2: the knob (11.9.29)
    std::string hlxAttrs;       // from a .hlx: members not held here (@fs_momentary, @fs_primary ...), JSON object text
};

// One Command Center command (preset key 2: 2.0[slot] = type, 2.1[slot] = [?, fields x 6]). Slots (NOTES "Command
// Center"): 0-5 Instant 1-6, 6-17 FS1-FS12, 18 EXP toe switch, 19 / 20 EXP 1 / 2, (21 EXP 3), 22 / 23 Variax Volume /
// Tone knob. Instant and footswitch commands hold their field values per snapshot (PresetSnapshot::commands); here
// they are the snapshot in use's, as the unit stores them.
struct CommandSlot {
    int type = 0;                // 0 = none; commandTypeName()
    std::array<int, 6> fields{};  // in commandFieldNames() order (= the unit's field 1..6)
};
// A command's field values in one snapshot (snapshot key 1: [slot, false, [?, fields x 6]]).
struct SnapshotCommand {
    int slot = 0;
    std::array<int, 6> fields{};
};
// HX Edit's names for the command types ("Type N" when unknown).
std::string commandTypeName(int type);
// The fields of a type with HX Edit's names; empty = none / not known.
std::vector<std::string> commandFieldNames(int type);
// Valid range of a field (MIDI channel 0-16, 0 = the base channel; a list's choices; others 0-127).
std::pair<int, int> commandFieldRange(int type, int field);
// HX Edit's names for a list field's values (MMC message, looper function, ...); empty = a number.
std::vector<std::string> commandFieldChoices(int type, int field);
// A value as HX Edit shows it ("Base" channel, "Off" bank, note names, a choice's name, ...).
std::string commandFieldText(int type, int field, int value);
// The scribble strip a switch gets with a command of this type ("CC Value", "Bank/PC", "Snapshot", ...).
std::string commandSwitchLabel(int type);
// The types a slot can take, in HX Edit's menu order (instant, switch, or pedal / knob ones).
std::vector<int> commandTypesFor(int slot);
constexpr int kCommandSlots = 24;

struct PresetSnapshot {
    std::string name;    // key 4
    int colour = 0;      // key 12: 0 = auto, 1..10 = HX Edit's snapshot colours
    double tempo = 120;  // key 5, BPM
    // Key 1: the field values of the instant / footswitch commands in this snapshot, for the slots the unit keeps a
    // row for (every slot that ever had a command). Preset::commands holds the snapshot in use's (edits go to both).
    std::vector<SnapshotCommand> commands;
};

// One row of the signal flow: lane A or B of a DSP path.
struct PresetLane {
    std::array<std::optional<PresetBlock>, kLaneSlots> slots;
    bool empty() const;
    bool full() const;

    // Puts `block` at slot `at`. An occupied slot makes room: the blocks from `at` up to the next
    // free slot move one right; if there is none to the right, the blocks down to the nearest free
    // slot on the left move one left. Returns the slot used, or -1 when the lane is full.
    int insert(int at, PresetBlock block);
    // Removes and returns the block at `at` (nullopt if empty). The others stay where they are.
    std::optional<PresetBlock> take(int at);
};

// How lane B relates to lane A (NOTES.md "Preset format", path keys 21 and the split/join slots).
// Factory presets use: mode 0 serial (no B); 1 split + join; 2 split, B to its own output;
// 3 B from its own input, joined; 4 B separate (own input and output).
struct PresetRouting {
    int mode = 0;                 // path key 21
    bool splitOn = false;         // split slot key 18
    bool joinOn = false;          // join slot key 18
    int splitPos = 0;             // gap 0..8: before lane column N (stored as N + 1, as HX Edit's @position + 1)
    int joinPos = 8;
    std::uint16_t splitModel = 0; // e.g. SplitY / SplitAB / SplitXOver / SplitDyn (model number)
    std::uint16_t joinModel = 0;  // the mixer (HD2_AppDSPFlowJoin)
    bool splitEnabled = true, joinEnabled = true;          // 15.10 / 17.10
    std::vector<PresetValue> splitValues, joinValues;       // 15.7.4 / 17.7.4, in the model's knob order
    bool hasOwnInputB() const { return mode == 3 || mode == 4; }
    bool hasOwnOutputB() const { return mode == 2 || mode == 4; }
};

// A lane's input: input A = slot 0 {5: source, 7.4: values}; input B = split slot 14 (same keys; used when lane B
// has its own input). Values in the knob order of rec 4 HD2_AppDSPFlow1Input / 2Input: gate on, threshold, decay.
struct PresetInput {
    int source = 0;  // index into ioInputs() (0 = none)
    std::vector<PresetValue> values;
};
// A lane's output: output A = slot 9 {6: destination, 7.4: values}; output B = join slot 16 (same keys; used when
// lane B has its own output). Values in the knob order of HD2_AppDSPFlowOutput: pan, gain.
struct PresetOutput {
    int dest = 0;  // index into ioOutputs() (0 = none)
    std::vector<PresetValue> values;
};
constexpr const char* kInputASymbol = "HD2_AppDSPFlow1Input";
constexpr const char* kInputBSymbol = "HD2_AppDSPFlow2Input";
constexpr const char* kOutputSymbol = "HD2_AppDSPFlowOutput";

// Input / output choices by the value presets store (the same numbers as HX Edit's lists): our short label and
// the firmware image that shows it ("" = none). Path 2A / 2B / 2A+B only exist as outputs of path 1.
struct IoPort {
    const char* label;
    const char* image;
};
std::span<const IoPort> ioInputs();
std::span<const IoPort> ioOutputs();
constexpr int kOutPath2A = 2, kOutPath2B = 3, kOutPath2AB = 4;

struct PresetPath {
    PresetLane a, b;
    PresetRouting routing;
    std::array<PresetInput, 2> inputs;    // lane A, lane B
    std::array<PresetOutput, 2> outputs;  // lane A, lane B
};

// After an edit: a path whose lane B emptied becomes serial; a serial path that gained a lane B
// block becomes split + join (split before B's first block, join at the end), as on the device.
void normalizeRouting(PresetPath& path);

struct Preset {
    std::string name;
    bool stored = false;        // false: the firmware has no body for this slot (name only)
    std::array<PresetPath, 2> paths;  // DSP A = path 1, DSP B = path 2
    std::vector<PresetSnapshot> snapshots;  // kSnapshots when the preset has them (key 10.10)
    int snapshot = 0;                       // the snapshot in use; a stored preset holds snapshot 1's state
    std::vector<ControllerAssign> controllers;
    std::array<std::vector<FootswitchAssign>, kFootswitchSlots> footswitches;
    std::vector<CommandSlot> commands;  // key 2, by slot (24 in factory presets)
    // Key 5: global params by id, in stored order (NOTES "HX Edit resources": 16 tempo, 30 guitar input Z, 134 guitar
    // pad, 45-56 / 111-116 Variax, 254-287 Powercab, 326-351 DT, 380 / 381 Powercab / DT select).
    std::vector<std::pair<int, PresetValue>> globals;
    // From an imported .hlx: the tone sections this model doesn't hold (dt0, variax, powercab0, ...) and global members
    // without an id here, as JSON text by name, written back on export.
    std::vector<std::pair<std::string, std::string>> hlxKeep, hlxGlobals;
    int nextUid = 2 * kSlotsPerPath;
    // The l6-helix blob it was read from (core/presetwriter writes the modelled parts over its map). Null when not
    // read from one (.hlx import, new). Bytes, not the parsed tree: the library holds ~300 of these.
    std::shared_ptr<const std::vector<std::uint8_t>> raw;

    const PresetValue* global(int id) const;
    void setGlobal(int id, PresetValue v);

    int newUid() { return nextUid++; }

    // Where a block sits; dsp -1 when it isn't in the preset.
    struct Place {
        int dsp = -1, lane = 0, slot = 0;
    };
    PresetBlock* findBlock(int uid, Place* where = nullptr);
    const PresetBlock* findBlock(int uid, Place* where = nullptr) const;
    // The values a target's knobs are stored in: a block's, or a split's / join's; nullptr for anything else.
    std::vector<PresetValue>* valuesOf(const PresetTarget& t);

    // Makes snapshot i current: block on/off states, the knobs controlled by Snapshot and the command values.
    void recallSnapshot(int i);
    // Makes snapshot i the one in use for the command values only (the unit sends the blocks' and knobs' changes as
    // their own events). recallSnapshot calls it.
    void useSnapshotCommands(int i);
    // Snapshot `to` takes `from`'s block states, Snapshot-controlled values, tempo and colour (its name stays). When
    // `to` is the one in use it is recalled.
    void copySnapshot(int from, int to);
    // Drops controller and footswitch assignments whose block is gone (deleted or replaced).
    void pruneAssignments();
    // The first free controller id, or -1 when all 64 are used.
    int freeControllerId() const;
    // Command Center: sets slot's type (fields to their defaults); a footswitch slot also gets / loses its kind 3
    // function, labelled with the type's name. Type 0 clears it.
    void setCommand(int slot, int type);
    void setCommandField(int slot, int field, int value);  // clamped to commandFieldRange
};

struct PresetLibrary {
    std::vector<std::string> setlists;  // kSetlists names
    std::vector<Preset> presets;        // kSetlists * kPresetsPerSetlist, indexed by slot
};

// Any of the three may be empty (record missing); what can be read is read. Throws nothing.
PresetLibrary readPresets(Bytes rec11, Bytes rec12, Bytes rec13, std::vector<std::string>* warnings = nullptr);

// Parses one preset body (the map after the "l6-helix" header). Exposed for tests.
std::optional<Preset> parsePresetBlob(Bytes blob, std::string* error = nullptr);

// "01A" .. "32D": bank and letter of a slot within its setlist.
std::string presetNumber(int indexInSetlist);

} // namespace hx

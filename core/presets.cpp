#include "core/presets.h"

#include "core/msgpack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hx {

namespace {

constexpr std::size_t kPresetHead = 0x3D;  // "l6-helix" str + 48-byte seek index, then the preset map
constexpr char kPresetMagic[] = "\xa9l6-helix";  // fixstr(9) "l6-helix\0" (NUL is the 10th byte)

// Slot list order per path (NOTES.md "Preset format"): input A, 8 x lane A, output A, split (with input B),
// 8 x lane B, join (with output B).
constexpr std::size_t kLaneAFirst = 1, kLaneBFirst = 11, kSlotCount = 20;
constexpr std::size_t kSlotOutputA = 9, kSlotSplit = 10, kSlotJoin = 19;

// Our labels for the numbers presets store; the images are the firmware's (rec 8/9 names).
constexpr IoPort kInputs[] = {
    {"None", "Block_InNone"},          {"Multi", "Block_InMulti"},      {"Guitar", "Block_InGuitar"},
    {"Aux", "Block_InAux"},            {"Variax", "Block_InVariax"},    {"Variax Mags", "Block_InVariaxMags"},
    {"Mic", "Block_InMic"},            {"Return 1", "Block_InReturn"},  {"Return 2", "Block_InReturn"},
    {"Return 3", "Block_InReturn"},    {"Return 4", "Block_InReturn"},  {"Return 1/2", "Block_InReturn"},
    {"Return 3/4", "Block_InReturn"},  {"S/PDIF", "Block_InSPDIF"},     {"USB 3/4", "Block_InUSB"},
    {"USB 5/6", "Block_InUSB"},        {"USB 7/8", "Block_InUSB"}};
constexpr IoPort kOutputs[] = {
    {"None", ""},                   {"Multi", "Block_OutMulti"},       {"Path 2A", "Block_OutPath2A"},
    {"Path 2B", "Block_OutPath2B"}, {"Path 2A+B", "Block_OutPath2A+B"}, {"1/4\"", "Block_OutQuarter"},
    {"XLR", "Block_OutXLR"},        {"Send 1/2", "Block_OutSend"},     {"Send 3/4", "Block_OutSend"},
    {"Digital", "Block_OutDigital"}, {"USB 1/2", "Block_OutUSB"},      {"USB 3/4", "Block_OutUSB"},
    {"USB 5/6", "Block_OutUSB"}};
constexpr std::int64_t kSlotBlock = 6, kSlotLooper = 7;

PresetValue toValue(const mp::Value& v) {
    switch (v.type) {
    case mp::Type::Bool: return {PresetValue::Kind::Bool, v.b ? 1.0 : 0.0};
    case mp::Type::Int: return {PresetValue::Kind::Int, double(v.i)};
    case mp::Type::Float: return {PresetValue::Kind::Float, v.f};
    default: return {};
    }
}

// A param set {2: n, 3: n, 4: [values]}; returns the values.
std::vector<PresetValue> toValues(const mp::Value* params) {
    std::vector<PresetValue> out;
    if (params)
        if (const mp::Value* values = params->get(4); values && values->isArray())
            for (const mp::Value& v : values->items) out.push_back(toValue(v));
    return out;
}

std::optional<PresetBlock> toBlock(const mp::Value& content) {
    const mp::Value* model = content.get(24);
    if (!model) return std::nullopt;
    PresetBlock b;
    b.modelNo = std::uint16_t(model->get(25) ? model->get(25)->asInt() : 0);
    if (const mp::Value* cab = model->get(26)) b.cabModelNo = int(cab->asInt(-1));
    if (const mp::Value* t = content.get(9)) b.blockType = std::uint32_t(t->asInt());
    if (const mp::Value* en = content.get(10)) b.enabled = en->asBool(true);
    b.values = toValues(content.get(11));
    b.cabValues = toValues(content.get(12));
    return b;
}

// Looper slots (type 7): {8: model, 9: block type, 10: enabled, 7: {4: values}}.
std::optional<PresetBlock> toLooper(const mp::Value& content) {
    const mp::Value* model = content.get(8);
    if (!model) return std::nullopt;
    PresetBlock b;
    b.slotType = std::uint8_t(kSlotLooper);
    b.modelNo = std::uint16_t(model->asInt());
    if (const mp::Value* t = content.get(9)) b.blockType = std::uint32_t(t->asInt());
    if (const mp::Value* en = content.get(10)) b.enabled = en->asBool(true);
    b.values = toValues(content.get(7));
    return b;
}

// Split slot: {14: .., 15: {8: model, 13: position + 1, 10: enabled, 7: params}, 18: on};
// join: {16: .., 17: {8, 13, 10, 7}, 18: on}.
void readRouting(const mp::Value& path, const mp::Value& slots, PresetRouting& r) {
    if (const mp::Value* mode = path.get(21)) r.mode = int(mode->asInt());
    if (const mp::Value* split = slots.items[kSlotSplit].get(20)) {
        if (const mp::Value* on = split->get(18)) r.splitOn = on->asBool();
        if (const mp::Value* m = split->get(15)) {
            if (const mp::Value* model = m->get(8)) r.splitModel = std::uint16_t(model->asInt());
            if (const mp::Value* pos = m->get(13)) r.splitPos = std::clamp(int(pos->asInt()) - 1, 0, kLaneSlots);
            if (const mp::Value* en = m->get(10)) r.splitEnabled = en->asBool(true);
            r.splitValues = toValues(m->get(7));
        }
    }
    if (const mp::Value* join = slots.items[kSlotJoin].get(20)) {
        if (const mp::Value* on = join->get(18)) r.joinOn = on->asBool();
        if (const mp::Value* m = join->get(17)) {
            if (const mp::Value* model = m->get(8)) r.joinModel = std::uint16_t(model->asInt());
            if (const mp::Value* pos = m->get(13)) r.joinPos = std::clamp(int(pos->asInt()) - 1, 0, kLaneSlots);
            if (const mp::Value* en = m->get(10)) r.joinEnabled = en->asBool(true);
            r.joinValues = toValues(m->get(7));
        }
    }
}

// {5: source, 7: {4: [gate on, threshold, decay]}}
void readInput(const mp::Value* content, PresetInput& in) {
    if (!content) return;
    if (const mp::Value* src = content->get(5)) in.source = int(src->asInt());
    in.values = toValues(content->get(7));
}

// {6: destination, 7: {4: [pan, gain]}}
void readOutput(const mp::Value* content, PresetOutput& out) {
    if (!content) return;
    if (const mp::Value* dst = content->get(6)) out.dest = int(dst->asInt());
    out.values = toValues(content->get(7));
}

void readPath(const mp::Value& path, int dsp, PresetPath& out) {
    const mp::Value* slots = path.get(22);
    if (!slots || !slots->isArray() || slots->size() != kSlotCount) return;
    for (std::size_t i = 0; i < kLaneSlots; ++i) {
        for (auto [first, lane] : {std::pair{kLaneAFirst, &out.a}, std::pair{kLaneBFirst, &out.b}}) {
            const mp::Value& slot = slots->items[first + i];
            const mp::Value* type = slot.get(19);
            const mp::Value* content = slot.get(20);
            if (!type || !content) continue;
            if (type->asInt() == kSlotBlock) lane->slots[i] = toBlock(*content);
            else if (type->asInt() == kSlotLooper) lane->slots[i] = toLooper(*content);
            if (auto& b = lane->slots[i]) {
                b->uid = dsp * kSlotsPerPath + int(first + i);
                b->snapOn.fill(b->enabled);
                b->raw = std::make_shared<const std::vector<std::uint8_t>>(mp::encode(*content));
            }
        }
    }
    readRouting(path, *slots, out.routing);
    readInput(slots->items[0].get(20), out.inputs[0]);
    readOutput(slots->items[kSlotOutputA].get(20), out.outputs[0]);
    if (const mp::Value* split = slots->items[kSlotSplit].get(20)) readInput(split->get(14), out.inputs[1]);
    if (const mp::Value* join = slots->items[kSlotJoin].get(20)) readOutput(join->get(16), out.outputs[1]);
}

std::string slotName(std::size_t i) { return "slot " + std::to_string(i); }

// The block at a global slot (dsp * 20 + slot list index); nullptr for inputs, split, join, empty slots.
PresetBlock* blockAt(Preset& p, int global) {
    if (global < 0 || global >= 2 * kSlotsPerPath) return nullptr;
    PresetPath& path = p.paths[std::size_t(global / kSlotsPerPath)];
    const int k = global % kSlotsPerPath;
    if (k >= int(kLaneAFirst) && k < int(kLaneAFirst) + kLaneSlots) {
        auto& s = path.a.slots[std::size_t(k - int(kLaneAFirst))];
        return s ? &*s : nullptr;
    }
    if (k >= int(kLaneBFirst) && k < int(kLaneBFirst) + kLaneSlots) {
        auto& s = path.b.slots[std::size_t(k - int(kLaneBFirst))];
        return s ? &*s : nullptr;
    }
    return nullptr;
}

PresetTarget targetAt(Preset& p, const mp::Value* slot) {
    PresetTarget t;
    if (!slot || slot->type != mp::Type::Int) return t;
    const int global = int(slot->asInt(-1));
    if (const PresetBlock* b = blockAt(p, global)) t.block = b->uid;
    else if (global >= 0 && global < 2 * kSlotsPerPath) t.slot = global;
    return t;
}

// Key 10: {10: [snapshot x 8], 8: not the snapshot in use (values up to 45; the stored block states are snapshot
// 1's in 76 of the 81 factory presets whose snapshots differ)}; a snapshot is {4: name, 5: tempo, 12: colour,
// 3: [[?, on] x 40 slots], 2: [[?, controller id, value] x 64], ...} (NOTES.md "Snapshots").
void readSnapshots(const mp::Value& root, Preset& p) {
    const mp::Value* s = root.get(10);
    const mp::Value* list = s ? s->get(10) : nullptr;
    if (!list || !list->isArray()) return;
    for (std::size_t i = 0; i < list->size() && i < std::size_t(kSnapshots); ++i) {
        const mp::Value& snap = list->items[i];
        PresetSnapshot out;
        if (const mp::Value* n = snap.get(4)) out.name = n->asText();
        if (const mp::Value* c = snap.get(12)) out.colour = int(c->asInt());
        if (const mp::Value* t = snap.get(5)) out.tempo = t->asNumber(120);
        if (const mp::Value* states = snap.get(3); states && states->isArray())
            for (std::size_t g = 0; g < states->size() && g < std::size_t(2 * kSlotsPerPath); ++g)
                if (PresetBlock* b = blockAt(p, int(g)))
                    if (const mp::Value* on = states->items[g].at(1)) b->snapOn[i] = on->asBool(b->enabled);
        // 1: [[slot, false, [?, fields x 6]] x 19]; slot 24 = an unused row (capture command-center3, 2026-11-13).
        if (const mp::Value* rows = snap.get(1); rows && rows->isArray())
            for (const mp::Value& row : rows->items) {
                const mp::Value* slot = row.at(0);
                const mp::Value* vals = row.at(2);
                if (!slot || !vals || !vals->isArray()) continue;
                SnapshotCommand c;
                c.slot = int(slot->asInt(kCommandSlots));
                if (c.slot < 0 || c.slot >= kCommandSlots) continue;
                for (std::size_t k = 1; k < vals->size() && k <= c.fields.size(); ++k) c.fields[k - 1] = int(vals->items[k].asInt());
                out.commands.push_back(c);
            }
        p.snapshots.push_back(std::move(out));
    }
}

// Key 4: [source] -> [{0: id, 1: {0: source, 2: min, 3: max, 5: slot, 6: {29: knob}, ...}}]. Snapshot values
// come from each snapshot's key 2[id].
void readControllers(const mp::Value& root, Preset& p) {
    const mp::Value* bySource = root.get(4);
    if (!bySource || !bySource->isArray()) return;
    const mp::Value* snaps = root.get(10) ? root.get(10)->get(10) : nullptr;
    for (std::size_t src = 0; src < bySource->size(); ++src) {
        const mp::Value& list = bySource->items[src];
        if (!list.isArray()) continue;
        for (const mp::Value& a : list.items) {
            const mp::Value* inner = a.get(1);
            if (!inner) continue;
            ControllerAssign c;
            c.id = int(a.get(0) ? a.get(0)->asInt() : 0);
            c.source = int(inner->get(0) ? inner->get(0)->asInt(std::int64_t(src)) : std::int64_t(src));
            if (const mp::Value* cc = inner->get(1)) c.cc = int(cc->asInt(4));
            c.target = targetAt(p, inner->get(5));
            if (const mp::Value* where = inner->get(6))
                if (const mp::Value* knob = where->get(29)) c.param = int(knob->asInt(-1));
            if (const mp::Value* v = inner->get(2)) c.min = toValue(*v);
            if (const mp::Value* v = inner->get(3)) c.max = toValue(*v);
            // Each snapshot's key 2 lists [flag, controller id (64 = unused), value]; the entry is found by its id, not
            // by position (DEVIN TOWNSEND has controller 2's at index 1).
            if (snaps && snaps->isArray())
                for (std::size_t i = 0; i < snaps->size() && i < std::size_t(kSnapshots); ++i)
                    if (const mp::Value* values = snaps->items[i].get(2); values && values->isArray())
                        for (const mp::Value& e : values->items)
                            if (const mp::Value* id = e.at(1); id && id->type == mp::Type::Int && id->i == c.id) {
                                if (const mp::Value* v = e.at(2)) c.snapValues[i] = toValue(*v);
                                break;
                            }
            p.controllers.push_back(std::move(c));
        }
    }
}

// Key 3: {8: [switch] -> [{10: order, 11: {0: kind, 2: ?, 5: label, 6: ?, 7: ?, 8: slot, 9: {29: knob}},
// 12: momentary, 13: ?, 14: ?, 15: LED colour set, 16: LED colour}, ...]}.
void readFootswitches(const mp::Value& root, Preset& p) {
    const mp::Value* fs = root.get(3);
    const mp::Value* bySwitch = fs ? fs->get(8) : nullptr;
    if (!bySwitch || !bySwitch->isArray()) return;
    for (std::size_t i = 0; i < bySwitch->size() && i < std::size_t(kFootswitchSlots); ++i) {
        const mp::Value& list = bySwitch->items[i];
        if (!list.isArray()) continue;
        for (const mp::Value& a : list.items) {
            const mp::Value* inner = a.get(11);
            if (!inner) continue;
            FootswitchAssign f;
            if (const mp::Value* k = inner->get(0)) f.kind = int(k->asInt(1));
            if (const mp::Value* l = inner->get(5)) f.label = l->asText();
            if (const mp::Value* c = inner->get(6)) f.colour = std::uint32_t(c->asInt()) & 0xffffff;
            if (const mp::Value* c = inner->get(7)) f.customColour = c->asBool();
            if (const mp::Value* c = a.get(16)) f.ledColour = int(c->asInt());
            if (const mp::Value* m = a.get(12)) f.momentary = m->asBool();
            f.target = targetAt(p, inner->get(8));
            if (const mp::Value* where = inner->get(9))
                if (const mp::Value* knob = where->get(29)) f.param = int(knob->asInt(-1));
            p.footswitches[i].push_back(std::move(f));
        }
    }
}

// Key 2: {0: [type x slots], 1: [[?, fields x 6] x slots]} (? = the type in factory presets, 0 in rows the unit made).
void readCommands(const mp::Value& root, Preset& p) {
    const mp::Value* cc = root.get(2);
    const mp::Value* types = cc ? cc->get(0) : nullptr;
    const mp::Value* rows = cc ? cc->get(1) : nullptr;
    if (!types || !types->isArray()) return;
    for (std::size_t i = 0; i < types->size(); ++i) {
        CommandSlot c;
        c.type = int(types->items[i].asInt());
        if (const mp::Value* row = rows ? rows->at(i) : nullptr; row && row->isArray())
            for (std::size_t k = 1; k < row->size() && k <= c.fields.size(); ++k) c.fields[k - 1] = int(row->items[k].asInt());
        p.commands.push_back(c);
    }
}

} // namespace

// Type numbers (NOTES "Command Center": captures command-center / -2, HX Edit's .hlx export): footswitch 1 MIDI CC,
// 2 CC Toggle, 4 Bank/Program, 5 Note On, 6 MMC, 7 Ext Amp, 12 Hotkey, 14 HX Preset, 15 HX Snapshot, 16 HX Looper;
// pedal / Variax knob 3 MIDI CC; instant 18 MIDI CC, 19 Bank/Program, 20 MMC, 8 Ext Amp, 13 Hotkey, 17 HX Looper.
std::string commandTypeName(int type) {
    switch (type) {
    case 0: return "None";
    case 1: case 3: case 18: return "MIDI CC";
    case 2: return "CC Toggle";
    case 4: case 19: return "Bank/Program";
    case 5: return "Note On";
    case 6: case 20: return "MMC";
    case 7: case 8: return "Ext Amp";
    case 12: case 13: return "Hotkey";
    case 14: return "HX Preset";
    case 15: return "HX Snapshot";
    case 16: case 17: return "HX Looper";
    default: return "Type " + std::to_string(type);
    }
}

// HX Edit's field names, in the unit's field order (our field k = the unit's field k + 1; its field 0 is never
// edited). MIDI CC / CC Toggle / Note On checked against captures; the others follow the .hlx keys in HX Edit's
// table order. State fields (CC Toggle / Bank "current", Note "pending") aren't listed.
std::vector<std::string> commandFieldNames(int type) {
    switch (type) {
    case 1: return {"MIDI Channel", "CC #", "Value"};
    case 2: return {"MIDI Channel", "CC #", "Dim Value", "Lit Value"};
    case 3: return {"MIDI Channel", "CC #", "Min Value", "Max Value"};
    case 4: return {"MIDI Channel", "Bank CC00", "Bank CC32", "Program"};
    case 5: return {"MIDI Channel", "Note", "Velocity", "Note Off"};
    case 6: return {"Message"};
    case 7: return {"Ext Amp"};
    case 12: return {"Modifier 1", "Modifier 2", "Modifier 3", "Key"};
    case 14: return {"Preset"};
    case 15: case 16: return {"Behavior", "Press", "Release/Hold"};
    case 18: return {"MIDI Channel", "CC #", "Value", "Wait Time"};
    case 19: return {"MIDI Channel", "Bank CC00", "Bank CC32", "Program", "Wait Time"};
    case 20: return {"Message", "Wait Time"};
    case 8: return {"Ext Amp", "Wait Time"};
    case 13: return {"Modifier 1", "Modifier 2", "Modifier 3", "Key", "Wait Time"};
    case 17: return {"Function", "Wait Time"};
    default: return {};
    }
}

namespace {
std::string commandFieldName(int type, int field) {
    const std::vector<std::string> names = commandFieldNames(type);
    return std::size_t(field) < names.size() ? names[std::size_t(field)] : "";
}
} // namespace

// HX Edit's value names (its res/HelixControls.json controls, as its field table assigns them: mmc, ext_amp_select,
// ext_amp_instant_select, key_modifier_win, looperFunction(Inst), snapshotFunction, fsBehavior, note_off). The "_f15"
// variants are another unit's.
std::vector<std::string> commandFieldChoices(int type, int field) {
    const std::string name = commandFieldName(type, field);
    if (name == "Note Off") return {"Latching", "Momentary"};
    if (name == "Message") return {"Stop", "Play", "Forward", "Rewind", "Pause"};
    if (name == "Ext Amp") {
        if (type == 8) return {"None", "1 (Tip)", "2 (Ring)", "1 and 2"};
        return {"1 (Tip)", "2 (Ring)", "1 and 2"};
    }
    if (name.rfind("Modifier", 0) == 0) return {"None", "Shift", "Alt", "Ctrl", "Windows"};
    // Hotkey key: USB keyboard usage - 3 (A = 1, as HX Edit's export shows; HX Edit's menu lists the same order).
    if (name == "Key") {
        std::vector<std::string> keys = {"(None)"};
        for (char c = 'A'; c <= 'Z'; ++c) keys.push_back(std::string(1, c));
        for (const char* k : {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Enter", "Esc", "Backspace", "Tab",
                              "Space", "-", "=", "[", "]", "\\", "Non-US #", "; (semi-colon)", "' (single quote)",
                              "` (back quote)", ", (comma)", ". (period)", "/", "Caps Lock"})
            keys.push_back(k);
        for (int f = 1; f <= 12; ++f) keys.push_back("F" + std::to_string(f));
        for (const char* k : {"Print Screen", "Scroll Lock", "Pause", "Insert", "Home", "Page Up", "Delete", "End",
                              "Page Down", "Right Arrow", "Left Arrow", "Down Arrow", "Up Arrow", "Num Lock",
                              "/ (Keypad)", "* (Keypad)", "- (Keypad)", "+ (Keypad)", "Enter (Keypad)"})
            keys.push_back(k);
        for (int n = 1; n <= 9; ++n) keys.push_back(std::to_string(n) + " (Keypad)");
        for (const char* k : {"0 (Keypad)", ". (Keypad)", "Non-US \\", "App", "Power"}) keys.push_back(k);
        return keys;
    }
    if (name == "Behavior") return {"Press/Release", "Press/Hold"};
    // looperFunctionInst: looperFunction without None, same numbers (new commands start at 1, capture command-center3).
    if (name == "Function")
        return {"None", "Play", "Stop", "Play/Stop", "Record", "Overdub", "Record/Overdub", "Play Once", "Reverse", "Forward",
                "Forward/Reverse", "Half Speed", "Full Speed", "Half/Full Speed", "Undo"};
    if (name == "Press" || name == "Release/Hold") {
        if (type == 15)
            return {"None", "Next", "Prev", "SNAPSHOT 1", "SNAPSHOT 2", "SNAPSHOT 3", "SNAPSHOT 4", "SNAPSHOT 5",
                    "SNAPSHOT 6", "SNAPSHOT 7", "SNAPSHOT 8"};
        return {"None", "Play", "Stop", "Play/Stop", "Record", "Overdub", "Record/Overdub", "Play Once", "Reverse",
                "Forward", "Forward/Reverse", "Half Speed", "Full Speed", "Half/Full Speed", "Undo"};
    }
    if (name == "Wait Time") {
        std::vector<std::string> out;
        for (int ms = 0; ms <= 1000; ms += 50) out.push_back(std::to_string(ms) + " ms");
        return out;
    }
    return {};
}

// A value as HX Edit shows it: a choice's name, "Base" channel, "Off" bank, a note name, "Next" / "Prev" preset.
std::string commandFieldText(int type, int field, int value) {
    const std::vector<std::string> choices = commandFieldChoices(type, field);
    if (!choices.empty()) return value >= 0 && std::size_t(value) < choices.size() ? choices[std::size_t(value)] : std::to_string(value);
    const std::string name = commandFieldName(type, field);
    if (name == "MIDI Channel") return value == 0 ? "Base" : std::to_string(value);
    if (name == "Bank CC00" || name == "Bank CC32") return value == 0 ? "Off" : std::to_string(value - 1);
    if (name == "Note") {
        static const char* const notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        return value >= 0 && value <= 127 ? notes[value % 12] + std::to_string(value / 12 - 1) : std::to_string(value);
    }
    if (name == "Preset") {
        if (value == 0) return "Next";
        if (value == 1) return "Prev";
        char buf[8];
        std::snprintf(buf, sizeof buf, "%03d", value);
        return buf;
    }
    return std::to_string(value);
}

std::pair<int, int> commandFieldRange(int type, int field) {
    const std::string name = commandFieldName(type, field);
    if (const std::vector<std::string> choices = commandFieldChoices(type, field); !choices.empty())
        return {name == "Function" ? 1 : 0, int(choices.size()) - 1};
    if (name == "MIDI Channel") return {0, 16};
    if (name == "Bank CC00" || name == "Bank CC32") return {0, 128};
    if (name == "Preset") return {0, 1023};
    return {0, 127};
}

// The scribble strip a switch gets with a command (HX Edit's @fs_label defaults in its .hlx export).
std::string commandSwitchLabel(int type) {
    switch (type) {
    case 1: return "CC Value";
    case 4: return "Bank/PC";
    case 5: return "Note";
    case 14: return "Preset";
    case 15: return "Snapshot";
    case 16: return "Looper";
    default: return commandTypeName(type);
    }
}

// The types HX Edit offers for a slot, in its menu order (its Command Center window, 2026-11-13).
std::vector<int> commandTypesFor(int slot) {
    if (slot >= 0 && slot < 6) return {0, 18, 19, 20, 8, 13, 17};                      // Instant 1-6
    if (slot >= 6 && slot <= 18) return {0, 1, 2, 4, 5, 6, 7, 12, 14, 15, 16};        // FS1-12, EXP toe switch
    if (slot >= 19 && slot <= 23) return {0, 3};                                       // EXP pedals, Variax knobs
    return {0};
}

void Preset::setCommand(int slot, int type) {
    if (slot < 0 || slot >= kCommandSlots) return;
    if (commands.size() < std::size_t(kCommandSlots)) commands.resize(kCommandSlots);
    CommandSlot& c = commands[std::size_t(slot)];
    if (c.type == type) return;
    c = {};
    c.type = type;
    // The unit's defaults (read back with cmd 48 after cmd 67, captures command-center2 / 3).
    if (type == 2 || type == 3) c.fields[3] = 127;  // CC Toggle: lit value; pedal: max
    if (type == 5) c.fields = {0, 48, 100, 1, 0, 0};  // Note On: C3, velocity 100, note off
    if (type == 12) c.fields[4] = 1;                  // Hotkey: momentary (.hlx @fs_momentary)
    if (type == 15 || type == 16) c.fields[1] = 1;    // HX Snapshot / Looper: press = Next / Play
    if (type == 17) c.fields[0] = 1;                  // instant Looper: Play
    // Instant and switch commands: every snapshot starts from the defaults; a slot without rows gets them.
    if (slot < 19)
        for (PresetSnapshot& s : snapshots) {
            auto it = std::find_if(s.commands.begin(), s.commands.end(), [slot](const SnapshotCommand& r) { return r.slot == slot; });
            if (it != s.commands.end()) it->fields = c.fields;
            else if (type != 0) s.commands.push_back({slot, c.fields});
        }
    // The switch lists the command among its functions (kind 3), as the unit stores it.
    const int fs = slot - controller::footswitch(0);
    if (fs < 0 || fs >= kToeSwitch) return;
    auto& list = footswitches[std::size_t(fs)];
    std::erase_if(list, [](const FootswitchAssign& f) { return f.kind == 3; });
    if (type == 0) return;
    FootswitchAssign f;
    f.kind = 3;
    f.label = commandSwitchLabel(type);
    f.colour = 462860;  // what factory command functions store (a non-custom colour; encoding unknown)
    f.momentary = type == 12;  // a new Hotkey is momentary (its field 5, HX Edit's export)
    list.push_back(std::move(f));
}

void Preset::setCommandField(int slot, int field, int value) {
    if (slot < 0 || std::size_t(slot) >= commands.size() || field < 0 || field >= 6) return;
    CommandSlot& c = commands[std::size_t(slot)];
    const auto [lo, hi] = commandFieldRange(c.type, field);
    c.fields[std::size_t(field)] = std::clamp(value, lo, hi);
    // The snapshot in use keeps its own copy (its row in key 10.10[n].1).
    if (std::size_t(snapshot) < snapshots.size())
        for (SnapshotCommand& r : snapshots[std::size_t(snapshot)].commands)
            if (r.slot == slot) r.fields = c.fields;
}

std::span<const IoPort> ioInputs() { return kInputs; }
std::span<const IoPort> ioOutputs() { return kOutputs; }

std::string controllerName(int source) {
    switch (source) {
    case controller::None: return "None";
    case controller::Exp1: return "EXP 1";
    case controller::Exp2: return "EXP 2";
    case controller::VariaxVolume: return "Variax Volume";
    case controller::VariaxTone: return "Variax Tone";
    case controller::Snapshot: return "Snapshot";
    case controller::MidiCc: return "MIDI CC";
    default: break;
    }
    if (source >= controller::footswitch(0) && source < controller::footswitch(12))
        return footswitchName(source - controller::footswitch(0));
    return "Controller " + std::to_string(source);
}

std::string footswitchName(int fsIndex) {
    if (fsIndex == kToeSwitch) return "EXP toe";
    return "FS" + std::to_string(fsIndex + 1);
}

PresetBlock* Preset::findBlock(int uid, Place* where) {
    if (uid < 0) return nullptr;
    for (int dsp = 0; dsp < 2; ++dsp)
        for (int lane = 0; lane < 2; ++lane) {
            PresetLane& l = lane ? paths[std::size_t(dsp)].b : paths[std::size_t(dsp)].a;
            for (int s = 0; s < kLaneSlots; ++s)
                if (auto& b = l.slots[std::size_t(s)]; b && b->uid == uid) {
                    if (where) *where = {dsp, lane, s};
                    return &*b;
                }
        }
    return nullptr;
}

const PresetBlock* Preset::findBlock(int uid, Place* where) const {
    return const_cast<Preset*>(this)->findBlock(uid, where);
}

std::vector<PresetValue>* Preset::valuesOf(const PresetTarget& t) {
    if (t.block >= 0) {
        PresetBlock* b = findBlock(t.block);
        return b ? &b->values : nullptr;
    }
    if (t.slot < 0 || t.slot >= 2 * kSlotsPerPath) return nullptr;
    PresetRouting& r = paths[std::size_t(t.slot / kSlotsPerPath)].routing;
    if (t.slot % kSlotsPerPath == int(kSlotSplit)) return &r.splitValues;
    if (t.slot % kSlotsPerPath == int(kSlotJoin)) return &r.joinValues;
    return nullptr;
}

void Preset::useSnapshotCommands(int i) {
    if (i < 0 || i >= kSnapshots) return;
    snapshot = i;
    if (std::size_t(i) < snapshots.size())
        for (const SnapshotCommand& r : snapshots[std::size_t(i)].commands)
            if (std::size_t(r.slot) < commands.size()) commands[std::size_t(r.slot)].fields = r.fields;
}

void Preset::recallSnapshot(int i) {
    if (i < 0 || i >= kSnapshots) return;
    useSnapshotCommands(i);
    for (PresetPath& path : paths)
        for (PresetLane* lane : {&path.a, &path.b})
            for (auto& b : lane->slots)
                if (b) b->enabled = b->snapOn[std::size_t(i)];
    for (const ControllerAssign& c : controllers) {
        const PresetValue& v = c.snapValues[std::size_t(i)];
        if (c.source != controller::Snapshot || c.param < 0 || v.kind == PresetValue::Kind::None) continue;
        if (std::vector<PresetValue>* values = valuesOf(c.target); values && std::size_t(c.param) < values->size())
            (*values)[std::size_t(c.param)] = v;
    }
}

const PresetValue* Preset::global(int id) const {
    for (const auto& [k, v] : globals)
        if (k == id) return &v;
    return nullptr;
}

void Preset::setGlobal(int id, PresetValue v) {
    for (auto& [k, old] : globals)
        if (k == id) {
            old = v;
            return;
        }
    globals.emplace_back(id, v);
}

void Preset::copySnapshot(int from, int to) {
    if (from < 0 || to < 0 || from >= kSnapshots || to >= kSnapshots || from == to) return;
    const std::size_t f = std::size_t(from), t = std::size_t(to);
    for (PresetPath& path : paths)
        for (PresetLane* lane : {&path.a, &path.b})
            for (auto& b : lane->slots)
                if (b) b->snapOn[t] = b->snapOn[f];
    for (ControllerAssign& c : controllers) c.snapValues[t] = c.snapValues[f];
    if (f < snapshots.size() && t < snapshots.size()) {
        snapshots[t].tempo = snapshots[f].tempo;
        snapshots[t].colour = snapshots[f].colour;
        // Command values by slot (the rows may be in a different order).
        for (SnapshotCommand& r : snapshots[t].commands)
            for (const SnapshotCommand& s : snapshots[f].commands)
                if (s.slot == r.slot) r.fields = s.fields;
        if (to == snapshot)
            for (const SnapshotCommand& r : snapshots[t].commands)
                if (std::size_t(r.slot) < commands.size()) commands[std::size_t(r.slot)].fields = r.fields;
    }
    if (to == snapshot) recallSnapshot(to);
}

void Preset::pruneAssignments() {
    auto gone = [this](const PresetTarget& t) { return t.block >= 0 && !findBlock(t.block); };
    std::erase_if(controllers, [&](const ControllerAssign& c) { return gone(c.target); });
    for (auto& list : footswitches) std::erase_if(list, [&](const FootswitchAssign& f) { return gone(f.target); });
}

int Preset::freeControllerId() const {
    for (int id = 0; id < kControllerIds; ++id)
        if (std::none_of(controllers.begin(), controllers.end(), [id](const ControllerAssign& c) { return c.id == id; }))
            return id;
    return -1;
}

bool PresetLane::empty() const {
    for (const auto& s : slots)
        if (s) return false;
    return true;
}

void normalizeRouting(PresetPath& path) {
    PresetRouting& r = path.routing;
    if (path.b.empty()) {
        // Back to serial; the split/join models and their settings stay for next time.
        if (r.mode != 0) {
            r.mode = 0;
            r.splitOn = r.joinOn = false;
            r.splitPos = 0;
            r.joinPos = kLaneSlots;
        }
        return;
    }
    if (r.mode == 0) {
        int first = 0;
        while (first < kLaneSlots && !path.b.slots[std::size_t(first)]) ++first;
        r.mode = 1;
        r.splitOn = r.joinOn = true;
        r.splitPos = first;
        r.joinPos = kLaneSlots;
    }
}

bool PresetLane::full() const {
    for (const auto& s : slots)
        if (!s) return false;
    return true;
}

int PresetLane::insert(int at, PresetBlock block) {
    if (at < 0 || at >= kLaneSlots) return -1;
    auto slot = [&](int i) -> std::optional<PresetBlock>& { return slots[std::size_t(i)]; };
    if (!slot(at)) {
        slot(at) = std::move(block);
        return at;
    }
    for (int j = at + 1; j < kLaneSlots; ++j)
        if (!slot(j)) {
            for (int k = j; k > at; --k) slot(k) = std::move(slot(k - 1));
            slot(at) = std::move(block);
            return at;
        }
    for (int j = at - 1; j >= 0; --j)
        if (!slot(j)) {
            for (int k = j; k < at; ++k) slot(k) = std::move(slot(k + 1));
            slot(at) = std::move(block);
            return at;
        }
    return -1;
}

std::optional<PresetBlock> PresetLane::take(int at) {
    if (at < 0 || at >= kLaneSlots) return std::nullopt;
    std::optional<PresetBlock> out = std::move(slots[std::size_t(at)]);
    slots[std::size_t(at)].reset();
    return out;
}

std::string presetNumber(int indexInSetlist) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d%c", indexInSetlist / 4 + 1, "ABCD"[indexInSetlist % 4]);
    return buf;
}

std::optional<Preset> parsePresetBlob(Bytes blob, std::string* error) {
    try {
        if (blob.size() < kPresetHead || std::memcmp(blob.data(), kPresetMagic, 10) != 0)
            throw FormatError("not an l6-helix preset");
        const mp::Value map = mp::parse(blob, kPresetHead);
        if (!map.isMap()) throw FormatError("preset body is not a map");
        Preset p;
        p.stored = true;
        p.raw = std::make_shared<const std::vector<std::uint8_t>>(blob.begin(), blob.end());
        for (int dsp = 0; dsp < 2; ++dsp)
            if (const mp::Value* path = map.get(dsp)) readPath(*path, dsp, p.paths[std::size_t(dsp)]);
        readSnapshots(map, p);
        readControllers(map, p);
        readFootswitches(map, p);
        readCommands(map, p);
        if (const mp::Value* g = map.get(5); g && g->isMap())
            for (std::size_t k = 0; k + 1 < g->items.size(); k += 2)
                if (g->items[k].type == mp::Type::Int) p.globals.emplace_back(int(g->items[k].i), toValue(g->items[k + 1]));
        return p;
    } catch (const FormatError& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

PresetLibrary readPresets(Bytes rec11, Bytes rec12, Bytes rec13, std::vector<std::string>* warnings) {
    PresetLibrary lib;
    lib.presets.resize(std::size_t(kSetlists) * kPresetsPerSetlist);
    for (int i = 0; i < kSetlists; ++i) lib.setlists.push_back("Setlist " + std::to_string(i + 1));
    auto warn = [&](const std::string& w) {
        if (warnings) warnings->push_back(w);
    };

    // rec 13: [ [.., .., .., <blob: [ {i: name} x 8 ]>, ...], ... ]
    try {
        if (!rec13.empty()) {
            const std::vector<mp::Value> top = mp::parseAll(rec13);
            const mp::Value* blob = !top.empty() ? top[0].at(3) : nullptr;
            if (!blob) throw FormatError("no setlist name list");
            const mp::Value names = mp::parse(blob->bytes());
            for (std::size_t i = 0; i < names.size() && i < lib.setlists.size(); ++i)
                if (const mp::Value* n = names.items[i].get(std::int64_t(i)))
                    if (std::string t = n->asText(); !t.empty()) lib.setlists[i] = t;
        }
    } catch (const FormatError& e) {
        warn(std::string("rec 13 (setlist names): ") + e.what());
    }

    // rec 12: [ [<blob: [ {slot: name | {109: name, ..}} x 128 ]> x 8 ], ... ]
    try {
        if (!rec12.empty()) {
            const std::vector<mp::Value> top = mp::parseAll(rec12);
            if (top.empty() || !top[0].isArray()) throw FormatError("no name lists");
            for (const mp::Value& blob : top[0].items) {
                const mp::Value list = mp::parse(blob.bytes());
                for (std::size_t k = 0; k < list.items.size(); ++k) {
                    const mp::Value& entry = list.items[k];
                    if (!entry.isMap() || entry.items.size() < 2) continue;
                    const std::int64_t slot = entry.items[0].asInt(-1);
                    if (slot < 0 || std::size_t(slot) >= lib.presets.size()) continue;
                    const mp::Value& v = entry.items[1];
                    const mp::Value* name = v.isMap() ? v.get(109) : &v;
                    if (name) lib.presets[std::size_t(slot)].name = name->asText();
                }
            }
        }
    } catch (const FormatError& e) {
        warn(std::string("rec 12 (preset names): ") + e.what());
    }

    // rec 11: [ [<preset blob> | nil] x 1026, ... ]; slots past the 8 setlists are ignored.
    try {
        if (!rec11.empty()) {
            const std::vector<mp::Value> top = mp::parseAll(rec11);
            if (top.empty() || !top[0].isArray()) throw FormatError("no preset list");
            const std::vector<mp::Value>& slots = top[0].items;
            int bad = 0;
            for (std::size_t s = 0; s < slots.size() && s < lib.presets.size(); ++s) {
                if (slots[s].isNil()) continue;
                std::string err;
                std::optional<Preset> p = parsePresetBlob(slots[s].bytes(), &err);
                if (!p) {
                    if (bad++ == 0) warn("rec 11 " + slotName(s) + ": " + err);
                    continue;
                }
                p->name = std::move(lib.presets[s].name);
                lib.presets[s] = std::move(*p);
            }
            if (bad > 1) warn("rec 11: " + std::to_string(bad) + " presets could not be read");
        }
    } catch (const FormatError& e) {
        warn(std::string("rec 11 (presets): ") + e.what());
    }
    return lib;
}

} // namespace hx

#include "core/presetwriter.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace hx {

namespace {

using mp::Type;
using mp::Value;

constexpr std::size_t kHead = 0x3D;
constexpr char kMagic[] = "\xa9l6-helix";  // fixstr(9) "l6-helix\0"
constexpr int kSlotInput = 0, kSlotOutputA = 9, kSlotSplit = 10, kSlotJoin = 19;
constexpr int kLaneAFirst = 1, kLaneBFirst = 11;
constexpr int kTypeInput = 0, kTypeOutput = 1, kTypeSplit = 2, kTypeJoin = 3, kTypeBlock = 6, kTypeLooper = 7,
              kTypeEmpty = 8;
constexpr int kNoController = 64;  // a snapshot's controller value entry that isn't used

struct WriteError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- map helpers (integer keys, insertion order kept; new keys go at the end)

Value* find(Value& m, std::int64_t key) {
    if (m.type != Type::Map) return nullptr;
    for (std::size_t k = 0; k + 1 < m.items.size(); k += 2)
        if (m.items[k].type == Type::Int && m.items[k].i == key) return &m.items[k + 1];
    return nullptr;
}

Value& at(Value& m, std::int64_t key) {
    if (m.type != Type::Map) {
        m = Value();
        m.type = Type::Map;
    }
    if (Value* v = find(m, key)) return *v;
    m.items.push_back(mp::integer(key));
    m.items.emplace_back();
    return m.items.back();
}

// Replaces the value, keeping the old one's encoding when it's the same kind (an unchanged value stays byte-equal).
void put(Value& slot, Value v) {
    if (slot.type == v.type && v.fmt == 0) v.fmt = slot.fmt;
    slot = std::move(v);
}
void set(Value& m, std::int64_t key, Value v) { put(at(m, key), std::move(v)); }

// The value a missing key stands for: nil, false, 0, -1, "" (the reader's defaults for absent keys).
bool isBlank(const Value& v) {
    switch (v.type) {
    case Type::Nil: return true;
    case Type::Bool: return !v.b;
    case Type::Int: return v.i == 0 || v.i == -1;
    case Type::Float: return v.f == 0;
    case Type::Str: return v.s.empty() || v.s == std::string(1, '\0');
    default: return false;
    }
}

// In an entry read from the preset: a key it has is updated; a missing one is added only when it says something
// (the unit leaves out keys at their default, and adding them would change an unchanged preset).
void update(Value& m, std::int64_t key, Value v) {
    if (Value* old = find(m, key)) return put(*old, std::move(v));
    if (!isBlank(v)) set(m, key, std::move(v));
}

Value newMap() {
    Value v;
    v.type = Type::Map;
    return v;
}

Value toValue(const PresetValue& p) {
    switch (p.kind) {
    case PresetValue::Kind::Bool: return mp::boolean(p.v != 0);
    case PresetValue::Kind::Int: return mp::integer(std::int64_t(p.v));
    case PresetValue::Kind::Float: return mp::real(p.v);
    default: return mp::nil();
    }
}

// A param set {2: count, 3: count shown, 4: [values]}. Key 3 is the model's (factory presets: the count, one less
// for delays and reverbs); kept from the stored set while the count is the same.
void writeParams(Value& params, const std::vector<PresetValue>& values, int blockType) {
    if (params.type != Type::Map) params = newMap();
    const Value* oldCount = find(params, 2);
    const Value* oldShown = find(params, 3);
    const std::int64_t n = std::int64_t(values.size());
    const bool sameCount = oldCount && oldCount->asInt() == n;
    const std::int64_t shown = sameCount && oldShown ? oldShown->asInt() : (blockType == 8 && n > 0 ? n - 1 : n);
    set(params, 2, mp::integer(n));
    set(params, 3, mp::integer(shown));
    Value& list = at(params, 4);
    const Value old = list;
    list = Value();
    list.type = Type::Array;
    list.fmt = old.type == Type::Array ? old.fmt : 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        Value v = toValue(values[i]);
        if (old.type == Type::Array && i < old.items.size() && old.items[i].type == v.type) v.fmt = old.items[i].fmt;
        list.items.push_back(std::move(v));
    }
}

Value parseBytes(const std::vector<std::uint8_t>& bytes) { return mp::parse(Bytes(bytes.data(), bytes.size())); }

// ---- the paths

// Slot content of a block: its stored content (the keys we don't model) with ours written over it.
Value blockContent(const PresetBlock& b, const PresetWriteOptions& opt) {
    Value c = b.raw ? parseBytes(*b.raw) : newMap();
    int type = int(b.blockType);
    if (type == 0 && opt.blockType) type = opt.blockType(b);
    if (type < 0) throw WriteError("a block's type is unknown (model " + std::to_string(b.modelNo) + ")");
    if (b.slotType == kTypeLooper) {
        set(c, 8, mp::integer(b.modelNo));
        set(c, 9, mp::integer(type));
        set(c, 10, mp::boolean(b.enabled));
        writeParams(at(c, 7), b.values, type);
        return c;
    }
    Value &model = at(c, 24);
    set(model, 23, mp::boolean(b.cabModelNo >= 0));
    set(model, 25, mp::integer(b.modelNo));
    set(model, 26, mp::integer(b.cabModelNo));
    set(c, 9, mp::integer(type));
    set(c, 10, mp::boolean(b.enabled));
    writeParams(at(c, 11), b.values, type);
    writeParams(at(c, 12), b.cabValues, type);
    return c;
}

Value slotEntry(int type, Value content) {
    Value s = newMap();
    set(s, 19, mp::integer(type));
    s.items.push_back(mp::integer(20));
    s.items.push_back(std::move(content));
    return s;
}

// Split / join key 13 = gap + 1. The reader clamps it (factory presets store 0 on unused ones), so on a split / join
// that is off a stored value meaning the same gap stays as it was. One that is on always gets gap + 1: sent with 0,
// the unit puts the split where it likes (a 0 kept from a serial path came back as 3, log 2026-11-08).
void setPosition(Value& m, int gap, bool on) {
    if (const Value* old = find(m, 13)) {
        const int stored = int(old->asInt());
        if (stored == gap + 1 || (!on && std::clamp(stored - 1, 0, kLaneSlots) == gap)) return;
    }
    set(m, 13, mp::integer(gap + 1));
}

// The path's 20-slot list, from the stored one (its entries for input / output / split / join are kept and updated).
void writePath(Value& path, const PresetPath& p, int dsp, const PresetWriteOptions& opt) {
    if (path.type != Type::Map) path = newMap();
    set(path, 21, mp::integer(p.routing.mode));
    Value& list = at(path, 22);
    if (list.type != Type::Array || list.items.size() != std::size_t(kSlotsPerPath)) {
        list = Value();
        list.type = Type::Array;
        list.items.resize(std::size_t(kSlotsPerPath));
    }
    auto entry = [&](int i) -> Value& { return list.items[std::size_t(i)]; };
    auto content = [&](int i, int type) -> Value& {
        Value& e = entry(i);
        if (e.type != Type::Map) e = newMap();
        set(e, 19, mp::integer(type));
        Value& c = at(e, 20);
        if (c.type != Type::Map) c = newMap();
        return c;
    };
    // Input A, output A.
    {
        Value& c = content(kSlotInput, kTypeInput);
        set(c, 5, mp::integer(p.inputs[0].source));
        writeParams(at(c, 7), p.inputs[0].values, 0);
    }
    {
        Value& c = content(kSlotOutputA, kTypeOutput);
        set(c, 6, mp::integer(p.outputs[0].dest));
        writeParams(at(c, 7), p.outputs[0].values, 0);
    }
    // Split (with input B) and join (with output B).
    {
        Value& c = content(kSlotSplit, kTypeSplit);
        Value& in = at(c, 14);
        set(in, 5, mp::integer(p.inputs[1].source));
        writeParams(at(in, 7), p.inputs[1].values, 0);
        Value& m = at(c, 15);
        set(m, 8, mp::integer(p.routing.splitModel));
        setPosition(m, p.routing.splitPos, p.routing.mode != 0 && p.routing.splitOn);
        set(m, 10, mp::boolean(p.routing.splitEnabled));
        writeParams(at(m, 7), p.routing.splitValues, 0);
        set(c, 18, mp::boolean(p.routing.splitOn));
    }
    {
        Value& c = content(kSlotJoin, kTypeJoin);
        Value& out = at(c, 16);
        set(out, 6, mp::integer(p.outputs[1].dest));
        writeParams(at(out, 7), p.outputs[1].values, 0);
        Value& m = at(c, 17);
        set(m, 8, mp::integer(p.routing.joinModel));
        setPosition(m, p.routing.joinPos, p.routing.mode != 0 && p.routing.joinOn);
        set(m, 10, mp::boolean(p.routing.joinEnabled));
        writeParams(at(m, 7), p.routing.joinValues, 0);
        set(c, 18, mp::boolean(p.routing.joinOn));
    }
    // The lanes.
    for (int lane = 0; lane < 2; ++lane)
        for (int i = 0; i < kLaneSlots; ++i) {
            const int index = (lane ? kLaneBFirst : kLaneAFirst) + i;
            const auto& b = (lane ? p.b : p.a).slots[std::size_t(i)];
            if (b) {
                Value& e = entry(index);
                const Value old = e;
                e = slotEntry(b->slotType == kTypeLooper ? kTypeLooper : kTypeBlock, blockContent(*b, opt));
                if (old.type == Type::Map) e.fmt = old.fmt;
            } else {
                Value& e = entry(index);
                // Empty: {19: 8, 20: nil}; an entry that already says so stays as it was.
                const Value* t = find(e, 19);
                if (!(t && t->asInt() == kTypeEmpty && find(e, 20) && find(e, 20)->isNil())) {
                    e = newMap();
                    set(e, 19, mp::integer(kTypeEmpty));
                    set(e, 20, mp::nil());
                }
            }
        }
    (void)dsp;
}

// ---- where each stored block went: old global slot (its uid when read) -> new global slot

std::map<int, int> slotMap(const Preset& p) {
    std::map<int, int> out;
    for (int dsp = 0; dsp < 2; ++dsp)
        for (int lane = 0; lane < 2; ++lane)
            for (int i = 0; i < kLaneSlots; ++i)
                if (const auto& b = (lane ? p.paths[std::size_t(dsp)].b : p.paths[std::size_t(dsp)].a).slots[std::size_t(i)];
                    b && b->uid >= 0 && b->uid < 2 * kSlotsPerPath)
                    out[b->uid] = dsp * kSlotsPerPath + (lane ? kLaneBFirst : kLaneAFirst) + i;
    return out;
}

// A target's global slot now: a block's (by uid), or a fixed item's own.
int targetSlot(const Preset& p, const PresetTarget& t) {
    if (t.block < 0) return t.slot;
    Preset::Place where;
    if (!p.findBlock(t.block, &where)) return -1;
    return where.dsp * kSlotsPerPath + (where.lane ? kLaneBFirst : kLaneAFirst) + where.slot;
}

bool isLaneSlot(int global) {
    const int k = global % kSlotsPerPath;
    return (k >= kLaneAFirst && k < kLaneAFirst + kLaneSlots) || (k >= kLaneBFirst && k < kLaneBFirst + kLaneSlots);
}

// ---- snapshots (key 10.10)

// The lane slots that held a block in the stored map (before the paths are written over).
std::vector<bool> storedOccupied(const Value& root) {
    std::vector<bool> out(std::size_t(2 * kSlotsPerPath), false);
    for (int dsp = 0; dsp < 2; ++dsp) {
        const Value* path = root.get(dsp);
        const Value* list = path ? path->get(22) : nullptr;
        if (!list || !list->isArray()) continue;
        for (std::size_t i = 0; i < list->items.size() && i < std::size_t(kSlotsPerPath); ++i)
            if (const Value* t = list->items[i].get(19); t && (t->asInt() == kTypeBlock || t->asInt() == kTypeLooper))
                out[std::size_t(dsp * kSlotsPerPath) + i] = true;
    }
    return out;
}

void writeSnapshots(Value& root, const Preset& p, const std::map<int, int>& moved, const std::vector<bool>& wasOccupied) {
    Value* top = find(root, 10);
    Value* list = top ? find(*top, 10) : nullptr;
    if (!list || list->type != Type::Array) return;  // no snapshots stored: nothing to keep in step
    for (std::size_t i = 0; i < list->items.size() && i < p.snapshots.size(); ++i) {
        Value& s = list->items[i];
        const PresetSnapshot& ps = p.snapshots[i];
        set(s, 4, mp::str(ps.name));
        set(s, 5, mp::real(ps.tempo));
        set(s, 12, mp::integer(ps.colour));
        // 3: [[?, on] x 40 slots]. Lane slots: each block's entry follows it; a slot a block left becomes
        // [nil, false]; one that was empty and still is keeps its entry.
        Value& states = at(s, 3);
        if (states.type == Type::Array && states.items.size() == std::size_t(2 * kSlotsPerPath)) {
            const Value old = states;
            for (int g = 0; g < 2 * kSlotsPerPath; ++g)
                if (isLaneSlot(g) && wasOccupied[std::size_t(g)]) {
                    Value e;
                    e.type = Type::Array;
                    e.items = {mp::nil(), mp::boolean(false)};
                    states.items[std::size_t(g)] = std::move(e);
                }
            for (int dsp = 0; dsp < 2; ++dsp)
                for (int lane = 0; lane < 2; ++lane)
                    for (int k = 0; k < kLaneSlots; ++k) {
                        const auto& b = (lane ? p.paths[std::size_t(dsp)].b : p.paths[std::size_t(dsp)].a).slots[std::size_t(k)];
                        if (!b) continue;
                        const int g = dsp * kSlotsPerPath + (lane ? kLaneBFirst : kLaneAFirst) + k;
                        Value& e = states.items[std::size_t(g)];
                        if (const auto it = moved.find(b->uid); it != moved.end() && old.items[std::size_t(b->uid)].isArray()) {
                            e = old.items[std::size_t(b->uid)];
                        } else if (e.type != Type::Array) {
                            e = Value();
                            e.type = Type::Array;
                            e.items = {mp::nil(), mp::boolean(false)};
                        }
                        if (e.items.size() < 2) e.items.resize(2);
                        put(e.items[1], mp::boolean(b->snapOn[i]));
                    }
        }
        // 2: [[flag, controller id (64 = unused), value] x 64]: the value of each controller that has snapshot values.
        // Entries are found by their id, not by position; only controllers with values have one.
        Value& values = at(s, 2);
        if (values.type == Type::Array) {
            auto idOf = [](const Value& e) {
                const Value* id = e.at(1);
                return id && id->type == Type::Int ? int(id->i) : kNoController;
            };
            auto release = [](Value& e) {
                e = Value();
                e.type = Type::Array;
                e.items = {mp::boolean(false), mp::integer(kNoController), mp::nil()};
            };
            std::map<int, const ControllerAssign*> live;
            for (const ControllerAssign& c : p.controllers) live[c.id] = &c;
            // Entries of controllers that are gone: unused.
            for (Value& e : values.items)
                if (const int id = idOf(e); id != kNoController && !live.count(id)) release(e);
            for (const ControllerAssign& c : p.controllers) {
                if (std::none_of(c.snapValues.begin(), c.snapValues.end(),
                                 [](const PresetValue& v) { return v.kind != PresetValue::Kind::None; }))
                    continue;
                Value* entry = nullptr;
                for (Value& e : values.items)
                    if (idOf(e) == c.id) entry = &e;
                // A new one: the entry at its id if free, else the first free one.
                if (!entry && c.id >= 0 && std::size_t(c.id) < values.items.size() &&
                    idOf(values.items[std::size_t(c.id)]) == kNoController)
                    entry = &values.items[std::size_t(c.id)];
                for (std::size_t k = 0; !entry && k < values.items.size(); ++k)
                    if (idOf(values.items[k]) == kNoController) entry = &values.items[k];
                if (!entry) continue;
                if (entry->type != Type::Array) {
                    *entry = Value();
                    entry->type = Type::Array;
                }
                if (entry->items.size() < 3) entry->items.resize(3, mp::boolean(false));
                put(entry->items[1], mp::integer(c.id));
                put(entry->items[2], toValue(c.snapValues[i]));
            }
        }
        // 1: [[slot (24 = unused), false, [?, fields x 6]] x 19]: the command values in this snapshot, found by slot;
        // a slot without a row takes the first unused one.
        Value* rows = find(s, 1);
        if (rows && rows->type == Type::Array)
            for (const SnapshotCommand& c : ps.commands) {
                auto slotOf = [](const Value& e) {
                    const Value* v = e.at(0);
                    return v && v->type == Type::Int ? int(v->i) : kCommandSlots;
                };
                Value* row = nullptr;
                for (Value& e : rows->items)
                    if (slotOf(e) == c.slot) row = &e;
                for (std::size_t k = 0; !row && k < rows->items.size(); ++k)
                    if (slotOf(rows->items[k]) == kCommandSlots) row = &rows->items[k];
                if (!row) continue;
                if (row->type != Type::Array) *row = Value(), row->type = Type::Array;
                if (row->items.size() < 3) row->items.resize(3, mp::boolean(false));
                put(row->items[0], mp::integer(c.slot));
                Value& vals = row->items[2];
                if (vals.type != Type::Array) vals = Value(), vals.type = Type::Array;
                if (vals.items.size() < 7) vals.items.resize(7, mp::integer(0));
                for (std::size_t k = 0; k < 6; ++k) put(vals.items[k + 1], mp::integer(c.fields[k]));
            }
    }
}

// ---- controllers (key 4: [source] -> [{0: id, 1: {0: source, 1, 2: min, 3: max, 4, 5: slot, 6: {28, 29: knob, 41},
// 7, 13}}]); a stored entry with the same id keeps its other keys.

void writeControllers(Value& root, const Preset& p) {
    Value& bySource = at(root, 4);
    std::map<int, Value> old;  // by id
    if (bySource.type == Type::Array)
        for (const Value& list : bySource.items)
            if (list.type == Type::Array)
                for (const Value& a : list.items)
                    if (const Value* id = a.get(0)) old[int(id->asInt())] = a;
    std::size_t sources = bySource.type == Type::Array ? bySource.items.size() : 0;
    for (const ControllerAssign& c : p.controllers) sources = std::max(sources, std::size_t(c.source) + 1);
    std::vector<Value> lists(sources);
    for (std::size_t s = 0; s < sources; ++s) {
        // An empty source keeps how it was stored (nil or []).
        const bool wasNil = bySource.type == Type::Array && s < bySource.items.size() && bySource.items[s].isNil();
        if (!wasNil) lists[s].type = Type::Array;
        if (bySource.type == Type::Array && s < bySource.items.size()) lists[s].fmt = bySource.items[s].fmt;
    }
    for (const ControllerAssign& c : p.controllers) {
        Value a;
        const auto it = old.find(c.id);
        if (it != old.end()) {
            // Stored: keys it has updated, missing ones only when they say something.
            a = it->second;
            update(a, 0, mp::integer(c.id));
            Value& in = at(a, 1);
            update(in, 0, mp::integer(c.source));
            update(in, 1, mp::integer(c.cc));
            update(in, 2, toValue(c.min));
            update(in, 3, toValue(c.max));
            update(in, 5, mp::integer(targetSlot(p, c.target)));
            if (find(in, 6) || c.param >= 0) update(at(in, 6), 29, mp::integer(c.param));
        } else {
            // New: the keys every factory entry has.
            a = newMap();
            set(a, 0, mp::integer(c.id));
            Value& in = at(a, 1);
            set(in, 0, mp::integer(c.source));
            set(in, 1, mp::integer(c.cc));  // the MIDI CC number (4 unless the source is MIDI CC)
            set(in, 2, toValue(c.min));
            set(in, 3, toValue(c.max));
            set(in, 4, mp::integer(0));
            set(in, 5, mp::integer(targetSlot(p, c.target)));
            Value& where = at(in, 6);
            set(where, 28, mp::integer(0));
            set(where, 29, mp::integer(c.param));
            set(where, 41, mp::boolean(false));
            set(in, 7, mp::integer(0));
            set(in, 13, mp::boolean(false));
        }
        Value& list = lists[std::size_t(c.source)];
        list.type = Type::Array;
        list.items.push_back(std::move(a));
    }
    const std::uint8_t fmt = bySource.fmt;
    bySource = Value();
    bySource.type = Type::Array;
    bySource.fmt = fmt;
    bySource.items = std::move(lists);
}

// ---- footswitches (key 3.8: [switch] -> [{10: order, 11: {0: kind, 2, 5: label, 6: colour, 7: custom, 8: slot,
// 9: {29: knob}}, 12..16}]); a stored entry for the same function keeps its other keys.

void writeFootswitches(Value& root, const Preset& p) {
    Value& fs = at(root, 3);
    Value& bySwitch = at(fs, 8);
    const Value old = bySwitch;
    const std::size_t count = std::max(old.type == Type::Array ? old.items.size() : 0, std::size_t(kFootswitchSlots));
    std::vector<Value> lists(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Value* was = old.type == Type::Array && i < old.items.size() ? &old.items[i] : nullptr;
        if (i >= p.footswitches.size()) {  // past the switches the model holds: as stored
            lists[i] = was ? *was : mp::nil();
            continue;
        }
        const std::vector<FootswitchAssign> none;
        const std::vector<FootswitchAssign>& now = i < p.footswitches.size() ? p.footswitches[i] : none;
        if (now.empty()) {
            lists[i] = was && (was->isNil() || (was->isArray() && was->items.empty())) ? *was : mp::nil();
            continue;
        }
        lists[i].type = Type::Array;
        if (was) lists[i].fmt = was->fmt;
        for (std::size_t n = 0; n < now.size(); ++n) {
            const FootswitchAssign& f = now[n];
            const int slot = f.kind == 3 ? -1 : targetSlot(p, f.target);
            // The stored entry of this function: same kind and target (by its slot when read) and knob.
            Value a;
            if (was && was->isArray())
                for (const Value& e : was->items) {
                    const Value* in = e.get(11);
                    if (!in || !in->get(0) || in->get(0)->asInt() != f.kind) continue;
                    const int storedSlot = in->get(8) ? int(in->get(8)->asInt(-1)) : -1;
                    const int readUid = f.target.block >= 0 ? f.target.block : f.target.slot;
                    const int knob = in->get(9) && in->get(9)->get(29) ? int(in->get(9)->get(29)->asInt(-1)) : -1;
                    if ((f.kind == 3 || storedSlot == readUid) && knob == f.param) {
                        a = e;
                        break;
                    }
                }
            if (a.type == Type::Map) {
                // Stored: keys it has updated, missing ones only when they say something.
                update(a, 10, mp::integer(std::int64_t(n)));
                Value& in = at(a, 11);
                update(in, 0, mp::integer(f.kind));
                update(in, 5, mp::str(f.label));
                // The reader keeps the low 24 bits: a stored colour with the same RGB stays as it was.
                if (const Value* c = find(in, 6); !c || (std::uint32_t(c->asInt()) & 0xffffff) != f.colour)
                    update(in, 6, mp::integer(std::int64_t(f.colour)));
                update(in, 7, mp::boolean(f.customColour));
                if (slot >= 0) update(in, 8, mp::integer(slot));
                if (f.param >= 0) update(at(in, 9), 29, mp::integer(f.param));
                update(a, 12, mp::boolean(f.momentary));
                // 15 = "LED colour set": the unit had it true with colour 10; only touched when the colour changes.
                if (const Value* c = find(a, 16); !c || int(c->asInt()) != f.ledColour) {
                    update(a, 16, mp::integer(f.ledColour));
                    update(a, 15, mp::boolean(f.ledColour != 0));
                }
            } else {
                // New: the keys factory entries have.
                a = newMap();
                set(a, 10, mp::integer(std::int64_t(n)));
                Value& in = at(a, 11);
                set(in, 0, mp::integer(f.kind));
                set(in, 2, mp::integer(0));
                set(in, 5, mp::str(f.label));
                set(in, 6, mp::integer(std::int64_t(f.colour)));
                set(in, 7, mp::boolean(f.customColour));
                if (slot >= 0) set(in, 8, mp::integer(slot));
                if (f.param >= 0) set(at(in, 9), 29, mp::integer(f.param));
                set(a, 12, mp::boolean(f.momentary));
                set(a, 13, mp::boolean(false));
                set(a, 14, mp::str(""));
                set(a, 15, mp::boolean(f.ledColour != 0));
                set(a, 16, mp::integer(f.ledColour));
            }
            lists[i].items.push_back(std::move(a));
        }
    }
    const std::uint8_t fmt = old.fmt;
    bySwitch = Value();
    bySwitch.type = Type::Array;
    bySwitch.fmt = fmt;
    bySwitch.items = std::move(lists);
}

// ---- Command Center (key 2: {0: [type x slots], 1: [[?, fields x 6] x slots]}; ? = the type in factory presets, 0
// in rows the unit wrote: kept unless the type changes, then 0 as the unit does)

void writeCommands(Value& root, const Preset& p) {
    if (p.commands.empty()) return;
    Value& cc = at(root, 2);
    at(cc, 0);
    at(cc, 1);  // both exist before taking references (adding one may move the other)
    Value& types = *find(cc, 0);
    Value& rows = *find(cc, 1);
    if (types.type != Type::Array) types = Value(), types.type = Type::Array;
    if (rows.type != Type::Array) rows = Value(), rows.type = Type::Array;
    types.items.resize(std::max(types.items.size(), p.commands.size()), mp::integer(0));
    rows.items.resize(std::max(rows.items.size(), p.commands.size()));
    for (std::size_t s = 0; s < p.commands.size(); ++s) {
        const CommandSlot& c = p.commands[s];
        const bool changed = types.items[s].type != Type::Int || types.items[s].i != c.type;
        put(types.items[s], mp::integer(c.type));
        Value& row = rows.items[s];
        if (row.type != Type::Array) row = Value(), row.type = Type::Array;
        if (row.items.size() < 7) row.items.resize(7, mp::integer(0));
        if (changed) put(row.items[0], mp::integer(0));
        for (std::size_t k = 0; k < 6; ++k) put(row.items[k + 1], mp::integer(c.fields[k]));
    }
}

// ---- globals (key 5: {id: value}); ids the model doesn't hold stay

void writeGlobals(Value& root, const Preset& p) {
    if (p.globals.empty()) return;
    Value& g = at(root, 5);
    for (const auto& [id, v] : p.globals) set(g, id, toValue(v));
}

// Key 6 {98: slot, 26: set}: the block selected when stored follows it.
void writeSelection(Value& root, const std::map<int, int>& moved) {
    Value* sel = find(root, 6);
    Value* slot = sel ? find(*sel, 98) : nullptr;
    if (!slot || slot->type != Type::Int || !isLaneSlot(int(slot->i))) return;
    if (const auto it = moved.find(int(slot->i)); it != moved.end()) put(*slot, mp::integer(it->second));
}

} // namespace

std::vector<std::uint8_t> encodePresetBlob(const mp::Value& map) {
    // The map, then the index of where each top-level key starts (NOTES.md "Preset blob header").
    std::vector<std::uint8_t> out(kMagic, kMagic + 10);
    const std::vector<std::uint8_t> head = {0xda, 0x00, 0x30};  // str16 of 48 bytes: the 12 words
    out.insert(out.end(), head.begin(), head.end());
    out.resize(kHead, 0);
    std::map<std::int64_t, std::uint32_t> offsets;
    // The map header, then key by key to learn where each starts.
    std::vector<std::uint8_t> body;
    const std::size_t n = map.items.size() / 2;
    if (map.fmt == 0xde || n > 15) body = {0xde, std::uint8_t(n >> 8), std::uint8_t(n)};
    else body = {std::uint8_t(0x80 | n)};
    for (std::size_t k = 0; k + 1 < map.items.size(); k += 2) {
        offsets[map.items[k].i] = std::uint32_t(kHead + body.size());
        const std::vector<std::uint8_t> kv = mp::encode(map.items[k]);
        body.insert(body.end(), kv.begin(), kv.end());
        const std::vector<std::uint8_t> vv = mp::encode(map.items[k + 1]);
        body.insert(body.end(), vv.begin(), vv.end());
    }
    out.insert(out.end(), body.begin(), body.end());
    const std::uint32_t length = std::uint32_t(out.size());
    auto offsetOf = [&](std::int64_t key) {
        const auto it = offsets.find(key);
        return it != offsets.end() ? it->second : length;
    };
    std::uint32_t words[12];
    words[0] = std::uint32_t(kHead);
    const std::int64_t order[] = {0, 1, 3, 4, 2, 5, 6, 7, 10};
    for (int i = 0; i < 9; ++i) words[i + 1] = offsetOf(order[i]);
    words[10] = words[11] = offsetOf(12);  // where key 10 ends: key 12 (the IR hashes) when present, else the end
    for (int i = 0; i < 12; ++i)
        for (int b = 0; b < 4; ++b) out[13 + std::size_t(i) * 4 + std::size_t(b)] = std::uint8_t(words[i] >> (8 * b));
    return out;
}

std::optional<std::vector<std::uint8_t>> writePresetBlob(const Preset& preset, const PresetWriteOptions& options,
                                                         std::string* error) {
    try {
        const std::vector<std::uint8_t>* source = preset.raw ? preset.raw.get() : options.base;
        if (!source || source->size() < kHead || std::memcmp(source->data(), kMagic, 10) != 0)
            throw WriteError("no stored preset to start from");
        Value root = mp::parse(Bytes(source->data(), source->size()), kHead);
        if (!root.isMap()) throw WriteError("the stored preset isn't a map");
        // A preset not read from this blob (the base): its stored blocks aren't ours, none of them moves.
        const std::map<int, int> moved = preset.raw ? slotMap(preset) : std::map<int, int>{};
        const std::vector<bool> wasOccupied = storedOccupied(root);
        for (int dsp = 0; dsp < 2; ++dsp) writePath(at(root, dsp), preset.paths[std::size_t(dsp)], dsp, options);
        writeSnapshots(root, preset, moved, wasOccupied);
        writeControllers(root, preset);
        writeFootswitches(root, preset);
        writeCommands(root, preset);
        writeGlobals(root, preset);
        writeSelection(root, moved);
        return encodePresetBlob(root);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

} // namespace hx

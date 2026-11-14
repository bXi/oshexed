#include "core/hlx.h"

#include "core/firmware.h"
#include "core/json.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <map>

namespace hx {

namespace {

// HX Edit's @topology letters: S = split, A / B = lanes, J = join (mode numbers as PresetRouting::mode).
constexpr const char* kTopology[] = {"A", "SABJ", "SAB", "ABJ", "AB"};
constexpr int kSplitIndex = 10, kJoinIndex = 19;  // in a path's slot list
constexpr std::uint32_t kLtDevice = 0x00210004;   // data.device of a Helix LT preset (= the .hxf product ID)

PresetValue::Kind knobKind(const MetaParam& k) {
    if (k.type == std::uint32_t(ParamType::Float)) return PresetValue::Kind::Float;
    if (k.type == std::uint32_t(ParamType::Switch)) return PresetValue::Kind::Bool;
    return PresetValue::Kind::Int;
}

double knobDefault(const MetaParam& k) {
    if (k.type == std::uint32_t(ParamType::Float)) return std::bit_cast<float>(k.def);
    if (k.type == std::uint32_t(ParamType::Switch)) return k.def != 0;
    return double(std::int32_t(k.def));
}

PresetValue fromJson(const MetaParam& k, const json::Value& v) {
    switch (knobKind(k)) {
    case PresetValue::Kind::Bool: return {PresetValue::Kind::Bool, v.asBool() ? 1.0 : 0.0};
    case PresetValue::Kind::Int: return {PresetValue::Kind::Int, double(v.asInt())};
    default: return {PresetValue::Kind::Float, v.asNumber()};
    }
}

std::string str(const json::Value* v) { return v ? v->asString() : std::string(); }

// A switch function's members as HX Edit writes them: @fs_label (its own name for the function), @fs_customlabel
// (the user's, when set), @fs_ledcolor (= preset 11.6), @fs_customcolor (Switch LED), @fs_momentary.
void readSwitchAttrs(FootswitchAssign& f, const json::Value& o) {
    const std::string custom = str(o.get("@fs_customlabel"));
    f.label = custom.empty() ? str(o.get("@fs_label")) : custom;
    if (const json::Value* c = o.get("@fs_ledcolor")) f.colour = std::uint32_t(c->asInt()) & 0xffffff;
    if (const json::Value* l = o.get("@fs_customcolor")) f.ledColour = int(l->asInt());
    if (const json::Value* m = o.get("@fs_momentary")) f.momentary = m->asBool();
}
// `name` = HX Edit's own name for the function (the knob's); a different label goes out as @fs_customlabel.
void writeSwitchAttrs(json::Value& o, const FootswitchAssign& f, const std::string& name) {
    o.set("@fs_label", json::Value::string(name.empty() ? f.label : name));
    if (!name.empty() && f.label != name) o.set("@fs_customlabel", json::Value::string(f.label));
    o.set("@fs_ledcolor", json::Value::integerNumber(f.colour));
    o.set("@fs_momentary", json::Value::boolean(f.momentary));
    if (f.ledColour != 0) o.set("@fs_customcolor", json::Value::integerNumber(f.ledColour));
}

// The "@" members of `obj` other than `held`, as JSON object text ("" if none).
std::string keptAttrs(const json::Value& obj, std::initializer_list<std::string_view> held) {
    json::Value keep = json::Value::object();
    for (std::size_t k = 0; k < obj.keys.size(); ++k) {
        const std::string& key = obj.keys[k];
        if (!key.starts_with("@") || std::find(held.begin(), held.end(), key) != held.end()) continue;
        keep.set(key, obj.items[k]);
    }
    return keep.keys.empty() ? std::string() : json::write(keep);
}
std::string keptAttrs(const json::Value& obj) {
    return keptAttrs(obj, {"@model", "@stereo", "@path", "@position", "@enabled", "@type", "@cab", "@mic"});
}

// Merges kept members back (they don't replace what the model wrote).
void addKept(json::Value& obj, const std::string& text) {
    if (text.empty()) return;
    const std::optional<json::Value> kept = json::parse(text);
    if (!kept || !kept->isObject()) return;
    for (std::size_t k = 0; k < kept->keys.size(); ++k)
        if (!obj.get(kept->keys[k])) obj.set(kept->keys[k], kept->items[k]);
}

// Global members with a preset key 5 id.
struct GlobalName {
    const char* name;
    int id;
    PresetValue::Kind kind;
};
constexpr GlobalName kGlobals[] = {{"@tempo", globalid::Tempo, PresetValue::Kind::Float},
                                   {"@guitarinputZ", globalid::GuitarInputZ, PresetValue::Kind::Int},
                                   {"@guitarpad", globalid::GuitarPad, PresetValue::Kind::Int},
                                   {"@PowercabSelect", globalid::PowercabSelect, PresetValue::Kind::Int},
                                   {"@DtSelect", globalid::DtSelect, PresetValue::Kind::Int}};
// Global members written from other parts of the preset.
bool derivedGlobal(std::string_view k) {
    return k == "@model" || k == "@topology0" || k == "@topology1" || k == "@current_snapshot";
}
// Command Center sections (the user's exports, Clean_Dirt w_d_w123.hlx): commandInst1-6 = slots 0-5, commandFS1-12 =
// 6-17, commandExpSwitch = 18 (the toe switch; per-snapshot values like the footswitches), commandExpPedal1 / 2 =
// 19 / 20, commandVariaxVol / Tone = 22 / 23 (the pedals and knobs hold one value for all snapshots).
std::string commandSection(int slot) {
    if (slot >= 0 && slot < 6) return "commandInst" + std::to_string(slot + 1);
    if (slot >= 6 && slot < 18) return "commandFS" + std::to_string(slot - 5);
    if (slot == 18) return "commandExpSwitch";
    if (slot == 19 || slot == 20) return "commandExpPedal" + std::to_string(slot - 18);
    if (slot == 22) return "commandVariaxVol";
    if (slot == 23) return "commandVariaxTone";
    return "";
}
int commandSlotOf(std::string_view k) {
    for (int s = 0; s < kCommandSlots; ++s)
        if (const std::string n = commandSection(s); !n.empty() && n == k) return s;
    return -1;
}

// A type's .hlx keys in the unit's field order (captures command-center2 / 3 next to HX Edit's exports). HX Edit's
// @current (CC Toggle, Bank/Program) and @pending (Note On) aren't stored by the unit; @fs_momentary is a field of
// CC Toggle, Ext Amp and Hotkey (per snapshot too).
std::vector<const char*> commandKeys(int type) {
    switch (type) {
    case 1: return {"@channel", "@cc", "@value"};
    case 2: return {"@channel", "@cc", "@initial", "@toggle", "@fs_momentary"};
    case 3: return {"@channel", "@cc", "@min", "@max"};
    case 4: return {"@channel", "@bank0", "@bank32", "@program"};
    case 5: return {"@channel", "@note", "@velocity", "@noteoff"};
    case 6: return {"@message"};
    case 7: return {"@select", "@fs_momentary"};
    case 12: return {"@modifier1", "@modifier2", "@modifier3", "@keystroke", "@fs_momentary"};
    case 14: return {"@preset"};
    case 15: case 16: return {"@behavior", "@press", "@relhold"};
    case 18: return {"@channel", "@cc", "@value", "@waittime"};
    case 19: return {"@channel", "@bank0", "@bank32", "@program", "@waittime"};
    case 20: return {"@message", "@waittime"};
    case 8: return {"@select", "@waittime"};
    case 13: return {"@modifier1", "@modifier2", "@modifier3", "@keystroke", "@waittime"};
    case 17: return {"@function", "@waittime"};
    default: return {};
    }
}

std::array<int, 6> commandFields(int type, const json::Value& o) {
    std::array<int, 6> out{};
    const std::vector<const char*> keys = commandKeys(type);
    for (std::size_t k = 0; k < keys.size() && k < out.size(); ++k)
        if (const json::Value* v = o.get(keys[k])) out[k] = int(std::lround(v->asNumber()));  // @fs_momentary may be a bool
    return out;
}

void setCommandFields(json::Value& o, int type, const std::array<int, 6>& fields) {
    const std::vector<const char*> keys = commandKeys(type);
    for (std::size_t k = 0; k < keys.size() && k < fields.size(); ++k)
        o.set(keys[k], json::Value::integerNumber(fields[k]));
    if (type == 2 || type == 4) o.set("@current", json::Value::integerNumber(0));
    if (type == 5) o.set("@pending", json::Value::integerNumber(0));
}

// Tone sections read into the model; everything else is kept as text.
bool modelledSection(std::string_view k) {
    if (k == "dsp0" || k == "dsp1" || k == "global" || k == "controller" || k == "footswitch") return true;
    if (commandSlotOf(k) >= 0) return true;
    return k.size() == 9 && k.starts_with("snapshot") && k[8] >= '0' && k[8] <= '7';
}

struct Importer {
    const Firmware& fw;
    std::vector<std::string>* warnings;
    Preset p;
    std::map<std::string, const ModelRef*> bySymbol;
    std::map<std::pair<int, std::string>, PresetTarget> targets;  // (dsp, "block3" / "split" / "join")
    std::map<std::string, int> missing;                             // model symbol -> blocks skipped
    int nextId = 0;

    void warn(std::string w) {
        if (warnings) warnings->push_back(std::move(w));
    }

    // HX Edit drops the Mono / Stereo of the symbol and says @stereo instead.
    const ModelRef* model(const std::string& sym, bool stereo) const {
        for (const std::string& s : {sym + (stereo ? "Stereo" : "Mono"), sym, sym + (stereo ? "Mono" : "Stereo")})
            if (auto it = bySymbol.find(s); it != bySymbol.end()) return it->second;
        return nullptr;
    }

    const MetaParam* knob(const MetaEntry* meta, std::string_view sym) const {
        if (!meta) return nullptr;
        for (const MetaParam& k : meta->knobs)
            if (k.number > 0 && fw.meta.symbol(k.nameSym) == sym) return &k;
        return nullptr;
    }

    std::vector<PresetValue> values(const MetaEntry* meta, const json::Value& obj) const {
        std::vector<PresetValue> out;
        if (!meta) return out;
        std::uint32_t count = 0;
        for (const MetaParam& k : meta->knobs) count = std::max(count, k.number);
        out.resize(count);
        for (const MetaParam& k : meta->knobs) {
            if (k.number == 0) continue;
            const json::Value* v = obj.get(fw.meta.symbol(k.nameSym));
            out[k.number - 1] = v ? fromJson(k, *v) : PresetValue{knobKind(k), knobDefault(k)};
        }
        // Legacy cabs: the mic isn't a knob; presets store it after the knobs, .hlx files as "@mic".
        if (const json::Value* mic = obj.get("@mic")) out.push_back({PresetValue::Kind::Int, double(mic->asInt())});
        return out;
    }

    const MetaEntry* metaOf(const PresetTarget& t) const {
        if (t.block >= 0) {
            const PresetBlock* b = p.findBlock(t.block);
            const ModelRef* m = b ? fw.model(b->modelNo) : nullptr;
            return m ? m->meta : nullptr;
        }
        if (t.slot < 0) return nullptr;
        const PresetRouting& r = p.paths[std::size_t(t.slot / kSlotsPerPath)].routing;
        const ModelRef* m = fw.model(t.slot % kSlotsPerPath == kSplitIndex ? r.splitModel : r.joinModel);
        return m ? m->meta : nullptr;
    }

    const PresetTarget* target(int dsp, const std::string& name) const {
        auto it = targets.find({dsp, name});
        return it == targets.end() ? nullptr : &it->second;
    }

    void path(int dsp, const json::Value& d, const json::Value* global) {
        PresetPath& out = p.paths[std::size_t(dsp)];
        for (std::size_t k = 0; k < d.keys.size(); ++k) {
            const std::string& name = d.keys[k];
            const json::Value& obj = d.items[k];
            if (name.rfind("block", 0) != 0 || !obj.isObject()) continue;
            const std::string sym = str(obj.get("@model"));
            const json::Value* stereo = obj.get("@stereo");
            const ModelRef* m = model(sym, stereo && stereo->asBool());
            if (!m) {
                ++missing[sym];
                continue;
            }
            const int lane = int(obj.get("@path") ? obj.get("@path")->asInt() : 0);
            const int pos = int(obj.get("@position") ? obj.get("@position")->asInt(-1) : -1);
            if (lane < 0 || lane > 1 || pos < 0 || pos >= kLaneSlots) {
                warn(name + " of path " + std::to_string(dsp + 1) + ": no place for lane " + std::to_string(lane) +
                     " position " + std::to_string(pos));
                continue;
            }
            auto& slot = (lane ? out.b : out.a).slots[std::size_t(pos)];
            if (slot) {
                warn(name + " of path " + std::to_string(dsp + 1) + ": position " + std::to_string(pos) + " is taken");
                continue;
            }
            PresetBlock b;
            b.modelNo = m->modelNo;
            b.enabled = obj.get("@enabled") ? obj.get("@enabled")->asBool(true) : true;
            b.blockType = std::uint32_t(obj.get("@type") ? obj.get("@type")->asInt() : 0);
            if (sym.rfind("HD2_Looper", 0) == 0) b.slotType = 7;
            b.values = values(m->meta, obj);
            // An amp with its cab: "@cab" names a "cabN" object of the path.
            if (const json::Value* cabName = obj.get("@cab"); cabName && cabName->isString())
                if (const json::Value* cab = d.get(cabName->asString()))
                    if (const ModelRef* cm = model(str(cab->get("@model")), false)) {
                        b.cabModelNo = cm->modelNo;
                        b.cabValues = values(cm->meta, *cab);
                    }
            b.hlxAttrs = keptAttrs(obj);
            b.uid = dsp * kSlotsPerPath + (lane ? 11 : 1) + pos;
            b.snapOn.fill(b.enabled);
            targets[{dsp, name}] = {b.uid, -1};
            slot = std::move(b);
        }

        PresetRouting& r = out.routing;
        for (const bool split : {true, false}) {
            const json::Value* s = d.get(split ? "split" : "join");
            if (!s) continue;
            const ModelRef* m = model(str(s->get("@model")), false);
            if (!m) {
                ++missing[str(s->get("@model"))];
                continue;
            }
            const int pos = std::clamp(int(s->get("@position") ? s->get("@position")->asInt() : (split ? 0 : 8)), 0, kLaneSlots);
            const bool enabled = s->get("@enabled") ? s->get("@enabled")->asBool(true) : true;
            (split ? r.splitModel : r.joinModel) = m->modelNo;
            (split ? r.splitValues : r.joinValues) = values(m->meta, *s);
            (split ? r.splitPos : r.joinPos) = pos;
            (split ? r.splitEnabled : r.joinEnabled) = enabled;
            targets[{dsp, split ? "split" : "join"}] = {-1, dsp * kSlotsPerPath + (split ? kSplitIndex : kJoinIndex)};
        }
        const std::string topo = global ? str(global->get("@topology" + std::to_string(dsp))) : "";
        int mode = -1;
        for (int i = 0; i < 5; ++i)
            if (topo == kTopology[i]) mode = i;
        if (mode < 0) {
            mode = out.b.empty() ? 0 : 1;
            if (!topo.empty()) warn("path " + std::to_string(dsp + 1) + ": unknown routing '" + topo + "'");
        }
        r.mode = mode;
        r.splitOn = mode == 1 || mode == 2;
        r.joinOn = mode == 1 || mode == 3;

        // Inputs / outputs: their knobs by the rec 4 symbols (noiseGate, threshold, decay / pan, gain), only as many
        // as the preset stores (the entries' displayParamCount).
        for (int lane = 0; lane < 2; ++lane) {
            if (const json::Value* in = d.get(lane ? "inputB" : "inputA")) {
                PresetInput& pi = out.inputs[std::size_t(lane)];
                if (const json::Value* v = in->get("@input")) pi.source = int(v->asInt());
                pi.values = values(fw.metaBySymbol(lane ? kInputBSymbol : kInputASymbol), *in);
                pi.values.resize(std::min<std::size_t>(pi.values.size(), 3));
            }
            if (const json::Value* o = d.get(lane ? "outputB" : "outputA")) {
                PresetOutput& po = out.outputs[std::size_t(lane)];
                if (const json::Value* v = o->get("@output")) po.dest = int(v->asInt());
                po.values = values(fw.metaBySymbol(kOutputSymbol), *o);
                po.values.resize(std::min<std::size_t>(po.values.size(), 2));
            }
        }
    }

    void snapshots(const json::Value& tone) {
        for (int i = 0; i < kSnapshots; ++i) {
            const json::Value* s = tone.get("snapshot" + std::to_string(i));
            if (!s || !s->isObject()) break;
            PresetSnapshot sn;
            sn.name = str(s->get("@name"));
            if (const json::Value* c = s->get("@ledcolor")) sn.colour = int(c->asInt());
            if (const json::Value* t = s->get("@tempo")) sn.tempo = t->asNumber(120);
            if (const json::Value* blocks = s->get("blocks"))
                for (int dsp = 0; dsp < 2; ++dsp)
                    if (const json::Value* d = blocks->get("dsp" + std::to_string(dsp)))
                        for (std::size_t k = 0; k < d->keys.size(); ++k)
                            if (const PresetTarget* t = target(dsp, d->keys[k]); t && t->block >= 0)
                                if (PresetBlock* b = p.findBlock(t->block)) b->snapOn[std::size_t(i)] = d->items[k].asBool(b->enabled);
            p.snapshots.push_back(std::move(sn));
        }
    }

    // tone.controller: {dspN: {blockK: {KnobSymbol: {@controller, @min, @max, @cc}}}}; each snapshot's
    // "controllers" holds the same tree with {@value} per knob.
    void controllers(const json::Value& tone) {
        const json::Value* c = tone.get("controller");
        if (!c) return;
        for (int dsp = 0; dsp < 2; ++dsp) {
            const json::Value* d = c->get("dsp" + std::to_string(dsp));
            if (!d) continue;
            for (std::size_t k = 0; k < d->keys.size(); ++k) {
                const PresetTarget* t = target(dsp, d->keys[k]);
                if (!t) continue;
                const MetaEntry* meta = metaOf(*t);
                const json::Value& knobs = d->items[k];
                for (std::size_t j = 0; j < knobs.keys.size(); ++j) {
                    const MetaParam* kn = knob(meta, knobs.keys[j]);
                    if (!kn) continue;
                    if (nextId >= kControllerIds) {
                        warn("more than 64 controller assignments; the rest are skipped");
                        return;
                    }
                    const json::Value& a = knobs.items[j];
                    ControllerAssign ca;
                    ca.id = nextId++;
                    ca.source = int(a.get("@controller") ? a.get("@controller")->asInt() : 0);
                    ca.target = *t;
                    ca.param = int(kn->number) - 1;
                    if (const json::Value* v = a.get("@min")) ca.min = fromJson(*kn, *v);
                    if (const json::Value* v = a.get("@max")) ca.max = fromJson(*kn, *v);
                    if (const json::Value* v = a.get("@cc")) ca.cc = int(v->asInt());
                    // @cc kept too: written back as read
                    ca.hlxAttrs = keptAttrs(a, {"@controller", "@min", "@max", "@fs_label", "@fs_customlabel",
                                                "@fs_ledcolor", "@fs_customcolor", "@fs_momentary"});
                    // A knob on a footswitch: the switch lists it among its functions (kind 2), as the unit stores it.
                    const int fs = ca.source - controller::footswitch(0);
                    if (fs >= 0 && fs < 12) {
                        FootswitchAssign f;
                        f.kind = 2;
                        f.colour = 462860;
                        readSwitchAttrs(f, a);
                        if (f.label.empty()) f.label = fw.meta.displayName(*kn);
                        f.target = *t;
                        f.param = ca.param;
                        p.footswitches[std::size_t(fs)].push_back(std::move(f));
                    }
                    p.controllers.push_back(std::move(ca));
                }
            }
        }
        for (int i = 0; i < kSnapshots; ++i) {
            const json::Value* s = tone.get("snapshot" + std::to_string(i));
            const json::Value* sc = s ? s->get("controllers") : nullptr;
            if (!sc) continue;
            for (ControllerAssign& ca : p.controllers) {
                const MetaEntry* meta = metaOf(ca.target);
                if (!meta) continue;
                // Find this assignment's block and knob names in the snapshot's tree.
                for (int dsp = 0; dsp < 2; ++dsp) {
                    const json::Value* d = sc->get("dsp" + std::to_string(dsp));
                    if (!d) continue;
                    for (std::size_t k = 0; k < d->keys.size(); ++k) {
                        const PresetTarget* t = target(dsp, d->keys[k]);
                        if (!t || t->block != ca.target.block || (t->block < 0 && t->slot != ca.target.slot)) continue;
                        const json::Value& knobs = d->items[k];
                        for (std::size_t j = 0; j < knobs.keys.size(); ++j) {
                            const MetaParam* kn = knob(meta, knobs.keys[j]);
                            if (kn && int(kn->number) - 1 == ca.param)
                                if (const json::Value* v = knobs.items[j].get("@value")) ca.snapValues[std::size_t(i)] = fromJson(*kn, *v);
                        }
                    }
                }
            }
        }
    }

    // tone.footswitch: {dspN: {blockK: {@fs_index, @fs_label, @fs_ledcolor, ...}}}. @fs_index is the switch number
    // (1-12), 13 = the pedal's toe switch.
    void footswitches(const json::Value& tone) {
        const json::Value* fs = tone.get("footswitch");
        if (!fs) return;
        int unknown = 0;
        for (int dsp = 0; dsp < 2; ++dsp) {
            const json::Value* d = fs->get("dsp" + std::to_string(dsp));
            if (!d) continue;
            for (std::size_t k = 0; k < d->keys.size(); ++k) {
                const PresetTarget* t = target(dsp, d->keys[k]);
                if (!t) continue;
                const json::Value& o = d->items[k];
                const int index = int(o.get("@fs_index") ? o.get("@fs_index")->asInt(-1) : -1);
                const int slot = index >= 1 && index <= 12 ? index - 1 : index == 13 ? kToeSwitch : -1;
                if (slot < 0) {
                    ++unknown;
                    continue;
                }
                FootswitchAssign f;
                f.kind = 1;
                readSwitchAttrs(f, o);
                f.hlxAttrs = keptAttrs(o, {"@fs_index", "@fs_label", "@fs_customlabel", "@fs_ledcolor",
                                           "@fs_customcolor", "@fs_momentary"});
                f.target = *t;
                p.footswitches[std::size_t(slot)].push_back(std::move(f));
            }
        }
        if (unknown) warn(std::to_string(unknown) + " footswitch assignment(s) with an unknown switch number skipped");
    }

    // tone.commandInstN / commandFSN / commandExpPedalN: {@command: type, fields..., on a switch @fs_label,
    // @fs_ledcolor, @fs_momentary, @fs_customcolor (Switch LED), @fs_primary, @fs_enabled}. Each snapshot's
    // "commands" holds the instant / switch commands' fields in that snapshot.
    void commands(const json::Value& tone) {
        for (std::size_t k = 0; k < tone.keys.size(); ++k) {
            const int slot = commandSlotOf(tone.keys[k]);
            const json::Value& o = tone.items[k];
            if (slot < 0 || !o.isObject()) continue;
            const int type = int(o.get("@command") ? o.get("@command")->asInt() : 0);
            if (p.commands.size() < std::size_t(kCommandSlots)) p.commands.resize(kCommandSlots);
            CommandSlot& c = p.commands[std::size_t(slot)];
            c.type = type;
            c.fields = commandFields(type, o);
            const int fs = slot - controller::footswitch(0);
            if (type == 0 || fs < 0 || fs >= 12) continue;
            FootswitchAssign f;
            f.kind = 3;
            f.colour = 462860;
            readSwitchAttrs(f, o);
            if (f.label.empty()) f.label = commandSwitchLabel(type);
            json::Value keep = json::Value::object();  // the switch's other @fs_ members (@fs_primary, @fs_enabled)
            for (std::size_t j = 0; j < o.keys.size(); ++j) {
                const std::string& key = o.keys[j];
                if (key.starts_with("@fs_") && key != "@fs_label" && key != "@fs_customlabel" && key != "@fs_ledcolor" &&
                    key != "@fs_momentary" && key != "@fs_customcolor")
                    keep.set(key, o.items[j]);
            }
            if (!keep.keys.empty()) f.hlxAttrs = json::write(keep);
            p.footswitches[std::size_t(fs)].push_back(std::move(f));
        }
        if (p.commands.empty()) return;
        for (int i = 0; i < kSnapshots && std::size_t(i) < p.snapshots.size(); ++i) {
            const json::Value* s = tone.get("snapshot" + std::to_string(i));
            const json::Value* sc = s ? s->get("commands") : nullptr;
            for (int slot = 0; slot < 19; ++slot) {
                const CommandSlot& c = p.commands[std::size_t(slot)];
                if (c.type == 0) continue;
                const json::Value* o = sc ? sc->get(commandSection(slot)) : nullptr;
                // A slot the snapshot leaves out has no row on the unit either (HX Edit exports the rows there are).
                // A snapshot without any commands (an older export): rows from the preset's own values.
                if (!o && sc) continue;
                p.snapshots[std::size_t(i)].commands.push_back({slot, o ? commandFields(c.type, *o) : c.fields});
            }
        }
    }
};

// ---------------------------------------------------------------- export

json::Value knobJson(const MetaParam& k, const PresetValue* v) {
    const double d = v && v->kind != PresetValue::Kind::None ? v->v : knobDefault(k);
    switch (knobKind(k)) {
    case PresetValue::Kind::Bool: return json::Value::boolean(d != 0);
    case PresetValue::Kind::Int: return json::Value::integerNumber(std::int64_t(std::llround(d)));
    default: return json::Value::real(d);
    }
}

struct Exporter {
    const Firmware& fw;
    const Preset& p;
    std::vector<std::string>* warnings;
    std::map<int, std::pair<int, std::string>> names;  // block uid -> (dsp, "blockN")

    void warn(std::string w) {
        if (warnings) warnings->push_back(std::move(w));
    }

    // The model's knobs by symbol (only the first `limit`: inputs / outputs keep displayParamCount).
    void knobs(json::Value& obj, const MetaEntry* meta, const std::vector<PresetValue>& values,
               std::size_t limit = SIZE_MAX) const {
        if (!meta) return;
        for (std::size_t i = 0; i < meta->knobs.size() && i < limit; ++i) {
            const MetaParam& k = meta->knobs[i];
            if (k.number == 0) continue;
            obj.set(fw.meta.symbol(k.nameSym), knobJson(k, k.number <= values.size() ? &values[k.number - 1] : nullptr));
        }
    }

    // Legacy cabs: the value after the knobs is the mic.
    void mic(json::Value& obj, const MetaEntry* meta, const std::vector<PresetValue>& values) const {
        if (!meta || fw.meta.attr(meta->modelAttrs, "category") != "cab") return;
        std::uint32_t count = 0;
        for (const MetaParam& k : meta->knobs) count = std::max(count, k.number);
        if (values.size() > count) obj.set("@mic", json::Value::integerNumber(std::int64_t(values[count].v)));
    }

    std::size_t limitOf(const MetaEntry* meta) const {
        if (!meta) return 0;
        const std::string n = fw.meta.attr(meta->modelAttrs, "displayParamCount");
        return n.empty() ? SIZE_MAX : std::size_t(std::max(0, std::atoi(n.c_str())));
    }

    json::Value path(int dsp) {
        const PresetPath& path = p.paths[std::size_t(dsp)];
        json::Value d = json::Value::object();
        int blockNo = 0, cabNo = 0;
        for (int lane = 0; lane < 2; ++lane)
            for (int pos = 0; pos < kLaneSlots; ++pos) {
                const auto& slot = (lane ? path.b : path.a).slots[std::size_t(pos)];
                if (!slot) continue;
                const PresetBlock& b = *slot;
                const ModelRef* m = fw.model(b.modelNo);
                if (!m || !m->meta) {
                    warn("path " + std::to_string(dsp + 1) + ": model #" + std::to_string(b.modelNo) +
                         " has no rec 4 entry; left out");
                    continue;
                }
                std::string sym = fw.meta.symbol(m->symId);
                json::Value o = json::Value::object();
                o.set("@enabled", json::Value::boolean(b.enabled));
                std::optional<bool> stereo;
                for (const auto& [suffix, st] : {std::pair{"Stereo", true}, std::pair{"Mono", false}})
                    if (sym.size() > std::strlen(suffix) && sym.ends_with(suffix)) {
                        sym.resize(sym.size() - std::strlen(suffix));
                        stereo = st;
                        break;
                    }
                o.set("@model", json::Value::string(sym));
                o.set("@path", json::Value::integerNumber(lane));
                o.set("@position", json::Value::integerNumber(pos));
                if (stereo) o.set("@stereo", json::Value::boolean(*stereo));
                o.set("@type", json::Value::integerNumber(b.blockType));
                if (b.cabModelNo >= 0)
                    if (const ModelRef* cm = fw.model(std::uint16_t(b.cabModelNo)); cm && cm->meta) {
                        const std::string cabName = "cab" + std::to_string(cabNo++);
                        json::Value cab = json::Value::object();
                        cab.set("@enabled", json::Value::boolean(true));
                        mic(cab, cm->meta, b.cabValues);
                        cab.set("@model", json::Value::string(fw.meta.symbol(cm->symId)));
                        knobs(cab, cm->meta, b.cabValues);
                        d.set(cabName, std::move(cab));
                        o.set("@cab", json::Value::string(cabName));
                    }
                mic(o, m->meta, b.values);
                if (b.hlxAttrs.empty()) o.set("@no_snapshot_bypass", json::Value::boolean(false));
                addKept(o, b.hlxAttrs);
                knobs(o, m->meta, b.values);
                const std::string name = "block" + std::to_string(blockNo++);
                names[b.uid] = {dsp, name};
                d.set(name, std::move(o));
            }

        for (int lane = 0; lane < 2; ++lane) {
            const PresetInput& in = path.inputs[std::size_t(lane)];
            const char* inSym = lane ? kInputBSymbol : kInputASymbol;
            json::Value i = json::Value::object();
            i.set("@input", json::Value::integerNumber(in.source));
            i.set("@model", json::Value::string(inSym));
            const MetaEntry* im = fw.metaBySymbol(inSym);
            knobs(i, im, in.values, limitOf(im));
            d.set(lane ? "inputB" : "inputA", std::move(i));

            const PresetOutput& out = path.outputs[std::size_t(lane)];
            json::Value o = json::Value::object();
            o.set("@model", json::Value::string(kOutputSymbol));
            o.set("@output", json::Value::integerNumber(out.dest));
            const MetaEntry* om = fw.metaBySymbol(kOutputSymbol);
            knobs(o, om, out.values, limitOf(om));
            d.set(lane ? "outputB" : "outputA", std::move(o));
        }

        const PresetRouting& r = path.routing;
        for (const bool split : {true, false}) {
            const ModelRef* m = fw.model(split ? r.splitModel : r.joinModel);
            if (!m || !m->meta) continue;
            json::Value s = json::Value::object();
            s.set("@enabled", json::Value::boolean(split ? r.splitEnabled : r.joinEnabled));
            s.set("@model", json::Value::string(fw.meta.symbol(m->symId)));
            s.set("@no_snapshot_bypass", json::Value::boolean(false));
            s.set("@position", json::Value::integerNumber(split ? r.splitPos : r.joinPos));
            knobs(s, m->meta, split ? r.splitValues : r.joinValues);
            d.set(split ? "split" : "join", std::move(s));
        }
        return d;
    }

    // "blockN" / "split" / "join" of a target, with its dsp; nullopt when gone.
    std::optional<std::pair<int, std::string>> nameOf(const PresetTarget& t) const {
        if (t.block >= 0) {
            const auto it = names.find(t.block);
            return it == names.end() ? std::nullopt : std::optional(it->second);
        }
        if (t.slot < 0) return std::nullopt;
        const int k = t.slot % kSlotsPerPath;
        if (k != kSplitIndex && k != kJoinIndex) return std::nullopt;
        return std::pair{t.slot / kSlotsPerPath, std::string(k == kSplitIndex ? "split" : "join")};
    }

    const MetaEntry* metaOf(const PresetTarget& t) const {
        if (t.block >= 0) {
            const PresetBlock* b = p.findBlock(t.block);
            const ModelRef* m = b ? fw.model(b->modelNo) : nullptr;
            return m ? m->meta : nullptr;
        }
        if (t.slot < 0) return nullptr;
        const PresetRouting& r = p.paths[std::size_t(t.slot / kSlotsPerPath)].routing;
        const ModelRef* m = fw.model(t.slot % kSlotsPerPath == kSplitIndex ? r.splitModel : r.joinModel);
        return m ? m->meta : nullptr;
    }

    const MetaParam* knobOf(const MetaEntry* meta, int param) const {
        if (!meta) return nullptr;
        for (const MetaParam& k : meta->knobs)
            if (int(k.number) - 1 == param) return &k;
        return nullptr;
    }

    // tree[dspN][name] (objects made on the way).
    static json::Value& at(json::Value& tree, int dsp, const std::string& name) {
        json::Value* d = const_cast<json::Value*>(tree.get("dsp" + std::to_string(dsp)));
        if (!d) d = &tree.set("dsp" + std::to_string(dsp), json::Value::object());
        json::Value* o = const_cast<json::Value*>(d->get(name));
        return o ? *o : d->set(name, json::Value::object());
    }

    json::Value controllers() const {
        json::Value tree = json::Value::object();
        for (const ControllerAssign& c : p.controllers) {
            const auto name = nameOf(c.target);
            const MetaParam* k = knobOf(metaOf(c.target), c.param);
            if (!name || !k) continue;
            json::Value a = json::Value::object();
            a.set("@controller", json::Value::integerNumber(c.source));
            a.set("@max", knobJson(*k, &c.max));
            a.set("@min", knobJson(*k, &c.min));
            // A knob on a switch: the switch function's label, LEDs and latching.
            if (const int fs = c.source - controller::footswitch(0); fs >= 0 && fs < 12)
                for (const FootswitchAssign& f : p.footswitches[std::size_t(fs)])
                    if (f.kind == 2 && f.param == c.param && f.target.block == c.target.block &&
                        f.target.slot == c.target.slot) {
                        writeSwitchAttrs(a, f, fw.meta.displayName(*k));
                        break;
                    }
            addKept(a, c.hlxAttrs);
            // Always false in the user's exports (meaning unknown); HX Edit writes it on every assignment.
            if (!a.get("@snapshot_disable")) a.set("@snapshot_disable", json::Value::boolean(false));
            if (c.source == controller::MidiCc) a.set("@cc", json::Value::integerNumber(c.cc));  // the edited number wins
            at(tree, name->first, name->second).set(fw.meta.symbol(k->nameSym), std::move(a));
        }
        return tree;
    }

    json::Value footswitches() const {
        json::Value tree = json::Value::object();
        for (int fs = 0; fs < kFootswitchSlots; ++fs)
            for (const FootswitchAssign& f : p.footswitches[std::size_t(fs)]) {
                // Bypass only: knobs go with their controller entries, commands with the commandFSn sections.
                if (f.kind != 1) continue;
                const auto name = nameOf(f.target);
                if (!name) continue;
                json::Value& o = at(tree, name->first, name->second);
                if (o.get("@fs_index")) continue;  // one switch per block in this format
                o.set("@fs_index", json::Value::integerNumber(fs == kToeSwitch ? 13 : fs + 1));
                writeSwitchAttrs(o, f, "");
                addKept(o, f.hlxAttrs);
                if (!o.get("@fs_enabled")) o.set("@fs_enabled", json::Value::boolean(false));
            }
        return tree;
    }

    json::Value snapshot(int i) const {
        const PresetSnapshot* s = std::size_t(i) < p.snapshots.size() ? &p.snapshots[std::size_t(i)] : nullptr;
        json::Value o = json::Value::object();
        o.set("@custom_name", json::Value::boolean(s && !s->name.empty()));
        o.set("@ledcolor", json::Value::integerNumber(s ? s->colour : 0));
        o.set("@name", json::Value::string(s && !s->name.empty() ? s->name : "SNAPSHOT " + std::to_string(i + 1)));
        o.set("@pedalstate", json::Value::integerNumber(2));
        const PresetValue* tempo = p.global(globalid::Tempo);
        o.set("@tempo", json::Value::real(s ? s->tempo : tempo ? tempo->v : 120.0));
        o.set("@valid", json::Value::boolean(s != nullptr || i == 0));
        json::Value blocks = json::Value::object();
        for (const auto& [uid, name] : names)
            if (const PresetBlock* b = p.findBlock(uid))
                at(blocks, name.first, name.second) = json::Value::boolean(s ? b->snapOn[std::size_t(i)] : b->enabled);
        for (int dsp = 0; dsp < 2; ++dsp)
            if (p.paths[std::size_t(dsp)].routing.splitOn)
                at(blocks, dsp, "split") = json::Value::boolean(p.paths[std::size_t(dsp)].routing.splitEnabled);
        o.set("blocks", std::move(blocks));
        json::Value ctl = json::Value::object();
        for (const ControllerAssign& c : p.controllers) {
            if (c.source != controller::Snapshot) continue;
            const auto name = nameOf(c.target);
            const MetaParam* k = knobOf(metaOf(c.target), c.param);
            if (!name || !k) continue;
            json::Value v = json::Value::object();
            v.set("@fs_enabled", json::Value::boolean(false));
            v.set("@value", knobJson(*k, &c.snapValues[std::size_t(i)]));
            at(ctl, name->first, name->second).set(fw.meta.symbol(k->nameSym), std::move(v));
        }
        if (!ctl.keys.empty()) o.set("controllers", std::move(ctl));
        // The instant / switch commands' values in this snapshot: the slots it has a row for (as HX Edit exports them);
        // a snapshot without any rows (a preset made here, an older file) gets every command with the preset's values.
        json::Value cmds = json::Value::object();
        for (int slot = 0; slot < 19 && std::size_t(slot) < p.commands.size(); ++slot) {
            const CommandSlot& c = p.commands[std::size_t(slot)];
            const std::string name = commandSection(slot);
            if (c.type == 0 || name.empty()) continue;
            std::array<int, 6> fields = c.fields;
            bool hasRow = false;
            if (s)
                for (const SnapshotCommand& r : s->commands)
                    if (r.slot == slot) {
                        hasRow = true;
                        if (i != p.snapshot) fields = r.fields;
                    }
            if (s && !s->commands.empty() && !hasRow) continue;
            json::Value v = json::Value::object();
            v.set("@fs_enabled", json::Value::boolean(false));
            setCommandFields(v, c.type, fields);
            cmds.set(name, std::move(v));
        }
        if (!cmds.keys.empty()) o.set("commands", std::move(cmds));
        return o;
    }

    // tone.commandX of a slot with a command; a switch's also carries the switch's label, LEDs and latching.
    json::Value command(int slot) const {
        const CommandSlot& c = p.commands[std::size_t(slot)];
        json::Value o = json::Value::object();
        o.set("@command", json::Value::integerNumber(c.type));
        setCommandFields(o, c.type, c.fields);
        const int fs = slot - controller::footswitch(0);  // 12 = the toe switch (kToeSwitch)
        if (fs < 0 || fs > kToeSwitch) return o;
        const FootswitchAssign* f = nullptr;
        for (const FootswitchAssign& a : p.footswitches[std::size_t(fs)])
            if (a.kind == 3) f = &a;
        FootswitchAssign none;
        none.colour = 462860;
        none.label = commandSwitchLabel(c.type);
        writeSwitchAttrs(o, f ? *f : none, commandSwitchLabel(c.type));
        if (f) addKept(o, f->hlxAttrs);
        if (!o.get("@fs_enabled")) o.set("@fs_enabled", json::Value::boolean(false));
        if (!o.get("@fs_primary")) o.set("@fs_primary", json::Value::boolean(true));
        return o;
    }

    json::Value global() const {
        json::Value g = json::Value::object();
        g.set("@model", json::Value::string("@global_params"));
        g.set("@current_snapshot", json::Value::integerNumber(p.snapshot));
        g.set("@topology0", json::Value::string(hlxTopology(p.paths[0].routing.mode)));
        g.set("@topology1", json::Value::string(hlxTopology(p.paths[1].routing.mode)));
        for (const GlobalName& n : kGlobals)
            if (const PresetValue* v = p.global(n.id))
                g.set(n.name, n.kind == PresetValue::Kind::Float ? json::Value::real(v->v)
                                                                 : json::Value::integerNumber(std::int64_t(v->v)));
        if (!g.get("@tempo")) g.set("@tempo", json::Value::real(120.0));
        for (const auto& [name, text] : p.hlxGlobals)
            if (!g.get(name))
                if (std::optional<json::Value> v = json::parse(text)) g.set(name, std::move(*v));
        if (!g.get("@pedalstate")) g.set("@pedalstate", json::Value::integerNumber(2));
        return g;
    }
};

// The firmware's version word (record header +0x04, e.g. 0x03800000 = 3.80), as .hlx data.device_version.
std::uint32_t firmwareVersion(const Firmware& fw) {
    const HxfRecord* r = fw.hxf ? fw.hxf->record(0) : nullptr;
    if (!r || r->header.size() < 8) return 0;
    const auto* h = r->header.data();
    return std::uint32_t(h[4]) << 24 | std::uint32_t(h[5]) << 16 | std::uint32_t(h[6]) << 8 | h[7];
}

} // namespace

std::string exportHlx(const Preset& preset, const Firmware& fw, std::vector<std::string>* warnings) {
    Exporter ex{fw, preset, warnings, {}};
    json::Value tone = json::Value::object();
    tone.set("dsp0", ex.path(0));
    tone.set("dsp1", ex.path(1));
    if (json::Value c = ex.controllers(); !c.keys.empty()) tone.set("controller", std::move(c));
    if (json::Value f = ex.footswitches(); !f.keys.empty()) tone.set("footswitch", std::move(f));
    tone.set("global", ex.global());
    for (int i = 0; i < kSnapshots; ++i) tone.set("snapshot" + std::to_string(i), ex.snapshot(i));
    int unnamed = 0;
    for (int slot = 0; slot < kCommandSlots && std::size_t(slot) < preset.commands.size(); ++slot) {
        if (preset.commands[std::size_t(slot)].type == 0) continue;
        const std::string name = commandSection(slot);
        if (name.empty()) ++unnamed;
        else tone.set(name, ex.command(slot));
    }
    if (unnamed && warnings)
        warnings->push_back(std::to_string(unnamed) +
                            " command(s) on EXP 3 left out (no .hlx name known; not on the LT)");
    for (const auto& [name, text] : preset.hlxKeep)
        if (!tone.get(name))
            if (std::optional<json::Value> v = json::parse(text)) tone.set(name, std::move(*v));

    json::Value meta = json::Value::object();
    meta.set("application", json::Value::string("oshexed"));
    meta.set("appversion", json::Value::integerNumber(firmwareVersion(fw)));
    meta.set("build_sha", json::Value::string(""));
    meta.set("modifieddate", json::Value::integerNumber(std::int64_t(std::time(nullptr))));
    meta.set("name", json::Value::string(preset.name));

    json::Value data = json::Value::object();
    data.set("device", json::Value::integerNumber(kLtDevice));
    data.set("device_version", json::Value::integerNumber(firmwareVersion(fw)));
    data.set("meta", std::move(meta));
    data.set("tone", std::move(tone));

    json::Value top = json::Value::object();
    top.set("data", std::move(data));
    json::Value m = json::Value::object();
    m.set("original", json::Value::integerNumber(0));
    m.set("pbn", json::Value::integerNumber(0));
    m.set("premium", json::Value::integerNumber(0));
    top.set("meta", std::move(m));
    top.set("schema", json::Value::string("L6Preset"));
    top.set("version", json::Value::integerNumber(6));
    return json::write(top);
}

const char* hlxTopology(int mode) { return mode >= 0 && mode < 5 ? kTopology[mode] : "A"; }

std::optional<Preset> importHlx(std::string_view text, const Firmware& fw, std::string* error,
                                std::vector<std::string>* warnings) {
    std::string err;
    const std::optional<json::Value> root = json::parse(text, &err);
    auto fail = [&](std::string message) -> std::optional<Preset> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (!root) return fail("not JSON: " + err);
    const json::Value* data = root->get("data");
    const json::Value* tone = data ? data->get("tone") : nullptr;
    if (!tone || !tone->isObject()) return fail("not an HX Edit preset (no data.tone)");

    Importer im{fw, warnings};
    for (const auto& [no, m] : fw.models) im.bySymbol.emplace(fw.meta.symbol(m.symId), &m);
    if (const json::Value* dev = data->get("device"); dev && std::uint32_t(dev->asInt()) != kLtDevice)
        im.warn("made for another Helix model (device " + std::to_string(dev->asInt()) + ")");

    im.p.stored = true;
    if (const json::Value* meta = data->get("meta")) im.p.name = str(meta->get("name"));
    const json::Value* global = tone->get("global");
    for (int dsp = 0; dsp < 2; ++dsp)
        if (const json::Value* d = tone->get("dsp" + std::to_string(dsp)); d && d->isObject()) im.path(dsp, *d, global);
    im.snapshots(*tone);
    im.controllers(*tone);
    im.footswitches(*tone);
    im.commands(*tone);
    for (const auto& [sym, n] : im.missing)
        im.warn("model " + (sym.empty() ? std::string("(none)") : sym) + " is not in this firmware: " + std::to_string(n) +
                " block(s) skipped");

    if (global && global->isObject())
        for (std::size_t k = 0; k < global->keys.size(); ++k) {
            const std::string& name = global->keys[k];
            const json::Value& v = global->items[k];
            if (derivedGlobal(name)) continue;
            const GlobalName* g = nullptr;
            for (const GlobalName& c : kGlobals)
                if (name == c.name) g = &c;
            if (g && v.isNumber()) im.p.setGlobal(g->id, {g->kind, v.asNumber()});
            else im.p.hlxGlobals.emplace_back(name, json::write(v));
        }
    for (std::size_t k = 0; k < tone->keys.size(); ++k)
        if (!modelledSection(tone->keys[k])) im.p.hlxKeep.emplace_back(tone->keys[k], json::write(tone->items[k]));

    // The stored block states are those of the snapshot in use.
    if (const json::Value* cur = global ? global->get("@current_snapshot") : nullptr) {
        const std::int64_t c = cur->asInt();
        if (c >= 0 && c < std::int64_t(im.p.snapshots.size())) im.p.snapshot = int(c);
    }
    im.p.useSnapshotCommands(im.p.snapshot);  // the command values are the snapshot in use's
    return std::move(im.p);
}

} // namespace hx

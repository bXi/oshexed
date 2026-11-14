#include "frontend/editorview.h"

#include "frontend/firmwaretextures.h"

#include "core/hlx.h"
#include "core/presetwriter.h"
#include "luminoveau.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <span>

namespace editor {

namespace {

// The 19 browser categories in device order (category index = position). Generic names.
struct CategoryInfo {
    const char *label;
    const char *shortLabel;
};
constexpr CategoryInfo kCategories[] = {
    {"None", ""},           {"Favorites", "Fav"},     {"Distortion", "Dist"}, {"Dynamics", "Dyn"},
    {"EQ", "EQ"},           {"Modulation", "Mod"},    {"Delay", "Delay"},     {"Reverb", "Reverb"},
    {"Pitch/Synth", "Pitch"}, {"Filter", "Filter"},   {"Wah", "Wah"},         {"Amp+Cab", "Amp"},
    {"Amp", "Amp"},         {"Preamp", "Preamp"},     {"Cab", "Cab"},         {"IR", "IR"},
    {"Volume/Pan", "Vol"},  {"Send/Return", "Send"},  {"Looper", "Looper"}};
constexpr int kCategoryCount = int(std::size(kCategories));  // stock; our builds add 19 (Custom, named by the firmware)
constexpr int kNone = 0, kFavorites = 1, kAmpCab = 11, kPreamp = 13, kCab = 14;

// Category names: ours for the stock 19, the firmware's own label for added ones (Custom).
std::string categoryLabel(const hx::Firmware *fw, int c, bool shortName) {
    if (c >= 0 && c < kCategoryCount) return shortName ? kCategories[c].shortLabel : kCategories[c].label;
    if (fw && c >= 0 && std::size_t(c) < fw->tables.labels.size()) {
        const hx::CategoryLabel &l = fw->tables.labels[std::size_t(c)];
        return shortName && !l.shortName.empty() ? l.shortName : l.name;
    }
    return "?";
}
constexpr int kLegacySub = 2;      // effect categories: mono, stereo, legacy
// The footswitches the LT lets presets use: FS2-5 and FS8-11 (indices; FS1 / FS7 are up / down, FS6 Mode, FS12 Tap).
constexpr int kAssignableSwitches[] = {1, 2, 3, 4, 7, 8, 9, 10};

bool sameTarget(const hx::PresetTarget &a, const hx::PresetTarget &b) {
    return a.block == b.block && (a.block >= 0 || a.slot == b.slot);
}

bool isSwitch(int source) {
    return source >= hx::controller::footswitch(0) && source <= hx::controller::footswitch(hx::kToeSwitch - 1);
}
constexpr int kLastEffectCategory = 10;
constexpr int kInputSlot = 8, kOutputSlot = 9;  // cell ids of the lane's input and output nodes
constexpr int kSplitCell = 50, kJoinCell = 60;   // dsp * 100 + these: the path's split / merge
constexpr float kBudget = 9000;    // cost units = 90% of one DSP (NOTES.md "HX Edit resources")

std::string catClass(int category) { return category >= 0 ? "cat-" + std::to_string(category) : "cat-none"; }

std::string capitalize(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = char(s[0] - 'a' + 'A');
    return s;
}

std::string formatNum(const char *f, double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string percent(double v) { return formatNum("%.2f%%", v); }


// Lane B's geometry in percent of the row: a gap g between columns sits at (g + 1) * 10%; the
// input and output nodes are centred at 5% and 95%. See UiLane.
void laneBLine(const hx::PresetPath &p, UiLane &l) {
    const hx::PresetRouting &r = p.routing;
    l.line = r.mode != 0 || !p.b.empty();
    if (!l.line) return;
    const double start = r.hasOwnInputB() ? 5 : 10;  // before lane B's first position
    const double end = r.hasOwnOutputB() ? 95 : 90;  // after its last
    l.lineLeft = percent(start);
    l.lineRight = percent(100 - end);
    l.split = r.splitOn && !r.hasOwnInputB();
    l.join = r.joinOn && !r.hasOwnOutputB();
    const double splitX = (r.splitPos + 1) * 10.0, joinX = (r.joinPos + 1) * 10.0;
    l.splitX = percent(splitX);
    l.splitBX = percent(start);
    l.splitHL = percent(std::min(splitX, start));
    l.splitHW = percent(std::abs(splitX - start));
    l.splitRight = splitX > start;
    l.splitHR = percent(100 - std::max(splitX, start));
    l.splitStraight = std::abs(splitX - start) < 0.01;
    l.joinX = percent(joinX);
    l.joinBX = percent(end);
    l.joinHL = percent(std::min(joinX, end));
    l.joinHW = percent(std::abs(joinX - end));
    l.joinRight = joinX > end;
    l.joinHR = percent(100 - std::max(joinX, end));
    l.joinStraight = std::abs(joinX - end) < 0.01;
}

// One line per preset for the device log: each path's routing mode, split / merge (on / off, column) and the lane B
// columns holding blocks, e.g. "path 1 serial | path 2 mode 1, split on @0, merge on @8, B [0]".
std::string routingText(const hx::Preset &p) {
    std::string out;
    for (int dsp = 0; dsp < 2; ++dsp) {
        const hx::PresetPath &path = p.paths[std::size_t(dsp)];
        const hx::PresetRouting &r = path.routing;
        out += (dsp ? " | path 2 " : "path 1 ");
        if (r.mode == 0 && path.b.empty()) {
            out += "serial";
            continue;
        }
        out += "mode " + std::to_string(r.mode) + ", split " + (r.splitOn ? "on" : "off") + " @" +
               std::to_string(r.splitPos) + ", merge " + (r.joinOn ? "on" : "off") + " @" + std::to_string(r.joinPos) +
               ", B [";
        bool first = true;
        for (int i = 0; i < hx::kLaneSlots; ++i)
            if (path.b.slots[std::size_t(i)]) {
                out += (first ? "" : " ") + std::to_string(i);
                first = false;
            }
        out += "]";
    }
    return out;
}

// Debug (split position on cmd 21): a blob's raw routing data per path: the path's own keys except the slot list,
// and the split / merge slot entries (list 10 / 19), as msgpack text.
std::string routingRaw(const std::vector<std::uint8_t> &blob) {
    std::string out;
    try {
        const hx::mp::Value root = hx::mp::parse(blob, 0x3d);
        for (int dsp = 0; dsp < 2; ++dsp) {
            const hx::mp::Value *path = root.get(dsp);
            if (!path || !path->isMap()) continue;
            hx::mp::Value own = hx::mp::map();
            for (std::size_t i = 0; i + 1 < path->items.size(); i += 2)
                if (path->items[i].asInt() != 22) {
                    own.items.push_back(path->items[i]);
                    own.items.push_back(path->items[i + 1]);
                }
            out += "\n    path " + std::to_string(dsp + 1) + " " + hx::mp::toText(own, 200);
            if (const hx::mp::Value *list = path->get(22); list && list->isArray() && list->items.size() >= 20) {
                out += "\n    split " + hx::mp::toText(list->items[10], 700);
                out += "\n    merge " + hx::mp::toText(list->items[19], 700);
            }
        }
    } catch (const std::exception &e) {
        out += " (" + std::string(e.what()) + ")";
    }
    return out;
}

float rawFloat(std::uint32_t raw) { return std::bit_cast<float>(raw); }

// A knob's range in display units: floats from their bit patterns, others as integers.
void knobRange(const hx::MetaParam &k, double &lo, double &hi, double &def) {
    if (k.type == std::uint32_t(hx::ParamType::Float)) {
        lo = rawFloat(k.min);
        hi = rawFloat(k.max);
        def = rawFloat(k.def);
    } else if (k.type == std::uint32_t(hx::ParamType::Switch)) {
        lo = 0;
        hi = 1;
        def = double(k.def != 0);
    } else {
        lo = double(std::int32_t(k.min));
        hi = double(std::int32_t(k.max));
        def = double(std::int32_t(k.def));
    }
    if (hi < lo) std::swap(lo, hi);
}

// The stored value for knob k, or the default when the stored one doesn't fit the knob
// (about 3% of factory values, mostly tempo-sync pairs whose order differs).
double knobValue(const hx::MetaParam &k, const std::vector<hx::PresetValue> *values) {
    double lo, hi, def;
    knobRange(k, lo, hi, def);
    if (!values || k.number == 0 || k.number > values->size()) return def;
    const hx::PresetValue &v = (*values)[k.number - 1];
    using K = hx::PresetValue::Kind;
    switch (hx::ParamType(k.type)) {
    case hx::ParamType::Float:
        if ((v.kind == K::Float || v.kind == K::Int) && v.v >= lo - 1e-4 && v.v <= hi + 1e-4) return v.v;
        break;
    case hx::ParamType::List:
        if (v.kind == K::Int && v.v >= lo && v.v <= hi) return v.v;
        break;
    case hx::ParamType::Switch:
        if (v.kind == K::Bool || (v.kind == K::Int && (v.v == 0 || v.v == 1))) return v.v;
        break;
    default: break;
    }
    return def;
}

std::string displayValue(const hx::DspMeta &meta, const hx::MetaParam &k, double v) {
    switch (hx::ParamType(k.type)) {
    case hx::ParamType::Switch: {
        const std::string label = meta.attr(k.attrs, v != 0 ? "true" : "false");
        return label.empty() ? (v != 0 ? "On" : "Off") : label;
    }
    case hx::ParamType::List: {
        const std::string label = meta.attr(k.attrs, std::to_string(int(v)));
        return label.empty() ? std::to_string(int(v)) : label;
    }
    case hx::ParamType::Command: return "";
    default: break;
    }
    double lo, hi, def;
    knobRange(k, lo, hi, def);
    const std::string type = meta.attr(k.attrs, "displayType");
    if (type == "knob") return formatNum("%.1f", v * 10);
    // Pans and the A/B split's route: 0..1 around a centre, labelled from rec 4
    // (valueMinString "Left"/"A", valueMidString "Center"/"Even Split", valueMaxString "Right"/"B").
    if (type == "pan" || type == "abpan") {
        if (std::abs(v - 0.5) < 0.005) {
            const std::string mid = meta.attr(k.attrs, "valueMidString");
            return mid.empty() ? "Center" : mid;
        }
        std::string side = meta.attr(k.attrs, v < 0.5 ? "valueMinStringShort" : "valueMaxStringShort");
        if (side.empty()) side = meta.attr(k.attrs, v < 0.5 ? "valueMinString" : "valueMaxString");
        return side + " " + formatNum("%.0f", std::abs(v - 0.5) * 200);
    }
    if (type == "percent") return formatNum("%.0f%%", hi <= 1 ? v * 100 : v);
    if (type == "db") return formatNum("%.1f dB", v);
    if (type == "hz") return v >= 1000 ? formatNum("%.2f kHz", v / 1000) : formatNum("%.0f Hz", v);
    std::string s = formatNum("%.2f", v);
    if (const std::string units = meta.attr(k.attrs, "units"); !units.empty() && units != "Hide") s += " " + units;
    return s;
}

// Knobs that only carry the item's on/off state; the power button handles that.
bool hiddenKnob(const hx::DspMeta &meta, const hx::MetaParam &k) { return meta.symbol(k.nameSym) == "bypass"; }

std::vector<hx::PresetValue> defaultValues(const hx::MetaEntry *meta) {
    std::vector<hx::PresetValue> values;
    if (!meta) return values;
    std::uint32_t count = 0;
    for (const hx::MetaParam &k : meta->knobs) count = std::max(count, k.number);
    values.resize(count);
    for (const hx::MetaParam &k : meta->knobs) {
        if (k.number == 0) continue;
        double lo, hi, def;
        knobRange(k, lo, hi, def);
        using K = hx::PresetValue::Kind;
        const K kind = k.type == std::uint32_t(hx::ParamType::Float)    ? K::Float
                       : k.type == std::uint32_t(hx::ParamType::Switch) ? K::Bool
                                                                          : K::Int;
        values[k.number - 1] = {kind, def};
    }
    return values;
}

hx::PresetBlock defaultBlock(const hx::ModelRef &m) {
    hx::PresetBlock b;
    b.modelNo = m.modelNo;
    b.values = defaultValues(m.meta);
    return b;
}

} // namespace

void EditorView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<UiItem>()) {
        s.RegisterMember("index", &UiItem::index);
        s.RegisterMember("label", &UiItem::label);
        s.RegisterMember("extra", &UiItem::extra);
        s.RegisterMember("cls", &UiItem::cls);
        s.RegisterMember("enabled", &UiItem::enabled);
        s.RegisterMember("icon", &UiItem::icon);
    }
    model.RegisterArray<std::vector<UiItem>>();
    if (auto s = model.RegisterStruct<UiCell>()) {
        s.RegisterMember("id", &UiCell::id);
        s.RegisterMember("kind", &UiCell::kind);
        s.RegisterMember("label", &UiCell::label);
        s.RegisterMember("cls", &UiCell::cls);
        s.RegisterMember("selected", &UiCell::selected);
        s.RegisterMember("bypassed", &UiCell::bypassed);
        s.RegisterMember("hidden", &UiCell::hidden);
        s.RegisterMember("drag_io", &UiCell::dragIo);
        s.RegisterMember("icon", &UiCell::icon);
        s.RegisterMember("tall_icon", &UiCell::tallIcon);
    }
    model.RegisterArray<std::vector<UiCell>>();
    if (auto s = model.RegisterStruct<UiLane>()) {
        s.RegisterMember("label", &UiLane::label);
        s.RegisterMember("ghost", &UiLane::ghost);
        s.RegisterMember("cells", &UiLane::cells);
        s.RegisterMember("line", &UiLane::line);
        s.RegisterMember("line_left", &UiLane::lineLeft);
        s.RegisterMember("line_right", &UiLane::lineRight);
        s.RegisterMember("split", &UiLane::split);
        s.RegisterMember("join", &UiLane::join);
        s.RegisterMember("split_x", &UiLane::splitX);
        s.RegisterMember("join_x", &UiLane::joinX);
        s.RegisterMember("split_bx", &UiLane::splitBX);
        s.RegisterMember("join_bx", &UiLane::joinBX);
        s.RegisterMember("split_hl", &UiLane::splitHL);
        s.RegisterMember("split_hw", &UiLane::splitHW);
        s.RegisterMember("join_hl", &UiLane::joinHL);
        s.RegisterMember("join_hw", &UiLane::joinHW);
        s.RegisterMember("split_right", &UiLane::splitRight);
        s.RegisterMember("join_right", &UiLane::joinRight);
        s.RegisterMember("split_hr", &UiLane::splitHR);
        s.RegisterMember("join_hr", &UiLane::joinHR);
        s.RegisterMember("split_straight", &UiLane::splitStraight);
        s.RegisterMember("join_straight", &UiLane::joinStraight);
        s.RegisterMember("split_id", &UiLane::splitId);
        s.RegisterMember("join_id", &UiLane::joinId);
        s.RegisterMember("split_sel", &UiLane::splitSel);
        s.RegisterMember("join_sel", &UiLane::joinSel);
    }
    model.RegisterArray<std::vector<UiLane>>();
    if (auto s = model.RegisterStruct<UiParam>()) {
        s.RegisterMember("index", &UiParam::index);
        s.RegisterMember("name", &UiParam::name);
        s.RegisterMember("value", &UiParam::value);
        s.RegisterMember("slider", &UiParam::slider);
        s.RegisterMember("pos", &UiParam::pos);
        s.RegisterMember("fill_left", &UiParam::fillLeft);
        s.RegisterMember("fill_width", &UiParam::fillWidth);
        s.RegisterMember("ir_slot", &UiParam::irSlot);
        s.RegisterMember("open", &UiParam::open);
    }
    model.RegisterArray<std::vector<UiParam>>();
    if (auto s = model.RegisterStruct<UiChoice>()) {
        s.RegisterMember("id", &UiChoice::id);
        s.RegisterMember("label", &UiChoice::label);
        s.RegisterMember("kind", &UiChoice::kind);
        s.RegisterMember("options", &UiChoice::options);
        s.RegisterMember("value", &UiChoice::value);
        s.RegisterMember("current", &UiChoice::current);
    }
    model.RegisterArray<std::vector<UiChoice>>();
    model.Bind("input_settings", &inputSettings_);
    model.BindEventCallback("input_setting", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.size() >= 2) setInputSetting(a[0].Get<int>(), a[1].Get<int>());
    });
    model.BindEventCallback("input_setting_step", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.size() < 2) return;
        const int id = a[0].Get<int>();
        for (const UiChoice &c : inputSettings_)
            if (c.id == id) setInputSetting(id, std::clamp(c.value + a[1].Get<int>(), 0, int(c.options.size()) - 1));
    });
    if (auto s = model.RegisterStruct<UiField>()) {
        s.RegisterMember("index", &UiField::index);
        s.RegisterMember("label", &UiField::label);
        s.RegisterMember("value", &UiField::value);
        s.RegisterMember("text", &UiField::text);
        s.RegisterMember("list", &UiField::list);
    }
    model.RegisterArray<std::vector<UiField>>();

    model.Bind("snapshots", &snapshots_);
    model.Bind("snap_sel", &snapSel_);
    model.Bind("snap_name", &snapName_);
    model.Bind("snap_tempo", &snapTempo_);
    model.Bind("snap_colour", &snapColour_);
    model.Bind("snap_colours", &snapColours_);
    model.Bind("snap_clip", &snapClip_);
    model.Bind("has_snaps", &hasSnaps_);
    model.Bind("edit_tab", &editTab_);

    model.Bind("online", &online_);
    model.Bind("connected", &connected_);
    model.Bind("device_text", &deviceText_);
    model.Bind("units", &units_);
    model.Bind("status", &status_);
    model.Bind("loaded", &loaded_);
    model.Bind("setlists", &setlists_);
    model.Bind("setlist_label", &setlistLabel_);
    model.Bind("setlist_open", &setlistOpen_);
    model.Bind("presets", &presets_);
    model.Bind("preset_sel", &presetSel_);
    model.Bind("preset_name", &presetName_);
    model.Bind("has_clip", &hasClip_);
    model.Bind("dsp1", &dsp1_);
    model.Bind("dsp2", &dsp2_);
    model.Bind("dsp1_w", &dsp1W_);
    model.Bind("dsp2_w", &dsp2W_);
    model.Bind("lanes", &lanes_);
    model.Bind("drop_id", &dropId_);
    model.Bind("dragging", &dragging_);
    model.Bind("categories", &categories_);
    model.Bind("cat_sel", &catSel_);
    model.Bind("subs", &subs_);
    model.Bind("sub_label", &subLabel_);
    model.Bind("sub_open", &subOpen_);
    model.Bind("models", &models_);
    model.Bind("model_sel", &modelSel_);
    model.Bind("has_block", &hasBlock_);
    model.Bind("confirm_save", &confirmSave_);
    model.Bind("preset_modified", &presetModified_);
    model.BindEventCallback("set_confirm_save", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        const bool on = !a.empty() && a[0].Get<int>() != 0;
        if (on == confirmSave_) return;
        setConfirmSave(on);
        if (onConfirmSaveChanged) onConfirmSaveChanged(on);
    });
    model.Bind("save_asking", &saveAsking_);
    model.Bind("save_question", &saveQuestion_);
    model.BindFunc("can_save", [this](Rml::Variant &v) { v = deviceCurrentSlot() >= 0; });
    model.BindEventCallback("save_ask", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { askSave(); });
    model.Bind("save_as_picking", &saveAsPicking_);
    // An imported / file preset while connected: send it to the unit as the preset being edited (then Save keeps it).
    model.BindFunc("can_send_preset", [this](Rml::Variant &v) { v = connected_ && onLiveEdit && !showingDevice_ && fw_; });
    model.BindEventCallback("send_preset", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        if (sendWholePreset("sending the preset"))
            setStatus("Sent \"" + presetName_ + "\" to the Helix as the preset being edited. Save there to keep it.");
    });
    model.Bind("renaming", &renaming_);
    model.Bind("rename_text", &renameText_);
    model.Bind("rename_index", &renameIndex_);
    // The in-place rename field in the preset list: Enter renames, Esc cancels, leaving the field renames.
    model.BindEventCallback("rename_key", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) {
        const int key = ev.GetParameter<int>("key_identifier", 0);
        if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER) confirmRename();
        else if (key == Rml::Input::KI_ESCAPE) cancelRename();
    });
    model.BindEventCallback("rename_blur", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { confirmRename(); });
    // Clicks in the field place the cursor; they must not reach the row (that would load the preset).
    model.BindEventCallback("rename_click", [](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) {
        ev.StopPropagation();
    });
    model.BindEventCallback("preset_rename_row", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (!a.empty()) openRename(a[0].Get<int>());
    });
    model.BindEventCallback("save_as", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { toggleSaveAs(); });
    model.BindEventCallback("save_confirm", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { confirmSave(); });
    model.BindEventCallback("save_cancel", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { cancelSave(); });
    model.Bind("can_copy_block", &canCopyBlock_);
    model.Bind("can_paste_block", &canPasteBlock_);
    model.BindEventCallback("block_copy", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { copyBlock(); });
    model.BindEventCallback("block_paste", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { pasteBlock(); });
    model.Bind("block_on", &blockOn_);
    model.Bind("block_title", &blockTitle_);
    model.Bind("block_cls", &blockCls_);
    model.Bind("edit_hint", &editHint_);
    model.Bind("has_cab", &hasCab_);
    model.Bind("edit_cab", &editCab_);
    model.Bind("main_label", &mainLabel_);
    model.Bind("cab_label", &cabLabel_);
    model.Bind("params", &params_);

    auto intArg = [](const Rml::VariantList &args) { return args.empty() ? -1 : args[0].Get<int>(); };
    model.Bind("irsel_choices", &irChoices_);
    model.Bind("irsel_shown", &irShown_);
    model.Bind("irsel_left", &irLeft_);
    model.Bind("irsel_top", &irTop_);
    model.Bind("irsel_height", &irHeight_);
    // IR slot dropdown of a param (IR Select, NAM Model): open / close it, pick a slot (same as moving the slider).
    // The list is a window-level popup placed under the head (above it when there's no room below), right edges
    // lined up: inside the scrolling param list it was clipped.
    model.BindEventCallback("irsel_toggle", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        const int index = intArg(a);
        ev.StopPropagation();  // not the param row's own mousedown (context menu)
        if (irOpen_ == index) return closeIr();
        Rml::Element *head = ev.GetCurrentElement();
        Rml::Context *ctx = RmlUI::GetContext();
        if (!head || !ctx) return;
        const float dp = ctx->GetDensityIndependentPixelRatio();
        const Rml::Vector2i win = ctx->GetDimensions();
        const Rml::Vector2f at = head->GetAbsoluteOffset(Rml::BoxArea::Border);
        const float w = 300.f * dp, hh = head->GetOffsetHeight();
        const float below = float(win.y) - (at.y + hh) - 8.f * dp, above = at.y - 8.f * dp;
        const bool down = below >= 200.f * dp || below >= above;
        const float h = std::min(420.f * dp, down ? below : above);
        const float x = std::clamp(at.x + head->GetOffsetWidth() - w, 0.f, std::max(0.f, float(win.x) - w));
        irLeft_ = std::to_string(int(x)) + "px";
        irTop_ = std::to_string(int(down ? at.y + hh : at.y - h)) + "px";
        irHeight_ = std::to_string(int(h)) + "px";
        irOpen_ = index;
        irOpenSlot_ = selectedSlotNumber();
        int current = -1;
        for (UiParam &p : params_) {
            p.open = p.index == irOpen_;
            if (p.open) current = std::atoi(p.value.c_str());  // "60  Name"
        }
        // Greyed: empty slots, and the wrong kind for this block (IRs for the NAM block, captures for an IR block,
        // which are also marked).
        const bool nam = wantsNam(editTarget().meta);
        for (UiItem &c : irChoices_) {
            const int kind = irKind(c.index);
            const bool named = std::size_t(c.index) <= irNames_.size() && !irNames_[std::size_t(c.index - 1)].empty();
            c.cls = c.index == current ? "on" : "";
            c.label = irLabel(c.index) + (kind == 2 && !nam ? "  [NAM]" : "");
            c.enabled = irNames_.empty() || (named && (kind == 0 || (nam ? kind == 2 : kind == 1)));
        }
        irShown_ = true;
        irScrollTries_ = current > 0 ? 10 : 0;  // update() scrolls the current slot into view once the list exists
        for (const char *v : {"params", "irsel_choices", "irsel_shown", "irsel_left", "irsel_top", "irsel_height"}) handle_.DirtyVariable(v);
    });
    model.BindEventCallback("irsel_close", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { closeIr(); });
    model.BindEventCallback("irsel_pick", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        pickIr(intArg(a));
        ev.StopPropagation();
    });
    model.BindEventCallback("toggle_setlist", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        setlistOpen_ = !setlistOpen_;
        subOpen_ = false;
        handle_.DirtyVariable("setlist_open");
        handle_.DirtyVariable("sub_open");
    });
    model.BindEventCallback("toggle_sub", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        subOpen_ = !subOpen_ && subs_.size() > 1;
        setlistOpen_ = false;
        handle_.DirtyVariable("setlist_open");
        handle_.DirtyVariable("sub_open");
    });
    // Connected: the lists are the unit's; picking a preset loads it on the unit, which then sends it back.
    model.BindEventCallback("pick_setlist", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        const int s = intArg(a);
        if (!deviceLists()) return pickSetlist(s);
        setlistOpen_ = false;
        if (s >= 0 && std::size_t(s) < setlists_.size()) setlist_ = s;
        setlistLabel_ = std::size_t(setlist_) < setlists_.size() ? setlists_[std::size_t(setlist_)].label : "";
        rebuildPresetList();
        refresh();
    });
    model.BindEventCallback("pick_preset", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        clickPreset(intArg(a));
    });
    model.BindEventCallback("set_online", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (onOnlineChanged) onOnlineChanged(intArg(a) != 0);
    });
    model.BindEventCallback("pick_unit", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        const int i = intArg(a);
        if (i < 0 || std::size_t(i) >= units_.size() || !units_[std::size_t(i)].enabled) return;
        if (onUnitChanged) onUnitChanged(units_[std::size_t(i)].extra);
    });
    // A click selects on the unit too (its screen follows); the unit's echo is ignored since it matches.
    model.BindEventCallback("pick_cell", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        clickCell(intArg(a));
    });
    // Right-click menus (mousedown with the right button).
    model.BindEventCallback("cell_menu", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        if (ContextMenu::isRightClick(ev)) cellMenu(ev, intArg(a));
    });
    model.BindEventCallback("param_menu", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        if (ContextMenu::isRightClick(ev)) paramMenu(ev, intArg(a));
    });
    model.BindEventCallback("preset_menu", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        if (ContextMenu::isRightClick(ev)) presetMenu(ev, intArg(a));
    });
    model.BindEventCallback("snap_menu", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        if (ContextMenu::isRightClick(ev)) snapshotMenu(ev, intArg(a));
    });
    model.BindEventCallback("pick_category", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) { pickCategory(intArg(a)); });
    model.BindEventCallback("pick_sub", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) { pickSub(intArg(a)); });
    model.BindEventCallback("pick_model", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) { pickModel(intArg(a)); });
    model.BindEventCallback("toggle_bypass", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { toggleBypass(); });
    model.BindEventCallback("param_change", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        paramChanged(intArg(a), ev.GetParameter<float>("value", 0.f));
    });
    model.BindEventCallback("drag_cell", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) { dragCell(intArg(a)); });
    model.BindEventCallback("drag_model", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) { dragModel(intArg(a)); });
    model.BindEventCallback("drop_cell", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        dropOnCell(intArg(a));
        ev.StopPropagation();
    });
    // dragout comes before dragover when moving between elements, so the highlight follows the pointer.
    model.BindEventCallback("drag_over", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (drag_.kind != Drag::Kind::None) setDropTarget(intArg(a));
    });
    model.BindEventCallback("drag_out", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { setDropTarget(-1); });
    // dragend comes after dragdrop, also when dropped outside any cell: forget the source.
    model.BindEventCallback("drag_end", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        drag_ = {};
        setDragging(false);
        setDropTarget(-1);
    });
    // Split / merge dots: dragging one moves it along lane A (style "drag: drag", no copy).
    model.BindEventCallback("junction_start", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (const int j = junctionOf(intArg(a)); j >= 0) pickCell(j);
    });
    model.BindEventCallback("junction_drag", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        if (junctionOf(intArg(a)) < 0) return;  // a block's drag also bubbles through its cell
        // Lane B's track (non-const: RmlUi's geometry getters may lay out).
        Rml::Element *track = ev.GetTargetElement();
        while (track && !track->IsClassSet("lane-track")) track = track->GetParentNode();
        if (!track) return;
        const Rml::Vector2f at = track->GetAbsoluteOffset(Rml::BoxArea::Border);
        moveJunction(intArg(a), ev.GetParameter<float>("mouse_x", 0.f), ev.GetParameter<float>("mouse_y", 0.f), at.x, at.y,
                     track->GetOffsetWidth(), track->GetOffsetHeight());
    });
    model.BindEventCallback("param_wheel", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        paramWheel(intArg(a), ev.GetParameter<float>("wheel_delta_y", 0.f), ev.GetParameter<bool>("shift_key", false));
        ev.StopPropagation();  // the wheel adjusts the value; don't also scroll the list
    });
    // Amp / Cab (or Cab 1 / Cab 2) in the params pane of a block with a linked cab.
    model.BindEventCallback("pick_cab_view", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        editCab_ = intArg(a) == 1;
        rebuildParams();
        refreshEditor();
        if (const int slot = selectedSlotNumber(); live() && slot >= 0 && selectedBlock())
            sendLive(hx::editor::kSelectBlock, hx::editor::selectArgs(slot, editCab_ ? 1 : 0));
    });
    model.BindEventCallback("preset_rename", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) {
        renamePreset(ev.GetParameter<Rml::String>("value", ""));
    });
    // Drag a preset row onto another to move it there.
    model.BindEventCallback("drag_preset", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) { dragPreset_ = intArg(a); });
    model.BindEventCallback("drop_preset", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        const int from = dragPreset_;
        dragPreset_ = -1;
        movePreset(from, intArg(a));
    });
    model.BindEventCallback("drag_preset_end", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { dragPreset_ = -1; });
    model.BindEventCallback("preset_copy", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { copyPreset(); });
    model.BindEventCallback("preset_paste", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { pastePreset(); });
    model.BindEventCallback("undo", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { undo(); });
    model.BindEventCallback("redo", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { redo(); });
    model.BindEventCallback("pick_snapshot", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        selectSnapshot(intArg(a));
    });
    // The rename field: every edit renames the selected snapshot at once.
    model.BindEventCallback("snap_rename", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) {
        renameSnapshot(ev.GetParameter<Rml::String>("value", ""));
    });
    model.BindEventCallback("snap_colour", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) { setSnapshotColour(intArg(a)); });
    // The tempo field: applied on Enter or when it loses focus, not per key (a half-typed "1" would clamp to 30).
    model.BindEventCallback("snap_tempo", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) {
        if (ev.GetType() == "change" && !ev.GetParameter<bool>("linebreak", false)) return;
        if (Rml::Element *el = ev.GetCurrentElement()) setSnapshotTempo(el->GetAttribute<Rml::String>("value", ""));
    });
    model.BindEventCallback("snap_copy", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { copySnapshot(); });
    model.BindEventCallback("snap_paste", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { pasteSnapshot(); });
    model.BindEventCallback("pick_edit_tab", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        pickEditTab(a.empty() ? "edit" : a[0].Get<Rml::String>());
    });
    // Command Center grid + slot editor.
    model.Bind("cc_top", &cmdTop_);
    model.Bind("cc_row1", &cmdRow1_);
    model.Bind("cc_row2", &cmdRow2_);
    model.Bind("cc_pedals", &cmdPedals_);
    model.Bind("cc_has_sel", &cmdHasSel_);
    model.Bind("cc_sel_name", &cmdSelName_);
    model.Bind("cc_sel_type", &cmdSelType_);
    model.Bind("cc_fields", &cmdFields_);
    model.Bind("cc_is_switch", &cmdIsSwitch_);
    model.Bind("cc_momentary", &cmdMomentary_);
    model.Bind("cc_label", &cmdLabel_);
    model.Bind("cc_led", &cmdLed_);
    model.Bind("cc_snap", &cmdSnapName_);
    model.BindEventCallback("cc_pick", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        cmdSel_ = intArg(a);
        rebuildCommandGrid();
        if (handle_)
            for (const char *v : {"cc_top", "cc_row1", "cc_row2", "cc_pedals", "cc_has_sel", "cc_sel_name", "cc_sel_type",
                                  "cc_fields", "cc_is_switch", "cc_momentary", "cc_label", "cc_led"})
                handle_.DirtyVariable(v);
    });
    model.BindEventCallback("cc_type_menu", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) { commandTypeMenu(ev); });
    model.BindEventCallback("cc_led_menu", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) { commandLedMenu(ev); });
    // A field typed in: applied on Enter or when it loses focus (as before).
    model.BindEventCallback("cc_field", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        if (ev.GetType() == "change" && !ev.GetParameter<bool>("linebreak", false)) return;
        if (Rml::Element *el = ev.GetCurrentElement(); el && cmdSel_ >= 0)
            setCommandField(cmdSel_, intArg(a), el->GetAttribute<Rml::String>("value", ""));
        rebuildCommandGrid();
        if (handle_) handle_.DirtyVariable("cc_fields");
    });
    // A list field (MMC message, looper function, ...): its choices by HX Edit's names.
    model.BindEventCallback("cc_field_menu", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        const int field = intArg(a);
        if (!menu_ || !cmdHasSel_ || std::size_t(cmdSel_) >= preset_.commands.size()) return;
        const hx::CommandSlot &c = preset_.commands[std::size_t(cmdSel_)];
        const std::vector<std::string> choices = hx::commandFieldChoices(c.type, field);
        const int lo = hx::commandFieldRange(c.type, field).first;  // instant Looper: no None
        std::vector<ContextMenu::Item> items;
        for (std::size_t k = std::size_t(std::max(lo, 0)); k < choices.size(); ++k) {
            const int slot = cmdSel_, v = int(k);
            ContextMenu::Item it{choices[k], [this, slot, field, v] {
                                     setCommandField(slot, field, std::to_string(v));
                                     rebuildCommandGrid();
                                     if (handle_) handle_.DirtyVariable("cc_fields");
                                 }};
            it.checked = std::size_t(field) < c.fields.size() && c.fields[std::size_t(field)] == v;
            items.push_back(std::move(it));
        }
        const std::vector<std::string> names = hx::commandFieldNames(c.type);
        menu_->openAt(ev, std::move(items), std::size_t(field) < names.size() ? names[std::size_t(field)] : "");
    });
    model.BindEventCallback("cc_step", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.size() >= 2) stepCommandField(a[0].Get<int>(), a[1].Get<int>());
    });
    model.BindEventCallback("cc_type", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (cmdIsSwitch_) {
            fsSel_ = cmdSel_ - hx::controller::footswitch(0);
            setSwitchMomentary(intArg(a) != 0);
        }
    });
    model.BindEventCallback("cc_label_set", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) {
        if (cmdIsSwitch_) {
            fsSel_ = cmdSel_ - hx::controller::footswitch(0);
            setSwitchLabel(ev.GetParameter<Rml::String>("value", ""));
        }
    });
    // Bypass / Controller Assign table + row editor.
    if (auto s = model.RegisterStruct<UiAssignRow>()) {
        s.RegisterMember("index", &UiAssignRow::index);
        s.RegisterMember("block", &UiAssignRow::block);
        s.RegisterMember("param", &UiAssignRow::param);
        s.RegisterMember("source", &UiAssignRow::source);
        s.RegisterMember("cls", &UiAssignRow::cls);
        s.RegisterMember("selected", &UiAssignRow::selected);
    }
    model.RegisterArray<std::vector<UiAssignRow>>();
    model.Bind("ca_rows", &caTable_);
    model.Bind("ca_sort", &caSortCol_);
    model.Bind("ca_sort_asc", &caSortAsc_);
    model.Bind("ca_has_sel", &caHasSel_);
    model.Bind("ca_block", &caBlock_);
    model.Bind("ca_block_cls", &caBlockCls_);
    model.Bind("ca_param", &caParam_);
    model.Bind("ca_source", &caSource_);
    model.Bind("ca_kind", &caKind_);
    model.Bind("ca_min_name", &caMinName_);
    model.Bind("ca_max_name", &caMaxName_);
    model.Bind("ca_min_text", &caMinText_);
    model.Bind("ca_max_text", &caMaxText_);
    model.Bind("ca_min_pos", &caMinPos_);
    model.Bind("ca_max_pos", &caMaxPos_);
    model.Bind("ca_cc", &caCc_);
    model.Bind("ca_label", &caLabel_);
    model.Bind("ca_led", &caLedName_);
    model.Bind("ca_momentary", &caMomentary_);
    model.BindEventCallback("ca_pick", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) { pickAssignRow(intArg(a)); });
    model.BindEventCallback("ca_sort_by", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        const int col = intArg(a);
        caSortAsc_ = col == caSortCol_ ? !caSortAsc_ : true;
        caSortCol_ = col;
        refreshEditor();
    });
    model.BindEventCallback("ca_param_menu", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) { assignParamMenu(ev); });
    model.BindEventCallback("ca_source_menu", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) { assignSourceMenu(ev); });
    model.BindEventCallback("ca_led_menu", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) { assignLedMenu(ev); });
    model.BindEventCallback("ca_range", [this, intArg](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        if (const int k = caKnobIndex(); k >= 0) setControllerRange(k, intArg(a) != 0, ev.GetParameter<float>("value", 0.f));
    });
    // Up / down arrows beside a range value: one step of the knob (1% of a float's range).
    model.BindEventCallback("ca_step", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        const int k = caKnobIndex();
        if (k < 0 || a.size() < 2) return;
        const bool max = a[0].Get<int>() != 0;
        const float step = a[1].Get<int>() > 0 ? 0.01f : -0.01f;
        setControllerRange(k, max, std::clamp((max ? caMaxPos_ : caMinPos_) + step, 0.f, 1.f));
        if (handle_) handle_.DirtyVariable(max ? "ca_max_pos" : "ca_min_pos");  // the slider follows the step
    });
    model.BindEventCallback("ca_cc_set", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) {
        if (ev.GetType() == "change" && !ev.GetParameter<bool>("linebreak", false)) return;
        if (const int k = caKnobIndex(); k >= 0)
            if (Rml::Element *el = ev.GetCurrentElement()) setControllerCc(k, el->GetAttribute<Rml::String>("value", ""));
    });
    model.BindEventCallback("ca_type", [this, intArg](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (caSel_ && isSwitch(caSel_->source)) {
            fsSel_ = caSel_->source - hx::controller::footswitch(0);
            setSwitchMomentary(intArg(a) != 0);
        }
    });
    model.BindEventCallback("ca_label_set", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &) {
        if (caSel_ && isSwitch(caSel_->source)) {
            fsSel_ = caSel_->source - hx::controller::footswitch(0);
            setSwitchLabel(ev.GetParameter<Rml::String>("value", ""));
        }
    });
    handle_ = model.GetModelHandle();

    rebuildLanes();
    rebuildCategories();
    rebuildUnitChoices();
}

void EditorView::setStatus(std::string status) {
    status_ = std::move(status);
    if (onStatus && !status_.empty()) onStatus(status_);
    if (handle_) handle_.DirtyVariable("status");
}

void EditorView::setFirmware(const hx::Firmware *fw, int setlist, int preset) {
    fw_ = fw;
    loaded_ = fw != nullptr;
    inputDefs_ = fw_ ? hx::readInputSettings(fw_->app) : std::vector<hx::GlobalSetting>{};
    fsColours_.clear();
    if (fw_) {
        // Swatches are ours (the unit's LED shades, roughly); the names are the firmware's.
        static const char *kSwatch[] = {"",        "#ffffff", "#ff2020", "#ff6000", "#ffa040", "#ffe000",
                                        "#20e040", "#20e0c0", "#2060ff", "#9040ff", "#ff50c0", "#303236"};
        const std::vector<std::string> names = hx::readLedColourNames(fw_->app);
        for (std::size_t i = 0; i < names.size() && i < std::size(kSwatch); ++i) {
            UiItem c;
            c.index = int(i);
            c.label = names[i];
            c.extra = kSwatch[i];
            fsColours_.push_back(std::move(c));
        }
    }
    inputAMeta_ = fw_ ? fw_->metaBySymbol(hx::kInputASymbol) : nullptr;
    inputBMeta_ = fw_ ? fw_->metaBySymbol(hx::kInputBSymbol) : nullptr;
    outputMeta_ = fw_ ? fw_->metaBySymbol(hx::kOutputSymbol) : nullptr;
    library_ = fw_ ? fw_->presets : hx::PresetLibrary{};
    edited_.assign(library_.presets.size(), false);
    clearHistory();
    presetSel_ = -1;  // nothing to store back into the new library
    presetClip_.reset();
    hasClip_ = false;
    blockTypes_.clear();
    setlists_.clear();
    const std::vector<std::string> &names = connected_ && !deviceSetlists_.empty() ? deviceSetlists_ : library_.setlists;
    for (std::size_t i = 0; i < names.size(); ++i) setlists_.push_back({int(i), names[i]});
    rebuildCategories();
    pickSetlist(setlist >= 0 && std::size_t(setlist) < setlists_.size() ? setlist : 0, preset);
    // Connected: show the unit's preset, not the file's.
    if (connected_ && devicePreset_) applyDevicePreset(devicePreset_->first, devicePreset_->second);
}

// ---------------------------------------------------------------- sidebar

void EditorView::pickSetlist(int index, int preset) {
    setlistOpen_ = false;
    storeWorking();
    presetSel_ = -1;  // stored; pickPreset must not store it again into the new setlist's slot
    if (index >= 0 && std::size_t(index) < setlists_.size()) setlist_ = index;
    setlistLabel_ = std::size_t(setlist_) < setlists_.size() ? setlists_[std::size_t(setlist_)].label : "";
    rebuildPresetList();
    pickPreset(preset >= 0 && std::size_t(preset) < presets_.size() ? preset : 0);
}

void EditorView::rebuildPresetList() {
    presets_.clear();
    if (!fw_) return;
    for (int i = 0; i < hx::kPresetsPerSetlist; ++i) {
        const std::size_t slot = slotOf(i);
        if (slot >= library_.presets.size()) break;
        const hx::Preset &p = library_.presets[slot];
        std::string name = p.name;
        if (connected_ && slot < deviceNames_.size()) name = deviceNames_[slot];  // the unit's own list
        // cls "edited": changed in this session (the list shows a mark).
        // Connected, only the open preset can be modified (the unit's edit buffer).
        const bool mark = connected_ ? i == presetSel_ && presetModified_ : bool(edited_[slot]);
        presets_.push_back({i, name.empty() ? "New Preset" : name, hx::presetNumber(i), mark ? "edited" : ""});
    }
}

// Connected, the open preset is the unit's: it doesn't go into the file's library.
void EditorView::storeWorking() {
    if (connected_) return;
    if (presetSel_ >= 0 && slotOf(presetSel_) < library_.presets.size()) library_.presets[slotOf(presetSel_)] = preset_;
}

void EditorView::pickPreset(int index) {
    storeWorking();
    showingDevice_ = false;
    presetSel_ = -1;
    preset_ = {};
    presetName_ = "No preset";
    if (fw_ && index >= 0 && std::size_t(index) < presets_.size()) {
        presetSel_ = index;
        preset_ = library_.presets[slotOf(index)];
        presetName_ = presets_[std::size_t(index)].label;
        if (onPresetOpened) onPresetOpened(setlist_, index);
    }
    clearHistory();
    showPreset();
    setModified(!connected_ && presetSel_ >= 0 && slotOf(presetSel_) < edited_.size() && edited_[slotOf(presetSel_)]);
}

// ---------------------------------------------------------------- the unit

namespace {

// Units offline editing can be for. Only the LT is supported so far (firmware addresses and slot counts).
struct UnitChoice {
    const char *id, *label;
    bool supported;
};
constexpr UnitChoice kUnits[] = {{"helix-lt", "Helix LT", true},       {"helix-floor", "Helix Floor", false},
                                 {"helix-rack", "Helix Rack", false},  {"hx-stomp", "HX Stomp", false},
                                 {"hx-stomp-xl", "HX Stomp XL", false}, {"hx-effects", "HX Effects", false}};

hx::mp::Value toMp(const hx::PresetValue &v) {
    switch (v.kind) {
    case hx::PresetValue::Kind::Bool: return hx::mp::boolean(v.v != 0);
    case hx::PresetValue::Kind::Int: return hx::mp::integer(std::int64_t(std::llround(v.v)));
    default: return hx::mp::real(v.v);
    }
}

hx::PresetValue fromMp(const hx::mp::Value &v) {
    using K = hx::PresetValue::Kind;
    switch (v.type) {
    case hx::mp::Type::Bool: return {K::Bool, v.b ? 1.0 : 0.0};
    case hx::mp::Type::Int: return {K::Int, double(v.i)};
    case hx::mp::Type::Float: return {K::Float, v.f};
    default: return {};
    }
}

} // namespace

void EditorView::rebuildUnitChoices() {
    units_.clear();
    for (const UnitChoice &u : kUnits)
        units_.push_back({int(units_.size()), std::string(u.label) + (u.supported ? "" : " (not yet)"), u.id,
                          unit_ == u.id ? "on" : "", u.supported});
}

void EditorView::setOnline(bool online, const std::string &unit) {
    online_ = online;
    unit_ = unit;
    rebuildUnitChoices();
    if (!online) setConnection(false, "Offline");
    if (handle_)
        for (const char *v : {"online", "units"}) handle_.DirtyVariable(v);
}

void EditorView::setUnitModel(hx::UnitModel model) {
    if (model == unitModel_) return;
    unitModel_ = model;
    if (isIo()) {  // an open port list: filtered again
        showSpecialInBrowser();
        refreshEditor();
    }
}

// Losing the unit keeps the open preset on screen; the lists go back to the file's.
void EditorView::setConnection(bool connected, const std::string &text) {
    const bool was = connected_;
    connected_ = connected;
    deviceText_ = text;
    if (!connected) showingDevice_ = false;
    if (was && !connected) {
        deviceSetlists_.clear();
        deviceNames_.clear();
        devicePreset_.reset();
        presetSel_ = -1;  // the unit's preset isn't one of the file's
        setlists_.clear();
        for (std::size_t i = 0; i < library_.setlists.size(); ++i) setlists_.push_back({int(i), library_.setlists[i]});
        setlistLabel_ = std::size_t(setlist_) < setlists_.size() ? setlists_[std::size_t(setlist_)].label : "";
        rebuildPresetList();
        refresh();
    }
    if (!connected) cancelPicking();
    if (handle_)
        for (const char *v : {"connected", "device_text", "can_save", "can_send_preset"}) handle_.DirtyVariable(v);
}

void EditorView::applyDeviceLibrary(const std::vector<std::string> &setlists,
                                    const std::vector<std::pair<int, std::string>> &names) {
    deviceSetlists_ = setlists;
    deviceNames_.assign(std::size_t(hx::kSetlists) * hx::kPresetsPerSetlist, "");
    for (const auto &[slot, name] : names)
        if (slot >= 0 && std::size_t(slot) < deviceNames_.size()) deviceNames_[std::size_t(slot)] = name;
    setlists_.clear();
    for (std::size_t i = 0; i < deviceSetlists_.size(); ++i) setlists_.push_back({int(i), deviceSetlists_[i]});
    setlistLabel_ = std::size_t(setlist_) < setlists_.size() ? setlists_[std::size_t(setlist_)].label : "";
    rebuildPresetList();
    refresh();
}

// The unit's current preset (after connecting, a load, a snapshot change, a new model). The same preset again keeps
// the selection, so a resync doesn't jump around.
void EditorView::applyDevicePreset(const hx::editor::CurrentInfo &info, const std::vector<std::uint8_t> &blob) {
    devicePreset_ = std::pair{info, blob};
    if (!fw_) {
        setStatus("Open the firmware: the editor needs its models to show the unit's preset");
        return;
    }
    std::string error;
    std::optional<hx::Preset> p = hx::parsePresetBlob(blob, &error);
    if (!p) {
        setStatus("Could not read the unit's preset: " + error);
        return;
    }
    if (onLog) onLog("unit's preset routing: " + routingText(*p) + routingRaw(blob));
    p->name = info.name;
    p->snapshot = std::clamp(info.snapshot, 0, hx::kSnapshots - 1);
    const bool same = showingDevice_ && info.setlist == setlist_ && info.preset == presetSel_ && sel_.valid();
    showingDevice_ = true;
    if (info.setlist >= 0 && std::size_t(info.setlist) < setlists_.size() && info.setlist != setlist_) {
        setlist_ = info.setlist;
        setlistLabel_ = setlists_[std::size_t(setlist_)].label;
        rebuildPresetList();
    }
    presetSel_ = info.preset >= 0 && std::size_t(info.preset) < presets_.size() ? info.preset : -1;
    preset_ = std::move(*p);
    presetName_ = preset_.name.empty() ? "New Preset" : preset_.name;
    // The same preset read again (after an edit, an undo sent whole, a change on the unit): undo / redo stay usable.
    // Another one loaded: its history starts empty (the old steps would overwrite it).
    const int shownSlot = info.setlist * hx::kPresetsPerSetlist + info.preset;
    if (shownSlot != historySlot_) clearHistory();
    historySlot_ = shownSlot;
    if (same) {
        reselect();
    } else {
        showPreset();
    }
    setModified(info.modified);  // the unit's own flag (117)
    refresh();
}

void EditorView::applyDeviceNotice(const hx::EditorNotice &n) {
    if (!showingDevice_) return;  // an imported file is open: the unit's changes are for its own preset
    if (const auto k = hx::editor::knobChange(n)) {
        // Our own change coming back: the slider may already be further along.
        if (k->slot == lastKnob_.slot && k->knob == lastKnob_.knob && k->paramSet == lastKnob_.set &&
            hx::steadySeconds() - lastKnob_.at < 1.0)
            return;
        const int dsp = k->slot / hx::kSlotsPerPath, at = k->slot % hx::kSlotsPerPath;
        if (dsp < 0 || dsp > 1 || k->knob < 0) return;
        hx::PresetPath &path = preset_.paths[std::size_t(dsp)];
        std::vector<hx::PresetValue> *values = nullptr;
        if (at >= 1 && at <= 8) {
            if (auto &b = path.a.slots[std::size_t(at - 1)]) values = k->paramSet ? &b->cabValues : &b->values;
        } else if (at >= 11 && at <= 18) {
            if (auto &b = path.b.slots[std::size_t(at - 11)]) values = k->paramSet ? &b->cabValues : &b->values;
        } else if (at == 0) {
            values = &path.inputs[0].values;
        } else if (at == 9) {
            values = &path.outputs[0].values;
        } else if (at == 10) {
            values = &path.routing.splitValues;
        } else if (at == 19) {
            values = &path.routing.joinValues;
        }
        if (!values) return;
        if (values->size() <= std::size_t(k->knob)) values->resize(std::size_t(k->knob) + 1);
        (*values)[std::size_t(k->knob)] = fromMp(k->value);
        if (selectedSlotNumber() == k->slot) noticeParams_ = true;
        return;
    }
    if (const auto b = hx::editor::bypassChange(n)) {
        const int dsp = b->first / hx::kSlotsPerPath, at = b->first % hx::kSlotsPerPath;
        if (dsp < 0 || dsp > 1) return;
        hx::PresetPath &path = preset_.paths[std::size_t(dsp)];
        std::optional<hx::PresetBlock> *slot = at >= 1 && at <= 8     ? &path.a.slots[std::size_t(at - 1)]
                                               : at >= 11 && at <= 18 ? &path.b.slots[std::size_t(at - 11)]
                                                                      : nullptr;
        if (!slot || !*slot) return;
        (*slot)->enabled = b->second;
        (*slot)->snapOn[std::size_t(preset_.snapshot)] = b->second;
        noticeFlow_ = true;
        return;
    }
    // A block selected on the unit: select it here too (the Cab view for param set 1). Not sent back.
    if (n.event == hx::editor::kNoticeSelect) {
        const hx::mp::Value *slot = n.payload.get(98);
        const int s = slot ? int(slot->asInt(-1)) : -1;
        const int dsp = s / hx::kSlotsPerPath, at = s % hx::kSlotsPerPath;
        if (s < 0 || dsp > 1) return;
        const int cab = n.payload.get(26) ? int(n.payload.get(26)->asInt()) : 0;
        int cell = -1;
        if (at >= 1 && at <= 8) cell = dsp * 100 + (at - 1);
        else if (at >= 11 && at <= 18) cell = dsp * 100 + 10 + (at - 11);
        else if (at == 0) cell = dsp * 100 + kInputSlot;
        else if (at == 9) cell = dsp * 100 + kOutputSlot;
        else if (at == 10) cell = dsp * 100 + kSplitCell;
        else if (at == 19) cell = dsp * 100 + kJoinCell;
        if (cell < 0) return;
        if (s != selectedSlotNumber()) pickCell(cell);
        if (editCab_ != (cab == 1)) {
            editCab_ = cab == 1;
            rebuildParams();
            refreshEditor();
        }
        return;
    }
    // A snapshot selected (on the unit or by us): the blocks and knobs that change follow as their own events.
    if (n.event == hx::editor::kNoticeSnapshotSelected) {
        if (const hx::mp::Value *s = n.payload.get(92)) {
            const int i = int(s->asInt());
            if (i >= 0 && std::size_t(i) < preset_.snapshots.size() && i != preset_.snapshot) {
                preset_.useSnapshotCommands(i);
                rebuildSnapshots();
                rebuildCommandGrid();
                if (handle_) handle_.DirtyVariable("cc_fields");
            }
        }
        return;
    }
    // A preset stored (our save, or one on the unit): its new name in the list; 5 says it's done.
    if (n.event == hx::editor::kNoticePresetNamed || n.event == hx::editor::kNoticePresetSaved) {
        const hx::mp::Value *sl = n.payload.get(107), *pr = n.payload.get(108), *nm = n.payload.get(109);
        if (!sl || !pr) return;
        const std::size_t slot = std::size_t(sl->asInt()) * hx::kPresetsPerSetlist + std::size_t(pr->asInt());
        if (nm && slot < deviceNames_.size()) {
            deviceNames_[slot] = nm->asText();
            rebuildPresetList();
            if (handle_) handle_.DirtyVariable("presets");
        }
        if (n.event == hx::editor::kNoticePresetSaved) {
            // Saved where it came from: no longer modified. Saved elsewhere (Save as): the re-read after the save
            // shows which preset the unit has open now, with its flag.
            setStatus("Saved to the Helix (" + hx::presetNumber(int(pr->asInt())) + ")");
            if (int(slot) == deviceCurrentSlot()) setModified(false);
        } else {
            setModified(presetModified_);  // the list was rebuilt: keep the mark on the open preset
        }
        return;
    }
    if (n.event == hx::editor::kNoticeSnapshotColour) {
        const hx::mp::Value *s = n.payload.get(92), *c = n.payload.get(66);
        if (s && c && std::size_t(s->asInt()) < preset_.snapshots.size()) {
            preset_.snapshots[std::size_t(s->asInt())].colour = int(c->asInt());
            rebuildSnapshots();
        }
        return;
    }
    if (n.event == hx::editor::kNoticeGlobal)
        if (const hx::mp::Value *id = n.payload.get(118), *v = n.payload.get(119); id && v) {
            preset_.setGlobal(int(id->asInt()), fromMp(*v));
            // The tempo is the snapshot in use's.
            if (id->asInt() == hx::globalid::Tempo && std::size_t(preset_.snapshot) < preset_.snapshots.size()) {
                preset_.snapshots[std::size_t(preset_.snapshot)].tempo = v->asNumber();
                rebuildSnapshots();
            }
            // The input's In-Z / Pad changed on the unit.
            if (id->asInt() == hx::globalid::GuitarInputZ || id->asInt() == hx::globalid::GuitarPad) {
                rebuildInputSettings();
                if (handle_) handle_.DirtyVariable("input_settings");
            }
        }
}

// The input block's preset settings (In-Z, Pad), shown with input A / B; values from the preset's key 5.
void EditorView::rebuildInputSettings() {
    inputSettings_.clear();
    if (sel_.special != Special::Input) return;
    for (const hx::GlobalSetting &d : inputDefs_) {
        UiChoice c;
        c.id = d.id;
        c.label = d.label;
        c.kind = d.options.size() <= 4 ? "segs" : "step";
        for (std::size_t k = 0; k < d.options.size(); ++k) c.options.push_back({int(k), d.options[k]});
        const hx::PresetValue *v = preset_.global(d.id);
        c.value = v ? int(std::lround(v->v)) : 0;
        c.current = c.value >= 0 && std::size_t(c.value) < d.options.size() ? d.options[std::size_t(c.value)] : "";
        inputSettings_.push_back(std::move(c));
    }
}

// Stored in the preset; connected, sent live (cmd 25, as HX Edit does for the input's In-Z / Pad).
void EditorView::setInputSetting(int id, int index) {
    const auto d = std::find_if(inputDefs_.begin(), inputDefs_.end(), [&](const hx::GlobalSetting &x) { return x.id == id; });
    if (d == inputDefs_.end() || index < 0 || std::size_t(index) >= d->options.size()) return;
    const hx::PresetValue *old = preset_.global(id);
    if (old && int(std::lround(old->v)) == index) return;
    beginEdit("input:" + std::to_string(id));
    preset_.setGlobal(id, {hx::PresetValue::Kind::Int, double(index)});
    if (live()) sendLive(hx::editor::kSetSetting, hx::editor::settingArgs(id, hx::mp::integer(index)));
    rebuildInputSettings();
    if (handle_) handle_.DirtyVariable("input_settings");
}

// The unit puts the model in and answers with its default knobs (the session reads the preset back). The block
// type: the block's own when it came from a preset (a pasted copy), else the factory presets' for the model.
bool EditorView::sendSelectedModel() {
    if (!live()) return false;
    const std::optional<hx::PresetBlock> *s = selectedSlot();
    if (!s || !*s) return false;
    return sendModelAt(selectedSlotNumber(), **s);
}

bool EditorView::sendModelAt(int slot, const hx::PresetBlock &b) {
    if (!live() || slot < 0) return false;
    const int type = b.blockType ? int(b.blockType) : blockTypeOf(b);
    if (type < 0) {
        notSent("this model (its block type is unknown)");
        return false;
    }
    sendLive(hx::editor::kSetModel, hx::editor::modelArgs(slot, b.slotType, b.modelNo, b.cabModelNo, type, true));
    return true;
}

// A whole block recreated at a slot with the commands the unit knows: select the slot, the model (the unit sets its
// default knobs), the knobs that differ from those (only the model's own: stored values also hold entries the unit
// refuses to set), the cab's knobs, then off if it was off. Controller / footswitch assignments aren't sent.
bool EditorView::sendBlockAt(int slot, const hx::PresetBlock &b) {
    if (!live()) return false;
    sendLive(hx::editor::kSelectBlock, hx::editor::selectArgs(slot, 0));
    if (!sendModelAt(slot, b)) return false;
    for (int set = 0; set < 2; ++set) {
        const int modelNo = set ? b.cabModelNo : b.modelNo;
        const hx::ModelRef *m = modelNo >= 0 && fw_ ? fw_->model(std::uint16_t(modelNo)) : nullptr;
        if (!m || !m->meta) continue;
        const std::vector<hx::PresetValue> &values = set ? b.cabValues : b.values;
        const std::vector<hx::PresetValue> defaults = defaultValues(m->meta);
        for (const hx::MetaParam &k : m->meta->knobs) {
            const std::size_t i = std::size_t(k.number) - 1;
            if (k.number == 0 || i >= values.size() || values[i].kind == hx::PresetValue::Kind::None) continue;
            if (i < defaults.size() && defaults[i].kind == values[i].kind && std::abs(defaults[i].v - values[i].v) < 1e-6)
                continue;
            sendLive(hx::editor::kSetKnob, hx::editor::knobArgs(slot, set, int(i), toMp(values[i])));
        }
    }
    if (!b.enabled) sendLive(hx::editor::kBypass, hx::editor::bypassArgs(slot, false));
    return true;
}

void EditorView::flushDeviceNotices() {
    if (noticeFlow_) {
        rebuildLanes();
        rebuildParams();
        refreshEditor();
    } else if (noticeParams_) {
        rebuildParams();
        if (handle_) handle_.DirtyVariable("params");
    }
    noticeFlow_ = noticeParams_ = false;
}

void EditorView::sendLive(int command, hx::mp::Value args) {
    if (live()) onLiveEdit(command, std::move(args));
}

void EditorView::notSent(const std::string &what) {
    if (!live()) return;
    setStatus("Not sent to the unit: " + what + " (the unit's command for it isn't known yet). It stays in the editor.");
}

int EditorView::selectedSlotNumber() const {
    if (!sel_.valid()) return -1;
    if (sel_.special == Special::Split) return sel_.dsp * hx::kSlotsPerPath + 10;
    if (sel_.special == Special::Join) return sel_.dsp * hx::kSlotsPerPath + 19;
    // Lane A's input and output are slots 0 and 9. Lane B's take the split's / merge's slot (10 / 19) once the split /
    // merge became them (kMoveSplit / kMoveMerge); their knobs are param set 1 there (hx-edit-ops-2).
    const hx::PresetRouting &r = preset_.paths[std::size_t(sel_.dsp)].routing;
    if (sel_.special == Special::Input)
        return !sel_.lane ? sel_.dsp * hx::kSlotsPerPath + 0
               : r.mode != 0 && !r.splitOn ? sel_.dsp * hx::kSlotsPerPath + hx::editor::kSlotInputB : -1;
    if (sel_.special == Special::Output)
        return !sel_.lane ? sel_.dsp * hx::kSlotsPerPath + 9
               : r.mode != 0 && !r.joinOn ? sel_.dsp * hx::kSlotsPerPath + hx::editor::kSlotOutputB : -1;
    if (sel_.special != Special::None) return -1;
    return sel_.dsp * hx::kSlotsPerPath + (sel_.lane ? 11 : 1) + sel_.slot;
}

// Presets store each block's type (key 9) next to its model; the unit wants it with a new model. Taken from the
// firmware's factory presets, where most models appear.
// Preset key 9 for a block, by the rule all factory presets follow (NOTES.md "Preset format"): delay / reverb 8;
// amp or preamp 17, with a legacy cab 18, with a new (MicIr) cab 33; cab 15 (legacy) / 31 (new), dual 16 / 32;
// other effects 1. The rest (FX loops 9 / 10, generators 23) as the factory presets have them; -1 if unknown.
int EditorView::blockTypeOf(const hx::PresetBlock &b) const {
    const hx::ModelRef *m = fw_ ? fw_->model(b.modelNo) : nullptr;
    if (!m) return -1;
    auto newCab = [&](int modelNo) {
        const hx::ModelRef *c = modelNo >= 0 ? fw_->model(std::uint16_t(modelNo)) : nullptr;
        return c && fw_->meta.symbol(c->symId).find("CabMicIr") != std::string::npos;
    };
    const bool linked = b.cabModelNo >= 0;
    switch (m->category) {
    case 6:  // delay
    case 7:  // reverb
        return 8;
    case kAmpCab:
    case kAmpCab + 1:  // amp
    case kPreamp:
        return !linked ? 17 : newCab(b.cabModelNo) ? 33 : 18;
    case kCab: {
        const bool isNew = newCab(b.modelNo);
        return linked ? (isNew ? 32 : 16) : (isNew ? 31 : 15);
    }
    default: break;
    }
    if (const int t = factoryBlockType(b.modelNo); t >= 0) return t;
    // The model's browser entry starts with the same block kind (NOTES "Which category a placed block gets": 0x0f
    // IR, 0x11 amp, ...; checked against every list: IR 19 / 20 / 21, loopers 22, our NAM 17, MIDI Guitar 1). Models
    // no factory preset uses (IRs, our own) get theirs from there.
    if (m->list >= 0 && std::size_t(m->list) < fw_->tables.lists.size()) {
        const hx::BrowserList &l = fw_->tables.lists[std::size_t(m->list)];
        if (m->entry >= 0 && std::size_t(m->entry) < l.entries.size() && l.entries[std::size_t(m->entry)].category > 0)
            return int(l.entries[std::size_t(m->entry)].category);
    }
    return m->category >= 2 && m->category <= kLastEffectCategory ? 1 : -1;
}

int EditorView::factoryBlockType(std::uint16_t model) const {
    if (!fw_) return -1;
    if (blockTypes_.empty())
        for (const hx::Preset &p : fw_->presets.presets)
            for (const hx::PresetPath &path : p.paths)
                for (const hx::PresetLane *lane : {&path.a, &path.b})
                    for (const auto &b : lane->slots)
                        if (b) blockTypes_.emplace(b->modelNo, int(b->blockType));
    const auto it = blockTypes_.find(model);
    return it == blockTypes_.end() ? -1 : it->second;
}

// ---------------------------------------------------------------- undo

void EditorView::beginEdit(const std::string &key, const hx::Preset *before) {
    const auto now = std::chrono::steady_clock::now();
    const bool same = !key.empty() && key == lastEditKey_ && now - lastEditAt_ < std::chrono::milliseconds(1500);
    lastEditKey_ = key;
    lastEditAt_ = now;
    markEdited();
    if (same) return;
    constexpr std::size_t kMaxUndo = 200;
    if (undo_.size() >= kMaxUndo) undo_.erase(undo_.begin());
    undo_.push_back(before ? *before : preset_);
    redo_.clear();
}

void EditorView::clearHistory() {
    undo_.clear();
    redo_.clear();
    lastEditKey_.clear();
    historySlot_ = -1;
}

void EditorView::undo() {
    if (undo_.empty()) return setStatus("Nothing to undo");
    redo_.push_back(std::move(preset_));
    preset_ = std::move(undo_.back());
    undo_.pop_back();
    lastEditKey_.clear();
    reselect();
    if (live()) sendWholePreset("undo");
}

void EditorView::redo() {
    if (redo_.empty()) return setStatus("Nothing to redo");
    undo_.push_back(std::move(preset_));
    preset_ = std::move(redo_.back());
    redo_.pop_back();
    lastEditKey_.clear();
    reselect();
    if (live()) sendWholePreset("redo");
}

// The unit's own fit check (NOTES.md "Compute budget"): a path's model costs must stay within 9000 units (the meter's
// 90%). An edit that grows a path past that is taken back before anything goes to the unit, which would refuse it
// (error -306). Call right after the edit, while undo_ holds the state before it.
bool EditorView::undoIfDspFull(int dsp) {
    if (!fw_ || undo_.empty() || dsp < 0 || dsp > 1) return false;
    const std::uint32_t now = fw_->pathCost(preset_.paths[std::size_t(dsp)]);
    const std::uint32_t before = fw_->pathCost(undo_.back().paths[std::size_t(dsp)]);
    if (now <= std::uint32_t(kBudget) || now <= before) return false;  // fits, or no worse than it was
    preset_ = std::move(undo_.back());
    undo_.pop_back();
    lastEditKey_.clear();
    reselect();
    char buf[96];
    std::snprintf(buf, sizeof buf, "DSP full: path %d would need %.0f%% (the unit allows 90%%)", dsp + 1,
                  double(now) / kBudget * 90.0);
    setStatus(buf);
    return true;
}

// The open preset's row shows the editor's name for it, offline only: connected, the list is the unit's stored names
// (deviceNames_), which change only when the unit says so. Writing the edit buffer's name there by index put it on
// the wrong row when the selection moved before a load finished.
void EditorView::setRowLabel(const std::string &name) {
    if (connected_ || presetSel_ < 0 || std::size_t(presetSel_) >= presets_.size()) return;
    presets_[std::size_t(presetSel_)].label = name;
}

void EditorView::reselect() {
    if (presetSel_ >= 0) {
        presetName_ = preset_.name.empty() ? "New Preset" : preset_.name;
        setRowLabel(presetName_);
    }
    if (sel_.special != Special::None) {
        showSpecialInBrowser();
    } else {
        const hx::PresetBlock *b = selectedBlock();
        showModelInBrowser(b && fw_ ? fw_->model(b->modelNo) : nullptr);
    }
    rebuildSnapshots();
    rebuildLanes();
    rebuildParams();
    rebuildDsp();
    refreshEditor();
    if (handle_)
        for (const char *v : {"presets", "preset_name"}) handle_.DirtyVariable(v);
}

void EditorView::markEdited() {
    if (connected_) {
        if (showingDevice_) setModified(true);  // the unit flags it too; its flag comes back with the next read
        return;
    }
    if (presetSel_ < 0 || slotOf(presetSel_) >= edited_.size() || edited_[slotOf(presetSel_)]) return;
    edited_[slotOf(presetSel_)] = true;
    setModified(true);
}

// "*": connected, the unit's state of the open preset (edited since loaded or saved); offline, edited in this
// session (the file's copy).
void EditorView::setModified(bool on) {
    presetModified_ = on;
    if (presetSel_ >= 0 && std::size_t(presetSel_) < presets_.size())
        presets_[std::size_t(presetSel_)].cls = on ? "edited" : "";
    if (handle_)
        for (const char *v : {"presets", "preset_modified"}) handle_.DirtyVariable(v);
}

void EditorView::applyDeviceInfo(const hx::editor::CurrentInfo &info) {
    if (showingDevice_ && info.setlist == setlist_ && info.preset == presetSel_) setModified(info.modified);
}

// ---------------------------------------------------------------- save to the unit

namespace {
// Preset names on the unit: up to 16 printable ASCII characters.
std::string unitPresetName(const std::string &name) {
    std::string out;
    for (char c : name) {
        if (out.size() >= 16) break;
        out += c >= 32 && c < 127 ? c : '?';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "New Preset" : out;
}
} // namespace

void EditorView::setConfirmSave(bool on) {
    confirmSave_ = on;
    if (!on) cancelSave();
    if (handle_) handle_.DirtyVariable("confirm_save");
}

// Save overwrites the stored preset. By default it does so at once, as HX Edit does; with the setting on it asks
// first, naming the slot and what's stored there.
// The unit's current preset as a library slot (setlist * 128 + preset); -1 when it isn't known or not shown.
int EditorView::deviceCurrentSlot() const {
    if (!live() || !devicePreset_ || devicePreset_->first.setlist < 0 || devicePreset_->first.preset < 0) return -1;
    return devicePreset_->first.setlist * hx::kPresetsPerSetlist + devicePreset_->first.preset;
}

void EditorView::askSave() {
    const int current = deviceCurrentSlot();
    if (current < 0) {
        setStatus(connected_ ? "Saving works on the Helix's own presets (an imported file is open)"
                             : "Connect the Helix to save to it");
        return;
    }
    askSaveTo(current / hx::kPresetsPerSetlist, current % hx::kPresetsPerSetlist);
}

void EditorView::askSaveTo(int setlist, int preset) {
    const int current = deviceCurrentSlot();
    if (current < 0 || setlist < 0 || preset < 0 || preset >= hx::kPresetsPerSetlist) return;
    saveName_ = unitPresetName(preset_.name);
    const std::size_t target = std::size_t(setlist) * hx::kPresetsPerSetlist + std::size_t(preset);
    const std::string stored =
        target < deviceNames_.size() && !deviceNames_[target].empty() ? deviceNames_[target] : "New Preset";
    const std::string where =
        (std::size_t(setlist) < setlists_.size() ? setlists_[std::size_t(setlist)].label + " " : "") + hx::presetNumber(preset);
    std::string question = "Save \"" + saveName_ + "\" to " + where + " on the Helix? It replaces the stored \"" + stored + "\".";
    if (preset_.name.size() > 16) question += " (Name shortened to the unit's 16 characters.)";
    const int to = int(target);
    const std::string name = saveName_;
    askOrDo(std::move(question), [this, current, to, name] {
        // Another preset loaded meanwhile (on the unit or here): it isn't the one the question was about.
        if (deviceCurrentSlot() != current) return setStatus("Not saved: the preset changed while asking. Save again.");
        sendLive(hx::editor::kSavePreset,
                 hx::editor::savePresetArgs(to / hx::kPresetsPerSetlist, to % hx::kPresetsPerSetlist, name));
        setStatus("Saving \"" + name + "\" to the Helix (" + hx::presetNumber(to % hx::kPresetsPerSetlist) + ")...");
    });
}

// A write to the unit's stored presets: at once, or (setting "Ask first") after the strip under the preset bar.
void EditorView::askOrDo(std::string question, std::function<void()> write) {
    if (!confirmSave_) return write();
    saveQuestion_ = std::move(question);
    pendingWrite_ = std::move(write);
    saveAsking_ = true;
    if (handle_)
        for (const char *v : {"save_asking", "save_question"}) handle_.DirtyVariable(v);
}

void EditorView::confirmSave() {
    if (!saveAsking_) return;
    const std::function<void()> write = std::move(pendingWrite_);
    cancelSave();
    if (write && connected_) write();
}

// ---------------------------------------------------------------- the whole preset to the unit (cmd 21)

// The preset in the unit's format (core/presetwriter.h). Presets not read from the unit or the firmware (an .hlx
// import) start from an empty stored "New Preset" of the firmware's library.
std::optional<std::vector<std::uint8_t>> EditorView::presetBlob(const hx::Preset &p, std::string *error) const {
    hx::PresetWriteOptions opt;
    opt.blockType = [this](const hx::PresetBlock &b) { return blockTypeOf(b); };
    if (!p.raw && fw_)
        for (const hx::Preset &lib : fw_->presets.presets)
            if (lib.raw && lib.name == "New Preset") {
                opt.base = lib.raw.get();
                break;
            }
    return hx::writePresetBlob(p, opt, error);
}

// Replaces the preset being edited on the unit with the editor's (what HX Edit does for edits without a command of
// their own). Nothing is stored until a save; the unit's notice 21 makes the session read it back.
bool EditorView::sendWholePreset(const std::string &what) {
    if (!connected_ || !onLiveEdit) return false;
    std::string error;
    const auto blob = presetBlob(preset_, &error);
    if (!blob) {
        setStatus("Not sent to the Helix (" + what + "): " + error);
        return false;
    }
    // What the blob says about routing, read back from the blob itself (the unit's answer is logged on re-read).
    if (onLog)
        if (const std::optional<hx::Preset> sent = hx::parsePresetBlob(*blob))
            onLog("cmd 21 (" + what + ") routing: " + routingText(*sent) + routingRaw(*blob));
    onLiveEdit(hx::editor::kReplacePreset, hx::editor::replacePresetArgs(*blob));
    return true;
}

// ---------------------------------------------------------------- clicks and right-click menus


void EditorView::clickCell(int id) {
    pickCell(id);
    // Empty slots, inputs / outputs and split / merge too, as HX Edit selects them; the unit's echo matches.
    if (const int slot = selectedSlotNumber(); live() && slot >= 0)
        sendLive(hx::editor::kSelectBlock, hx::editor::selectArgs(slot, 0));
}

void EditorView::clickPreset(int p) {
    // Save as: this click is the target slot, not a load.
    if (saveAsPicking_) {
        toggleSaveAs();
        return askSaveTo(setlist_, p);
    }
    if (!deviceLists()) return pickPreset(p);
    if (p < 0 || std::size_t(p) >= presets_.size()) return;
    presetSel_ = p;
    if (handle_) handle_.DirtyVariable("preset_sel");
    onLiveEdit(hx::editor::kLoadPreset, hx::editor::loadPresetArgs(setlist_, p));
    setStatus("Loading " + presets_[std::size_t(p)].label + " on the unit...");
}

// A block or an empty lane slot (inputs, outputs, split and merge have no menu yet).
void EditorView::cellMenu(Rml::Event &ev, int id) {
    const int dsp = id / 100, lane = id % 100 / 10, slot = id % 10;
    if (!menu_ || !fw_ || id < 0 || dsp > 1 || lane > 1 || slot >= hx::kLaneSlots) return;
    clickCell(id);
    const hx::PresetBlock *b = selectedBlock();
    std::vector<ContextMenu::Item> items;
    if (b) {
        items.push_back({"Copy", [this] { copyBlock(); }});
        items.push_back({"Paste here (replaces it)", [this] { pasteBlock(); }, blockClip_.has_value()});
        items.push_back({""});
        items.push_back({b->enabled ? "Turn off" : "Turn on", [this] { toggleBypass(); }});
        items.push_back({"Delete", [this] { pickCategory(kNone); }});
        items.push_back({""});
        // Its on / off on a footswitch: the next menu lists the LT's 8, the ones it's on marked; picking one moves it
        // there (or takes it off that one) and opens the Footswitches tab on it.
        const int uid = b->uid;
        items.push_back({"On / off on a footswitch...", [this, uid] {
                             std::vector<ContextMenu::Item> next;
                             for (int fs : kAssignableSwitches) {
                                 bool here = false;
                                 std::string what;
                                 for (const hx::FootswitchAssign &f : preset_.footswitches[std::size_t(fs)]) {
                                     here = here || (f.kind == 1 && f.target.block == uid);
                                     what += (what.empty() ? "  -  " : ", ") + (f.label.empty() ? std::string("?") : f.label);
                                 }
                                 ContextMenu::Item it{hx::footswitchName(fs) + what, [this, fs] {
                                                          toggleBypassSwitch(fs);
                                                          showAssignRow(-1, hx::controller::footswitch(fs));
                                                      }};
                                 it.checked = here;
                                 next.push_back(std::move(it));
                             }
                             menu_->openAgain(std::move(next), "On / off on");
                         }});
        items.push_back({"Footswitch and controllers...", [this] { pickEditTab("assign"); }});
    } else {
        items.push_back({"Paste here", [this] { pasteBlock(); }, blockClip_.has_value()});
    }
    menu_->openAt(ev, std::move(items), b ? blockLabel(*b) : "Empty slot");
}

void EditorView::resetParam(int index) {
    const EditTarget t = editTarget();
    if (!t.meta || index < 0 || std::size_t(index) >= t.meta->knobs.size()) return;
    double lo, hi, def;
    knobRange(t.meta->knobs[std::size_t(index)], lo, hi, def);
    paramChanged(index, hi > lo ? float((def - lo) / (hi - lo)) : 0.f);
    rebuildParams();
    if (handle_) handle_.DirtyVariable("params");
}

// A knob: back to its default, and which controller drives it (the current one marked).
void EditorView::paramMenu(Rml::Event &ev, int index) {
    const EditTarget t = editTarget();
    if (!menu_ || !t.meta || index < 0 || std::size_t(index) >= t.meta->knobs.size()) return;
    const hx::MetaParam &k = t.meta->knobs[std::size_t(index)];
    int current = hx::controller::None;
    for (const hx::ControllerAssign &c : preset_.controllers)
        if (sameTarget(c.target, t.target) && c.param == int(k.number) - 1) current = c.source;
    auto mark = [&](int source, const char *label) { return std::string(source == current ? "\xE2\x80\xA2 " : "   ") + label; };
    std::vector<ContextMenu::Item> items;
    items.push_back({"Back to default", [this, index] { resetParam(index); }});
    const bool assignable = t.target.block >= 0 || t.target.slot >= 0;
    items.push_back({""});
    items.push_back({mark(hx::controller::None, "No controller"), [this, index] { setController(index, hx::controller::None); },
                     assignable && current != hx::controller::None});
    // A pedal: assigned, then Controller Assign shows its range (min / max) to set.
    for (const auto &pedal : {std::pair{hx::controller::Exp1, "EXP 1"}, std::pair{hx::controller::Exp2, "EXP 2"}}) {
        const int source = pedal.first;
        items.push_back({mark(source, pedal.second),
                         [this, index, source] {
                             setController(index, source);
                             showAssignRow(index, source);
                         },
                         assignable});
    }
    // A switch: the next menu lists the 8 the LT lets presets use, with what's on each; assigned, then the
    // Footswitches tab opens on that switch (its two values, label, LED).
    const bool onSwitch = isSwitch(current);
    items.push_back({std::string(onSwitch ? "\xE2\x80\xA2 " : "   ") + "On a footswitch...",
                     [this, index] {
                         std::vector<ContextMenu::Item> next;
                         for (int fs : kAssignableSwitches) {
                             std::string what;
                             for (const hx::FootswitchAssign &f : preset_.footswitches[std::size_t(fs)])
                                 what += (what.empty() ? "  -  " : ", ") + (f.label.empty() ? std::string("?") : f.label);
                             next.push_back({hx::footswitchName(fs) + what, [this, index, fs] {
                                                 setController(index, hx::controller::footswitch(fs));
                                                 showAssignRow(index, hx::controller::footswitch(fs));
                                             }});
                         }
                         menu_->openAgain(std::move(next), "Put it on");
                     },
                     assignable});
    items.push_back({mark(hx::controller::Snapshot, "Snapshot"),
                     [this, index] { setController(index, hx::controller::Snapshot); }, assignable});
    menu_->openAt(ev, std::move(items), fw_ ? fw_->meta.displayName(k) : "");
}

// A preset row: the open one gets save / copy / paste / export; another one load, or (connected) save here.
void EditorView::presetMenu(Rml::Event &ev, int index) {
    if (!menu_ || index < 0 || std::size_t(index) >= presets_.size()) return;
    std::vector<ContextMenu::Item> items;
    const int current = deviceCurrentSlot();
    const bool open = deviceLists() ? int(slotOf(index)) == current : index == presetSel_;
    if (open) {
        if (deviceLists()) {
            items.push_back({"Save", [this] { askSave(); }, current >= 0});
            items.push_back({"Save as...", [this] { toggleSaveAs(); }, current >= 0});
            items.push_back({""});
        }
        items.push_back({"Copy preset", [this] { copyPreset(); }});
        items.push_back({"Paste preset (replaces it)", [this] { pastePreset(); }, presetClip_.has_value()});
        items.push_back({""});
        items.push_back({"Export .hlx...", [this] {
                             if (onExport) onExport();
                         }});
    } else {
        items.push_back({deviceLists() ? "Load" : "Open", [this, index] { clickPreset(index); }});
        if (deviceLists())
            items.push_back({"Save the open preset here", [this, index] { askSaveTo(setlist_, index); }, current >= 0});
    }
    // The unit's stored presets: any row, open or not.
    if (deviceLists()) {
        const int sl = setlist_;
        items.push_back({""});
        if (!open) items.push_back({"Copy preset", [this, index] { copyStored(index); }});
        items.push_back({"Paste preset into this slot", [this, index] { pasteStored(index); }, !clipBlob_.empty()});
        items.push_back({"Rename...", [this, index] { openRename(index); }});
        items.push_back({"Move up", [this, index] { movePreset(index, index - 1); }, index > 0});
        items.push_back({"Move down", [this, index] { movePreset(index, index + 1); },
                         index + 1 < int(presets_.size())});
        items.push_back({""});
        items.push_back({"Back up to a file...", [this, sl, index] {
                             if (onBackupPreset) onBackupPreset(sl, index, storedName(sl, index));
                         }});
        items.push_back({"Restore from a file...", [this, sl, index] {
                             if (onRestorePreset) onRestorePreset(sl, index);
                         }});
        items.push_back({"Back up the whole setlist...", [this, sl] {
                             if (onBackupSetlist) onBackupSetlist(sl);
                         }});
    }
    menu_->openAt(ev, std::move(items), hx::presetNumber(index) + "  " + presets_[std::size_t(index)].label);
}

// A snapshot button: recall, copy, paste onto it.
void EditorView::snapshotMenu(Rml::Event &ev, int index) {
    if (!menu_ || index < 0 || std::size_t(index) >= preset_.snapshots.size()) return;
    std::vector<ContextMenu::Item> items;
    items.push_back({"Recall", [this, index] { selectSnapshot(index); }});
    items.push_back({""});
    items.push_back({"Copy", [this, index] {
                         selectSnapshot(index);
                         copySnapshot();
                     }});
    items.push_back({"Paste here", [this, index] {
                         selectSnapshot(index);
                         pasteSnapshot();
                     },
                     snapClip_ >= 0 && snapClip_ != index});
    // Its colour, without recalling it: Auto and the 10 colours, as the snapshot bar's colour row.
    ContextMenu::Item colours;
    colours.label = "Colour";
    for (int c = 0; c <= 10; ++c)
        colours.swatches.push_back({"snapc-" + std::to_string(c), [this, index, c] { setSnapshotColourAt(index, c); },
                                    preset_.snapshots[std::size_t(index)].colour == c});
    items.push_back({""});
    items.push_back(std::move(colours));
    const std::string label = index < int(snapshots_.size()) ? snapshots_[std::size_t(index)].label : "Snapshot";
    menu_->openAt(ev, std::move(items), label);
}

// Save as: the next click in the preset list picks the slot (EditorView's pick_preset); again or Esc cancels.
void EditorView::toggleSaveAs() {
    if (!saveAsPicking_ && deviceCurrentSlot() < 0) return askSave();  // says why it can't
    saveAsPicking_ = !saveAsPicking_;
    if (saveAsPicking_) cancelSave();
    if (handle_) handle_.DirtyVariable("save_as_picking");
}

void EditorView::cancelPicking() {
    if (saveAsPicking_) toggleSaveAs();
    cancelSave();
}

void EditorView::cancelSave() {
    saveAsking_ = false;
    saveQuestion_.clear();
    pendingWrite_ = nullptr;
    if (handle_)
        for (const char *v : {"save_asking", "save_question"}) handle_.DirtyVariable(v);
}

void EditorView::renamePreset(const std::string &name) {
    if (preset_.name == name) return;
    beginEdit("presetname");
    // Connected, the name goes to the unit with the next save (cmd 71 carries it).
    if (live()) setStatus("The new name goes to the Helix when you save (Ctrl+S)");
    preset_.name = name;
    setRowLabel(name.empty() ? "New Preset" : name);
    markEdited();
    if (handle_) handle_.DirtyVariable("presets");  // not preset_name: that would reset the field while typing
}

void EditorView::movePreset(int from, int to) {
    const int n = int(presets_.size());
    if (from < 0 || to < 0 || from >= n || to >= n || from == to) return;
    // Connected: the unit's setlist in its new order (lib 2); the list comes back from the unit when it's done.
    if (deviceLists()) {
        std::vector<std::pair<int, std::string>> order;
        for (int i = 0; i < hx::kPresetsPerSetlist; ++i) {
            const std::size_t slot = std::size_t(setlist_) * hx::kPresetsPerSetlist + std::size_t(i);
            order.emplace_back(int(slot), slot < deviceNames_.size() ? deviceNames_[slot] : "");
        }
        if (from < to) std::rotate(order.begin() + from, order.begin() + from + 1, order.begin() + to + 1);
        else std::rotate(order.begin() + to, order.begin() + from, order.begin() + from + 1);
        if (onLibraryWrite) onLibraryWrite(hx::editor::kReorderSetlist, hx::editor::reorderSetlistArgs(setlist_, order));
        setStatus("Moving " + storedName(setlist_, from) + " to " + hx::presetNumber(to) + "...");
        return;
    }
    storeWorking();
    auto &lib = library_.presets;
    const auto first = lib.begin() + std::ptrdiff_t(slotOf(0));
    const auto a = first + from, b = first + to;
    if (from < to) std::rotate(a, a + 1, b + 1);
    else std::rotate(b, a, a + 1);
    std::vector<bool>::iterator ef = edited_.begin() + std::ptrdiff_t(slotOf(0));
    std::vector<bool> e(ef, ef + n);
    if (from < to) std::rotate(e.begin() + from, e.begin() + from + 1, e.begin() + to + 1);
    else std::rotate(e.begin() + to, e.begin() + from, e.begin() + from + 1);
    // Every slot between changed number: they count as edited.
    for (int i = std::min(from, to); i <= std::max(from, to); ++i) e[std::size_t(i)] = true;
    std::copy(e.begin(), e.end(), ef);
    // The selection follows the preset it was on.
    if (presetSel_ == from) presetSel_ = to;
    else if (presetSel_ > from && presetSel_ <= to) --presetSel_;
    else if (presetSel_ < from && presetSel_ >= to) ++presetSel_;
    rebuildPresetList();
    if (presetSel_ >= 0 && onPresetOpened) onPresetOpened(setlist_, presetSel_);
    if (handle_)
        for (const char *v : {"presets", "preset_sel"}) handle_.DirtyVariable(v);
}

// ---------------------------------------------------------------- stored presets on the unit (library channel)

std::string EditorView::storedName(int setlist, int preset) const {
    const std::size_t slot = std::size_t(setlist) * hx::kPresetsPerSetlist + std::size_t(preset);
    return slot < deviceNames_.size() && !deviceNames_[slot].empty() ? deviceNames_[slot] : "New Preset";
}

std::string EditorView::slotLabel(int setlist, int preset) const {
    return (std::size_t(setlist) < setlists_.size() ? setlists_[std::size_t(setlist)].label + " " : "") +
           hx::presetNumber(preset);
}

// Copy from any row: the open preset as the unit has it now (cmd 22), another one as stored (lib 4).
void EditorView::copyStored(int index) {
    if (!deviceLists() || index < 0) return;
    if (int(slotOf(index)) == deviceCurrentSlot()) return copyPreset();
    // The name isn't in the preset's bytes (the unit keeps names in its list): remember it for the paste.
    pendingCopyName_ = storedName(setlist_, index);
    if (onLibraryRead) onLibraryRead(hx::editor::kReadPreset, hx::editor::readPresetArgs(setlist_, index), "copy");
    setStatus("Copying " + pendingCopyName_ + "...");
}

// A stored preset read for Copy: becomes the clipboard (its bytes for pasting on the unit, its parse for offline use).
void EditorView::applyStoredCopy(const std::vector<std::uint8_t> &blob) {
    std::string error;
    std::optional<hx::Preset> p = hx::parsePresetBlob(blob, &error);
    if (!p) return setStatus("Could not read the copied preset: " + error);
    presetClip_ = std::move(*p);
    presetClip_->name = pendingCopyName_;
    clipBlob_ = blob;
    hasClip_ = true;
    if (handle_) handle_.DirtyVariable("has_clip");
    setStatus("Copied " + (presetClip_->name.empty() ? std::string("preset") : presetClip_->name));
}

// A whole preset into a stored slot (lib 8): the clipboard (Paste here) or a backup file (Restore). The bytes don't
// hold the name, so it comes with them (lib 8's 109 names the slot).
void EditorView::writeStored(int setlist, int index, const std::vector<std::uint8_t> &blob, const std::string &rawName,
                             const std::string &what) {
    std::string error;
    if (!hx::parsePresetBlob(blob, &error)) return setStatus("Not a preset the Helix can take: " + error);
    const std::string name = unitPresetName(rawName);
    askOrDo(what + " \"" + name + "\" into " + slotLabel(setlist, index) + " on the Helix? It replaces the stored \"" +
                storedName(setlist, index) + "\".",
            [this, setlist, index, name, blob] {
                if (onLibraryWrite) onLibraryWrite(hx::editor::kWritePreset, hx::editor::writePresetArgs(setlist, index, name, blob));
                setStatus("Writing \"" + name + "\" to " + hx::presetNumber(index) + "...");
            });
}

void EditorView::pasteStored(int index) {
    if (!deviceLists() || index < 0) return;
    if (clipBlob_.empty()) return setStatus("Copy a preset while the Helix is connected first");
    writeStored(setlist_, index, clipBlob_, presetClip_ ? presetClip_->name : "", "Paste");
}

void EditorView::restoreStored(int setlist, int index, const std::vector<std::uint8_t> &blob, const std::string &name) {
    if (!deviceLists()) return setStatus("Connect the Helix to restore to it");
    writeStored(setlist, index, blob, name, "Restore");
}

// Rename a stored preset without loading it (lib 6): its row in the list turns into a text field (update() gives
// it the keyboard focus with the name selected).
void EditorView::openRename(int index) {
    if (!deviceLists() || index < 0) return;
    if (renaming_) confirmRename();  // another row was open
    renameIndex_ = index;
    renameSetlist_ = setlist_;
    renameText_ = storedName(setlist_, index);
    renaming_ = true;
    focusRename_ = true;
    if (handle_)
        for (const char *v : {"renaming", "rename_text", "rename_index"}) handle_.DirtyVariable(v);
}

// The IR popup opened: scroll its list so the current slot is in the middle. The popup and its rows appear a frame or
// two after the model changed (data-if / data-for), so this retries for a few frames.
void EditorView::scrollIrList() {
    Rml::Context *ctx = RmlUI::GetContext();
    Rml::ElementList popups;
    for (int i = 0; ctx && i < ctx->GetNumDocuments(); ++i) ctx->GetDocument(i)->QuerySelectorAll(popups, ".ir-popup");
    for (Rml::Element *p : popups) {
        if (!p->IsVisible(true) || p->GetClientHeight() <= 0) continue;
        if (Rml::Element *on = p->QuerySelector(".ir-item.on"); on && on->GetOffsetHeight() > 0) {
            p->SetScrollTop(std::max(0.f, on->GetOffsetTop() - (p->GetClientHeight() - on->GetOffsetHeight()) / 2));
            irScrollTries_ = 0;
            return;
        }
    }
    if (--irScrollTries_ <= 0) irScrollTries_ = 0;  // not there (no current slot): give up
}

void EditorView::update() {
    if (irScrollTries_ > 0) scrollIrList();
    if (!focusRename_) return;
    // The field sits in a data-for row, and RmlUi keeps that row's template in the document (hidden), with the same
    // id: GetElementById found the template, so the keyboard went to an invisible field (no caret, no selection
    // shown; the visible copy only followed through the data binding). Take the copy that is shown.
    Rml::Context *ctx = RmlUI::GetContext();
    Rml::ElementList found;
    for (int i = 0; ctx && i < ctx->GetNumDocuments(); ++i) ctx->GetDocument(i)->QuerySelectorAll(found, "#rename-edit");
    for (Rml::Element *e : found)
        if (e->IsVisible(true)) {
            auto *input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(e);
            // The data model fills the value a frame after the field appears: until it holds the name, a cursor
            // "at the end" would land at the start. Keep trying until then.
            if (!input || input->GetValue() != renameText_) return;
            e->Focus(true);
            const int end = int(renameText_.size());  // preset names are ASCII: characters = bytes
            input->SetSelectionRange(end, end);
            focusRename_ = false;
            return;
        }
}

void EditorView::confirmRename() {
    if (!renaming_) return;
    renaming_ = false;
    focusRename_ = false;
    if (handle_) handle_.DirtyVariable("renaming");
    const std::string name = unitPresetName(renameText_);
    const int setlist = renameSetlist_, index = renameIndex_;
    if (!deviceLists() || index < 0) return;
    if (name == storedName(setlist, index)) return;  // unchanged: nothing to write
    if (onLibraryWrite) onLibraryWrite(hx::editor::kRenamePreset, hx::editor::renamePresetArgs(setlist, index, name));
    // The open one: its name in the editor too, or the next Save would write the old one back.
    if (setlist * hx::kPresetsPerSetlist + index == deviceCurrentSlot()) {
        preset_.name = name;
        presetName_ = name;
        if (handle_) handle_.DirtyVariable("preset_name");
    }
    setStatus("Renaming to \"" + name + "\"...");
}

void EditorView::cancelRename() {
    renaming_ = false;  // the field's blur that follows finds nothing to apply
    focusRename_ = false;
    if (handle_) handle_.DirtyVariable("renaming");
}

void EditorView::copyPreset() {
    presetClip_ = preset_;
    hasClip_ = true;
    // Connected: also the unit's own bytes for it (they arrive in setClipBlob), so a paste there loses nothing.
    clipBlob_.clear();
    if (live()) sendLive(hx::editor::kCurrentPreset, hx::mp::nil());
    if (handle_) handle_.DirtyVariable("has_clip");
    setStatus("Copied " + (preset_.name.empty() ? std::string("preset") : preset_.name));
}

void EditorView::pastePreset() {
    if (!presetClip_ || presetSel_ < 0) return;
    beginEdit();
    // On the unit it replaces the preset being edited (as HX Edit's paste does before a save); nothing is stored.
    if (live()) {
        if (!clipBlob_.empty()) sendLive(hx::editor::kReplacePreset, hx::editor::replacePresetArgs(clipBlob_));
        else notSent("a preset copied while not connected");
    }
    preset_ = *presetClip_;
    presetName_ = preset_.name.empty() ? "New Preset" : preset_.name;
    setRowLabel(presetName_);
    markEdited();
    showPreset();
}

void EditorView::setClipBlob(std::vector<std::uint8_t> blob) { clipBlob_ = std::move(blob); }

void EditorView::copyBlock() {
    const hx::PresetBlock *b = selectedBlock();
    if (!b) return;
    blockClip_ = *b;
    setStatus("Copied " + blockLabel(*b));
    rebuildParams();
    if (handle_) handle_.DirtyVariable("can_paste_block");
}

// A copy with its own uid: the original's controllers and footswitches stay with the original. Live: the model goes
// in (the unit sets its default knobs), then every knob, the cab's knobs and the on / off state, as separate edits.
void EditorView::pasteBlock() {
    std::optional<hx::PresetBlock> *s = selectedSlot();
    if (!blockClip_ || !s || sel_.special != Special::None) return;
    beginEdit();
    hx::PresetBlock b = *blockClip_;
    b.uid = preset_.newUid();
    *s = std::move(b);
    hx::normalizeRouting(preset_.paths[std::size_t(sel_.dsp)]);
    preset_.pruneAssignments();  // the replaced block's assignments go with it
    if (undoIfDspFull(sel_.dsp)) return;
    sendBlockAt(selectedSlotNumber(), **s);
    setStatus("Pasted " + blockLabel(**s));
    rebuildLanes();
    rebuildParams();
    rebuildDsp();
    refreshEditor();
}

std::optional<hx::Preset> EditorView::importHlx(std::string_view text, std::string *error,
                                                std::vector<std::string> *warnings) const {
    if (!fw_) {
        if (error) *error = "no firmware open";
        return std::nullopt;
    }
    return hx::importHlx(text, *fw_, error, warnings);
}

std::string EditorView::exportHlx(std::vector<std::string> *warnings) const {
    return fw_ ? hx::exportHlx(preset_, *fw_, warnings) : std::string();
}

void EditorView::openPreset(hx::Preset preset, std::string title) {
    if (!fw_) return;
    storeWorking();   // keep the edits of the library preset that was open
    presetSel_ = -1;  // not one of the firmware's slots
    showingDevice_ = false;  // a file: its edits stay in the editor
    setlistOpen_ = subOpen_ = false;
    preset_ = std::move(preset);
    // The name field edits the preset's name; the title (with the file name) only when it has none.
    presetName_ = preset_.name.empty() ? std::move(title) : preset_.name;
    clearHistory();
    showPreset();
}

void EditorView::showPreset() {
    // Select the first block, as the device does.
    sel_ = {};
    for (int dsp = 0; dsp < 2 && !sel_.valid(); ++dsp)
        for (int lane = 0; lane < 2 && !sel_.valid(); ++lane)
            for (int s = 0; s < hx::kLaneSlots; ++s) {
                const hx::PresetPath &p = preset_.paths[std::size_t(dsp)];
                if ((lane ? p.b : p.a).slots[std::size_t(s)]) {
                    sel_ = {dsp, lane, s};
                    break;
                }
            }
    snapClip_ = -1;  // a copied snapshot belongs to its preset
    fsSel_ = -1;
    rebuildSnapshots();
    pickCell(sel_.valid() ? sel_.dsp * 100 + sel_.lane * 10 + sel_.slot : -1);
    refresh();  // a new preset changes the lists and names too, not only the flow
}

// ---------------------------------------------------------------- snapshots

void EditorView::rebuildSnapshots() {
    snapshots_.clear();
    for (std::size_t i = 0; i < preset_.snapshots.size(); ++i) {
        const hx::PresetSnapshot &s = preset_.snapshots[i];
        snapshots_.push_back({int(i), s.name.empty() ? "Snapshot " + std::to_string(i + 1) : s.name, "",
                              "snap-dot snapc-" + std::to_string(std::clamp(s.colour, 0, 10))});
    }
    hasSnaps_ = !snapshots_.empty();
    snapSel_ = preset_.snapshot;
    const hx::PresetSnapshot *cur =
        std::size_t(snapSel_) < preset_.snapshots.size() ? &preset_.snapshots[std::size_t(snapSel_)] : nullptr;
    snapName_ = cur ? cur->name : "";
    snapTempo_ = cur ? formatNum("%.1f", cur->tempo) : "";
    snapColour_ = cur ? cur->colour : 0;
    if (snapColours_.empty())
        for (int c = 0; c <= 10; ++c) snapColours_.push_back({c, "", "", "snap-dot snapc-" + std::to_string(c)});
    if (handle_)
        for (const char *v : {"snapshots", "snap_sel", "snap_name", "snap_tempo", "snap_colour", "snap_colours",
                              "snap_clip", "has_snaps"})
            handle_.DirtyVariable(v);
}

void EditorView::setSnapshotColour(int colour) { setSnapshotColourAt(preset_.snapshot, colour); }

// Any snapshot's colour, not only the one in use (cmd 98 names the snapshot): the snapshot buttons' right-click menu.
void EditorView::setSnapshotColourAt(int snapshot, int colour) {
    const std::size_t i = std::size_t(snapshot);
    if (snapshot < 0 || i >= preset_.snapshots.size() || colour < 0 || colour > 10 || preset_.snapshots[i].colour == colour)
        return;
    beginEdit();
    if (live()) sendLive(hx::editor::kSnapshotColour, hx::editor::snapshotColourArgs(int(i), colour));
    preset_.snapshots[i].colour = colour;
    rebuildSnapshots();
}

// The unit's tempo range: 30..240 BPM. Text that isn't a number puts the old value back.
void EditorView::setSnapshotTempo(const std::string &text) {
    const std::size_t i = std::size_t(preset_.snapshot);
    if (i >= preset_.snapshots.size()) return;
    char *end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end != text.c_str()) {
        const double tempo = std::clamp(std::round(v * 10) / 10, 30.0, 240.0);
        if (tempo != preset_.snapshots[i].tempo) {
            beginEdit();
            // The unit's tempo setting (id 16) is the snapshot in use's.
            if (live()) sendLive(hx::editor::kSetSetting, hx::editor::settingArgs(hx::globalid::Tempo,hx::mp::real(tempo)));
            preset_.snapshots[i].tempo = tempo;
        }
    }
    rebuildSnapshots();
}

void EditorView::copySnapshot() {
    snapClip_ = preset_.snapshot;
    if (live()) sendLive(hx::editor::kCopySnapshot, hx::mp::map());  // the unit copies the one in use
    if (handle_) handle_.DirtyVariable("snap_clip");
    setStatus("Copied " + (std::size_t(snapClip_) < snapshots_.size() ? snapshots_[std::size_t(snapClip_)].label : "snapshot"));
}

void EditorView::pasteSnapshot() {
    if (snapClip_ < 0 || std::size_t(snapClip_) >= preset_.snapshots.size() || snapClip_ == preset_.snapshot) return;
    beginEdit();
    if (live()) sendLive(hx::editor::kPasteSnapshot, hx::mp::map());  // onto the one in use
    preset_.copySnapshot(snapClip_, preset_.snapshot);
    rebuildSnapshots();
    rebuildLanes();
    rebuildParams();
    refreshEditor();
}

// A click on the snapshot bar or Ctrl+1..8: recall it here and, connected, on the unit.
void EditorView::selectSnapshot(int index) {
    if (index < 0 || std::size_t(index) >= preset_.snapshots.size()) return;
    pickSnapshot(index);
    if (live()) sendLive(hx::editor::kSelectSnapshot, hx::editor::snapshotArgs(index));
}

void EditorView::pickSnapshot(int index) {
    if (index < 0 || std::size_t(index) >= preset_.snapshots.size()) return;
    preset_.recallSnapshot(index);
    rebuildSnapshots();
    rebuildLanes();
    rebuildParams();
    refreshEditor();
}

void EditorView::renameSnapshot(const std::string &name) {
    const std::size_t i = std::size_t(preset_.snapshot);
    if (i >= preset_.snapshots.size() || preset_.snapshots[i].name == name) return;
    beginEdit("snapname");
    if (live()) sendLive(hx::editor::kRenameSnapshot, hx::editor::snapshotNameArgs(int(i), name));
    preset_.snapshots[i].name = name;
    snapName_ = name;
    if (i < snapshots_.size()) snapshots_[i].label = name.empty() ? "Snapshot " + std::to_string(i + 1) : name;
    if (handle_) handle_.DirtyVariable("snapshots");  // not snap_name: that would reset the field while typing
}

// ---------------------------------------------------------------- controller assign, footswitches

void EditorView::pickEditTab(const std::string &tab) {
    if (tab != "edit" && tab != "assign" && tab != "commands") return;
    editTab_ = tab;
    setlistOpen_ = subOpen_ = false;
    refreshEditor();
}

hx::PresetBlock EditorView::newBlock(const hx::ModelRef &m) {
    hx::PresetBlock b = defaultBlock(m);
    b.uid = preset_.newUid();
    return b;
}

int EditorView::cellOf(int uid) const {
    hx::Preset::Place at;
    if (!preset_.findBlock(uid, &at)) return -1;
    return at.dsp * 100 + at.lane * 10 + at.slot;
}

std::string EditorView::blockLabel(const hx::PresetBlock &b) const {
    const hx::ModelRef *m = fw_ ? fw_->model(b.modelNo) : nullptr;
    return m ? m->name : "Model #" + std::to_string(b.modelNo);
}

// One controller per knob: the old assignment goes (with a footswitch function driving it), then the new one
// spans the knob's whole range. Snapshot starts every snapshot at the current value.
void EditorView::setController(int knobIndex, int source) {
    const EditTarget t = editTarget();
    if (!t.meta || !t.values || knobIndex < 0 || std::size_t(knobIndex) >= t.meta->knobs.size()) return;
    if (t.target.block < 0 && t.target.slot < 0) return;
    const hx::MetaParam &k = t.meta->knobs[std::size_t(knobIndex)];
    if (k.number == 0) return;
    const int param = int(k.number) - 1;
    beginEdit();
    const int slot = selectedSlotNumber(), set = editCab_ && t.block ? 1 : 0;
    // On the wire the source is the preset's number (hx-edit-ops-2); 0 clears.
    const bool known = source == hx::controller::None || source == hx::controller::Exp1 ||
                       source == hx::controller::Exp2 || source == hx::controller::Snapshot ||
                       source == hx::controller::VariaxVolume || source == hx::controller::VariaxTone ||
                       source == hx::controller::MidiCc || isSwitch(source);
    if (known && slot >= 0) {
        if (live()) sendLive(hx::editor::kAssignController, hx::editor::controllerArgs(slot, set, param, source));
    } else {
        notSent("this controller assignment");
    }
    auto mine =[&](const hx::PresetTarget &x, int p) { return sameTarget(x, t.target) && p == param; };
    std::erase_if(preset_.controllers, [&](const hx::ControllerAssign &c) { return mine(c.target, c.param); });
    for (auto &list : preset_.footswitches)
        std::erase_if(list, [&](const hx::FootswitchAssign &f) { return f.kind == 2 && mine(f.target, f.param); });
    if (source != hx::controller::None) {
        const int id = preset_.freeControllerId();
        if (id < 0) {
            setStatus("All 64 controller assignments of this preset are in use");
        } else {
            double lo, hi, def;
            knobRange(k, lo, hi, def);
            using K = hx::PresetValue::Kind;
            const K kind = k.type == std::uint32_t(hx::ParamType::Float)    ? K::Float
                           : k.type == std::uint32_t(hx::ParamType::Switch) ? K::Bool
                                                                              : K::Int;
            hx::ControllerAssign c;
            c.id = id;
            c.source = source;
            c.target = t.target;
            c.param = param;
            c.min = {kind, lo};
            c.max = {kind, hi};
            c.snapValues.fill({kind, knobValue(k, t.values)});
            preset_.controllers.push_back(std::move(c));
            // A switch also lists the knob among its functions (kind 2), labelled with the knob's name.
            if (isSwitch(source)) {
                hx::FootswitchAssign f;
                f.kind = 2;
                f.label = fw_->meta.displayName(k);
                f.target = t.target;
                f.param = param;
                preset_.footswitches[std::size_t(source - hx::controller::footswitch(0))].push_back(std::move(f));
            }
        }
    }
    refreshEditor();
}

void EditorView::setControllerRange(int knobIndex, bool max, float pos) {
    const EditTarget t = editTarget();
    if (!t.meta || knobIndex < 0 || std::size_t(knobIndex) >= t.meta->knobs.size()) return;
    const hx::MetaParam &k = t.meta->knobs[std::size_t(knobIndex)];
    if (k.number == 0) return;
    for (hx::ControllerAssign &c : preset_.controllers) {
        if (!sameTarget(c.target, t.target) || c.param != int(k.number) - 1) continue;
        double lo, hi, def;
        knobRange(k, lo, hi, def);
        double v = lo + double(std::clamp(pos, 0.f, 1.f)) * (hi - lo);
        if (k.type != std::uint32_t(hx::ParamType::Float)) v = std::round(v);
        hx::PresetValue &stored = max ? c.max : c.min;
        if (stored.kind != hx::PresetValue::Kind::None && std::abs(stored.v - v) < 1e-9) return;  // the slider echoing
        beginEdit("range:" + std::to_string(knobIndex) + (max ? "max" : "min"));
        stored.v = v;
        if (stored.kind == hx::PresetValue::Kind::None) stored.kind = (max ? c.min : c.max).kind;
        if (const int slot = selectedSlotNumber(); live() && slot >= 0)
            sendLive(max ? hx::editor::kControllerMax : hx::editor::kControllerMin,
                     hx::editor::knobArgs(slot, editCab_ && t.block ? 1 : 0, c.param, toMp(stored)));
        // Only the value text changes: rebuilding would reset the slider being dragged.
        (max ? caMaxPos_ : caMinPos_) = float((v - lo) / (hi > lo ? hi - lo : 1));
        (max ? caMaxText_ : caMinText_) = displayValue(fw_->meta, k, v);
        if (handle_) handle_.DirtyVariable(max ? "ca_max_text" : "ca_min_text");
        return;
    }
}

// The CC number of a knob's MIDI CC controller (cmd 64, as HX Edit sends it per step).
void EditorView::setControllerCc(int knobIndex, const std::string &text) {
    const EditTarget t = editTarget();
    if (!t.meta || knobIndex < 0 || std::size_t(knobIndex) >= t.meta->knobs.size()) return;
    const hx::MetaParam &k = t.meta->knobs[std::size_t(knobIndex)];
    if (k.number == 0) return;
    char *end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    for (hx::ControllerAssign &c : preset_.controllers) {
        if (!sameTarget(c.target, t.target) || c.param != int(k.number) - 1 || c.source != hx::controller::MidiCc) continue;
        if (end == text.c_str() || n < 0 || n > 127 || n == c.cc) break;  // not a CC number, or unchanged: the field resets
        beginEdit();
        c.cc = int(n);
        if (const int slot = selectedSlotNumber(); live() && slot >= 0)
            sendLive(hx::editor::kControllerCc,
                     hx::editor::controllerCcArgs(slot, editCab_ && t.block ? 1 : 0, c.param, c.cc));
        break;
    }
    refreshEditor();
}

// A block's bypass sits on one switch: picking a switch moves it there, picking its current one removes it.
void EditorView::toggleBypassSwitch(int fsIndex) {
    const EditTarget t = editTarget();
    if (!t.block || fsIndex < 0 || fsIndex >= hx::kToeSwitch) return;  // not the toe switch: it picks EXP 1 / 2
    beginEdit();
    const int uid = t.block->uid;
    bool wasHere = false;
    for (std::size_t fs = 0; fs < preset_.footswitches.size(); ++fs) {
        auto &list = preset_.footswitches[fs];
        const std::size_t before = list.size();
        std::erase_if(list, [&](const hx::FootswitchAssign &f) { return f.kind == 1 && f.target.block == uid; });
        if (int(fs) == fsIndex && list.size() != before) wasHere = true;
    }
    // Switch numbers = our FS index, for assigning (56) and removing (57).
    if (const int slot = selectedSlotNumber(); live() && slot >= 0)
        sendLive(wasHere ? hx::editor::kRemoveBypassSwitch : hx::editor::kAssignBypassSwitch,
                 hx::editor::bypassSwitchArgs(slot, fsIndex));
    if (!wasHere) {
        hx::FootswitchAssign f;
        f.kind = 1;
        f.label = blockLabel(*t.block);
        f.target = t.target;
        preset_.footswitches[std::size_t(fsIndex)].push_back(std::move(f));
    }
    refreshEditor();
}

// ---------------------------------------------------------------- bypass / controller assign (HX Edit's layout)

std::string EditorView::caSourceName(int source) const {
    if (source == hx::controller::None) return "None";
    if (source == hx::controller::Exp1) return "EXP Pedal 1";
    if (source == hx::controller::Exp2) return "EXP Pedal 2";
    if (source == hx::controller::VariaxVolume) return "Variax Volume";
    if (source == hx::controller::VariaxTone) return "Variax Tone";
    if (source == hx::controller::MidiCc) return "MIDI CC";
    if (source == hx::controller::Snapshot) return "Snapshots";
    if (isSwitch(source)) return "Footswitch " + std::to_string(source - hx::controller::footswitch(0) + 1);
    return hx::controllerName(source);
}

std::string EditorView::caBlockName(const hx::PresetTarget &t, int *category) {
    if (category) *category = -1;
    if (const hx::PresetBlock *b = t.block >= 0 ? preset_.findBlock(t.block) : nullptr) {
        if (category)
            if (const hx::ModelRef *m = fw_ ? fw_->model(b->modelNo) : nullptr) *category = m->category;
        return blockLabel(*b);
    }
    if (t.slot >= 0) return t.slot % hx::kSlotsPerPath == 10 ? "Split" : "Merge";
    return "?";
}

std::string EditorView::caParamName(const hx::PresetTarget &t, int param) {
    if (param < 0) return "Bypass";
    const hx::PresetBlock *b = t.block >= 0 ? preset_.findBlock(t.block) : nullptr;
    if (const hx::ModelRef *m = b && fw_ ? fw_->model(b->modelNo) : nullptr; m && m->meta)
        for (const hx::MetaParam &k : m->meta->knobs)
            if (int(k.number) - 1 == param) return fw_->meta.displayName(k);
    if (sameTarget(t, editTarget().target))  // a split / merge: the selected one's knobs
        if (const EditTarget e = editTarget(); e.meta)
            for (const hx::MetaParam &k : e.meta->knobs)
                if (int(k.number) - 1 == param) return fw_->meta.displayName(k);
    return "Knob " + std::to_string(param + 1);
}

int EditorView::caKnobIndex() {
    const EditTarget t = editTarget();
    if (!caSel_ || caSel_->param < 0 || !t.meta) return -1;
    for (std::size_t i = 0; i < t.meta->knobs.size(); ++i)
        if (int(t.meta->knobs[i].number) - 1 == caSel_->param) return int(i);
    return -1;
}

// Every assignment in the preset as a row (a knob's controller; a block's bypass on a switch), sorted by the chosen
// column; then the editor follows the selected block (its first row, or a new one to set up).
void EditorView::rebuildAssignTable() {
    caRows_.clear();
    for (const hx::ControllerAssign &c : preset_.controllers)
        if (c.source != hx::controller::None) caRows_.push_back({c.target, c.param, c.source});
    for (int fs = 0; fs < hx::kFootswitchSlots; ++fs)
        for (const hx::FootswitchAssign &f : preset_.footswitches[std::size_t(fs)])
            if (f.kind == 1) caRows_.push_back({f.target, -1, hx::controller::footswitch(fs)});
    caTable_.clear();
    for (std::size_t i = 0; i < caRows_.size(); ++i) {
        const CaRow &r = caRows_[i];
        UiAssignRow u;
        u.index = int(i);
        int cat = -1;
        u.block = caBlockName(r.target, &cat);
        u.cls = catClass(cat);
        u.param = caParamName(r.target, r.param);
        u.source = caSourceName(r.source);
        caTable_.push_back(std::move(u));
    }
    std::stable_sort(caTable_.begin(), caTable_.end(), [this](const UiAssignRow &a, const UiAssignRow &b) {
        const std::string &x = caSortCol_ == 0 ? a.block : caSortCol_ == 1 ? a.param : a.source;
        const std::string &y = caSortCol_ == 0 ? b.block : caSortCol_ == 1 ? b.param : b.source;
        return caSortAsc_ ? x < y : y < x;
    });

    // The editor follows the block selected in the signal flow.
    const EditTarget t = editTarget();
    const bool hasTarget = t.meta && (t.target.block >= 0 || t.target.slot >= 0);
    if (!hasTarget) {
        caSel_.reset();
    } else if (!caSel_ || !sameTarget(caSel_->target, t.target)) {
        caSel_.reset();
        for (const CaRow &r : caRows_)
            if (sameTarget(r.target, t.target)) {
                caSel_ = r;
                break;
            }
        if (!caSel_) caSel_ = CaRow{t.target, t.block ? -1 : 0, hx::controller::None};  // a new row: pick a controller
    } else {
        // The row may have changed (assigned elsewhere, removed): take its current source.
        int source = hx::controller::None;
        for (const CaRow &r : caRows_)
            if (sameTarget(r.target, caSel_->target) && r.param == caSel_->param) source = r.source;
        caSel_->source = source;
    }
    for (UiAssignRow &u : caTable_) {
        const CaRow &r = caRows_[std::size_t(u.index)];
        u.selected = caSel_ && sameTarget(r.target, caSel_->target) && r.param == caSel_->param;
    }

    caHasSel_ = caSel_.has_value();
    caCanBypass_ = t.block != nullptr;
    caKind_.clear();
    if (!caSel_) return;
    int cat = -1;
    caBlock_ = caBlockName(caSel_->target, &cat);
    caBlockCls_ = catClass(cat);
    caParam_ = caParamName(caSel_->target, caSel_->param);
    caSource_ = caSourceName(caSel_->source);
    const int src = caSel_->source;
    if (src == hx::controller::None || src == hx::controller::Snapshot) caKind_ = "";
    else if (caSel_->param < 0) caKind_ = "bypass";
    else if (isSwitch(src)) caKind_ = "switch";
    else if (src == hx::controller::MidiCc) caKind_ = "midi";
    else caKind_ = "range";
    caMinName_ = "Min " + caParam_;
    caMaxName_ = "Max " + caParam_;
    // The range, in the knob's own units.
    if (const int k = caKnobIndex(); k >= 0 && caSel_->param >= 0)
        for (const hx::ControllerAssign &c : preset_.controllers) {
            if (!sameTarget(c.target, caSel_->target) || c.param != caSel_->param) continue;
            const hx::MetaParam &m = t.meta->knobs[std::size_t(k)];
            double lo, hi, def;
            knobRange(m, lo, hi, def);
            auto pos = [&](double v) { return hi > lo ? float(std::clamp((v - lo) / (hi - lo), 0.0, 1.0)) : 0.f; };
            caMinPos_ = pos(c.min.v);
            caMaxPos_ = pos(c.max.v);
            caMinText_ = displayValue(fw_->meta, m, c.min.v);
            caMaxText_ = displayValue(fw_->meta, m, c.max.v);
            caCc_ = std::to_string(c.cc);
        }
    // A switch: its type, LED and label (they belong to the switch; its first function holds them).
    if (isSwitch(src)) {
        const auto &list = preset_.footswitches[std::size_t(src - hx::controller::footswitch(0))];
        const hx::FootswitchAssign *f = list.empty() ? nullptr : &list.front();
        caMomentary_ = f && f->momentary;
        caLabel_ = f ? f->label : "";
        const int led = f ? f->ledColour : 0;
        caLedName_ = led >= 0 && std::size_t(led) < fsColours_.size() ? fsColours_[std::size_t(led)].label : "Auto Color";
    }
}

// After a right-click assignment: Bypass / Controller Assign with that row selected (knobIndex: in the edit target's
// rec 4 list; -1 = the bypass), so its range, label and LED can be set there.
void EditorView::showAssignRow(int knobIndex, int source) {
    const EditTarget t = editTarget();
    int param = -1;
    if (knobIndex >= 0 && t.meta && std::size_t(knobIndex) < t.meta->knobs.size())
        param = int(t.meta->knobs[std::size_t(knobIndex)].number) - 1;
    caSel_ = CaRow{t.target, param, source};
    pickEditTab("assign");
}

void EditorView::pickAssignRow(int index) {
    if (index < 0 || std::size_t(index) >= caRows_.size()) return;
    const CaRow r = caRows_[std::size_t(index)];
    caSel_ = r;
    if (const int cell = cellOfTarget(r.target); cell >= 0) pickCell(cell);  // its block, as HX Edit selects it
    else refreshEditor();
}

// The selected row onto another controller (None takes it off). A bypass only goes on footswitches.
void EditorView::setAssignSource(int source) {
    if (!caSel_ || source == caSel_->source) return;
    const CaRow r = *caSel_;
    if (r.param < 0) {
        if (source == hx::controller::None) {
            if (isSwitch(r.source)) toggleBypassSwitch(r.source - hx::controller::footswitch(0));  // it's there: off
        } else if (isSwitch(source)) {
            toggleBypassSwitch(source - hx::controller::footswitch(0));  // moves it there
        } else {
            setStatus("A block's bypass can only go on a footswitch");
            return;
        }
    } else if (const int k = caKnobIndex(); k >= 0) {
        setController(k, source);
    } else {
        return;
    }
    caSel_->source = source;
    refreshEditor();
}

// The selected row onto another parameter of the same block: unassigned there, assigned here with the same
// controller (a new row only changes what it will assign).
void EditorView::setAssignParam(int param) {
    if (!caSel_ || param == caSel_->param) return;
    const int source = caSel_->source;
    if (source == hx::controller::None) {
        caSel_->param = param;
        refreshEditor();
        return;
    }
    if (param < 0 && !isSwitch(source)) {
        setStatus("A block's bypass can only go on a footswitch");
        return;
    }
    setAssignSource(hx::controller::None);
    caSel_->param = param;
    caSel_->source = hx::controller::None;
    setAssignSource(source);
}

void EditorView::assignParamMenu(Rml::Event &ev) {
    const EditTarget t = editTarget();
    if (!menu_ || !caSel_ || !t.meta || !fw_) return;
    std::vector<ContextMenu::Item> items;
    if (t.block) {
        ContextMenu::Item it{"Bypass", [this] { setAssignParam(-1); }};
        it.checked = caSel_->param < 0;
        items.push_back(std::move(it));
    }
    for (const hx::MetaParam &k : t.meta->knobs) {
        if (hiddenKnob(fw_->meta, k) || k.number == 0 || k.type == std::uint32_t(hx::ParamType::Command)) continue;
        const int p = int(k.number) - 1;
        ContextMenu::Item it{fw_->meta.displayName(k), [this, p] { setAssignParam(p); }};
        it.checked = caSel_->param == p;
        items.push_back(std::move(it));
    }
    menu_->openAt(ev, std::move(items), caBlock_);
}

// HX Edit's list: None, EXP Pedal 1 / 2, the LT's 8 footswitches with what's on each ("[Scream 808:Bypass]"),
// MIDI CC, Snapshots. A bypass: None and the footswitches.
void EditorView::assignSourceMenu(Rml::Event &ev) {
    if (!menu_ || !caSel_) return;
    const bool bypass = caSel_->param < 0;
    std::vector<ContextMenu::Item> items;
    auto add = [&](int source, std::string label) {
        ContextMenu::Item it{std::move(label), [this, source] { setAssignSource(source); }};
        it.checked = caSel_->source == source;
        items.push_back(std::move(it));
    };
    add(hx::controller::None, "None");
    if (!bypass) {
        add(hx::controller::Exp1, "EXP Pedal 1");
        add(hx::controller::Exp2, "EXP Pedal 2");
        add(hx::controller::VariaxVolume, "Variax Volume");
        add(hx::controller::VariaxTone, "Variax Tone");
    }
    for (int fs : kAssignableSwitches) {
        std::string on;
        for (const hx::FootswitchAssign &f : preset_.footswitches[std::size_t(fs)]) {
            const std::string what = caBlockName(f.target) + ":" +
                                     (f.kind == 1 ? std::string("Bypass") : f.kind == 2 ? caParamName(f.target, f.param) : std::string("Command"));
            on += (on.empty() ? " [" : ", ") + what;
        }
        if (!on.empty()) on += "]";
        add(hx::controller::footswitch(fs), caSourceName(hx::controller::footswitch(fs)) + on);
    }
    if (!bypass) {
        add(hx::controller::MidiCc, "MIDI CC");
        add(hx::controller::Snapshot, "Snapshots");
    }
    menu_->openAt(ev, std::move(items), caParam_);
}

void EditorView::assignLedMenu(Rml::Event &ev) {
    if (!menu_ || !caSel_ || !isSwitch(caSel_->source)) return;
    const int fs = caSel_->source - hx::controller::footswitch(0);
    const auto &list = preset_.footswitches[std::size_t(fs)];
    const int current = list.empty() ? 0 : list.front().ledColour;
    std::vector<ContextMenu::Item> items;
    for (const UiItem &c : fsColours_) {
        const int k = c.index;
        ContextMenu::Item it{c.label, [this, fs, k] {
                                 fsSel_ = fs;
                                 setSwitchColour(k);
                             }};
        it.checked = k == current;
        items.push_back(std::move(it));
    }
    menu_->openAt(ev, std::move(items), "Switch LED");
}

int EditorView::cellOfTarget(const hx::PresetTarget &t) {
    int cell = t.block >= 0 ? cellOf(t.block) : -1;
    if (cell < 0 && t.slot >= 0) {  // a split / merge
        const int dsp = t.slot / hx::kSlotsPerPath, k = t.slot % hx::kSlotsPerPath;
        if (k == 10) cell = dsp * 100 + kSplitCell;
        if (k == 19) cell = dsp * 100 + kJoinCell;
    }
    return cell;
}

std::string EditorView::commandSlotName(int slot, bool shortName) {
    if (slot >= 0 && slot < 6) return (shortName ? "Inst " : "Instant ") + std::to_string(slot + 1);
    if (slot >= 6 && slot < 18) return (shortName ? "FS" : "Footswitch ") + std::to_string(slot - 5);
    switch (slot) {
    case 18: return shortName ? "Toe" : "EXP Toe Switch";
    case 19: return "EXP 1";
    case 20: return "EXP 2";
    case 22: return shortName ? "Var Vol" : "Variax Volume Knob";
    case 23: return shortName ? "Var Tone" : "Variax Tone Knob";
    default: return "Slot " + std::to_string(slot);
    }
}

// HX Edit's layout: Instant 1-6, toe switch, Variax knobs on top; FS1-6, FS7-12; EXP 1 / 2 beside them. Then the
// selected slot: its command, the command's fields (HX Edit's names), and for a switch its scribble strip.
void EditorView::rebuildCommandGrid() {
    auto cell = [this](int slot) {
        UiItem it;
        it.index = slot;
        it.label = commandSlotName(slot, true);
        const int type = std::size_t(slot) < preset_.commands.size() ? preset_.commands[std::size_t(slot)].type : 0;
        it.extra = type ? hx::commandTypeName(type) : "";
        it.cls = std::string(slot < 6 ? "cc-cell instant" : "cc-cell") + (slot == cmdSel_ ? " selected" : "") +
                 (type ? " set" : "");
        return it;
    };
    // Rows (user's layout, 2026-11-13): Instant 1-6; FS1-6; FS7-12; EXP 1, EXP 2, EXP toe, Variax Volume / Tone.
    cmdTop_.clear();
    cmdRow1_.clear();
    cmdRow2_.clear();
    cmdPedals_.clear();
    for (int s = 0; s < 6; ++s) cmdTop_.push_back(cell(s));
    for (int s = 6; s < 12; ++s) cmdRow1_.push_back(cell(s));
    for (int s = 12; s < 18; ++s) cmdRow2_.push_back(cell(s));
    for (int s : {19, 20, 18, 22, 23}) cmdPedals_.push_back(cell(s));
    const std::size_t snap = std::size_t(preset_.snapshot);
    cmdSnapName_ = snap < preset_.snapshots.size() && !preset_.snapshots[snap].name.empty()
                       ? preset_.snapshots[snap].name
                       : "SNAPSHOT " + std::to_string(snap + 1);

    cmdHasSel_ = cmdSel_ >= 0 && cmdSel_ < hx::kCommandSlots && cmdSel_ != 21;
    cmdFields_.clear();
    cmdIsSwitch_ = false;
    if (!cmdHasSel_) return;
    const hx::CommandSlot c = std::size_t(cmdSel_) < preset_.commands.size() ? preset_.commands[std::size_t(cmdSel_)] : hx::CommandSlot{};
    cmdSelName_ = commandSlotName(cmdSel_, false);
    cmdSelType_ = hx::commandTypeName(c.type);
    const std::vector<std::string> names = hx::commandFieldNames(c.type);
    for (std::size_t k = 0; k < names.size() && k < c.fields.size(); ++k) {
        UiField f;
        f.index = int(k);
        f.label = names[k];
        f.value = std::to_string(c.fields[k]);
        const std::string text = hx::commandFieldText(c.type, int(k), c.fields[k]);
        f.text = text == f.value ? "" : text;
        f.list = !hx::commandFieldChoices(c.type, int(k)).empty();
        cmdFields_.push_back(std::move(f));
    }
    // A footswitch with a command: its scribble strip, LED and latching (the switch's, as the unit keeps them).
    const int fs = cmdSel_ - hx::controller::footswitch(0);
    cmdIsSwitch_ = c.type != 0 && fs >= 0 && fs < 12;
    if (cmdIsSwitch_) {
        const auto &list = preset_.footswitches[std::size_t(fs)];
        const hx::FootswitchAssign *f = list.empty() ? nullptr : &list.front();
        cmdLabel_ = f ? f->label : "";
        cmdMomentary_ = f && f->momentary;
        const int led = f ? f->ledColour : 0;
        cmdLed_ = led >= 0 && std::size_t(led) < fsColours_.size() ? fsColours_[std::size_t(led)].label : "Auto Color";
    }
}

void EditorView::commandTypeMenu(Rml::Event &ev) {
    if (!menu_ || !cmdHasSel_) return;
    const int current = std::size_t(cmdSel_) < preset_.commands.size() ? preset_.commands[std::size_t(cmdSel_)].type : 0;
    std::vector<ContextMenu::Item> items;
    for (int type : hx::commandTypesFor(cmdSel_)) {
        const int slot = cmdSel_;
        ContextMenu::Item it{hx::commandTypeName(type), [this, slot, type] { setCommandType(slot, type); }};
        it.checked = type == current;
        items.push_back(std::move(it));
    }
    menu_->openAt(ev, std::move(items), cmdSelName_);
}

void EditorView::commandLedMenu(Rml::Event &ev) {
    if (!menu_ || !cmdIsSwitch_) return;
    const int fs = cmdSel_ - hx::controller::footswitch(0);
    const auto &list = preset_.footswitches[std::size_t(fs)];
    const int current = list.empty() ? 0 : list.front().ledColour;
    std::vector<ContextMenu::Item> items;
    for (const UiItem &c : fsColours_) {
        const int k = c.index;
        ContextMenu::Item it{c.label, [this, fs, k] {
                                 fsSel_ = fs;
                                 setSwitchColour(k);
                             }};
        it.checked = k == current;
        items.push_back(std::move(it));
    }
    menu_->openAt(ev, std::move(items), "Switch LED");
}

// The -/+ beside a field: one step, kept inside the field's range.
void EditorView::stepCommandField(int field, int delta) {
    if (!cmdHasSel_ || std::size_t(cmdSel_) >= preset_.commands.size() || field < 0 || field >= 6) return;
    const hx::CommandSlot &c = preset_.commands[std::size_t(cmdSel_)];
    const auto [lo, hi] = hx::commandFieldRange(c.type, field);
    const int v = std::clamp(c.fields[std::size_t(field)] + (delta > 0 ? 1 : -1), lo, hi);
    setCommandField(cmdSel_, field, std::to_string(v));
    rebuildCommandGrid();
    if (handle_) handle_.DirtyVariable("cc_fields");
}

void EditorView::setCommandType(int slot, int type) {
    const int current = std::size_t(slot) < preset_.commands.size() ? preset_.commands[std::size_t(slot)].type : 0;
    if (type == current) return;
    beginEdit();
    if (live()) sendLive(hx::editor::kCommandType, hx::editor::commandTypeArgs(slot, type));
    preset_.setCommand(slot, type);
    refreshEditor();
}

// Applied on Enter or when the field loses focus; text that isn't a number puts the old value back.
void EditorView::setCommandField(int slot, int field, const std::string &text) {
    if (std::size_t(slot) >= preset_.commands.size() || field < 0 || field >= 6) return;
    char *end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end != text.c_str() && int(v) != preset_.commands[std::size_t(slot)].fields[std::size_t(field)]) {
        beginEdit();
        if (live())
            // The unit's field numbers start at 1 for our first field (MIDI channel = 1, capture command-center2).
            sendLive(hx::editor::kCommandField,
                     hx::editor::commandFieldArgs(slot, preset_.commands[std::size_t(slot)].type, field + 1, int(v)));
        preset_.setCommandField(slot, field, int(v));
    }
}

// The selected switch's functions (label, LED, latching belong to the switch on the unit); null when it has none.
std::vector<hx::FootswitchAssign> *EditorView::selectedSwitchList() {
    if (fsSel_ < 0 || fsSel_ >= hx::kFootswitchSlots) return nullptr;
    auto &list = preset_.footswitches[std::size_t(fsSel_)];
    return list.empty() ? nullptr : &list;
}

void EditorView::setSwitchLabel(const std::string &label) {
    std::vector<hx::FootswitchAssign> *list = selectedSwitchList();
    if (!list || list->front().label == label) return;
    beginEdit("fslabel");
    // The unit keeps one label per switch.
    if (live()) sendLive(hx::editor::kSwitchLabel, hx::editor::switchLabelArgs(fsSel_, label));
    for (hx::FootswitchAssign &f : *list) f.label = label;
    // Not refreshEditor: that would reset the field while typing.
}

// LED colour and momentary / latching belong to the switch: every function on it gets them (the unit keeps them per
// entry, keys 16 / 12).
void EditorView::setSwitchColour(int colour) {
    if (!selectedSwitchList() || colour < 0 || std::size_t(colour) >= fsColours_.size()) return;
    beginEdit();
    if (live()) sendLive(hx::editor::kSwitchColour, hx::editor::switchColourArgs(fsSel_, colour));
    for (hx::FootswitchAssign &f : preset_.footswitches[std::size_t(fsSel_)]) f.ledColour = colour;
    refreshEditor();
}

void EditorView::setSwitchMomentary(bool momentary) {
    if (!selectedSwitchList()) return;
    beginEdit();
    if (live()) sendLive(hx::editor::kSwitchMomentary, hx::editor::switchMomentaryArgs(fsSel_, momentary));
    for (hx::FootswitchAssign &f : preset_.footswitches[std::size_t(fsSel_)]) f.momentary = momentary;
    refreshEditor();
}

// ---------------------------------------------------------------- signal flow

std::optional<hx::PresetBlock> *EditorView::selectedSlot() {
    if (!sel_.valid() || sel_.special != Special::None) return nullptr;
    hx::PresetPath &p = preset_.paths[std::size_t(sel_.dsp)];
    return &(sel_.lane ? p.b : p.a).slots[std::size_t(sel_.slot)];
}

hx::PresetBlock *EditorView::selectedBlock() {
    std::optional<hx::PresetBlock> *s = selectedSlot();
    return s && *s ? &**s : nullptr;
}

void EditorView::pickCell(int id) {
    setlistOpen_ = subOpen_ = false;
    editCab_ = false;
    sel_ = {};
    if (id >= 0 && id / 100 < 2) {
        const int dsp = id / 100, rest = id % 100;
        const int lane = rest / 10;
        const hx::PresetRouting &r = preset_.paths[std::size_t(dsp)].routing;
        if (rest == kSplitCell) sel_ = {dsp, 0, 0, Special::Split};
        else if (rest == kJoinCell) sel_ = {dsp, 0, 0, Special::Join};
        else if (lane < 2 && rest % 10 < hx::kLaneSlots) sel_ = {dsp, lane, rest % 10};
        // Lane B's nodes only while it has its own input / output.
        else if (lane < 2 && rest % 10 == kInputSlot && (lane == 0 || r.hasOwnInputB())) sel_ = {dsp, lane, 0, Special::Input};
        else if (lane < 2 && rest % 10 == kOutputSlot && (lane == 0 || r.hasOwnOutputB())) sel_ = {dsp, lane, 0, Special::Output};
    }
    if (sel_.special != Special::None) {
        showSpecialInBrowser();
    } else {
        const hx::PresetBlock *b = selectedBlock();
        showModelInBrowser(b && fw_ ? fw_->model(b->modelNo) : nullptr);
    }
    rebuildLanes();
    rebuildParams();
    rebuildDsp();
    refreshEditor();
}

// Path 2's inputs show "Path 1" when path 1 sends there and their own source is None, as on the unit.
void EditorView::ioCell(int dsp, int lane, bool input, UiCell &cell) const {
    const hx::PresetPath &p = preset_.paths[std::size_t(dsp)];
    const int value = input ? p.inputs[std::size_t(lane)].source : p.outputs[std::size_t(lane)].dest;
    const std::span<const hx::IoPort> ports = input ? hx::ioInputs() : hx::ioOutputs();
    if (value < 0 || std::size_t(value) >= ports.size()) {
        cell.label = "#" + std::to_string(value);
        return;
    }
    if (input && dsp == 1 && value == 0) {
        bool fed = false;
        for (int l = 0; l < 2; ++l) {
            const int d = preset_.paths[0].outputs[std::size_t(l)].dest;
            fed = fed || d == hx::kOutPath2AB || d == (lane ? hx::kOutPath2B : hx::kOutPath2A);
        }
        if (fed) {
            cell.label = "Path 1";
            return;
        }
    }
    const hx::IoPort &port = ports[std::size_t(value)];
    cell.label = port.label;
    if (fw_ && fw_->gfx && *port.image) {
        const std::string name = std::string(port.image) + (cell.selected ? "Sel" : "");
        const hx::GfxImage *img = fw_->gfx->byName(name);
        if (!img) img = fw_->gfx->byName(port.image);
        if (img) {
            cell.icon = fwtex::source(img->index);
            // Path 2A / 2B / 2A+B: the circle with an arrow under it (36x66, not 36x38): drawn at its own height.
            cell.tallIcon = img->height > img->width * 3 / 2;
        }
    }
}

void EditorView::rebuildLanes() {
    lanes_.clear();
    for (int dsp = 0; dsp < 2; ++dsp) {
        for (int lane = 0; lane < 2; ++lane) {
            UiLane l;
            l.label = std::to_string(dsp + 1) + (lane ? "B" : "A");
            const hx::PresetPath &p = preset_.paths[std::size_t(dsp)];
            const hx::PresetLane &pl = lane ? p.b : p.a;
            const hx::PresetRouting &r = p.routing;
            auto ioSel = [&](Special s) { return sel_.special == s && sel_.dsp == dsp && sel_.lane == lane; };
            UiCell in{dsp * 100 + lane * 10 + kInputSlot, "io", "Input", "", ioSel(Special::Input), false};
            in.hidden = lane && !r.hasOwnInputB();
            in.dragIo = lane && !in.hidden;
            if (in.hidden) in.label = "";
            else ioCell(dsp, lane, true, in);
            l.cells.push_back(std::move(in));
            for (int s = 0; s < hx::kLaneSlots; ++s) {
                UiCell c;
                c.id = dsp * 100 + lane * 10 + s;
                c.selected = sel_.special == Special::None && sel_.dsp == dsp && sel_.lane == lane && sel_.slot == s;
                if (const auto &b = pl.slots[std::size_t(s)]) {
                    const hx::ModelRef *m = fw_ ? fw_->model(b->modelNo) : nullptr;
                    const int cat = m ? m->category : -1;
                    c.kind = "block";
                    c.label = categoryLabel(fw_, cat, true);
                    c.cls = catClass(cat);
                    if (m && !irMismatch(m->meta, &b->values).empty()) c.cls += " ir-warn";  // IR / NAM slot mix-up
                    c.bypassed = !b->enabled;
                    // The unit's own block icon with its frame, as Helix Native draws it: Sel = white frame,
                    // Off = dimmed.
                    if (fw_ && fw_->icons && cat >= 0) {
                        using S = hx::IconState;
                        const S st = c.selected ? (c.bypassed ? S::SelOff : S::Sel) : (c.bypassed ? S::Off : S::Normal);
                        c.icon = fwtex::source(fw_->icons->id(cat, st));
                    }
                } else {
                    c.kind = "empty";
                }
                l.cells.push_back(std::move(c));
            }
            UiCell out{dsp * 100 + lane * 10 + kOutputSlot, "io", "Output", "", ioSel(Special::Output), false};
            out.hidden = lane && !r.hasOwnOutputB();
            out.dragIo = lane && !out.hidden;
            if (out.hidden) out.label = "";
            else ioCell(dsp, lane, false, out);
            l.cells.push_back(std::move(out));
            if (lane) {
                laneBLine(p, l);
                // Dragging over a serial path's lane B slot: lane B as the drop would make it (normalizeRouting: split
                // before the first lane B block, merge at the end), greyed. Not shown elsewhere during the drag.
                if (!l.line && dragging_ && ghostDsp_ == dsp) {
                    hx::PresetPath ghost;
                    ghost.routing.mode = 1;
                    ghost.routing.splitOn = ghost.routing.joinOn = true;
                    ghost.routing.splitPos = ghostSplit_;
                    ghost.routing.joinPos = hx::kLaneSlots;
                    laneBLine(ghost, l);
                    l.ghost = true;
                }
                l.splitId = dsp * 100 + kSplitCell;
                l.joinId = dsp * 100 + kJoinCell;
                l.splitSel = sel_.dsp == dsp && sel_.special == Special::Split;
                l.joinSel = sel_.dsp == dsp && sel_.special == Special::Join;
            }
            lanes_.push_back(std::move(l));
        }
    }
}

void EditorView::rebuildDsp() {
    for (int dsp = 0; dsp < 2; ++dsp) {
        const float pct = fw_ ? float(fw_->pathCost(preset_.paths[std::size_t(dsp)])) / kBudget * 90.f : 0.f;
        (dsp ? dsp2_ : dsp1_) = "DSP " + std::to_string(dsp + 1) + "  " + formatNum("%.0f%%", pct);
        (dsp ? dsp2W_ : dsp1W_) = formatNum("%.0f%%", std::min(pct, 100.f));
    }
}

// ---------------------------------------------------------------- browser

void EditorView::rebuildCategories() {
    categories_.clear();
    // Device order: None, Favorites, then categories our builds add (Custom, 19: shown before Distortion as on
    // the unit), then the stock ones.
    std::vector<int> order{kNone, kFavorites};
    for (int c = kCategoryCount; fw_ && c < fw_->tables.categories; ++c) order.push_back(c);
    for (int c = kFavorites + 1; c < kCategoryCount; ++c) order.push_back(c);
    for (int c : order) {
        bool has = c == kNone;
        if (fw_)
            for (const hx::BrowserList &l : fw_->tables.lists) has = has || l.category == c;
        if (c >= kCategoryCount && !has) continue;  // an added category with no list in this build
        UiItem item{c, categoryLabel(fw_, c, false), "", catClass(c), has && c != kFavorites};
        if (fw_ && fw_->icons && c > kFavorites) item.icon = fwtex::source(fw_->icons->id(c, hx::IconState::Normal), true);
        categories_.push_back(std::move(item));
    }
    updateFits();
}

void EditorView::showModelInBrowser(const hx::ModelRef *m) {
    catSel_ = m ? m->category : -1;
    subLists_.clear();
    if (fw_ && catSel_ >= 0)
        for (std::size_t i = 0; i < fw_->tables.lists.size(); ++i)
            if (fw_->tables.lists[i].category == catSel_) subLists_.push_back(int(i));
    setBrowserList(m ? m->list : (subLists_.empty() ? -1 : subLists_[0]));
    modelSel_ = m ? m->entry : -1;
}

// Sub-list names: rec 4 subcategory (Mono, Stereo, Guitar, Bass, Single, Dual...), with
// "Legacy" for the third effect list and for lists of the older cab models.
void EditorView::setBrowserList(int list) {
    list_ = list;
    subs_.clear();
    subLabel_.clear();
    if (!fw_) return rebuildModels();
    for (std::size_t k = 0; k < subLists_.size(); ++k) {
        const hx::BrowserList &l = fw_->tables.lists[std::size_t(subLists_[k])];
        std::string label;
        bool legacyCab = false;
        if (!l.entries.empty()) {
            const hx::BrowserEntry &e = l.entries[0];
            const std::uint32_t cabSym = l.category == kAmpCab ? e.cabSymId : (l.category == kCab ? e.symId : hx::kNoCab);
            if (const hx::MetaEntry *cab = cabSym != hx::kNoCab ? fw_->meta.bySymId(cabSym) : nullptr)
                legacyCab = fw_->meta.attr(cab->modelAttrs, "category") == "cab";
            if (const hx::MetaEntry *me = fw_->meta.bySymId(e.symId))
                label = capitalize(fw_->meta.attr(me->modelAttrs, "subcategory"));
        }
        if (l.category == kAmpCab || l.category == kCab)
            label = std::string(l.sub % 2 ? (l.category == kCab ? "Dual" : "Bass") : (l.category == kCab ? "Single" : "Guitar"));
        if (legacyCab) label = "Legacy " + label;
        if (l.category >= kCategoryCount && !l.subName.empty()) label = l.subName;  // Custom: the build's own names
        if (l.category >= 2 && l.category <= kLastEffectCategory && l.sub == kLegacySub) label = "Legacy";
        if (label.empty()) label = "List " + std::to_string(l.sub + 1);
        subs_.push_back({int(k), label});
        if (subLists_[k] == list) subLabel_ = label;
    }
    rebuildModels();
}

// A selected split shows the split types (Y, A/B, Crossover, Dynamic: rec 4 entries of category
// "split") in the model column; a merge shows its one mixer model.
void EditorView::showSpecialInBrowser() {
    catSel_ = -1;
    subLists_.clear();
    subs_.clear();
    list_ = -1;
    models_.clear();
    splitTypes_.clear();
    ioChoices_.clear();
    modelSel_ = -1;
    if (!fw_ || !sel_.valid()) return;
    if (isIo()) {
        // The ports, as the unit lists them: only the ones its hardware has (no Send / Return 3/4 or Mic on an LT);
        // Path 2A / 2B / 2A+B only from path 1. A port the preset already uses stays listed, marked.
        const bool input = sel_.special == Special::Input;
        const hx::PresetPath &p = preset_.paths[std::size_t(sel_.dsp)];
        const int current = input ? p.inputs[std::size_t(sel_.lane)].source : p.outputs[std::size_t(sel_.lane)].dest;
        subLabel_ = input ? "Input" : "Output";
        const std::span<const hx::IoPort> ports = input ? hx::ioInputs() : hx::ioOutputs();
        for (int v = 0; v < int(ports.size()); ++v) {
            if (!input && sel_.dsp == 1 && v >= hx::kOutPath2A && v <= hx::kOutPath2AB) continue;
            const bool here = input ? hx::inputAvailable(unitModel_, v) : hx::outputAvailable(unitModel_, v);
            if (!here && v != current) continue;
            if (v == current) modelSel_ = int(ioChoices_.size());
            models_.push_back({int(ioChoices_.size()),
                               std::string(ports[std::size_t(v)].label) + (here ? "" : "  (not on this unit)")});
            ioChoices_.push_back(v);
        }
        return;
    }
    const hx::PresetRouting &r = preset_.paths[std::size_t(sel_.dsp)].routing;
    const bool split = sel_.special == Special::Split;
    subLabel_ = split ? "Split type" : "Merge";
    for (const auto &[no, m] : fw_->models)
        if (m.meta && fw_->meta.attr(m.meta->modelAttrs, "category") == (split ? "split" : "join")) splitTypes_.push_back(no);
    // Y first (the default split), then the rest in model-number order.
    std::stable_sort(splitTypes_.begin(), splitTypes_.end(), [&](std::uint16_t a, std::uint16_t b) {
        return (fw_->model(a)->name == "Y") > (fw_->model(b)->name == "Y");
    });
    const std::uint16_t current = split ? r.splitModel : r.joinModel;
    for (std::size_t i = 0; i < splitTypes_.size(); ++i) {
        models_.push_back({int(i), fw_->model(splitTypes_[i])->name});
        if (splitTypes_[i] == current) modelSel_ = int(i);
    }
}

void EditorView::rebuildModels() {
    models_.clear();
    if (!fw_ || list_ < 0) return;
    const hx::BrowserList &l = fw_->tables.lists[std::size_t(list_)];
    for (std::size_t i = 0; i < l.entries.size(); ++i) models_.push_back({int(i), l.entries[i].name});
    updateFits();
}

// A browser entry's DSP cost: its model, plus the cab of an amp+cab entry.
std::uint32_t EditorView::entryCost(const hx::BrowserEntry &e) const {
    std::uint32_t c = 0;
    if (const hx::ModelRef *m = fw_->modelBySym(e.symId)) c += m->cost;
    if (e.cabSymId != hx::kNoCab)
        if (const hx::ModelRef *cab = fw_->modelBySym(e.cabSymId)) c += cab->cost;
    return c;
}

// DSP units left for the selected slot: the budget minus the path's total without the block in that slot (a new
// model replaces it). Unlimited when no block slot is selected.
std::uint32_t EditorView::dspRoom() {
    if (!fw_ || !sel_.valid() || sel_.special != Special::None) return UINT32_MAX;
    std::uint32_t used = fw_->pathCost(preset_.paths[std::size_t(sel_.dsp)]);
    if (const hx::PresetBlock *b = selectedBlock()) {
        if (const hx::ModelRef *m = fw_->model(b->modelNo)) used -= m->cost;
        if (b->cabModelNo >= 0)
            if (const hx::ModelRef *c = fw_->model(std::uint16_t(b->cabModelNo))) used -= c->cost;
    }
    const auto budget = std::uint32_t(kBudget);
    return used >= budget ? 0 : budget - used;
}

// Greys out what won't fit on the selected slot's path, as the unit does: models in the list, and categories with
// no model that fits (those can't be opened either).
void EditorView::updateFits() {
    if (!fw_) return;
    const std::uint32_t room = dspRoom();
    for (UiItem &c : categories_) {
        bool listed = false, fits = false;
        for (const hx::BrowserList &l : fw_->tables.lists) {
            if (l.category != c.index) continue;
            listed = true;
            for (const hx::BrowserEntry &e : l.entries)
                if (entryCost(e) <= room) {
                    fits = true;
                    break;
                }
            if (fits) break;
        }
        c.enabled = c.index == kNone || (listed && fits && c.index != kFavorites);
    }
    if (list_ >= 0 && std::size_t(list_) < fw_->tables.lists.size()) {
        const hx::BrowserList &l = fw_->tables.lists[std::size_t(list_)];
        for (UiItem &m : models_)
            if (m.index >= 0 && std::size_t(m.index) < l.entries.size()) m.enabled = entryCost(l.entries[std::size_t(m.index)]) <= room;
    }
}

void EditorView::pickCategory(int category) {
    setlistOpen_ = subOpen_ = false;
    if (sel_.special != Special::None) return;  // categories are for blocks
    for (const UiItem &c : categories_)
        if (c.index == category && !c.enabled) {
            if (category != kFavorites) setStatus("DSP full: nothing in " + categoryLabel(fw_, category, false) + " fits on this path");
            return;
        }
    if (category == kNone) {
        if (std::optional<hx::PresetBlock> *s = selectedSlot(); s && *s) {
            beginEdit();
            if (live()) sendLive(hx::editor::kClearSlot, hx::editor::slotArgs(selectedSlotNumber()));
            s->reset();  // "None" clears the slot
            hx::normalizeRouting(preset_.paths[std::size_t(sel_.dsp)]);
            preset_.pruneAssignments();
        }
        showModelInBrowser(nullptr);
        catSel_ = kNone;
    } else if (category > kFavorites && category < (fw_ ? fw_->tables.categories : kCategoryCount)) {
        const hx::PresetBlock *b = selectedBlock();
        const hx::ModelRef *m = b && fw_ ? fw_->model(b->modelNo) : nullptr;
        if (m && m->category == category) {
            showModelInBrowser(m);
        } else {
            catSel_ = category;
            subLists_.clear();
            if (fw_)
                for (std::size_t i = 0; i < fw_->tables.lists.size(); ++i)
                    if (fw_->tables.lists[i].category == category) subLists_.push_back(int(i));
            setBrowserList(subLists_.empty() ? -1 : subLists_[0]);
            modelSel_ = -1;
        }
    }
    rebuildLanes();
    rebuildParams();
    rebuildDsp();
    refreshEditor();
}

void EditorView::pickSub(int index) {
    subOpen_ = false;
    if (index >= 0 && std::size_t(index) < subLists_.size()) {
        setBrowserList(subLists_[std::size_t(index)]);
        modelSel_ = -1;
    }
    refreshEditor();
}

void EditorView::pickModel(int index) {
    setlistOpen_ = subOpen_ = false;
    if (fw_ && isIo()) {
        if (index < 0 || std::size_t(index) >= ioChoices_.size()) return;
        hx::PresetPath &p = preset_.paths[std::size_t(sel_.dsp)];
        const int v = ioChoices_[std::size_t(index)];
        beginEdit();
        if (const int slot = selectedSlotNumber(); slot >= 0) {
            if (live()) sendLive(hx::editor::kSetIo, hx::editor::ioArgs(slot, sel_.special == Special::Output, v));
        } else {
            notSent("lane B input / output changes");
        }
        if (sel_.special == Special::Input) p.inputs[std::size_t(sel_.lane)].source = v;
        else p.outputs[std::size_t(sel_.lane)].dest = v;
        modelSel_ = index;
        rebuildLanes();
        rebuildParams();
        refreshEditor();
        return;
    }
    if (fw_ && sel_.special != Special::None) {
        // Change the split type (or merge model): its knobs differ, so start from the defaults.
        if (index < 0 || std::size_t(index) >= splitTypes_.size()) return;
        hx::PresetRouting &r = preset_.paths[std::size_t(sel_.dsp)].routing;
        const hx::ModelRef *m = fw_->model(splitTypes_[std::size_t(index)]);
        const bool split = sel_.special == Special::Split;
        if ((split ? r.splitModel : r.joinModel) != m->modelNo) {
            beginEdit();
            if (live()) sendLive(hx::editor::kSetSplitModel, hx::editor::splitModelArgs(selectedSlotNumber(), m->modelNo));
            (split ? r.splitModel : r.joinModel) = m->modelNo;
            (split ? r.splitValues : r.joinValues) = defaultValues(m->meta);
        }
        modelSel_ = index;
        rebuildParams();
        rebuildDsp();
        refreshEditor();
        return;
    }
    if (!fw_ || list_ < 0) return;
    const hx::BrowserList &l = fw_->tables.lists[std::size_t(list_)];
    if (index < 0 || std::size_t(index) >= l.entries.size()) return;
    const hx::BrowserEntry &e = l.entries[std::size_t(index)];
    const hx::ModelRef *m = fw_->modelBySym(e.symId);
    if (entryCost(e) > dspRoom()) return setStatus("DSP full: " + e.name + " doesn't fit on this path");
    modelSel_ = index;
    if (m) {
        if (std::optional<hx::PresetBlock> *s = selectedSlot()) {
            beginEdit();
            hx::PresetBlock b = newBlock(*m);
            if (e.cabSymId != hx::kNoCab)
                if (const hx::ModelRef *cab = fw_->modelBySym(e.cabSymId)) {
                    b.cabModelNo = cab->modelNo;
                    b.cabValues = defaultValues(cab->meta);
                }
            *s = std::move(b);
            hx::normalizeRouting(preset_.paths[std::size_t(sel_.dsp)]);
            preset_.pruneAssignments();  // the old block's footswitch and controllers go with it
            if (undoIfDspFull(sel_.dsp)) return;
            sendSelectedModel();
        }
    }
    rebuildLanes();
    rebuildParams();
    rebuildDsp();
    refreshEditor();
}

// ---------------------------------------------------------------- params

EditorView::EditTarget EditorView::editTarget() {
    EditTarget t;
    if (!fw_ || !sel_.valid()) return t;
    if (isIo()) {
        hx::PresetPath &p = preset_.paths[std::size_t(sel_.dsp)];
        const bool input = sel_.special == Special::Input;
        t.meta = input ? (sel_.lane ? inputBMeta_ : inputAMeta_) : outputMeta_;
        t.values = input ? &p.inputs[std::size_t(sel_.lane)].values : &p.outputs[std::size_t(sel_.lane)].values;
        if (t.meta)
            if (const std::string n = fw_->meta.attr(t.meta->modelAttrs, "displayParamCount"); !n.empty())
                t.knobCount = std::size_t(std::max(0, std::atoi(n.c_str())));
        UiCell c;
        ioCell(sel_.dsp, sel_.lane, input, c);
        t.title = std::string(input ? "Input " : "Output ") + (sel_.lane ? "B" : "A") + " > " + c.label;
        return t;
    }
    if (sel_.special != Special::None) {
        hx::PresetRouting &r = preset_.paths[std::size_t(sel_.dsp)].routing;
        const bool split = sel_.special == Special::Split;
        const hx::ModelRef *m = fw_->model(split ? r.splitModel : r.joinModel);
        t.meta = m ? m->meta : nullptr;
        t.values = split ? &r.splitValues : &r.joinValues;
        t.enabled = split ? &r.splitEnabled : &r.joinEnabled;
        t.target.slot = sel_.dsp * hx::kSlotsPerPath + (split ? 10 : 19);  // the split / join slot of the path
        t.title = std::string(split ? "Split" : "Merge") + (m ? " > " + m->name : "");
        return t;
    }
    if (hx::PresetBlock *b = selectedBlock()) {
        const hx::ModelRef *m = fw_->model(b->modelNo);
        t.meta = m ? m->meta : nullptr;
        t.values = &b->values;
        t.enabled = &b->enabled;
        t.block = b;
        t.target.block = b->uid;
        t.title = m ? m->name : "Model #" + std::to_string(b->modelNo);
        t.category = m ? m->category : -1;
        // The linked cab's knobs. How a controller would point at them isn't known, so none (no target).
        if (editCab_ && b->cabModelNo >= 0) {
            const hx::ModelRef *cab = fw_->model(std::uint16_t(b->cabModelNo));
            t.meta = cab ? cab->meta : nullptr;
            t.values = &b->cabValues;
            t.target = {};
            t.title += "  >  " + (cab ? cab->name : "Cab #" + std::to_string(b->cabModelNo));
        }
    }
    return t;
}

UiParam *EditorView::findParam(int knobIndex) {
    for (UiParam &p : params_)
        if (p.index == knobIndex) return &p;
    return nullptr;
}

void EditorView::rebuildParams() {
    params_.clear();
    if (irOpen_ >= 0 && selectedSlotNumber() != irOpenSlot_) {  // another block: its dropdown closes
        irOpen_ = -1;
        irShown_ = false;
        if (handle_) handle_.DirtyVariable("irsel_shown");
    }
    if (irChoices_.empty()) rebuildIrChoices();
    rebuildInputSettings();
    const hx::PresetBlock *sb = selectedBlock();
    hasCab_ = sb && sb->cabModelNo >= 0 && fw_;
    if (!hasCab_) editCab_ = false;
    const hx::ModelRef *sm = sb && fw_ ? fw_->model(sb->modelNo) : nullptr;
    const bool dual = hasCab_ && sm && sm->category == kCab;  // a dual cab: the linked model is the second cab
    mainLabel_ = dual ? "Cab 1" : sm && sm->category == kPreamp ? "Preamp" : "Amp";
    cabLabel_ = dual ? "Cab 2" : "Cab";
    const EditTarget t = editTarget();
    hasBlock_ = t.enabled != nullptr;
    blockOn_ = t.enabled && *t.enabled;
    canCopyBlock_ = sb != nullptr;
    canPasteBlock_ = blockClip_ && sel_.valid() && sel_.special == Special::None;
    blockTitle_ = t.title;
    blockCls_ = catClass(t.category);
    if (!fw_) editHint_ = "";
    else if (!sel_.valid()) editHint_ = "Select a block in the signal flow.";
    else if (!t.values) editHint_ = "Empty slot. Pick a model to add a block.";
    else editHint_ = t.meta ? "" : "This model has no parameter data in rec 4.";
    if (!t.meta) return;
    for (std::size_t i = 0; i < t.meta->knobs.size() && i < t.knobCount; ++i) {
        const hx::MetaParam &k = t.meta->knobs[i];
        if (hiddenKnob(fw_->meta, k)) continue;
        double lo, hi, def;
        knobRange(k, lo, hi, def);
        const double v = knobValue(k, t.values);
        UiParam p;
        p.index = int(i);
        p.name = fw_->meta.displayName(k);
        p.value = displayValue(fw_->meta, k, v);
        p.slider = k.type != std::uint32_t(hx::ParamType::Command);
        p.pos = hi > lo ? float((v - lo) / (hi - lo)) : 0.f;
        // Bipolar: a range symmetric around 0, or a pan / A-B balance knob (rec 4 displayType, 0..1 around the middle).
        const std::string type = fw_->meta.attr(k.attrs, "displayType");
        p.centered = (lo < 0 && hi > 0 && std::abs(lo + hi) < 1e-6 * (hi - lo)) || type == "pan" || type == "abpan";
        // A user IR slot (rec 4: list 1..128 shown as "dynamicList"): IR Select, and our NAM block's Model.
        p.irSlot = k.type == std::uint32_t(hx::ParamType::List) && type == "dynamicList" && lo == 1 && hi == hx::editor::kIrSlots;
        if (p.irSlot) {
            p.value = irLabel(int(std::lround(v)));
            p.open = int(i) == irOpen_;
        }
        setFill(p);
        params_.push_back(std::move(p));
    }
    if (const std::string w = irMismatch(t.meta, t.values); !w.empty()) editHint_ = w;
}

// "60  Name" for user IR slot 60 (1-based); "60  (empty)" for an empty one; "60" when the names aren't known.
std::string EditorView::irLabel(int slot) const {
    std::string s = std::to_string(slot);
    if (slot >= 1 && std::size_t(slot) <= irNames_.size())
        s += "  " + (irNames_[std::size_t(slot - 1)].empty() ? std::string("(empty)") : irNames_[std::size_t(slot - 1)]);
    return s;
}

void EditorView::rebuildIrChoices() {
    irChoices_.clear();
    for (int s = 1; s <= hx::editor::kIrSlots; ++s) {
        UiItem c{s, irLabel(s)};
        c.enabled = std::size_t(s) > irNames_.size() || !irNames_[std::size_t(s - 1)].empty();  // empty slots: greyed
        irChoices_.push_back(std::move(c));
    }
}

void EditorView::setIrNames(std::vector<std::string> names, std::vector<int> kinds) {
    irNames_ = std::move(names);
    irKinds_ = std::move(kinds);
    rebuildIrChoices();
    rebuildLanes();  // the wrong-kind warning on blocks
    rebuildParams();
    refreshEditor();
}

int EditorView::irKind(int slot) const {
    return slot >= 1 && std::size_t(slot) <= irKinds_.size() ? irKinds_[std::size_t(slot - 1)] : 0;
}

bool EditorView::wantsNam(const hx::MetaEntry *meta) const {
    return fw_ && meta && fw_->meta.attr(meta->modelAttrs, "category") != "ir";
}

std::string EditorView::irMismatch(const hx::MetaEntry *meta, const std::vector<hx::PresetValue> *values) const {
    if (!fw_ || !meta || irKinds_.empty()) return "";
    const bool nam = wantsNam(meta);
    for (const hx::MetaParam &k : meta->knobs) {
        if (k.type != std::uint32_t(hx::ParamType::List) || fw_->meta.attr(k.attrs, "displayType") != "dynamicList") continue;
        double lo, hi, def;
        knobRange(k, lo, hi, def);
        if (lo != 1 || hi != hx::editor::kIrSlots) continue;
        const int slot = int(std::lround(knobValue(k, values)));
        const int kind = irKind(slot);
        if (nam && kind == 1)
            return "Slot " + std::to_string(slot) + " holds an IR, not a NAM capture: pick a capture for this block.";
        if (!nam && kind == 2)
            return "Slot " + std::to_string(slot) + " holds a NAM capture: as an IR it plays as a very loud burst. "
                   "Pick an IR, or use the NAM block for it.";
    }
    return "";
}

void EditorView::closeIr() {
    irOpen_ = -1;
    irShown_ = false;
    for (UiParam &p : params_) p.open = false;
    if (handle_)
        for (const char *v : {"params", "irsel_shown"}) handle_.DirtyVariable(v);
}

// A slot picked in the dropdown: the same edit as dragging the slider there.
void EditorView::pickIr(int slot) {
    const int index = irOpen_;
    closeIr();
    const EditTarget t = editTarget();
    if (slot >= 1 && t.meta && index >= 0 && std::size_t(index) < t.meta->knobs.size()) {
        double lo, hi, def;
        knobRange(t.meta->knobs[std::size_t(index)], lo, hi, def);
        if (hi > lo) paramChanged(index, float((slot - lo) / (hi - lo)));
    }
    if (handle_) handle_.DirtyVariable("params");
}

void EditorView::toggleBypass() {
    if (const EditTarget t = editTarget(); t.enabled) {
        beginEdit();
        *t.enabled = !*t.enabled;
        // As on the unit: a block's on/off is part of the snapshot in use.
        if (t.block) t.block->snapOn[std::size_t(preset_.snapshot)] = *t.enabled;
        if (t.block) sendLive(hx::editor::kBypass, hx::editor::bypassArgs(selectedSlotNumber(), *t.enabled));
        else notSent("turning a split / merge on or off");
    }
    rebuildLanes();
    rebuildParams();
    refreshEditor();
}

void EditorView::paramChanged(int index, float pos) {
    const EditTarget t = editTarget();
    UiParam *p = findParam(index);
    if (!t.meta || !t.values || !p || std::size_t(index) >= t.meta->knobs.size()) return;
    const hx::MetaParam &k = t.meta->knobs[std::size_t(index)];
    if (k.number == 0) return;
    double lo, hi, def;
    knobRange(k, lo, hi, def);
    double v = lo + double(std::clamp(pos, 0.f, 1.f)) * (hi - lo);
    using K = hx::PresetValue::Kind;
    K kind = K::Float;
    if (k.type == std::uint32_t(hx::ParamType::List)) kind = K::Int, v = std::round(v);
    if (k.type == std::uint32_t(hx::ParamType::Switch)) kind = K::Bool, v = std::round(v);
    std::vector<hx::PresetValue> &values = *t.values;
    if (values.size() < k.number) values.resize(k.number);
    hx::PresetValue &stored = values[k.number - 1];
    // Setting the slider's value attribute (a rebuild: selecting, undo / redo, a re-read) also fires "change", with the
    // stored value rounded to the slider's 0.001 steps. Ignore anything within half a step of where the stored value
    // sits: else the rounded value replaced the stored one and went to the unit (after an undo's cmd 21, before the
    // unit had the block back: refused, log 2026-11-08).
    if (stored.kind == kind && std::abs(stored.v - v) < 1e-9) return;
    if (stored.kind != K::None && hi > lo && std::abs(double(std::clamp(pos, 0.f, 1.f)) - (stored.v - lo) / (hi - lo)) < 0.0006)
        return;
    beginEdit("param:" + std::to_string(index));
    stored = {kind, v};
    // Live: blocks (own knobs or the linked cab's, param set 1), split / merge and the inputs / outputs by their slot
    // (lane B's: param set 1 on the split's / merge's slot).
    if (live()) {
        if (const int slot = selectedSlotNumber(); slot >= 0) {
            const bool laneBIo = sel_.lane && (sel_.special == Special::Input || sel_.special == Special::Output);
            const int set = (editCab_ && t.block) || laneBIo ? 1 : 0;
            sendLive(hx::editor::kSetKnob, hx::editor::knobArgs(slot, set, int(k.number) - 1, toMp(stored)));
            lastKnob_ = {slot, set, int(k.number) - 1, hx::steadySeconds()};
        } else {
            notSent("lane B input / output settings");
        }
    }
    // A knob on the Snapshot controller keeps its value per snapshot.
    for (hx::ControllerAssign &c : preset_.controllers)
        if (c.source == hx::controller::Snapshot && c.param == int(k.number) - 1 && sameTarget(c.target, t.target))
            c.snapValues[std::size_t(preset_.snapshot)] = stored;
    p->value = p->irSlot ? irLabel(int(std::lround(v))) : displayValue(fw_->meta, k, v);
    p->pos = hi > lo ? float((v - lo) / (hi - lo)) : 0.f;
    setFill(*p);
    if (p->irSlot) {  // another slot: the IR / NAM mix-up warning follows (the params stay: a drag may be running)
        editHint_ = irMismatch(t.meta, t.values);
        rebuildLanes();
        if (handle_) {
            handle_.DirtyVariable("edit_hint");
            handle_.DirtyVariable("lanes");
        }
    }
    if (handle_) handle_.DirtyVariable("params");
}

// The coloured part of a slider: from the left edge to the value, or for a centred knob between the middle and the
// value, as Helix Native draws them.
void EditorView::setFill(UiParam &p) {
    const double pos = std::clamp(double(p.pos), 0.0, 1.0) * 100.0;
    const double from = p.centered ? std::min(50.0, pos) : 0.0, to = p.centered ? std::max(50.0, pos) : pos;
    p.fillLeft = percent(from);
    p.fillWidth = percent(to - from);
}

// One wheel notch: 1% of a float's range (0.1% with shift), or one step of a list or switch.
// RmlUi's delta is negative for wheel-up, which raises the value.
void EditorView::paramWheel(int index, float delta, bool fine) {
    const EditTarget t = editTarget();
    const UiParam *p = findParam(index);
    if (delta == 0 || !t.meta || !p || !p->slider || std::size_t(index) >= t.meta->knobs.size()) return;
    if (p->open) return;  // the wheel scrolls its open IR list instead
    const hx::MetaParam &k = t.meta->knobs[std::size_t(index)];
    double lo, hi, def;
    knobRange(k, lo, hi, def);
    if (hi <= lo) return;
    const bool stepped = k.type != std::uint32_t(hx::ParamType::Float);
    const float step = stepped ? float(1.0 / (hi - lo)) : (fine ? 0.001f : 0.01f);
    const float notches = delta > 0 ? -1.f : 1.f;
    paramChanged(index, std::clamp(p->pos + notches * step, 0.f, 1.f));
}

// ---------------------------------------------------------------- drag and drop

void EditorView::dragCell(int id) {
    drag_ = {};
    const int dsp = id / 100, lane = id % 100 / 10, slot = id % 10;
    if (id < 0 || dsp > 1 || lane > 1 || slot >= hx::kLaneSlots) return;
    const hx::PresetPath &p = preset_.paths[std::size_t(dsp)];
    if ((lane ? p.b : p.a).slots[std::size_t(slot)]) drag_ = {Drag::Kind::Block, id};
    setDragging(drag_.kind != Drag::Kind::None);
}

void EditorView::setDragging(bool on) {
    if (dragging_ == on) return;
    dragging_ = on;
    ghostDsp_ = -1;
    rebuildLanes();  // a serial path's lane B shows (or hides) its grey preview
    if (handle_)
        for (const char *v : {"dragging", "lanes"}) handle_.DirtyVariable(v);
}

void EditorView::setDropTarget(int id) {
    if (dropId_ == id) return;
    dropId_ = id;
    if (handle_) handle_.DirtyVariable("drop_id");
    if (!dragging_ || id < 0) return;  // between cells: the grey lane B stays as it is
    // The grey lane B follows the pointer over a serial path's lane B (its hidden input / output ends count as the
    // first slot); any other cell hides it.
    int dsp = id / 100, rest = id % 100, gDsp = -1, gSplit = 0;
    if (dsp <= 1 && rest >= 10 && rest < 20 && preset_.paths[std::size_t(dsp)].routing.mode == 0 &&
        preset_.paths[std::size_t(dsp)].b.empty()) {
        gDsp = dsp;
        gSplit = rest - 10 < hx::kLaneSlots ? rest - 10 : 0;
    }
    if (gDsp == ghostDsp_ && gSplit == ghostSplit_) return;
    ghostDsp_ = gDsp;
    ghostSplit_ = gSplit;
    rebuildLanes();
    if (handle_) handle_.DirtyVariable("lanes");
}

int EditorView::junctionOf(int id) {
    if (id < 0 || id / 100 > 1) return -1;
    const int dsp = id / 100, rest = id % 100;
    if (rest == kSplitCell || rest == 10 + kInputSlot) return dsp * 100 + kSplitCell;  // lane B input = split off
    if (rest == kJoinCell || rest == 10 + kOutputSlot) return dsp * 100 + kJoinCell;   // lane B output = merge off
    return -1;
}

// Where the pointer is decides the result:
// - in lane B's row over its input node (split) / output node (merge): split / merge off, so lane B
//   gets its own input / output;
// - anywhere else: on, at the gap nearest the pointer (gap g sits at (g + 1) * 10% of the track;
//   0 = start of the lane, 8 = after the last column).
// A split stays at or left of its merge while both are on. The mode follows the two flags.
void EditorView::moveJunction(int id, float mouseX, float mouseY, float trackLeft, float trackTop, float trackWidth,
                              float trackHeight) {
    const int j = junctionOf(id);
    if (j < 0 || trackWidth <= 0) return;
    const int dsp = j / 100;
    const bool isSplit = j % 100 == kSplitCell;
    hx::PresetPath &path = preset_.paths[std::size_t(dsp)];
    hx::PresetRouting &r = path.routing;
    if (r.mode == 0) return;  // serial: nothing to move

    const double pct = double(mouseX - trackLeft) / double(trackWidth) * 100.0;
    // Lane B's line is 17/60 of a track height below the track top, lane A's one track height
    // higher; the pointer counts as in lane B's row below the halfway mark between them.
    const bool inLaneB = mouseY - trackTop > (17.f / 60.f - 0.5f) * trackHeight;
    const bool overOwnNode = inLaneB && (isSplit ? pct < 10.0 : pct > 90.0);
    const int gap = std::clamp(int(std::lround(pct / 10.0 - 1.0)), 0, hx::kLaneSlots);

    const hx::PresetRouting before = r;
    if (isSplit) {
        if (overOwnNode) r.splitOn = false;
        else {
            r.splitOn = true;
            r.splitPos = r.joinOn ? std::min(gap, r.joinPos) : gap;
        }
    } else {
        if (overOwnNode) r.joinOn = false;
        else {
            r.joinOn = true;
            r.joinPos = r.splitOn ? std::max(gap, r.splitPos) : gap;
        }
    }
    r.mode = r.splitOn ? (r.joinOn ? 1 : 2) : (r.joinOn ? 3 : 4);
    if (r.mode == before.mode && r.splitPos == before.splitPos && r.joinPos == before.joinPos) return;
    {
        hx::Preset prev = preset_;  // preset_ already moved: save the state before this step
        prev.paths[std::size_t(dsp)].routing = before;
        beginEdit("junction", &prev);
        // Cmd 46 / 47 move the split / merge: {98: position + 1} (as the preset and notice 26 store it), or the
        // split's / merge's slot (10 / 19) to make it lane B's own input / output. Seen: the merge to 7 (hx-edit-ops),
        // split to 10 and merge to 19 (hx-edit-ops-2); the split to a position and either one back from input /
        // output follow the same pattern but weren't captured.
        // The target is a slot number with the path in it (path * 20 + position + 1; path 1's split / merge slot 10 /
        // 19 = off): a bare position on path 2 moved path 1's split (log 2026-11-08). As HX Edit, the split / merge is
        // selected on the unit first.
        if (live()) {
            const int base = dsp * hx::kSlotsPerPath;
            if (isSplit) {
                sendLive(hx::editor::kSelectBlock, hx::editor::selectArgs(base + hx::editor::kSlotInputB, 0));
                sendLive(hx::editor::kMoveSplit,
                         hx::editor::slotArgs(base + (r.splitOn ? r.splitPos + 1 : hx::editor::kSlotInputB)));
            } else {
                sendLive(hx::editor::kSelectBlock, hx::editor::selectArgs(base + hx::editor::kSlotOutputB, 0));
                sendLive(hx::editor::kMoveMerge,
                         hx::editor::slotArgs(base + (r.joinOn ? r.joinPos + 1 : hx::editor::kSlotOutputB)));
            }
        }
    }
    if (r.mode != before.mode) sel_ = {dsp, 0, 0, isSplit ? Special::Split : Special::Join};
    rebuildLanes();
    rebuildParams();
    refreshEditor();
}

void EditorView::dragModel(int index) {
    drag_ = {};
    if (fw_ && list_ >= 0 && index >= 0 && std::size_t(index) < fw_->tables.lists[std::size_t(list_)].entries.size())
        drag_ = {Drag::Kind::Model, index};
    setDragging(drag_.kind != Drag::Kind::None);
}

// Dropping on a slot inserts there (occupied slots make room, see PresetLane::insert); on the
// input node the block goes first, on the output node last.
void EditorView::dropOnCell(int id) {
    const Drag drag = drag_;
    drag_ = {};
    setDragging(false);
    const int dsp = id / 100, lane = id % 100 / 10, slot = id % 10;
    if (drag.kind == Drag::Kind::None || id < 0 || dsp > 1 || lane > 1 || !fw_) return;
    const int at = slot == kInputSlot ? 0 : slot == kOutputSlot ? hx::kLaneSlots - 1 : slot;
    hx::PresetPath &path = preset_.paths[std::size_t(dsp)];
    hx::PresetLane &target = lane ? path.b : path.a;

    std::optional<hx::PresetBlock> block;
    hx::PresetLane *source = nullptr;
    if (drag.kind == Drag::Kind::Block) {
        if (drag.id == id) return;  // dropped on itself
        hx::PresetPath &sp = preset_.paths[std::size_t(drag.id / 100)];
        source = drag.id % 100 / 10 ? &sp.b : &sp.a;
    }
    // A full lane only takes a block that is moving within it (its own slot frees up).
    if (target.full() && source != &target) {
        setStatus("Lane " + std::to_string(dsp + 1) + (lane ? "B" : "A") + " is full");
        return;
    }
    beginEdit();
    // Live: the path's new layout is replayed on the unit as block moves (cmd 43, lane A <-> B too), then a new
    // block's model goes into its slot. To the other path HX Edit sends the whole preset (cmd 21), which the
    // editor can't write yet.
    const bool otherPath = source && drag.id / 100 != dsp;
    const std::map<int, int> before = blockSlots(dsp);
    if (source) {
        block = source->take(drag.id % 10);
    } else {
        const hx::BrowserEntry &e = fw_->tables.lists[std::size_t(list_)].entries[std::size_t(drag.id)];
        if (const hx::ModelRef *m = fw_->modelBySym(e.symId)) {
            block = newBlock(*m);
            if (e.cabSymId != hx::kNoCab)
                if (const hx::ModelRef *cab = fw_->modelBySym(e.cabSymId)) {
                    block->cabModelNo = cab->modelNo;
                    block->cabValues = defaultValues(cab->meta);
                }
        }
    }
    if (!block) return;

    const int placed = target.insert(at, std::move(*block));  // can't fail: a slot is free
    hx::normalizeRouting(path);
    if (source && drag.id / 100 != dsp) hx::normalizeRouting(preset_.paths[std::size_t(drag.id / 100)]);
    if (undoIfDspFull(dsp)) return;
    pickCell(dsp * 100 + lane * 10 + (placed >= 0 ? placed : at));
    if (!live()) return;
    // To the other path: the whole preset, as HX Edit does (the block keeps its knobs and its controller / footswitch
    // assignments).
    if (otherPath) return void(sendWholePreset("moving a block to the other path"));
    // Within the path: moves (cmd 43), making room first.
    const std::optional<std::vector<std::pair<int, int>>> moves = planMoves(before, blockSlots(dsp), dsp);
    if (!moves) return void(sendWholePreset("this move"));
    for (const auto &[from, to] : *moves) sendLive(hx::editor::kMoveBlock, hx::editor::moveArgs(from, to));
    if (source) return;
    // A new block from the browser: its model into the slot.
    const std::optional<hx::PresetBlock> *s = selectedSlot();
    if (s && *s) sendBlockAt(selectedSlotNumber(), **s);
}

// Each block of a path by uid -> its global slot (path * 20 + 1..8 lane A, 11..18 lane B).
std::map<int, int> EditorView::blockSlots(int dsp) const {
    std::map<int, int> out;
    const hx::PresetPath &path = preset_.paths[std::size_t(dsp)];
    for (int lane = 0; lane < 2; ++lane)
        for (int i = 0; i < hx::kLaneSlots; ++i)
            if (const auto &b = (lane ? path.b : path.a).slots[std::size_t(i)])
                out[b->uid] = dsp * hx::kSlotsPerPath + (lane ? 11 : 1) + i;
    return out;
}

// Moves (from, to) that take the unit from `before` to `after` (uid -> slot), each onto an empty slot. Blocks only
// in `after` (new ones) are left out. When the moves form a ring (a full lane rotating), one block steps aside to a
// free slot of the path first; nullopt if there is none.
std::optional<std::vector<std::pair<int, int>>> EditorView::planMoves(std::map<int, int> cur,
                                                                       const std::map<int, int> &after, int dsp) {
    std::vector<std::pair<int, int>> moves;
    std::set<int> used;
    for (const auto &[uid, s] : cur) used.insert(s);
    for (const auto &[uid, s] : after)
        if (!cur.count(uid)) used.insert(s);  // a new block's slot: keep it free for its model
    auto step = [&](int uid, int to) {
        int &from = cur[uid];
        moves.emplace_back(from, to);
        used.erase(from);
        used.insert(to);
        from = to;
    };
    for (;;) {
        bool pending = false, progress = false;
        for (const auto &[uid, to] : after) {
            const auto it = cur.find(uid);
            if (it == cur.end() || it->second == to) continue;
            pending = true;
            if (!used.count(to)) {
                step(uid, to);
                progress = true;
            }
        }
        if (!pending) return moves;
        if (progress) continue;
        int hole = -1;
        for (int lane = 0; lane < 2 && hole < 0; ++lane)
            for (int i = 0; i < hx::kLaneSlots && hole < 0; ++i)
                if (const int s = dsp * hx::kSlotsPerPath + (lane ? 11 : 1) + i; !used.count(s)) hole = s;
        if (hole < 0) return std::nullopt;
        for (const auto &[uid, to] : after)
            if (const auto it = cur.find(uid); it != cur.end() && it->second != to) {
                step(uid, hole);
                break;
            }
    }
}

void EditorView::refresh() {
    if (handle_) handle_.DirtyAllVariables();
}

// What a click, drop, bypass or junction move can change: the flow and the edit area, not the preset and setlist
// lists. DirtyAllVariables re-checks every binding in the document (128 presets, the model list, ...), which is
// what made dragging and clicking feel heavy.
void EditorView::refreshEditor() {
    updateFits();
    rebuildCommandGrid();
    rebuildAssignTable();
    if (!handle_) return;
    for (const char *v : {"lanes", "drop_id", "dsp1", "dsp2", "dsp1_w", "dsp2_w", "cat_sel", "subs", "sub_label",
                          "sub_open", "setlist_open", "models", "model_sel", "has_block", "block_on", "block_title", "can_copy_block", "can_paste_block", "categories", "can_save", "can_send_preset",
                          "input_settings",
                          "has_cab", "edit_cab", "main_label", "cab_label",
                          "block_cls", "edit_hint", "params", "edit_tab",
                          "ca_rows", "ca_sort", "ca_sort_asc", "ca_has_sel", "ca_block", "ca_block_cls", "ca_param",
                          "ca_source", "ca_kind", "ca_min_name", "ca_max_name", "ca_min_text", "ca_max_text",
                          "ca_min_pos", "ca_max_pos", "ca_cc", "ca_label", "ca_led", "ca_momentary",
                          "cc_top", "cc_row1", "cc_row2", "cc_pedals", "cc_has_sel", "cc_sel_name", "cc_sel_type",
                          "cc_fields", "cc_is_switch", "cc_momentary", "cc_label", "cc_led", "cc_snap"})
        handle_.DirtyVariable(v);
}

} // namespace editor

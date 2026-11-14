#include "frontend/irview.h"

#include "core/ir.h"
#include "core/nam.h"
#include "core/text.h"
#include "luminoveau.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <SDL3/SDL_dialog.h>

#include <cstdio>
#include <fstream>
#include <iterator>

namespace editor {

void IrView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("slot", &Row::slot);
        s.RegisterMember("number", &Row::number);
        s.RegisterMember("name", &Row::name);
        s.RegisterMember("hash", &Row::hash);
        s.RegisterMember("empty", &Row::empty);
        s.RegisterMember("kind", &Row::kind);
        s.RegisterMember("shown", &Row::shown);
    }
    model.RegisterArray<std::vector<Row>>();
    // std::vector<std::string> is registered by ShellView.
    model.Bind("ir_connected", &connected_);
    model.Bind("ir_status", &status_);
    model.Bind("ir_rows", &rows_);
    model.Bind("ir_summary", &summary_);
    model.Bind("ir_sel", &sel_);
    model.Bind("ir_sel_name", &selName_);
    model.Bind("ir_sel_hash", &selHash_);
    model.Bind("ir_sel_number", &selNumber_);
    model.Bind("ir_sel_kind", &selKind_);
    model.Bind("ir_sel_details", &selDetails_);
    model.Bind("ir_filter", &filter_);
    model.BindEventCallback("ir_set_filter", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (!a.empty()) filter_ = a[0].Get<Rml::String>("all");
        applyFilter();
        dirty();
    });
    model.Bind("ir_name_edit", &nameEdit_);
    model.Bind("ir_question", &question_);
    model.Bind("ir_notes", &notes_);
    model.Bind("ir_busy", &busy_);
    model.Bind("ir_message", &message_);
    model.BindFunc("ir_asking", [this](Rml::Variant &v) { v = action_ != Action::None; });
    model.BindFunc("ir_sel_empty", [this](Rml::Variant &v) {
        v = sel_ < 0 || std::size_t(sel_) >= rows_.size() || rows_[std::size_t(sel_)].empty;
    });

    using Args = const Rml::VariantList &;
    model.BindEventCallback("ir_pick", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        select(a.empty() ? -1 : a[0].Get<int>(-1));
    });
    model.BindEventCallback("ir_refresh", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (connected_ && !busy_ && onRefresh) onRefresh();
    });
    model.BindEventCallback("ir_import", [this](Rml::DataModelHandle, Rml::Event &, Args) { chooseWav(); });
    // Right-click on a slot: the same actions as the buttons (each still asks before writing).
    model.BindEventCallback("ir_menu", [this](Rml::DataModelHandle, Rml::Event &ev, Args a) {
        const int slot = a.empty() ? -1 : a[0].Get<int>(-1);
        if (!menu_ || !ContextMenu::isRightClick(ev) || slot < 0 || std::size_t(slot) >= rows_.size()) return;
        select(slot);
        const Row &r = rows_[std::size_t(slot)];
        const bool can = connected_ && !busy_;
        std::vector<ContextMenu::Item> items;
        items.push_back({"Import WAV / NAM...", [this] { chooseWav(); }, can});
        items.push_back({"Rename", [this] {
                             const Row &cur = rows_[std::size_t(sel_)];
                             if (hx::irName(nameEdit_) == cur.name) {
                                 message_ = "Type the new name under \"New name\", then Rename.";
                                 dirty();
                             } else {
                                 ask(Action::Rename);
                             }
                         },
                         can && !r.empty});
        items.push_back({"Clear", [this] { ask(Action::Clear); }, can && !r.empty});
        menu_->openAt(ev, std::move(items), "Slot " + r.number + (r.empty ? "" : "  " + r.name));
    });
    model.BindEventCallback("ir_rename", [this](Rml::DataModelHandle, Rml::Event &, Args) { ask(Action::Rename); });
    model.BindEventCallback("ir_clear", [this](Rml::DataModelHandle, Rml::Event &, Args) { ask(Action::Clear); });
    model.BindEventCallback("ir_confirm", [this](Rml::DataModelHandle, Rml::Event &, Args) { confirm(); });
    model.BindEventCallback("ir_cancel", [this](Rml::DataModelHandle, Rml::Event &, Args) { cancel(); });
    // The name field (rename, or the name an import gets): kept as typed, cleaned when sent.
    model.BindEventCallback("ir_name", [this](Rml::DataModelHandle, Rml::Event &ev, Args) {
        nameEdit_ = ev.GetParameter<Rml::String>("value", "");
        if (action_ == Action::Upload || action_ == Action::Rename) ask(action_);  // the question names it
    });
    handle_ = model.GetModelHandle();
    setIrs({});
}

void IrView::setConnection(bool connected, const std::string &text) {
    connected_ = connected;
    status_ = text;
    if (!connected) {
        busy_ = false;
        cancel();
        setIrs({});  // the list is the unit's: nothing to show without it
    }
    dirty();
}

// Slot numbers as the unit shows them: 1-128.
void IrView::setIrs(const std::vector<hx::editor::IrSlot> &irs) {
    rows_.assign(hx::editor::kIrSlots, Row{});
    for (int i = 0; i < hx::editor::kIrSlots; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%03d", i + 1);
        rows_[std::size_t(i)].slot = i;
        rows_[std::size_t(i)].number = buf;
    }
    int used = 0;
    for (const hx::editor::IrSlot &s : irs)
        if (s.slot >= 0 && s.slot < hx::editor::kIrSlots) {
            Row &r = rows_[std::size_t(s.slot)];
            r.name = s.name;
            r.hash = s.hash;
            r.empty = false;
            using K = hx::editor::IrSlot::Kind;
            r.kind = s.kind == K::Nam ? "nam" : s.kind == K::Ir ? "ir" : "";
            r.loudness = s.namLoudness;
            r.levelDbu = s.namLevelDbu;
            ++used;
        }
    int nams = 0;
    for (const Row &r : rows_) nams += r.kind == "nam";
    summary_ = connected_ ? std::to_string(used) + " of " + std::to_string(hx::editor::kIrSlots) + " slots used" +
                                (nams ? ", " + std::to_string(nams) + " NAM" : "")
                          : "";
    applyFilter();
    // A write finished: say how it went (an upload can be checked by its hash).
    if (busy_) {
        busy_ = false;
        if (!wavHash_.empty() && sel_ >= 0) {
            const Row &r = rows_[std::size_t(sel_)];
            message_ = r.hash == wavHash_ ? "Written to slot " + r.number + " (hash checked)."
                                          : "Slot " + r.number + " doesn't show the uploaded IR: check Monitor > USB.";
        } else {
            message_ = "Done.";
        }
        wavHash_.clear();
    }
    if (sel_ >= 0) select(sel_);
    dirty();
}

void IrView::select(int slot) {
    if (slot < 0 || std::size_t(slot) >= rows_.size()) return;
    if (slot != sel_) {
        cancel();
        message_.clear();
    }
    sel_ = slot;
    const Row &r = rows_[std::size_t(slot)];
    selNumber_ = r.number;
    selName_ = r.empty ? "(empty)" : r.name;
    selHash_ = r.hash;
    selKind_ = r.empty ? "Empty" : r.kind == "nam" ? "NAM capture (for the NAM block)" : r.kind == "ir" ? "IR" : "Checking...";
    selDetails_.clear();
    if (r.kind == "nam") {
        char buf[64];
        std::snprintf(buf, sizeof buf, "Loudness %.1f dB", r.loudness);
        selDetails_.push_back(buf);
        if (r.levelDbu != 0) {
            std::snprintf(buf, sizeof buf, "Input level %.1f dBu", r.levelDbu);
            selDetails_.push_back(buf);
        }
    }
    if (action_ == Action::None) nameEdit_ = r.empty ? "" : r.name;
    dirty();
}

// Rows the filter lets through: all, IRs (and empty slots: somewhere to import to), NAM captures.
void IrView::applyFilter() {
    for (Row &r : rows_)
        r.shown = filter_ == "all" || (filter_ == "nam" ? r.kind == "nam" : r.kind != "nam");
}

void IrView::chooseWav() {
    if (!connected_ || busy_ || sel_ < 0) return;
    static const SDL_DialogFileFilter filters[] = {{"IR or NAM capture", "wav;nam"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(
        [](void *self, const char *const *files, int) {
            if (!files || !files[0]) return;
            auto *view = static_cast<IrView *>(self);
            std::lock_guard lock(view->pickMutex_);
            view->picked_ = files[0];
        },
        this, Window::GetWindow(), filters, 2, nullptr, false);
}

void IrView::update() {
    std::string picked;
    {
        std::lock_guard lock(pickMutex_);
        picked.swap(picked_);
    }
    if (!picked.empty()) loadFile(picked);
}

namespace {
std::string lowerExtension(const std::string &path) {
    const std::u8string e = hx::pathFromUtf8(path).extension().u8string();
    std::string s(e.begin(), e.end());
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
} // namespace

bool IrView::handles(const std::string &path) {
    const std::string e = lowerExtension(path);
    return e == ".wav" || e == ".nam";
}

void IrView::openFile(const std::string &path, int slot) {
    if (!connected_) {
        message_ = "Connect the Helix first: the IR slots are on it.";
        dirty();
        return;
    }
    if (busy_) return;
    if (slot >= 0) select(slot);
    if (sel_ < 0) {
        // No slot under the pointer and none selected: the first empty one.
        for (const Row &r : rows_)
            if (r.empty) {
                select(r.slot);
                break;
            }
    }
    if (sel_ < 0) {
        message_ = "All 128 slots are used: pick one to replace.";
        dirty();
        return;
    }
    loadFile(path);
}

// Converted at once (it's small); the result waits for the confirmation. A .nam capture becomes the slot data of our
// NAM block (named like nam2wav.py's files: "<file> NAM"); a .wav is an ordinary IR.
void IrView::loadFile(const std::string &path) {
    const std::filesystem::path p = hx::pathFromUtf8(path);
    std::ifstream in(p, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::u8string stem8 = p.stem().u8string();
    const std::string stem(stem8.begin(), stem8.end());
    std::string error;
    if (lowerExtension(path) == ".nam") {
        const std::optional<hx::NamIr> nam =
            hx::namToIr(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), &error);
        if (!nam) {
            message_ = "Can't use that capture: " + error;
            cancel();
            return;
        }
        wavData_ = hx::irBytes(nam->samples);
        // The capture card: what the file says about itself, then what it means for the slot.
        notes_.clear();
        notes_.push_back("NAM capture for the NAM block (" + std::to_string(nam->weights) + " weights), not a cab IR");
        if (!nam->name.empty()) notes_.push_back("Name: " + nam->name);
        if (!nam->modeledBy.empty()) notes_.push_back("Captured by: " + nam->modeledBy);
        std::string gear = nam->gearMake + (nam->gearMake.empty() || nam->gearModel.empty() ? "" : " ") + nam->gearModel;
        if (!nam->gearType.empty()) gear += (gear.empty() ? "" : " ") + std::string("(") + nam->gearType + ")";
        if (!gear.empty()) notes_.push_back("Gear: " + gear);
        if (!nam->toneType.empty()) notes_.push_back("Tone: " + nam->toneType);
        char buf[96];
        if (nam->loudness != 0) {
            std::snprintf(buf, sizeof buf, "Loudness %.1f dB", nam->loudness);
            notes_.push_back(buf);
        }
        if (nam->levelDbu != 0) {
            std::snprintf(buf, sizeof buf, "Captured at input level %.1f dBu", nam->levelDbu);
            notes_.push_back(buf);
        }
        if (nam->sampleRate != 0 && nam->sampleRate != 48000) {
            std::snprintf(buf, sizeof buf, "Made at %.0f Hz; the Helix runs at 48 kHz, so it will sound a little off",
                          nam->sampleRate);
            notes_.push_back(buf);
        }
        if (const std::string hint = hx::namGearHint(nam->gearType); !hint.empty()) notes_.push_back(hint);
        if (nam->flushed) notes_.push_back(std::to_string(nam->flushed) + " tiny (denormal) weights set to 0");
        if (sel_ >= 0 && std::size_t(sel_) < rows_.size() && rows_[std::size_t(sel_)].kind == "ir")
            notes_.push_back("This slot holds an IR now: blocks using it as an IR would play this capture as a loud burst");
        // Proposed slot name: gear-type prefix + its name (P / A / AC / PA / PAC / Pre), editable in the name field.
        nameEdit_ = hx::namSlotName(nam->gearType, nam->name.empty() ? stem : nam->name);
    } else {
        const std::optional<hx::IrData> ir = hx::irFromWav(bytes, &error);
        if (!ir) {
            message_ = "Can't use that file: " + error;
            cancel();
            return;
        }
        wavData_ = hx::irBytes(ir->samples);
        notes_ = ir->notes;
        nameEdit_ = hx::irName(stem);
    }
    message_.clear();
    ask(Action::Upload);
}

void IrView::ask(Action action) {
    if (!connected_ || busy_ || sel_ < 0) return;
    const Row &r = rows_[std::size_t(sel_)];
    const std::string slot = "slot " + r.number;
    const std::string replaces = r.empty ? "" : " It replaces \"" + r.name + "\" (presets using that IR will use the new one).";
    switch (action) {
    case Action::Upload:
        question_ = "Write \"" + hx::irName(nameEdit_) + "\" to " + slot + " on the Helix?" + replaces;
        break;
    case Action::Rename:
        if (r.empty) return;
        question_ = "Rename " + slot + " on the Helix to \"" + hx::irName(nameEdit_) + "\"?";
        break;
    case Action::Clear:
        if (r.empty) return;
        question_ = "Clear " + slot + " (\"" + r.name + "\") on the Helix? Presets using it lose their IR.";
        break;
    case Action::None: return;
    }
    if (action != Action::Upload) notes_.clear();
    action_ = action;
    dirty();
}

void IrView::confirm() {
    if (action_ == Action::None || !connected_ || busy_ || sel_ < 0 || !onWrite) return;
    const std::string name = hx::irName(nameEdit_);
    switch (action_) {
    case Action::Upload:
        wavHash_ = hx::irHash(wavData_);
        onWrite(hx::editor::kIrUpload, hx::editor::irUploadArgs(sel_, name, wavData_, hx::irSum(wavData_)));
        break;
    case Action::Rename: onWrite(hx::editor::kIrRename, hx::editor::irRenameArgs(sel_, name)); break;
    case Action::Clear: onWrite(hx::editor::kIrClear, hx::editor::irClearArgs(sel_)); break;
    case Action::None: return;
    }
    action_ = Action::None;
    question_.clear();
    notes_.clear();
    wavData_.clear();
    busy_ = true;
    message_ = "Writing to the Helix...";
    dirty();
}

void IrView::cancel() {
    action_ = Action::None;
    question_.clear();
    notes_.clear();
    wavData_.clear();
    if (sel_ >= 0 && std::size_t(sel_) < rows_.size()) nameEdit_ = rows_[std::size_t(sel_)].empty ? "" : rows_[std::size_t(sel_)].name;
    dirty();
}

void IrView::dirty() {
    if (!handle_) return;
    for (const char *v : {"ir_connected", "ir_status", "ir_rows", "ir_summary", "ir_sel", "ir_sel_name", "ir_sel_hash",
                          "ir_sel_number", "ir_name_edit", "ir_question", "ir_notes", "ir_busy", "ir_message",
                          "ir_asking", "ir_sel_empty"})
        handle_.DirtyVariable(v);
}

} // namespace editor

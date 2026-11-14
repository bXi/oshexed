#include "frontend/flashview.h"

#include "core/hxf.h"
#include "core/text.h"
#include "luminoveau.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace editor {

namespace {

constexpr std::size_t kLogLines = 200;

std::string megabytes(std::size_t n) {
    char buf[32];
    if (n >= 1000 * 1000) std::snprintf(buf, sizeof buf, "%.1f MB", double(n) / 1e6);
    else std::snprintf(buf, sizeof buf, "%.1f KB", double(n) / 1e3);
    return buf;
}

std::string mmss(double t) {
    const int s = std::max(0, int(t));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

std::string percent(double f) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.1f%%", 100.0 * std::clamp(f, 0.0, 1.0));
    return buf;
}

std::string fileName(const std::string &path) {
    const std::u8string name = hx::pathFromUtf8(path).filename().u8string();
    return std::string(name.begin(), name.end());
}

const char *kVariables[] = {"fl_file",   "fl_status",   "fl_summary",       "fl_presets_text", "fl_phase",
                            "fl_ready",  "fl_fast",     "fl_slots",         "fl_sim",          "fl_can_stop",
                            "fl_recovery", "fl_busy",   "fl_presets",       "fl_rows",         "fl_progress_w",
                            "fl_progress_text", "fl_result", "fl_device",   "fl_log"};

} // namespace

void FlashView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("name", &Row::name);
        s.RegisterMember("size", &Row::size);
        s.RegisterMember("plan", &Row::plan);
        s.RegisterMember("step", &Row::step);
        s.RegisterMember("cls", &Row::cls);
        s.RegisterMember("bar", &Row::bar);
    }
    model.RegisterArray<std::vector<Row>>();
    // std::vector<std::string> is registered by ShellView.

    model.Bind("fl_file", &file_);
    model.Bind("fl_status", &status_);
    model.Bind("fl_summary", &summary_);
    model.Bind("fl_presets_text", &presetsText_);
    model.Bind("fl_phase", &phase_);
    model.Bind("fl_ready", &ready_);
    model.Bind("fl_fast", &fast_);
    model.Bind("fl_slots", &checkSlots_);
    model.Bind("fl_sim", &sim_);
    model.Bind("fl_can_stop", &canStop_);
    model.Bind("fl_recovery", &recovery_);
    model.Bind("fl_busy", &busy_);
    model.Bind("fl_presets", &presetsMode_);
    model.Bind("fl_rows", &rows_);
    model.Bind("fl_progress_w", &progressW_);
    model.Bind("fl_progress_text", &progressText_);
    model.Bind("fl_result", &result_);
    model.Bind("fl_device", &deviceText_);
    model.Bind("fl_log", &log_);

    using Args = const Rml::VariantList &;
    model.BindEventCallback("fl_choose", [this](Rml::DataModelHandle, Rml::Event &, Args) { choose(); });
    model.BindEventCallback("fl_info", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (device_.readInfo()) {
            log_.clear();
            addLog("Read unit info (every USB message is listed below):");
            deviceText_ = "Reading from the unit...";
            dirtyAll();
        }
    });
    model.BindEventCallback("fl_toggle", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        if (a.empty() || phase_ == "running" || phase_ == "confirm") return;
        const Rml::String what = a[0].Get<Rml::String>();
        if (what == "fast") fast_ = !fast_;
        else if (what == "slots") checkSlots_ = !checkSlots_;
        replan();
    });
    model.BindEventCallback("fl_presets_mode", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        if (a.empty() || phase_ == "running" || phase_ == "confirm") return;
        presetsMode_ = std::clamp(a[0].Get<int>(), 0, 2);
        replan();
    });
    model.BindEventCallback("fl_ask", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (!ready_ || planned_.empty() || device_.busy() || phase_ == "running") return;
        phase_ = "confirm";
        dirtyAll();
    });
    model.BindEventCallback("fl_cancel", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (phase_ != "confirm") return;
        phase_ = "idle";
        dirtyAll();
    });
    model.BindEventCallback("fl_confirm", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (phase_ == "confirm") start(false);
    });
    model.BindEventCallback("fl_rehearse", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (phase_ == "idle" || phase_ == "done" || phase_ == "failed") start(true);
    });
    model.BindEventCallback("fl_stop", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (!canStop_) return;
        device_.requestStop();
        addLog("Stop requested: the flash ends before it touches the next slot.");
        canStop_ = false;
        dirtyAll();
    });
    model.BindEventCallback("fl_assume", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (!ready_ || device_.busy()) return;
        device_.assume(files_, file_);
        addLog("Noted: the unit runs " + file_ + " in every slot.");
        replan();
    });
    model.BindEventCallback("fl_dismiss", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (phase_ != "done" && phase_ != "failed") return;
        phase_ = "idle";
        replan();
    });
    handle_ = model.GetModelHandle();
}

void FlashView::dirtyAll() {
    if (!handle_) return;
    for (const char *v : kVariables) handle_.DirtyVariable(v);
}

void FlashView::addLog(const std::string &s) {
    log_.push_back(s);
    if (log_.size() > kLogLines) log_.erase(log_.begin(), log_.begin() + std::ptrdiff_t(log_.size() - kLogLines));
    if (handle_) handle_.DirtyVariable("fl_log");
}

void FlashView::setEditorFile(const std::string &path) {
    if (!userPicked_) setPath(path);
}

void FlashView::useFile(const std::string &path) {
    if (phase_ == "running" || phase_ == "confirm") return;
    userPicked_ = true;
    if (path == path_) path_.clear();  // rebuilt at the same name: read it again
    setPath(path);
}

void FlashView::setPath(const std::string &path) {
    if (path == path_ || phase_ == "running" || phase_ == "confirm") return;
    path_ = path;
    file_ = fileName(path);
    files_.clear();
    loadedPath_.clear();
    rows_.clear();
    planned_.clear();
    ready_ = false;
    status_.clear();
    summary_.clear();
    presetsText_.clear();
    dirtyAll();
}

void FlashView::choose() {
    if (phase_ == "running" || phase_ == "confirm") return;
    static const SDL_DialogFileFilter filters[] = {{"Helix firmware", "hxf"}, {"All files", "*"}};
    // The callback may run on another thread: it only hands the path over.
    SDL_ShowOpenFileDialog(
        [](void *self, const char *const *files, int) {
            if (!files || !files[0]) return;
            auto *view = static_cast<FlashView *>(self);
            std::lock_guard lock(view->pickMutex_);
            view->picked_ = files[0];
        },
        this, Window::GetWindow(), filters, 2, nullptr, false);
}

int FlashView::rowOf(std::uint32_t record) const {
    for (std::size_t i = 0; i < files_.size(); ++i)
        if (files_[i].index == record) return int(i);
    return -1;
}

void FlashView::replan() {
    if (device_.busy()) return;  // the flash thread owns the unit state until it ends
    rows_.clear();
    planned_.clear();
    totalBytes_ = 0;
    const hx::UnitState &state = device_.unitState();
    for (const hx::RecordFile &f : files_) {
        Row r;
        r.name = std::to_string(f.index) + "  " + hx::recordName(f.index);
        r.size = megabytes(f.data.size());
        const bool skip = fast_ && state.same(f);
        r.plan = skip ? "on the unit: skipped" : "write";
        r.cls = skip ? "skip" : "";
        if (!skip) {
            planned_.push_back(f.index);
            totalBytes_ += f.data.size();
        }
        rows_.push_back(std::move(r));
    }
    const hx::PresetsChoice pc = hx::presetsDecision(hx::PresetsMode(presetsMode_), files_, state);
    keepPresets_ = pc.keep;
    presetsText_ = std::string(pc.keep ? "kept" : "rebuilt") + " (" + pc.reason + ")";
    ready_ = !files_.empty();
    if (!ready_) summary_.clear();
    else if (planned_.empty()) summary_ = "Nothing to write: every record is already on the unit.";
    else
        summary_ = "Writes " + std::to_string(planned_.size()) + " of " + std::to_string(files_.size()) + " records, " +
                   megabytes(totalBytes_) + ".";
    if (ready_ && fast_ && state.empty())
        summary_ += " This program hasn't flashed the unit yet, so everything is written. If the unit already runs "
                    "this file (e.g. from the Line 6 Updater), click \"The unit runs this file\".";
    dirtyAll();
}

void FlashView::start(bool simulate) {
    if (!ready_ || planned_.empty() || device_.busy()) return;
    hx::FlashOptions opt;
    opt.checkSlots = checkSlots_;
    opt.keepPresets = keepPresets_;
    opt.sourceName = file_;
    for (const hx::RecordFile &f : files_)
        if (std::find(planned_.begin(), planned_.end(), f.index) == planned_.end()) opt.skip.insert(f.index);
    if (!device_.flash(files_, opt, simulate)) return;

    sim_ = simulate;
    phase_ = "running";
    wrote_ = false;
    canStop_ = true;
    recovery_ = false;
    result_.clear();
    log_.clear();
    doneBytes_ = recordBase_ = 0;
    startedAt_ = hx::steadySeconds();
    progressW_ = "0%";
    stepText_ = "Connecting...";
    progressText_ = stepText_;
    for (Row &r : rows_) {
        r.step = r.cls == "skip" ? "" : "waiting";
        r.bar = "0%";
    }
    addLog(simulate ? "Rehearsal against the fake unit: nothing is sent to the Helix."
                    : "Flashing " + path_ + " (presets " + presetsText_ + ").");
    dirtyAll();
}

void FlashView::handle(const hx::DeviceEvent &e) {
    using K = hx::DeviceEvent::Kind;
    const int row = rowOf(e.record);
    switch (e.kind) {
    case K::Log:
        addLog(e.text);
        break;
    case K::Info:
        deviceText_ = "Helix " + e.info.product + ", firmware " + e.info.versionText() + " (build " +
                      [&] {
                          char b[16];
                          std::snprintf(b, sizeof b, "%08x", unsigned(e.info.build));
                          return std::string(b);
                      }() +
                      ")";
        break;
    case K::Record:
        if (e.step == hx::RecordStep::Prepare) {
            recordBase_ = doneBytes_;
            canStop_ = false;  // the flasher no longer stops: the next step wipes a slot header
            if (!sim_) wrote_ = true;
        }
        if (row >= 0) {
            Row &r = rows_[std::size_t(row)];
            switch (e.step) {
            case hx::RecordStep::Prepare: r.step = "preparing"; break;
            case hx::RecordStep::Writing: r.step = "writing"; break;
            case hx::RecordStep::Verify: r.step = "verifying"; break;
            case hx::RecordStep::Done:
                r.step = "done";
                r.bar = "100%";
                r.cls = "done";
                break;
            }
            stepText_ = "Record " + std::to_string(e.record) + ": " + r.step;
        }
        break;
    case K::RecordBytes:
        doneBytes_ = recordBase_ + e.done;
        if (row >= 0) {
            rows_[std::size_t(row)].bar = percent(e.total ? double(e.done) / double(e.total) : 0);
            rows_[std::size_t(row)].step = "writing " + rows_[std::size_t(row)].bar;
        }
        break;
    case K::SlotCheck:
        stepText_ = "Checking slot " + std::to_string(e.done + 1) + " of " + std::to_string(e.total);
        break;
    case K::Done:
        phase_ = "done";
        canStop_ = false;
        progressW_ = "100%";
        progressText_ = mmss(hx::steadySeconds() - startedAt_) + " in all";
        result_ = sim_ ? "Rehearsal passed: every record and checksum went through the fake unit. Nothing was sent."
                       : e.text;
        addLog(result_);
        break;
    case K::Failed:
        if (phase_ != "running") {
            deviceText_ = e.text;  // the read-only info request
            break;
        }
        phase_ = "failed";
        canStop_ = false;
        result_ = e.text;
        recovery_ = e.wrote && !sim_;
        addLog("Failed: " + e.text);
        break;
    }
}

void FlashView::update(bool visible) {
    {
        std::string picked;
        {
            std::lock_guard lock(pickMutex_);
            picked.swap(picked_);
        }
        if (!picked.empty()) {
            userPicked_ = true;
            setPath(picked);
        }
    }

    // Read the file in the background when the tab is shown: checks every record before it can be flashed.
    if (visible && !path_.empty() && loadedPath_ != path_ && !preparing_.valid()) {
        preparingPath_ = path_;
        status_ = "Reading " + file_ + "...";
        dirtyAll();
        preparing_ = std::async(std::launch::async, [p = path_] {
            Prepared r;
            std::string error;
            std::unique_ptr<hx::HxfImage> img = hx::loadHxfFile(p, &error);
            if (!img) {
                r.error = error;
                return r;
            }
            try {
                r.files = hx::recordFiles(*img);
            } catch (const std::exception &e) {
                r.error = e.what();
            }
            return r;
        });
    }
    if (preparing_.valid() && preparing_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Prepared r = preparing_.get();
        loadedPath_ = preparingPath_;
        if (preparingPath_ == path_) {
            files_ = std::move(r.files);
            status_ = r.error.empty() ? "" : "Can't flash this file: " + r.error;
            replan();
        }
    }

    for (const hx::DeviceEvent &e : device_.takeEvents()) {
        handle(e);
        dirtyAll();
    }
    if (busy_ != device_.busy()) {
        busy_ = device_.busy();
        dirtyAll();
    }
    if (phase_ == "running") {
        const double f = totalBytes_ ? double(doneBytes_) / double(totalBytes_) : 0;
        const double el = hx::steadySeconds() - startedAt_;
        std::string text = mmss(el) + " elapsed";
        if (f > 0.02) text += ", about " + mmss(el / f - el) + " left";
        text += "  |  " + stepText_;
        const std::string w = percent(f);
        if (w != progressW_ || text != progressText_) {
            progressW_ = w;
            progressText_ = text;
            if (handle_) {
                handle_.DirtyVariable("fl_progress_w");
                handle_.DirtyVariable("fl_progress_text");
                handle_.DirtyVariable("fl_rows");
            }
        }
    }
}

} // namespace editor

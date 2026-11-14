#include "frontend/baseview.h"

#include "core/firmware.h"
#include "core/text.h"
#include "core/unitprofile.h"

#include <RmlUi/Core/DataModelHandle.h>

#include <chrono>
#include <filesystem>

namespace editor {

void BaseView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("label", &Row::label);
        s.RegisterMember("value", &Row::value);
    }
    model.RegisterArray<std::vector<Row>>();
    model.Bind("base_headline", &headline_);
    model.Bind("base_detail", &detail_);
    model.Bind("base_tone", &tone_);
    model.Bind("base_rows", &rows_);
    model.Bind("base_ok", &ok_);
    model.Bind("base_can_use_open", &canUseOpen_);
    model.Bind("base_busy", &busy_);
    using Args = const Rml::VariantList &;
    model.BindEventCallback("base_choose", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (onChoose) onChoose();
    });
    model.BindEventCallback("base_use_open", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (canUseOpen_) setPath(openPath_);
    });
    model.BindEventCallback("base_flash", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (ok_ && onFlash) onFlash(info_.path);
    });
    handle_ = model.GetModelHandle();
}

void BaseView::setPath(const std::string &path) {
    if (job_.valid()) job_.wait();  // a check still running: this one replaces it
    info_ = {path, {}};
    if (path.empty()) {
        info_.stock.why = "no base firmware chosen";
        rebuild();
        if (onChanged) onChanged(info_);
        return;
    }
    busy_ = true;
    rebuild();
    job_ = std::async(std::launch::async, [path] {
        Info i{path, {}};
        std::error_code ec;
        if (!std::filesystem::is_regular_file(hx::pathFromUtf8(path), ec)) i.stock.why = "the file isn't there any more";
        else i.stock = hx::mods::stockOfFile(path);
        return i;
    });
}

void BaseView::setOpenFile(const hx::Firmware *fw) {
    openPath_ = fw ? fw->path : std::string();
    openIsStock_ = fw && hx::mods::stockOf(*fw).ok;
    rebuild();
}

void BaseView::update() {
    if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    info_ = job_.get();
    busy_ = false;
    rebuild();
    if (onChanged) onChanged(info_);
}

void BaseView::rebuild() {
    rows_.clear();
    ok_ = !busy_ && info_.stock.ok;
    if (busy_) {
        headline_ = "Checking...";
        tone_ = "none";
        detail_ = info_.path;
    } else if (info_.path.empty()) {
        headline_ = "Not set";
        tone_ = "none";
        detail_ = "Choose your stock firmware (from Line 6's updater). Mod builds start from it, builds are saved next to it, "
                  "and flashing it puts the unit back to stock.";
    } else if (!info_.stock.ok) {
        headline_ = "Not usable";
        tone_ = "bad";
        detail_ = info_.path + ": " + info_.stock.why;
    } else {
        headline_ = std::string(hx::unitName(hx::unitFromId(info_.stock.model))) + " " + info_.stock.version + " (stock)";
        tone_ = "stock";
        detail_ = "Mod builds start from this file; nothing is ever written to it.";
        rows_.push_back({"File", info_.path});
        rows_.push_back({"Unit", std::string(hx::unitName(hx::unitFromId(info_.stock.model)))});
        rows_.push_back({"Version", info_.stock.version});
        rows_.push_back({"Image SHA-256", info_.stock.imageSha256});
    }
    canUseOpen_ = !busy_ && openIsStock_ && !openPath_.empty() && openPath_ != info_.path;
    dirty();
}

void BaseView::dirty() {
    if (!handle_) return;
    for (const char *v : {"base_headline", "base_detail", "base_tone", "base_rows", "base_ok", "base_can_use_open", "base_busy"})
        handle_.DirtyVariable(v);
}

} // namespace editor

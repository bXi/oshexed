#include "frontend/contextmenu.h"

#include "luminoveau.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>

namespace editor {

void ContextMenu::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<SwatchRow>()) {
        s.RegisterMember("index", &SwatchRow::index);
        s.RegisterMember("cls", &SwatchRow::cls);
        s.RegisterMember("selected", &SwatchRow::selected);
    }
    model.RegisterArray<std::vector<SwatchRow>>();
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("index", &Row::index);
        s.RegisterMember("label", &Row::label);
        s.RegisterMember("enabled", &Row::enabled);
        s.RegisterMember("separator", &Row::separator);
        s.RegisterMember("palette", &Row::palette);
        s.RegisterMember("checked", &Row::checked);
        s.RegisterMember("swatches", &Row::swatches);
    }
    model.RegisterArray<std::vector<Row>>();
    model.Bind("ctx_open", &open_);
    model.Bind("ctx_rows", &rows_);
    model.Bind("ctx_left", &left_);
    model.Bind("ctx_top", &top_);
    model.Bind("ctx_title", &title_);
    using Args = const Rml::VariantList &;
    model.BindEventCallback("ctx_pick", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        pick(a.empty() ? -1 : a[0].Get<int>(-1));
    });
    model.BindEventCallback("ctx_close", [this](Rml::DataModelHandle, Rml::Event &, Args) { close(); });
    model.BindEventCallback("ctx_swatch", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        if (a.size() >= 2) pickSwatch(a[0].Get<int>(-1), a[1].Get<int>(-1));
    });
    handle_ = model.GetModelHandle();
}

bool ContextMenu::isRightClick(Rml::Event &ev) { return ev.GetParameter<int>("button", 0) == 1; }

// Kept inside the window: a menu near the right or bottom edge opens to the left / upwards of the pointer.
void ContextMenu::openAt(Rml::Event &ev, std::vector<Item> items, std::string title) {
    if (items.empty()) return;
    float x = ev.GetParameter<float>("mouse_x", 0.f), y = ev.GetParameter<float>("mouse_y", 0.f);
    if (Rml::Context *ctx = RmlUI::GetContext()) {
        const float dp = ctx->GetDensityIndependentPixelRatio();
        const Rml::Vector2i size = ctx->GetDimensions();
        const float w = 230.f * dp, h = (24.f * float(items.size()) + (title.empty() ? 10.f : 34.f)) * dp;
        if (x + w > float(size.x)) x = std::max(0.f, x - w);
        if (y + h > float(size.y)) y = std::max(0.f, float(size.y) - h);
    }
    left_ = std::to_string(int(x)) + "px";
    top_ = std::to_string(int(y)) + "px";
    ev.StopPropagation();
    fill(std::move(items), std::move(title));
}

void ContextMenu::openAgain(std::vector<Item> items, std::string title) {
    if (!items.empty()) fill(std::move(items), std::move(title));
}

void ContextMenu::fill(std::vector<Item> items, std::string title) {
    items_ = std::move(items);
    rows_.clear();
    for (std::size_t i = 0; i < items_.size(); ++i) {
        Row r;
        r.index = int(i);
        r.label = items_[i].label;
        r.palette = !items_[i].swatches.empty();
        r.checked = items_[i].checked;
        r.enabled = items_[i].enabled && (bool(items_[i].action) || r.palette);
        r.separator = r.label.empty() && !r.palette;
        for (std::size_t s = 0; s < items_[i].swatches.size(); ++s)
            r.swatches.push_back({int(s), items_[i].swatches[s].cls, items_[i].swatches[s].selected});
        rows_.push_back(std::move(r));
    }
    title_ = std::move(title);
    open_ = true;
    dirty();
}

void ContextMenu::close() {
    if (!open_) return;
    open_ = false;
    dirty();
}

// Closed before the action runs: an action may open something else (a file dialog, another menu).
void ContextMenu::pick(int index) {
    if (index < 0 || std::size_t(index) >= items_.size() || !rows_[std::size_t(index)].enabled) return;
    const std::function<void()> action = items_[std::size_t(index)].action;
    close();
    if (action) action();
}

void ContextMenu::pickSwatch(int item, int swatch) {
    if (item < 0 || std::size_t(item) >= items_.size() || !rows_[std::size_t(item)].enabled) return;
    const auto &swatches = items_[std::size_t(item)].swatches;
    if (swatch < 0 || std::size_t(swatch) >= swatches.size()) return;
    const std::function<void()> action = swatches[std::size_t(swatch)].action;
    close();
    if (action) action();
}

void ContextMenu::dirty() {
    if (!handle_) return;
    for (const char *v : {"ctx_open", "ctx_rows", "ctx_left", "ctx_top", "ctx_title"}) handle_.DirtyVariable(v);
}

} // namespace editor

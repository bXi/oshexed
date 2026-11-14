#include "frontend/globalsettingsview.h"

#include "frontend/contextmenu.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace editor {

void GlobalSettingsView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<Option>()) {
        s.RegisterMember("index", &Option::index);
        s.RegisterMember("label", &Option::label);
    }
    model.RegisterArray<std::vector<Option>>();
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("id", &Row::id);
        s.RegisterMember("label", &Row::label);
        s.RegisterMember("kind", &Row::kind);
        s.RegisterMember("options", &Row::options);
        s.RegisterMember("value", &Row::value);
        s.RegisterMember("current", &Row::current);
        s.RegisterMember("known", &Row::known);
    }
    model.RegisterArray<std::vector<Row>>();
    if (auto s = model.RegisterStruct<Group>()) {
        s.RegisterMember("name", &Group::name);
        s.RegisterMember("rows", &Group::rows);
    }
    model.RegisterArray<std::vector<Group>>();
    model.Bind("gs_ready", &ready_);
    model.Bind("gs_pages", &pages_);
    model.Bind("gs_page", &page_);
    model.Bind("gs_page_name", &pageName_);
    model.Bind("gs_rows", &shown_);
    using Args = const Rml::VariantList &;
    auto arg = [](Args a, std::size_t i) { return a.size() > i ? a[i].Get<int>(0) : 0; };
    model.BindEventCallback("gs_pick", [this, arg](Rml::DataModelHandle, Rml::Event &, Args a) { pick(arg(a, 0), arg(a, 1)); });
    model.BindEventCallback("gs_list", [this, arg](Rml::DataModelHandle, Rml::Event &ev, Args a) { openList(ev, arg(a, 0)); });
    model.BindEventCallback("gs_pick_page", [this, arg](Rml::DataModelHandle, Rml::Event &, Args a) {
        const int p = arg(a, 0);
        if (p >= 0 && std::size_t(p) < groups_.size()) page_ = p;
        dirty();
    });
    // Applied on Enter or when the field loses focus, not per keystroke (typing "50" sent 5, then 50).
    model.BindEventCallback("gs_number", [this, arg](Rml::DataModelHandle, Rml::Event &ev, Args a) {
        if (ev.GetType() == "change" && !ev.GetParameter<bool>("linebreak", false)) return;
        if (Rml::Element *el = ev.GetCurrentElement()) number(arg(a, 0), el->GetAttribute<Rml::String>("value", ""));
    });
    handle_ = model.GetModelHandle();
}

// The menu pages in the order the unit lists them.
void GlobalSettingsView::setDefinitions(std::vector<hx::GlobalSetting> defs) {
    defs_ = std::move(defs);
    groups_.clear();
    for (const hx::GlobalSetting &d : defs_) {
        auto g = std::find_if(groups_.begin(), groups_.end(), [&](const Group &x) { return x.name == d.group; });
        if (g == groups_.end()) {
            groups_.push_back({d.group, {}});
            g = groups_.end() - 1;
        }
        Row r;
        r.id = d.id;
        r.label = d.label;
        r.kind = d.kind == hx::GlobalSetting::Kind::Number ? "number" : d.options.size() <= 4 ? "segs" : "list";
        for (std::size_t k = 0; k < d.options.size(); ++k) r.options.push_back({int(k), d.options[k]});
        r.stored = d.values;
        g->rows.push_back(std::move(r));
    }
    for (Group &g : groups_)
        for (Row &r : g.rows)
            if (const auto it = values_.find(r.id); it != values_.end()) {
                r.value = choiceOf(r, int(std::lround(it->second)));
                r.known = true;
                show(r);
            }
    dirty();
}

std::vector<int> GlobalSettingsView::ids() const {
    std::vector<int> out;
    for (const hx::GlobalSetting &d : defs_) out.push_back(d.id);
    return out;
}

void GlobalSettingsView::setConnection(bool connected) {
    connected_ = connected;
    if (!connected) {
        ready_ = false;
        values_.clear();
        for (Group &g : groups_)
            for (Row &r : g.rows) r.known = false;
    }
    dirty();
}

void GlobalSettingsView::setValues(const std::vector<double> &pairs) {
    for (std::size_t i = 0; i + 1 < pairs.size(); i += 2) set(int(pairs[i]), pairs[i + 1]);
    ready_ = connected_;
    dirty();
}

void GlobalSettingsView::applyNotice(const hx::EditorNotice &n) {
    if (n.event != hx::editor::kNoticeGlobal || !ready_) return;
    const hx::mp::Value *id = n.payload.get(118), *v = n.payload.get(119);
    if (id && v && row(int(id->asInt()))) {
        set(int(id->asInt()), v->asNumber());
        dirty();
    }
}

GlobalSettingsView::Row *GlobalSettingsView::row(int id) {
    for (Group &g : groups_)
        for (Row &r : g.rows)
            if (r.id == id) return &r;
    return nullptr;
}

// The option a stored value stands for (-1: none of them), and the reverse.
int GlobalSettingsView::choiceOf(const Row &r, int stored) {
    if (r.stored.empty()) return stored;
    const auto it = std::find(r.stored.begin(), r.stored.end(), stored);
    return it == r.stored.end() ? -1 : int(it - r.stored.begin());
}

int GlobalSettingsView::storedOf(const Row &r, int index) {
    return r.stored.empty() ? index : r.stored[std::size_t(index)];
}

void GlobalSettingsView::set(int id, double value) {
    values_[id] = value;
    if (Row *r = row(id)) {
        r->value = r->kind == "number" ? 0 : choiceOf(*r, int(std::lround(value)));
        r->known = true;
        show(*r);
    }
}

void GlobalSettingsView::show(Row &r) {
    const auto it = values_.find(r.id);
    if (r.kind == "number") {
        // as the unit shows it: rounded to the setting's step (-36.8551 stored, "-36.9" on the screen)
        const auto d = std::find_if(defs_.begin(), defs_.end(), [&](const hx::GlobalSetting &x) { return x.id == r.id; });
        const double step = d != defs_.end() && d->step > 0 ? d->step : 1;
        const int decimals = step >= 1 ? 0 : int(std::ceil(-std::log10(step) - 1e-9));
        double v = it != values_.end() ? it->second : 0.0;
        v = std::round(v / step) * step;
        if (std::abs(v) < step / 2) v = 0;  // no "-0.0"
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
        r.current = buf;
    } else if (r.value >= 0 && std::size_t(r.value) < r.options.size()) {
        r.current = r.options[std::size_t(r.value)].label;
    } else {  // a value the editor has no name for (shown as is, so it can be checked against the unit's screen)
        r.current = "value " + std::to_string(it != values_.end() ? std::lround(it->second) : 0L);
    }
}

// One setting to the unit, as the kind of value it holds: true / false, a choice's stored value, or a number.
void GlobalSettingsView::pick(int id, int index) {
    Row *r = row(id);
    if (!r || !r->known || index < 0 || std::size_t(index) >= r->options.size() || index == r->value) return;
    const auto d = std::find_if(defs_.begin(), defs_.end(), [&](const hx::GlobalSetting &x) { return x.id == id; });
    if (d == defs_.end()) return;
    const int stored = storedOf(*r, index);
    set(id, stored);
    if (onSend)
        onSend(hx::editor::kSetSetting, hx::editor::settingArgs(id, d->kind == hx::GlobalSetting::Kind::Bool
                                                                        ? hx::mp::boolean(stored != 0)
                                                                        : hx::mp::integer(stored)));
    dirty();
}

// A long list's choices in the window's menu (it sits above everything, so nothing clips it), the current one
// highlighted.
void GlobalSettingsView::openList(Rml::Event &ev, int id) {
    const Row *r = row(id);
    if (!menu_ || !r || !r->known || r->options.empty()) return;
    std::vector<ContextMenu::Item> items;
    for (const Option &o : r->options) {
        ContextMenu::Item it;
        it.label = o.label;
        it.checked = o.index == r->value;
        it.action = [this, id, k = o.index] { pick(id, k); };
        items.push_back(std::move(it));
    }
    menu_->openAt(ev, std::move(items), r->label);
}

void GlobalSettingsView::number(int id, const std::string &text) {
    Row *r = row(id);
    const auto d = std::find_if(defs_.begin(), defs_.end(), [&](const hx::GlobalSetting &x) { return x.id == id; });
    if (!r || !r->known || d == defs_.end()) return;
    char *end = nullptr;
    double v = std::strtod(text.c_str(), &end);
    if (end != text.c_str()) {
        v = std::clamp(std::round(v / d->step) * d->step, d->lo, d->hi);
        if (const auto it = values_.find(id); it == values_.end() || std::abs(it->second - v) > 1e-9) {
            set(id, v);
            if (onSend)
                onSend(hx::editor::kSetSetting,
                       hx::editor::settingArgs(id, d->step < 1 ? hx::mp::real(v) : hx::mp::integer(std::int64_t(v))));
        }
    }
    show(*r);
    dirty();
}

// The page list and the shown page's rows (copies for the data binding) follow every change.
void GlobalSettingsView::dirty() {
    pages_.clear();
    for (std::size_t g = 0; g < groups_.size(); ++g) pages_.push_back({int(g), groups_[g].name});
    if (page_ < 0 || std::size_t(page_) >= groups_.size()) page_ = 0;
    shown_ = groups_.empty() ? std::vector<Row>{} : groups_[std::size_t(page_)].rows;
    pageName_ = groups_.empty() ? std::string() : groups_[std::size_t(page_)].name;
    if (!handle_) return;
    for (const char *v : {"gs_ready", "gs_pages", "gs_page", "gs_page_name", "gs_rows"}) handle_.DirtyVariable(v);
}

} // namespace editor

#include "frontend/modsview.h"

#include "core/firmware.h"
#include "core/hxf.h"
#include "core/modbuild.h"
#include "core/modspec.h"
#include "core/unitprofile.h"
#include "frontend/readonlytext.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <SDL3/SDL_clipboard.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace editor {

namespace {

std::string kib(std::uint64_t n) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f KB", double(n) / 1024.0);
    return buf;
}

bool endsWith(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && std::equal(tail.rbegin(), tail.rend(), s.rbegin(),
                                                 [](char a, char b) { return std::tolower(unsigned(a)) == b; });
}

} // namespace

void ModsView::bind(Rml::DataModelConstructor &model) {
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("id", &Row::id);
        s.RegisterMember("name", &Row::name);
        s.RegisterMember("version", &Row::version);
        s.RegisterMember("slot", &Row::slot);
        s.RegisterMember("size", &Row::size);
        s.RegisterMember("needs", &Row::needs);
        s.RegisterMember("state", &Row::state);
        s.RegisterMember("always", &Row::always);
        s.RegisterMember("selected", &Row::selected);
        s.RegisterMember("cached", &Row::cached);
        s.RegisterMember("imported", &Row::imported);
    }
    model.RegisterArray<std::vector<Row>>();
    model.Bind("mods_rows", &rows_);
    model.Bind("mods_header", &header_);
    model.Bind("mods_status", &status_);
    model.Bind("mods_signed", &signedText_);
    model.Bind("mods_is_signed", &isSigned_);
    model.Bind("mods_busy", &busy_);
    model.Bind("mods_ready", &ready_);
    model.Bind("mods_report", &report_);
    model.Bind("mods_built", &builtPath_);
    model.Bind("mods_building", &buildRunning_);
    model.Bind("mods_build_step", &buildStep_);
    model.Bind("mods_build_pct", &buildPct_);  // "42%": the label and the bar's width
    using Args = const Rml::VariantList &;
    model.BindEventCallback("mods_build", [this](Rml::DataModelHandle, Rml::Event &, Args) { build(); });
    model.BindEventCallback("mods_flash", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (!busy_ && !builtPath_.empty() && onFlash) onFlash(builtPath_);
    });
    model.BindEventCallback("mods_refresh", [this](Rml::DataModelHandle, Rml::Event &, Args) { refresh(); });
    model.BindEventCallback("mods_copy_report", [this](Rml::DataModelHandle, Rml::Event &, Args) {
        if (!report_.empty()) SDL_SetClipboardText(report_.c_str());
    });
    model.BindEventCallback("mods_download", [this](Rml::DataModelHandle, Rml::Event &, Args) { download(); });
    model.BindEventCallback("mods_toggle", [this](Rml::DataModelHandle, Rml::Event &, Args a) {
        if (!a.empty()) toggle(a[0].Get<Rml::String>());
    });
    handle_ = model.GetModelHandle();
}

void ModsView::setBase(const std::string &path, const hx::mods::Stock &stock) {
    if (!header_.empty() && path == fwPath_ && stock.ok == stock_.ok && stock.imageSha256 == stock_.imageSha256)
        return;  // same base: keep the list and the choice
    stock_ = stock;
    listed_ = false;
    listing_ = {};
    chosen_.clear();
    fwPath_ = path;
    report_.clear();
    builtPath_.clear();
    if (stock_.ok)
        header_ = "Modules for " + std::string(hx::unitName(hx::unitFromId(stock_.model))) + " " + stock_.version;
    else
        header_ = path.empty() ? "No base firmware: choose your stock firmware on Firmware > Base firmware"
                               : "No modules: the base firmware " + stock_.why;
    status_.clear();
    rebuildRows();
}

bool ModsView::handles(const std::string &path) { return endsWith(path, ".ohem"); }

// The manifest (server, else the cached copy), which packages are cached, and imported extras.
void ModsView::refresh(std::string message) {
    if (!stock_.ok || busy_) return;
    busy_ = true;
    status_ = message.empty() ? "Looking up modules at " + store_.baseUrl() + "..." : message;
    dirty();
    job_ = std::async(std::launch::async, [store = store_, stock = stock_, message] {
        Listing l;
        l.message = message;
        l.manifest = store.manifest(stock.model, stock.version, &l.fromCache, &l.error);
        if (l.manifest)
            for (const hx::mods::ModuleRef &r : l.manifest->modules)
                if (store.cached(*l.manifest, r)) l.cached.insert(r.id);
        l.imported = store.extras(l.manifest ? &*l.manifest : nullptr, stock.model, stock.version);
        return l;
    });
}

// The chosen modules (and what they need) that aren't cached yet: downloaded, checked, cached; then listed again.
void ModsView::download() {
    if (busy_ || !listing_.manifest) return;
    std::vector<hx::mods::ModuleRef> todo;
    std::set<std::string> want = chosen_;
    for (bool grew = true; grew;) {  // requires, transitively
        grew = false;
        for (const std::string &id : std::set<std::string>(want))
            if (const hx::mods::ModuleRef *r = listing_.manifest->find(id))
                for (const std::string &n : r->needs) grew = want.insert(n).second || grew;
    }
    for (const hx::mods::ModuleRef &r : listing_.manifest->modules)
        if ((r.always || want.count(r.id)) && !listing_.cached.count(r.id)) todo.push_back(r);
    if (todo.empty()) {
        status_ = "Everything chosen is already downloaded.";
        dirty();
        return;
    }
    busy_ = true;
    status_ = "Downloading " + std::to_string(todo.size()) + " package(s)...";
    dirty();
    job_ = std::async(std::launch::async, [store = store_, stock = stock_, m = *listing_.manifest, todo] {
        Listing l;
        int ok = 0;
        for (const hx::mods::ModuleRef &r : todo) {
            std::string err;
            if (store.fetch(m, r, &err)) ++ok;
            else l.message += (l.message.empty() ? "" : "; ") + err;
        }
        l.message = "Downloaded and checked " + std::to_string(ok) + " of " + std::to_string(todo.size()) +
                    (l.message.empty() ? "." : ". Failed: " + l.message);
        l.manifest = store.manifest(stock.model, stock.version, &l.fromCache, &l.error);
        if (l.manifest)
            for (const hx::mods::ModuleRef &r : l.manifest->modules)
                if (store.cached(*l.manifest, r)) l.cached.insert(r.id);
        l.imported = store.extras(l.manifest ? &*l.manifest : nullptr, stock.model, stock.version);
        return l;
    });
}

// The chosen modules (+ always-in, + what they need; missing ones downloaded) applied to a fresh read of the stock
// file, written next to it. Only a file is written; flashing it is the Flash tab's job.
void ModsView::build() {
    if (busy_ || !ready_ || !stock_.ok || fwPath_.empty()) return;
    busy_ = true;
    report_.clear();
    builtPath_.clear();
    status_ = "Building modded firmware (this takes a while)...";
    progress_ = std::make_shared<Progress>();
    shownDone_ = -1;
    buildRunning_ = true;
    buildStep_ = "Starting";
    buildPct_ = "0%";
    dirty();
    building_ = std::async(std::launch::async, [store = store_, stock = stock_, path = fwPath_, ids = chosen_,
                                                p = progress_] {
        // Getting the modules and reading the base take the first 10%, the build itself the rest.
        const auto report = [p](double done, const std::string &step) {
            std::lock_guard<std::mutex> lock(p->mutex);
            p->done = done;
            p->step = step;
        };
        Built b;
        try {
            std::string err;
            report(0.0, "Getting the modules");
            const std::optional<std::vector<hx::mods::Package>> pkgs = hx::mods::choosePackages(store, stock, ids, &err, true);
            if (!pkgs) {
                b.error = err;
                return b;
            }
            report(0.03, "Reading the base firmware");
            const std::unique_ptr<hx::HxfImage> img = hx::loadHxfFile(path, &err);
            if (!img) {
                b.error = err;
                return b;
            }
            const std::string out = hx::mods::defaultOutPath(stock, path);
            const hx::mods::BuildProgress building = [&report](double done, const std::string &step) {
                report(0.1 + 0.9 * done, step);
            };
            const hx::mods::BuildReport r = hx::mods::buildFirmware(*img, stock, path, *pkgs, out, hx::mods::toolCommit(),
                                                                    hx::mods::toolDirty(), building);
            for (const std::string &l : hx::mods::reportLines(r)) b.report += l + "\n";
            b.outPath = out;
            b.ok = true;
        } catch (const std::exception &e) {
            b.error = e.what();
        }
        return b;
    });
}

void ModsView::importFile(const std::string &path) {
    if (!stock_.ok) {
        status_ = "Choose the base firmware first (Firmware > Base firmware): packages are made for one stock file.";
        dirty();
        return;
    }
    std::string err;
    if (const std::optional<hx::mods::Package> p = store_.import(path, stock_, &err)) {
        chosen_.insert(p->id);
        refresh("Imported " + p->name + " " + p->version + " (not from the module server: unchecked).");
    } else {
        status_ = "Not imported: " + err;
        dirty();
    }
}

void ModsView::toggle(const std::string &id) {
    for (const Row &r : rows_)
        if (r.id == id && r.always) return;  // the base is in every build
    if (!chosen_.erase(id)) chosen_.insert(id);
    rebuildRows();
}

void ModsView::update(bool visible) {
    if (visible && !reportGuarded_) reportGuarded_ = guardReadOnly("mods-report-wrap");  // once the document has it
    // The build's progress, from its thread: the bar moves in whole percents (no rebuild per deflate piece).
    if (progress_ && building_.valid()) {
        double done;
        std::string step;
        {
            std::lock_guard<std::mutex> lock(progress_->mutex);
            done = progress_->done;
            step = progress_->step;
        }
        const int pct = int(done * 100.0 + 0.5);
        if (pct != shownDone_ || step != buildStep_) {
            shownDone_ = pct;
            buildStep_ = step;
            buildPct_ = std::to_string(pct) + "%";
            if (handle_)
                for (const char *v : {"mods_build_step", "mods_build_pct"}) handle_.DirtyVariable(v);
        }
    }
    if (visible && stock_.ok && !listed_ && !busy_) {
        listed_ = true;
        refresh();
    }
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        busy_ = false;
        apply(job_.get());
    }
    if (building_.valid() && building_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        busy_ = false;
        Built b = building_.get();
        progress_.reset();
        buildRunning_ = false;
        report_ = std::move(b.report);
        builtPath_ = b.ok ? b.outPath : std::string();
        // listed again: missing packages may have been downloaded for the build (the message stays if it can't be)
        status_ = b.ok ? "Built " + b.outPath + ". Nothing has been sent to the unit."
                       : "Build refused (nothing written): " + b.error;
        dirty();
        refresh(status_);
    }
}

void ModsView::apply(Listing l) {
    listing_ = std::move(l);
    // A build's / download's own message comes first; a list problem is added after it, not instead of it.
    const std::string lead = listing_.message.empty() ? "" : listing_.message + "  ";
    if (!listing_.manifest) {
        status_ = lead + "No module list: " + listing_.error;
    } else if (listing_.manifest->stockSha256 != stock_.imageSha256) {
        status_ = lead + "The server's modules are for another " + listing_.manifest->model + " " +
                  listing_.manifest->version + " file than the open one.";
    } else {
        status_ = listing_.message.empty()
                      ? std::to_string(listing_.manifest->modules.size()) + " module(s) available" +
                            (listing_.fromCache ? " (server not reached: the list from the last visit)." : ".")
                      : listing_.message;
    }
    rebuildRows();
}

void ModsView::rebuildRows() {
    rows_.clear();
    const hx::mods::Manifest *m = listing_.manifest ? &*listing_.manifest : nullptr;
    const bool usable = m && stock_.ok && m->stockSha256 == stock_.imageSha256;
    ready_ = usable;
    isSigned_ = m && m->isSigned;
    signedText_ = !m ? "" : m->isSigned ? "Signed" : "Not signed yet: packages are checked against the list (size, SHA-256) only";
    if (usable)
        for (const hx::mods::ModuleRef &r : m->modules) {
            Row row;
            row.id = r.id;
            row.name = r.name;
            row.version = r.version;
            row.slot = r.slot < 0 ? "-" : std::to_string(r.slot);
            row.size = kib(r.size);
            for (const std::string &n : r.needs) row.needs += (row.needs.empty() ? "needs " : ", ") + n;
            row.always = r.always;
            row.selected = r.always || chosen_.count(r.id);
            row.cached = listing_.cached.count(r.id) > 0;
            row.state = row.cached ? "Downloaded" : "Not downloaded";
            rows_.push_back(std::move(row));
        }
    for (const hx::mods::Package &p : listing_.imported) {
        if (!stock_.ok || p.fwImageSha256 != stock_.imageSha256) continue;
        Row row;
        row.id = p.id;
        row.name = p.name;
        row.version = p.version;
        row.slot = p.slot < 0 ? "-" : std::to_string(p.slot);
        for (const std::string &n : p.needs) row.needs += (row.needs.empty() ? "needs " : ", ") + n;
        row.selected = chosen_.count(p.id) > 0;
        row.cached = row.imported = true;
        row.state = "Imported";
        rows_.push_back(std::move(row));
    }
    dirty();
}

void ModsView::dirty() {
    if (!handle_) return;
    for (const char *v : {"mods_rows", "mods_header", "mods_status", "mods_signed", "mods_is_signed", "mods_busy", "mods_ready",
                          "mods_report", "mods_built", "mods_building", "mods_build_step", "mods_build_pct"})
        handle_.DirtyVariable(v);
}

} // namespace editor

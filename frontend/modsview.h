#pragma once

#include "core/modstore.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
}
namespace hx {
struct Firmware;
}

namespace editor {

// The Mods tab: the module marketplace for the open stock firmware (../mod.md "Distribution"). Lists the server's
// modules for this model and version (and imported packages), downloads the chosen ones into the cache (each checked
// against the manifest's size and SHA-256), takes dropped .ohem files. Network work runs in the background.
// Build: the chosen modules (missing ones downloaded first) applied to the stock file, saved next to it as
// "<Unit> <version> + mods.hxf" (core/modbuild.h), with a report; "Flash this build..." hands it to the Flash tab.
class ModsView {
public:
    // After ShellView::bind (it registers the string array type).
    void bind(Rml::DataModelConstructor &model);
    // "Flash this build...": the app opens the Flash tab with this file (nothing is sent from here).
    std::function<void(const std::string &)> onFlash;
    // The base firmware (Firmware tab > Base firmware): modules are listed and built for it, whatever file is open.
    void setBase(const std::string &path, const hx::mods::Stock &stock);
    // Every frame: picks up finished background work. visible: the tab is shown (the first show fetches the list).
    void update(bool visible);
    static bool handles(const std::string &path);  // a .ohem
    void importFile(const std::string &path);

private:
    struct Row {
        std::string id, name, version, slot, size, needs, state;
        bool always = false, selected = false, cached = false, imported = false;
    };
    struct Listing {
        std::optional<hx::mods::Manifest> manifest;
        bool fromCache = false;
        std::string error;
        std::set<std::string> cached;                // ids whose package is in the cache and matches
        std::vector<hx::mods::Package> imported;     // packages in the cache the manifest doesn't list
        std::string message;                         // from a download / import that ran before the listing
    };

    struct Built {
        bool ok = false;
        std::string error, outPath;
        std::string report;  // hx::mods::reportLines, one per line
    };

    void refresh(std::string message = {});
    void download();
    void build();
    void toggle(const std::string &id);
    void apply(Listing listing);
    void rebuildRows();
    void dirty();

    Rml::DataModelHandle handle_;
    hx::mods::Store store_;
    hx::mods::Stock stock_;
    std::future<Listing> job_;
    std::future<Built> building_;
    std::string fwPath_;
    // A build's progress, written by its thread (hx::mods::BuildProgress), read by update().
    struct Progress {
        std::mutex mutex;
        double done = 0;
        std::string step;
    };
    std::shared_ptr<Progress> progress_;
    int shownDone_ = -1;               // the percent last shown
    bool buildRunning_ = false;
    std::string buildStep_, buildPct_; // "Packing the firmware", "42%"
    std::string report_;         // the last build's report, in a read-only text box (selectable, Copy report)
    bool reportGuarded_ = false; // the read-only guard is on the box's wrapper
    std::string builtPath_;
    bool listed_ = false;  // fetched once for this firmware

    Listing listing_;
    std::set<std::string> chosen_;
    std::vector<Row> rows_;
    std::string header_, status_, signedText_;
    bool busy_ = false, ready_ = false, isSigned_ = false;
};

} // namespace editor

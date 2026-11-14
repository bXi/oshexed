#pragma once

#include "core/modstore.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
}
namespace hx {
struct Firmware;
}

namespace editor {

// The base firmware: the user's stock .hxf, chosen once (settings.ini) and the starting point for everything that
// makes or restores firmware: mod builds start from it (whatever file the editor has open), builds are saved next to
// it, and "Flash the base" puts stock back. Firmware tab > Base firmware. Checked in the background (stock, a unit
// and version this editor knows).
class BaseView {
public:
    struct Info {
        std::string path;
        hx::mods::Stock stock;  // stock.ok: usable
    };

    void bind(Rml::DataModelConstructor &model);
    // A path from the settings or the file dialog ("" = none); checked in the background, then onChanged.
    void setPath(const std::string &path);
    // The editor's open file, for "Use the open file" (offered when it is stock and not already the base).
    void setOpenFile(const hx::Firmware *fw);
    void update();  // every frame: picks up the finished check
    const Info &info() const { return info_; }
    bool checking() const { return job_.valid(); }

    std::function<void()> onChoose;                      // the app's file dialog
    std::function<void(const Info &)> onChanged;          // checked (also when it failed: stock.ok false)
    std::function<void(const std::string &)> onFlash;     // "Flash the base...": the Flash tab takes over

private:
    struct Row {
        std::string label, value;
    };
    void rebuild();
    void dirty();

    Rml::DataModelHandle handle_;
    Info info_;
    std::future<Info> job_;
    std::string openPath_;
    bool openIsStock_ = false;

    std::string headline_ = "Not set", detail_, tone_ = "none";
    std::vector<Row> rows_;
    bool ok_ = false, canUseOpen_ = false, busy_ = false;
};

} // namespace editor

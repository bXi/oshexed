#pragma once

#include "core/device/devicelink.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
}

namespace editor {

// The Flash tab: pick a .hxf (default: the one the editor has open), see what would be written, rehearse it
// against the fake unit, then flash with a confirmation that names the file and the records. Device safety: nothing
// is written without that confirmation; Stop works only until the first slot is
// touched, after which the flash runs to the end and a failure shows the recovery steps.
class FlashView {
public:
    explicit FlashView(hx::DeviceLink &device) : device_(device) {}

    // After ShellView::bind (it registers the string array type).
    void bind(Rml::DataModelConstructor &model);
    // The editor opened a firmware: it becomes the file to flash unless the user picked another one here.
    void setEditorFile(const std::string &path);
    // A file picked elsewhere (the Mods tab's "Flash this build"): as if chosen here. Nothing is sent until the
    // user starts the flash and confirms it on this tab.
    void useFile(const std::string &path);
    // Every frame. visible: the tab is shown (the file is only read then).
    void update(bool visible);

private:
    struct Row {
        std::string name, size, plan, step, cls;
        std::string bar = "0%";
    };
    struct Prepared {
        std::vector<hx::RecordFile> files;
        std::string error;
    };

    void setPath(const std::string &path);
    void choose();
    void replan();
    void start(bool simulate);
    void handle(const hx::DeviceEvent &e);
    void addLog(const std::string &s);
    void dirtyAll();
    int rowOf(std::uint32_t record) const;

    hx::DeviceLink &device_;
    Rml::DataModelHandle handle_;

    std::string path_;
    bool userPicked_ = false;           // chosen in this tab: a newly opened editor file doesn't replace it
    std::future<Prepared> preparing_;
    std::string preparingPath_;
    std::vector<hx::RecordFile> files_;
    std::string loadedPath_;            // what files_ came from
    std::mutex pickMutex_;
    std::string picked_;                // from the file dialog's thread

    // Bound to the page.
    std::string file_, status_, summary_, presetsText_, phase_ = "idle";
    bool ready_ = false, fast_ = false, checkSlots_ = false, sim_ = false, canStop_ = false, recovery_ = false;
    bool busy_ = false;
    int presetsMode_ = 0;               // 0 auto, 1 keep, 2 rebuild
    std::vector<Row> rows_;
    std::string progressW_ = "0%", progressText_, result_, deviceText_;
    std::vector<std::string> log_;

    // The running flash.
    std::vector<std::uint32_t> planned_;  // records being written, in order
    std::size_t totalBytes_ = 0, doneBytes_ = 0, recordBase_ = 0;
    double startedAt_ = 0;
    std::string stepText_;              // "Record 5: writing"; progressText_ = timing + this
    bool keepPresets_ = false;
    bool wrote_ = false;
};

} // namespace editor

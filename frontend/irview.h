#pragma once

#include "core/device/editorclient.h"
#include "frontend/contextmenu.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
}

namespace editor {

// The IRs tab: the unit's 128 user IR slots (names and hashes, read when the editor connects). Import a WAV into a
// slot, rename or clear one. Each of those writes to the unit, so it first shows a confirmation that names the
// slot and what it replaces (device safety: nothing writes to the unit without one); only "Write to Helix" there
// sends it.
class IrView {
public:
    void bind(Rml::DataModelConstructor &model);
    void setConnection(bool connected, const std::string &text);
    void setIrs(const std::vector<hx::editor::IrSlot> &irs);
    // Every frame: picks up a file chosen in the file dialog (its callback may run on another thread).
    void update();
    // A .wav or .nam dropped on the window: into `slot` (the row under the pointer; -1 = the selected slot).
    // A .nam capture is converted on the way (core/nam.h). Asks before anything is written, as Import does.
    void openFile(const std::string &path, int slot);
    static bool handles(const std::string &path);  // .wav / .nam
    void setMenu(ContextMenu *menu) { menu_ = menu; }  // the window's right-click menu

    std::function<void()> onRefresh;  // read the list from the unit again
    // A confirmed write: library command + args (DeviceLink::sendLibraryWrite).
    std::function<void(int command, hx::mp::Value args)> onWrite;

private:
    struct Row {
        int slot = 0;
        std::string number, name, hash;
        bool empty = true;
        std::string kind;       // "ir", "nam" (one of our NAM captures), "" = empty or not read yet
        bool shown = true;      // passes the filter
        double loudness = 0, levelDbu = 0;  // NAM: from its slot header (not bound)
    };
    enum class Action { None, Upload, Rename, Clear };

    void select(int slot);
    void chooseWav();
    void loadFile(const std::string &path);
    void ask(Action action);
    void confirm();
    void cancel();
    void dirty();

    Rml::DataModelHandle handle_;
    ContextMenu *menu_ = nullptr;
    bool connected_ = false;
    std::string status_ = "Not connected";
    std::vector<Row> rows_;
    std::string summary_;
    int sel_ = -1;
    std::string selName_, selHash_, selNumber_, selKind_;
    std::vector<std::string> selDetails_;  // a NAM capture's loudness / input level
    std::string filter_ = "all";           // "all", "ir", "nam"
    void applyFilter();
    std::string nameEdit_;  // the name field: for a rename, or the imported IR's name

    // The pending write, shown for confirmation.
    Action action_ = Action::None;
    std::string question_;              // "Write X to slot 012? It replaces Y."
    std::vector<std::string> notes_;    // what the import changed to fit the slot
    std::vector<std::uint8_t> wavData_; // the converted samples (Upload)
    std::string wavHash_;
    bool busy_ = false;                 // a write sent, waiting for the new list
    std::string message_;               // last result / error

    std::mutex pickMutex_;
    std::string picked_;
};

} // namespace editor

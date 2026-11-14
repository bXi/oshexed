#pragma once

#include "core/device/devicelink.h"
#include "core/firmware.h"
#include "core/globalsettings.h"
#include "core/unitprofile.h"
#include "frontend/baseview.h"
#include "frontend/modsview.h"
#include "frontend/monitorview.h"
#include "frontend/editorview.h"
#include "frontend/flashview.h"
#include "frontend/globaleqview.h"
#include "frontend/globalsettingsview.h"
#include "frontend/irview.h"
#include "frontend/settings.h"
#include "frontend/shellview.h"

#include <SDL3/SDL_events.h>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace editor {

// Owns the window, the loaded firmware and the main RmlUi document.
class App {
public:
    bool init(int argc, char *argv[]);
    // Returns false when the app should quit.
    bool iterate();
    void handleEvent(const SDL_Event &event);
    void shutdown();

private:
    struct LoadResult {
        std::string path;
        std::unique_ptr<hx::Firmware> fw;
        std::string error;
    };

    void loadFonts();
    void openDialog();
    void exportDialog();  // "Export .hlx": asks where, then pollLoad writes the open preset there
    void requestLoad(std::string path);  // any thread
    void writeExport(const std::string &path);
    // Online: keeps the editor session running (also after a flash job) and hands its events to the view.
    void pollDevice();
    void updateUnitModel(bool force = false);
    // The unit runs one of our builds (its tag) and the open file isn't that build: open the build's file (build
    // history, else "<Unit> <version> + mods.hxf" next to the open stock file), so its models and lists are known.
    void matchUnitBuild();
    std::string unitTag_;     // the connected unit's build tag, "" = stock / unknown / not connected
    std::string triedBuild_;  // the build file last opened for unitTag_ (no retry loops)
    hx::UnitModel unitModel_ = hx::UnitModel::Unknown;      // what the editor works for (see updateUnitModel)
    hx::UnitModel connectedUnit_ = hx::UnitModel::Unknown;  // the unit on USB, Unknown when none
    std::vector<hx::GlobalSetting> allGlobalSettings_;      // from the firmware, before the model's filter
    void setOnline(bool online);
    void startLoad(const std::string &path);
    void pollLoad();
    std::string settingsPath() const;
    void saveSettings();

    MonitorView monitor_;  // Monitor tab: USB traffic, MIDI in
    bool rmlDebugger_ = false;  // F12
    EditorView view_;
    ShellView shell_;
    IrView irs_;
    ModsView mods_;  // Mods tab: the module marketplace
    BaseView base_;  // Firmware tab > Base firmware: the stock .hxf builds start from
    void baseDialog();
    void openFlash(const std::string &path);  // the Flash tab with this file (it asks before sending anything)
    GlobalEqView globalEq_;  // Helix tab > Global EQ
    GlobalSettingsView globalSettings_;  // Helix tab > Global Settings (the unit's menu pages)
    ContextMenu menu_;  // the window's right-click menu, shared by the views
    // The unit's connection (own thread) and the Flash tab; made in init(), once the writable directory is known.
    std::unique_ptr<hx::DeviceLink> device_;
    std::unique_ptr<FlashView> flash_;
    Settings settings_;
    std::unique_ptr<hx::Firmware> fw_;
    std::future<LoadResult> loading_;
    std::vector<std::vector<unsigned char>> fontData_;  // RmlUi reads fonts from these while running

    // A path handed over by the file dialog callback or a drop, picked up on the main thread.
    std::mutex pendingMutex_;
    std::string pendingPath_;
    std::string pendingExport_;  // from the save dialog
    std::string pendingBase_;    // from the base firmware dialog

    // Backups of the unit's stored presets: the file / folder dialog's answer for the request below.
    enum class LibJob { None, BackupPreset, BackupSetlist, Restore };
    struct LibRequest {
        LibJob job = LibJob::None;
        int setlist = 0, preset = 0;
        std::string name;  // the preset's name (file name suggestion)
    };
    void libraryDialog(LibRequest request);
    void runLibraryJob(const LibRequest &request, const std::string &path);  // main thread
    void storedRead(const std::string &tag, const std::vector<std::uint8_t> &blob);  // a lib 4 answer
    LibRequest libRequest_;        // the dialog being shown
    std::string pendingLibPath_;   // its answer (guarded by pendingMutex_)
    int backupLeft_ = 0, backupDone_ = 0;  // a setlist backup in progress
};

} // namespace editor

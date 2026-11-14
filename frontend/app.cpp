#include "frontend/app.h"

#include "core/appdata.h"
#include "core/modbuild.h"
#include "core/text.h"
#include "frontend/embeddedassets.h"
#include "frontend/firmwaretextures.h"
#include "luminoveau.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_dialog.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace editor {

namespace {

constexpr const char *kMainDocument = "ui/main.rml";
// Lucide icon font (ISC), fetched at configure time and copied next to the exe. Family "lucide".
constexpr const char *kIconFont = "assets/fonts/lucide.ttf";

std::string fileName(const std::string &path) {
    const std::u8string name = hx::pathFromUtf8(path).filename().u8string();
    return std::string(name.begin(), name.end());
}

bool readFile(const std::filesystem::path &path, std::vector<unsigned char> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

bool isPresetFile(const std::string &path) {
    std::u8string ext = hx::pathFromUtf8(path).extension().u8string();
    for (char8_t &c : ext)
        if (c >= 'A' && c <= 'Z') c = char8_t(c - 'A' + 'a');
    return ext == u8".hlx";
}

// An HX Edit preset file, shown in the editor (needs the firmware for its models). Small, so read right here.
void importPreset(EditorView &view, const std::string &path) {
    if (!view.hasFirmware()) {
        view.setStatus("Open the firmware first: presets are read with its models");
        return;
    }
    std::vector<unsigned char> bytes;
    if (!readFile(hx::pathFromUtf8(path), bytes)) {
        view.setStatus(fileName(path) + ": could not read the file");
        return;
    }
    std::string error;
    std::vector<std::string> warnings;
    std::optional<hx::Preset> p = view.importHlx(std::string(bytes.begin(), bytes.end()), &error, &warnings);
    if (!p) {
        view.setStatus(fileName(path) + ": " + error);
        return;
    }
    for (const std::string &w : warnings) LOG_WARNING("{}: {}", fileName(path), w);
    const std::string name = p->name.empty() ? fileName(path) : p->name;
    view.openPreset(std::move(*p), name + "  (from " + fileName(path) + ")");
    view.setStatus("Opened " + fileName(path) +
                   (warnings.empty() ? std::string() : "  (" + std::to_string(warnings.size()) + " warnings: " + warnings[0] + ")"));
}

bool textFieldFocused() {
    Rml::Context *ctx = RmlUI::GetContext();
    Rml::Element *el = ctx ? ctx->GetFocusElement() : nullptr;
    return el && (el->GetTagName() == "textarea" ||
                  (el->GetTagName() == "input" && el->GetAttribute<Rml::String>("type", "text") == "text"));
}

// The focused text box's selection onto the system clipboard. False: no text box with a selection has focus.
bool copyTextSelection() {
    Rml::Context *ctx = RmlUI::GetContext();
    Rml::Element *el = ctx ? ctx->GetFocusElement() : nullptr;
    std::string text;
    if (auto *area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea *>(el)) area->GetSelection(nullptr, nullptr, &text);
    else if (auto *input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(el)) input->GetSelection(nullptr, nullptr, &text);
    else return false;
    if (text.empty()) return false;
    SDL_SetClipboardText(text.c_str());
    return true;
}

} // namespace

std::string App::settingsPath() const { return FileHandler::GetWritableDirectory() + "settings.ini"; }

void App::saveSettings() {
    if (!settings_.save(settingsPath())) LOG_WARNING("Could not write {}", settingsPath());
}

// The UI font is family "UiSans" in theme.rcss: the OS's own UI font when found (read from the
// user's system, not shipped), else the engine's built-in font, which is also the fallback.
void App::loadFonts() {
    RmlUI::LoadDefaultFont();
    struct Face {
        const char *path;
        Rml::Style::FontWeight weight;
    };
    const Face faces[] = {
#if defined(_WIN32)
        {"C:/Windows/Fonts/segoeui.ttf", Rml::Style::FontWeight::Normal},
        {"C:/Windows/Fonts/segoeuib.ttf", Rml::Style::FontWeight::Bold},
#elif defined(__APPLE__)
        {"/System/Library/Fonts/Supplemental/Arial.ttf", Rml::Style::FontWeight::Normal},
        {"/System/Library/Fonts/Supplemental/Arial Bold.ttf", Rml::Style::FontWeight::Bold},
#else
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", Rml::Style::FontWeight::Normal},
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", Rml::Style::FontWeight::Bold},
#endif
    };
    for (const Face &f : faces) {
        std::vector<unsigned char> data;
        if (!readFile(f.path, data)) continue;
        fontData_.push_back(std::move(data));
        const std::vector<unsigned char> &d = fontData_.back();
        if (!RmlUI::LoadFontFromMemory(d.data(), d.size(), "UiSans", Rml::Style::FontStyle::Normal, f.weight))
            LOG_WARNING("Could not load font {}", f.path);
    }
    // Without it the rail shows empty boxes but still works (labels, tooltips).
    RmlUI::LoadFontFromFile(kIconFont);
}

bool App::init(int argc, char *argv[]) {
    // Before any write: without these the writable directory falls back to the executable's. appDataDir() makes the
    // same folder SDL's pref path names, and copies the old "helix-editor" one into it once (the rename, 2026-11-08).
    hx::appDataDir();
    FileHandler::SetOrganizationName("oshexed");
    FileHandler::SetApplicationName("oshexed");
    // What this program last wrote to the unit's slots; the command line (--flash) uses the same file.
    device_ = std::make_unique<hx::DeviceLink>(FileHandler::GetWritableDirectory() + "unit_state.txt");
    flash_ = std::make_unique<FlashView>(*device_);

    // Serves the firmware's own images to RmlUi; must exist before the window creates the context.
    fwtex::installHook();
    // An editor draws no particles and few sprites: the engine's defaults reserve ~3.5 GB of VRAM for them, and
    // startup failed when the GPU couldn't spare that (2026-11-13).
    Lumi::Config engine;
    engine.particles = false;
    engine.maxSprites = 50'000;
    Lumi::Configure(engine);
    // Before InitWindow, which starts RmlUi and loads its shaders: ui/ and assets/ may live inside the executable.
    // Mounted ahead of the working directory, so a stale loose copy next to the exe can't win.
    if (const EmbeddedAssets assets = embeddedAssets(); assets.size > 0)
        if (!FileHandler::MountMemory(assets.data, assets.size, "assets.zip")) LOG_ERROR("Could not mount the built-in assets");
    Window::InitWindow("OpenSource Helix Editor", 1400, 900, 1, SDL_WINDOW_RESIZABLE);
    // Title bar / taskbar while running (the exe's own icon is the resource, frontend/oshexed.rc).
    Window::SetIcon("assets/icon.png");
    // Text fields need text-input events; the engine leaves starting them to the app (as lumiSudoku does). On the
    // desktop leaving them on is harmless: keys still come as key events.
    Window::StartTextInput(Window::Main());

    // 4x MSAA; Luminoveau's RmlUi renderer draws the UI's base layer at the renderer's sample count.
    Renderer::SetSampleCount(GpuSampleCount::X4);

    // After InitWindow (which starts RmlUi), before the first document.
    loadFonts();

    settings_.load(settingsPath());
    // Older builds kept only the firmware path, in last_firmware.txt.
    if (settings_.firmware.empty())
        if (std::ifstream last(hx::pathFromUtf8(FileHandler::GetWritableDirectory() + "last_firmware.txt")); last)
            std::getline(last, settings_.firmware);
    view_.onPresetOpened = [this](int setlist, int preset) {
        if (setlist == settings_.setlist && preset == settings_.preset) return;
        settings_.setlist = setlist;
        settings_.preset = preset;
        saveSettings();
    };

    // The unit: live edits go to the session; the Online / Offline switch and the unit picker are settings.
    view_.onLiveEdit = [this](int command, hx::mp::Value args) { device_->sendEdit(command, std::move(args)); };
    view_.onStatus = [this](const std::string &s) { monitor_.addLog("status: " + s); };
    view_.onLog = [this](const std::string &s) { monitor_.addLog(s); };
    irs_.onRefresh = [this] { device_->refreshIrs(); };
    globalEq_.onSend = [this](int command, hx::mp::Value args) { device_->sendEdit(command, std::move(args)); };
    globalSettings_.onSend = [this](int command, hx::mp::Value args) { device_->sendEdit(command, std::move(args)); };
    irs_.onWrite = [this](int command, hx::mp::Value args) { device_->sendLibraryWrite(command, std::move(args)); };
    view_.onLibraryWrite = [this](int command, hx::mp::Value args) { device_->sendLibraryWrite(command, std::move(args)); };
    view_.onLibraryRead = [this](int command, hx::mp::Value args, std::string tag) {
        device_->sendLibraryRead(command, std::move(args), std::move(tag));
    };
    view_.onBackupPreset = [this](int setlist, int preset, std::string name) {
        libraryDialog({LibJob::BackupPreset, setlist, preset, std::move(name)});
    };
    view_.onBackupSetlist = [this](int setlist) { libraryDialog({LibJob::BackupSetlist, setlist, 0, ""}); };
    view_.onRestorePreset = [this](int setlist, int preset) { libraryDialog({LibJob::Restore, setlist, preset, ""}); };
    view_.onOnlineChanged = [this](bool online) { setOnline(online); };
    view_.onUnitChanged = [this](const std::string &unit) {
        settings_.unit = unit;
        saveSettings();
        view_.setOnline(settings_.online, unit);
        updateUnitModel();
    };
    view_.setOnline(settings_.online, settings_.unit);
    updateUnitModel(true);  // offline picker until a firmware or a unit says otherwise
    view_.setConfirmSave(settings_.confirmSave);
    view_.onConfirmSaveChanged = [this](bool on) {
        settings_.confirmSave = on;
        saveSettings();
    };
    if (settings_.online) view_.setConnection(false, "Looking for a Helix...");

    shell_.setFirmwareNoticeEnabled(settings_.firmwareNotice);  // before setPage: starting on Firmware shows it
    shell_.setPage(settings_.page);
    shell_.setRailOpen(settings_.railOpen);
    shell_.onChanged = [this] {
        settings_.page = shell_.page();
        settings_.railOpen = shell_.railOpen();
        settings_.firmwareNotice = shell_.firmwareNoticeEnabled();
        saveSettings();
    };

    // The model must exist before a document that names it is loaded.
    if (Rml::DataModelConstructor model = RmlUI::BindDataModel("editor")) {
        view_.bind(model);
        shell_.bind(model);
        flash_->bind(model);
        monitor_.bind(model);
        irs_.bind(model);
        mods_.bind(model);
        menu_.bind(model);
        globalEq_.bind(model);
        globalSettings_.bind(model);
        view_.setMenu(&menu_);
        irs_.setMenu(&menu_);
        globalSettings_.setMenu(&menu_);
        view_.onExport = [this] { exportDialog(); };
        mods_.onFlash = [this](const std::string &path) { openFlash(path); };
        base_.bind(model);
        base_.onChoose = [this] { baseDialog(); };
        base_.onFlash = [this](const std::string &path) { openFlash(path); };
        base_.onChanged = [this](const BaseView::Info &info) {
            settings_.base = info.path;
            saveSettings();
            mods_.setBase(info.path, info.stock);
        };
        model.BindEventCallback("open_file", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { openDialog(); });
        model.BindEventCallback("export_hlx", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { exportDialog(); });
    }
    if (!RmlUI::LoadDocument(kMainDocument)) {
        LOG_ERROR("Could not load {}", kMainDocument);
        return false;
    }
    RmlUI::ShowDocument(kMainDocument);
    base_.setPath(settings_.base);  // checked in the background

    // A file on the command line, else the one opened last time.
    if (argc > 1) {
        requestLoad(argv[1]);
    } else if (std::error_code ec; !settings_.firmware.empty() && std::filesystem::exists(hx::pathFromUtf8(settings_.firmware), ec)) {
        requestLoad(settings_.firmware);
    }
    return true;
}

void App::openDialog() {
    static const SDL_DialogFileFilter filters[] = {
        {"Helix firmware or HX Edit preset", "hxf;hlx"}, {"Helix firmware", "hxf"}, {"HX Edit preset", "hlx"}, {"All files", "*"}};
    // The callback may run on another thread, so it only hands the path over.
    SDL_ShowOpenFileDialog(
        [](void *self, const char *const *files, int) {
            if (files && files[0]) static_cast<App *>(self)->requestLoad(files[0]);
        },
        this, Window::GetWindow(), filters, 4, nullptr, false);
}

void App::baseDialog() {
    static const SDL_DialogFileFilter filters[] = {{"Helix firmware", "hxf"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(
        [](void *self, const char *const *files, int) {
            if (!files || !files[0]) return;
            App *app = static_cast<App *>(self);
            std::lock_guard lock(app->pendingMutex_);
            app->pendingBase_ = files[0];
        },
        this, Window::GetWindow(), filters, 2, nullptr, false);
}

void App::openFlash(const std::string &path) {
    flash_->useFile(path);
    shell_.setPage("flash");  // = Firmware > Flash
    if (shell_.onChanged) shell_.onChanged();
}

void App::exportDialog() {
    if (!view_.hasFirmware()) return;
    static const SDL_DialogFileFilter filters[] = {{"HX Edit preset", "hlx"}};
    // A file name from the preset's name, without characters Windows refuses.
    std::string name = view_.presetName().empty() ? "preset" : view_.presetName();
    for (char &c : name)
        if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 32) c = '_';
    const std::u8string u = (hx::pathFromUtf8(settings_.firmware).parent_path() / hx::pathFromUtf8(name + ".hlx")).u8string();
    const std::string suggested(u.begin(), u.end());
    SDL_ShowSaveFileDialog(
        [](void *self, const char *const *files, int) {
            if (!files || !files[0]) return;
            App *app = static_cast<App *>(self);
            std::lock_guard lock(app->pendingMutex_);
            app->pendingExport_ = files[0];
        },
        this, Window::GetWindow(), filters, 1, suggested.c_str());
}

namespace {
// A file name from a preset name, without characters Windows refuses.
std::string safeFileName(std::string name) {
    if (name.empty()) name = "preset";
    for (char &c : name)
        if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 32) c = '_';
    return name;
}
constexpr const char *kBackupExt = ".hxp";  // one stored preset as the unit holds it (its l6-helix bytes)
} // namespace

// Backups and restores of the unit's stored presets: a file (or folder) dialog, then runLibraryJob on the main
// thread. Backups only read from the unit; a restore goes through the editor's write (and its "Ask first").
void App::libraryDialog(LibRequest request) {
    static const SDL_DialogFileFilter filters[] = {{"Helix preset backup", "hxp"}, {"All files", "*"}};
    auto done = [](void *self, const char *const *files, int) {
        if (!files || !files[0]) return;
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->pendingMutex_);
        app->pendingLibPath_ = files[0];
    };
    libRequest_ = std::move(request);
    const std::u8string dir = hx::pathFromUtf8(settings_.firmware).parent_path().u8string();
    const std::string folder(dir.begin(), dir.end());
    switch (libRequest_.job) {
    case LibJob::BackupPreset: {
        const std::u8string u = (hx::pathFromUtf8(folder) /
                                 hx::pathFromUtf8(hx::presetNumber(libRequest_.preset) + " " + safeFileName(libRequest_.name) +
                                                  kBackupExt))
                                    .u8string();
        const std::string suggested(u.begin(), u.end());
        SDL_ShowSaveFileDialog(done, this, Window::GetWindow(), filters, 2, suggested.c_str());
        break;
    }
    case LibJob::BackupSetlist:
        SDL_ShowOpenFolderDialog(done, this, Window::GetWindow(), folder.empty() ? nullptr : folder.c_str(), false);
        break;
    case LibJob::Restore: SDL_ShowOpenFileDialog(done, this, Window::GetWindow(), filters, 2, nullptr, false); break;
    case LibJob::None: break;
    }
}

void App::runLibraryJob(const LibRequest &request, const std::string &path) {
    switch (request.job) {
    case LibJob::BackupPreset: {
        std::filesystem::path out = hx::pathFromUtf8(path);
        if (out.extension().empty()) out += kBackupExt;
        const std::u8string u = out.u8string();
        device_->sendLibraryRead(hx::editor::kReadPreset, hx::editor::readPresetArgs(request.setlist, request.preset),
                                 "file:" + std::string(u.begin(), u.end()));
        view_.setStatus("Backing up " + request.name + "...");
        break;
    }
    case LibJob::BackupSetlist: {
        // Every slot, named "08B Name.hxp" so a folder lists them in setlist order.
        backupLeft_ = hx::kPresetsPerSetlist;
        backupDone_ = 0;
        for (int i = 0; i < hx::kPresetsPerSetlist; ++i) {
            const std::u8string u =
                (hx::pathFromUtf8(path) /
                 hx::pathFromUtf8(hx::presetNumber(i) + " " + safeFileName(view_.storedName(request.setlist, i)) + kBackupExt))
                    .u8string();
            device_->sendLibraryRead(hx::editor::kReadPreset, hx::editor::readPresetArgs(request.setlist, i),
                                     "file:" + std::string(u.begin(), u.end()));
        }
        view_.setStatus("Backing up the setlist (128 presets)...");
        break;
    }
    case LibJob::Restore: {
        std::vector<unsigned char> bytes;
        if (!readFile(hx::pathFromUtf8(path), bytes)) return view_.setStatus("Could not read " + fileName(path));
        // The name from the file's: "08B My Tone.hxp" (as backups are written) -> "My Tone".
        const std::u8string stem8 = hx::pathFromUtf8(path).stem().u8string();
        std::string name(stem8.begin(), stem8.end());
        if (name.size() > 4 && std::isdigit(static_cast<unsigned char>(name[0])) &&
            std::isdigit(static_cast<unsigned char>(name[1])) && name[2] >= 'A' && name[2] <= 'D' && name[3] == ' ')
            name = name.substr(4);
        view_.restoreStored(request.setlist, request.preset, bytes, name);
        break;
    }
    case LibJob::None: break;
    }
}

// A stored preset the unit sent: the clipboard (Copy) or a backup file.
void App::storedRead(const std::string &tag, const std::vector<std::uint8_t> &blob) {
    if (tag == "copy") {
        if (!blob.empty()) view_.applyStoredCopy(blob);
        return;
    }
    if (tag.rfind("file:", 0) != 0) return;
    const std::string path = tag.substr(5);
    bool ok = !blob.empty();
    if (ok) {
        std::ofstream f(hx::pathFromUtf8(path), std::ios::binary);
        ok = f && f.write(reinterpret_cast<const char *>(blob.data()), std::streamsize(blob.size()));
    }
    if (backupLeft_ > 0) {
        --backupLeft_;
        if (ok) ++backupDone_;
        if (backupLeft_ == 0) {
            const std::u8string folder = hx::pathFromUtf8(path).parent_path().u8string();
            view_.setStatus("Backed up " + std::to_string(backupDone_) + " of 128 presets to " +
                            std::string(folder.begin(), folder.end()));
        }
        return;
    }
    view_.setStatus(ok ? "Backed up to " + fileName(path) : "Could not back up to " + fileName(path));
}

// The dialog may hand over a name without the extension.
void App::writeExport(const std::string &path) {
    std::filesystem::path out = hx::pathFromUtf8(path);
    if (out.extension().empty()) out += ".hlx";
    std::vector<std::string> warnings;
    const std::string text = view_.exportHlx(&warnings);
    for (const std::string &w : warnings) LOG_WARNING("export: {}", w);
    std::ofstream f(out, std::ios::binary);
    if (!f || !f.write(text.data(), std::streamsize(text.size()))) {
        view_.setStatus("Could not write " + fileName(path));
        return;
    }
    view_.setStatus("Exported " + fileName(path) +
                    (warnings.empty() ? std::string() : "  (" + std::to_string(warnings.size()) + " warnings: " + warnings[0] + ")"));
}

void App::matchUnitBuild() {
    if (unitTag_.empty() || !fw_ || loading_.valid()) return;
    if (fw_->identity.kind == hx::FirmwareIdentity::Kind::Ours && fw_->identity.tag == unitTag_) return;
    std::string path = hx::mods::findBuild(unitTag_);
    if (path.empty() && base_.info().stock.ok) {  // a build made before the history: next to the base firmware
        std::error_code ec;
        if (const std::string next = hx::mods::defaultOutPath(base_.info().stock, base_.info().path);
            std::filesystem::is_regular_file(hx::pathFromUtf8(next), ec))
            path = next;
    }
    if (path.empty() || path == triedBuild_) {
        view_.setStatus("The Helix runs mod build " + unitTag_ + ", not the open file: open that build's firmware to see its models");
        return;
    }
    triedBuild_ = path;
    LOG_INFO("Helix runs build {}: opening {}", unitTag_, path);
    requestLoad(path);
}

void App::requestLoad(std::string path) {
    std::lock_guard lock(pendingMutex_);
    pendingPath_ = std::move(path);
}

void App::startLoad(const std::string &path) {
    view_.setStatus("Loading " + fileName(path) + "...");
    loading_ = std::async(std::launch::async, [path] {
        LoadResult r;
        r.path = path;
        r.fw = hx::loadFirmware(path, &r.error);
        return r;
    });
}

void App::pollLoad() {
    {
        std::string out, lib, base;
        {
            std::lock_guard lock(pendingMutex_);
            out.swap(pendingExport_);
            lib.swap(pendingLibPath_);
            base.swap(pendingBase_);
        }
        if (!base.empty()) base_.setPath(base);
        if (!out.empty()) writeExport(out);
        if (!lib.empty()) runLibraryJob(libRequest_, lib);
    }
    if (!loading_.valid()) {
        std::string path;
        {
            std::lock_guard lock(pendingMutex_);
            path.swap(pendingPath_);
        }
        if (!path.empty()) {
            if (isPresetFile(path)) importPreset(view_, path);
            else startLoad(path);
        }
        return;
    }
    if (loading_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    LoadResult r = loading_.get();
    if (!r.fw) {
        LOG_WARNING("Could not load {}: {}", r.path, r.error);
        view_.setStatus(fileName(r.path) + ": " + r.error);
        return;
    }
    // The view points into the firmware: hand it the new one before the old one goes.
    std::unique_ptr<hx::Firmware> old = std::move(fw_);
    fw_ = std::move(r.fw);
    // Reopen the remembered setlist and preset (also when a different .hxf is opened: same layout).
    fwtex::setGraphics(fw_->gfx ? &*fw_->gfx : nullptr);
    view_.setFirmware(fw_.get(), settings_.setlist, settings_.preset);
    shell_.setFirmware(fw_.get());
    base_.setOpenFile(fw_.get());
    // No base firmware yet: the first stock file opened becomes it.
    if (settings_.base.empty() && !base_.checking() && hx::mods::stockOf(*fw_).ok) base_.setPath(fw_->path);
    // The unit's global settings are named from this firmware (kept whole, shown filtered for the unit model).
    allGlobalSettings_ = hx::readGlobalSettings(fw_->app, &fw_->warnings);
    updateUnitModel(true);
    if (device_->sessionRunning()) device_->readSettings(globalSettings_.ids());
    flash_->setEditorFile(fw_->path);
    old.reset();
    settings_.firmware = fw_->path;
    saveSettings();

    std::string status = fileName(fw_->path);
    if (!fw_->warnings.empty()) status += "  (" + std::to_string(fw_->warnings.size()) + " warnings, see the Firmware tab)";
    for (const std::string &w : fw_->warnings) LOG_WARNING("{}", w);
    view_.setStatus(status);
    matchUnitBuild();  // connected to one of our builds: is this its file?
}

void App::setOnline(bool online) {
    if (online == settings_.online) return;
    settings_.online = online;
    saveSettings();
    if (!online) device_->stopSession();  // frees the unit for HX Edit or the Updater
    view_.setOnline(online, settings_.unit);
    if (online) view_.setConnection(false, "Looking for a Helix...");
    irs_.setConnection(false, online ? "Looking for a Helix..." : "Offline: the IR slots are on the Helix");
    globalEq_.setConnection(false);
    globalSettings_.setConnection(false);
}

// The model the editor works for: the connected unit, else the open .hxf's product, else the offline unit picker.
// Its hardware filters the editor's own lists (I/O ports, global settings). force: refilter even when it is the same
// (a new firmware brings new setting names).
void App::updateUnitModel(bool force) {
    hx::UnitModel m = connectedUnit_;
    if (m == hx::UnitModel::Unknown && fw_ && fw_->hxf) m = hx::unitFromProductId(fw_->hxf->productId);
    if (m == hx::UnitModel::Unknown) m = hx::unitFromId(settings_.unit);
    if (m == unitModel_ && !force) return;
    unitModel_ = m;
    view_.setUnitModel(m);
    std::vector<hx::GlobalSetting> shown;
    for (const hx::GlobalSetting &s : allGlobalSettings_)
        if (hx::globalSettingAvailable(m, s.id)) shown.push_back(s);
    globalSettings_.setDefinitions(std::move(shown));
}

void App::pollDevice() {
    if (settings_.online && !device_->busy() && !device_->sessionRunning()) device_->startSession();
    for (hx::SessionEvent &e : device_->takeSessionEvents()) {
        using K = hx::SessionEvent::Kind;
        switch (e.kind) {
        case K::Searching:
            LOG_INFO("Helix: {}", e.text);
            if (settings_.online) {
                const char *why = e.text.find("in use") != std::string::npos ? "Helix in use by another program"
                                                                             : "Looking for a Helix...";
                view_.setConnection(false, why);
                irs_.setConnection(false, why);
            }
            globalEq_.setConnection(false);
            globalSettings_.setConnection(false);
            shell_.setUnit(nullptr);
            break;
        case K::Connected:
            LOG_INFO("Helix connected: {}", e.text);
            // The USB link only opens the LT's product ID (core/device/usbio.h): connected means a Helix LT.
            connectedUnit_ = hx::UnitModel::HelixLt;
            updateUnitModel();
            view_.setConnection(true, std::string(hx::unitName(connectedUnit_)) + " connected");
            irs_.setConnection(true, std::string(hx::unitName(connectedUnit_)) + " connected");
            globalEq_.setConnection(true);
            globalSettings_.setConnection(true);
            if (const std::vector<int> ids = globalSettings_.ids(); !ids.empty()) device_->readSettings(ids);
            break;
        case K::GlobalEq: globalEq_.setValues(e.numbers, e.flag); break;
        case K::Build: {
            const hx::FirmwareIdentity id = hx::identifyUnit(e.flag ? std::optional<std::string>(e.text) : std::nullopt);
            LOG_INFO("Helix runs: {}", id.summary);
            shell_.setUnit(&id);
            unitTag_ = id.kind == hx::FirmwareIdentity::Kind::Ours ? id.tag : std::string();
            matchUnitBuild();
            break;
        }
        case K::Settings: globalSettings_.setValues(e.numbers); break;
        case K::Irs: {
            irs_.setIrs(e.irs);
            // for the IR Select / NAM Model dropdowns and the IR / NAM mix-up warning
            std::vector<std::string> names(hx::editor::kIrSlots);
            std::vector<int> kinds(hx::editor::kIrSlots, 0);
            for (const hx::editor::IrSlot &s : e.irs)
                if (s.slot >= 0 && s.slot < hx::editor::kIrSlots) {
                    names[std::size_t(s.slot)] = s.name.empty() ? "(no name)" : s.name;
                    using K = hx::editor::IrSlot::Kind;
                    kinds[std::size_t(s.slot)] = s.kind == K::Nam ? 2 : s.kind == K::Ir ? 1 : 0;
                }
            view_.setIrNames(std::move(names), std::move(kinds));
            break;
        }
        case K::Info: view_.applyDeviceInfo(e.current); break;
        case K::Stored: storedRead(e.text, e.presetBlob); break;
        case K::Library: view_.applyDeviceLibrary(e.setlists, e.presetNames); break;
        case K::Preset: view_.applyDevicePreset(e.current, e.presetBlob); break;
        case K::Notice:
            view_.applyDeviceNotice(e.notice);
            globalEq_.applyNotice(e.notice);
            globalSettings_.applyNotice(e.notice);
            break;
        case K::Copied: view_.setClipBlob(std::move(e.presetBlob)); break;
        case K::Log: monitor_.addLog(std::move(e.text)); break;
        case K::Refused:
            LOG_WARNING("Helix: {}", e.text);
            view_.setStatus("The Helix refused an edit: " + e.text);
            break;
        case K::Disconnected:
            if (e.text != "stopped") LOG_WARNING("Helix connection lost: {}", e.text);
            if (settings_.online) {
                const char *why = device_->busy() ? "Helix busy (Firmware > Flash)" : "Looking for a Helix...";
                view_.setConnection(false, why);
                irs_.setConnection(false, why);
            }
            globalEq_.setConnection(false);
            globalSettings_.setConnection(false);
            shell_.setUnit(nullptr);
            unitTag_.clear();
            view_.setIrNames({});
            connectedUnit_ = hx::UnitModel::Unknown;
            updateUnitModel();
            break;
        }
    }
    view_.flushDeviceNotices();
}

bool App::iterate() {
    pollLoad();
    pollDevice();
    flash_->update(shell_.page() == "firmware" && shell_.firmwareTab() == "flash");
    irs_.update();
    base_.update();
    mods_.update(shell_.page() == "mods");
    monitor_.update(shell_.page() == "monitor");
    view_.update();
    globalEq_.update(shell_.page() == "helix");
    if (Input::KeyPressed(SDLK_F12)) RmlUI::EnableDebugger(rmlDebugger_ = !rmlDebugger_);

    Window::StartFrame();
    Window::EndFrame();
    return true;
}

// The engine has already routed the event to RmlUi/Input and handles the window close.
void App::handleEvent(const SDL_Event &event) {
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
        // .wav / .nam: into the IR slot under the pointer (the IRs tab opens for it); anything else is a firmware or
        // a preset file.
        if (IrView::handles(event.drop.data)) {
            int slot = -1;
            if (shell_.page() == "irs")
                if (Rml::Context *ctx = RmlUI::GetContext()) {
                    const float density = SDL_GetWindowPixelDensity(Window::GetWindow());
                    for (Rml::Element *e = ctx->GetElementAtPoint({event.drop.x * density, event.drop.y * density}); e;
                         e = e->GetParentNode())
                        if (e->HasAttribute("irslot")) {
                            slot = e->GetAttribute<int>("irslot", -1);
                            break;
                        }
                }
            shell_.setPage("irs");
            irs_.openFile(event.drop.data, slot);
        } else if (ModsView::handles(event.drop.data)) {
            // A mod package: into the cache, listed in the Mods tab.
            shell_.setPage("mods");
            mods_.importFile(event.drop.data);
        } else {
            requestLoad(event.drop.data);
        }
    }
    if (event.type != SDL_EVENT_KEY_DOWN) return;
    // F1..F9 pick a rail tab (F12: RmlUi debugger, in iterate()).
    if (!event.key.repeat && event.key.key >= SDLK_F1 && event.key.key <= SDLK_F9) {
        shell_.pickTab(int(event.key.key - SDLK_F1));
        return;
    }
    if (event.key.key == SDLK_ESCAPE && menu_.isOpen()) return menu_.close();
    if (event.key.key == SDLK_ESCAPE && shell_.page() == "presets" && !textFieldFocused()) return view_.cancelPicking();
    if (!(event.key.mod & SDL_KMOD_CTRL)) return;
    // Ctrl+C in a text box: the engine's RmlUi system interface has no clipboard, so the selection goes to the
    // system clipboard from here (e.g. lines out of Monitor > USB).
    if (event.key.key == SDLK_C && copyTextSelection()) return;
    // The preset editor's keys. Not while a text field has focus: there they are the field's.
    if (shell_.page() != "presets" || textFieldFocused()) return;
    // Ctrl+1..8: snapshot 1..8.
    if (!event.key.repeat && event.key.key >= SDLK_1 && event.key.key <= SDLK_8) {
        view_.selectSnapshot(int(event.key.key - SDLK_1));
        return;
    }
    // Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z): undo / redo; Ctrl+C / Ctrl+V: block copy / paste; Ctrl+S: save (asks).
    const bool shift = event.key.mod & SDL_KMOD_SHIFT;
    if (event.key.key == SDLK_S && !event.key.repeat) return view_.askSave();
    if (event.key.key == SDLK_Z) shift ? view_.redo() : view_.undo();
    else if (event.key.key == SDLK_Y) view_.redo();
    else if (event.key.key == SDLK_C) view_.copyBlock();
    else if (event.key.key == SDLK_V) view_.pasteBlock();
}

void App::shutdown() {
    // Let a load in progress finish rather than leave its thread running past the window.
    if (loading_.valid()) loading_.wait();
    // Waits for a running device job: a flash stopped halfway would leave a wiped slot.
    flash_.reset();
    device_.reset();
}

} // namespace editor

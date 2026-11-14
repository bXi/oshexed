#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <functional>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
}
namespace hx {
struct Firmware;
struct FirmwareIdentity;
}

namespace editor {

// The tab rail on the left and the pages that aren't the preset editor (that one is EditorView).
// Only pages that exist get a tab; new modules (mods, NAM, flash) add theirs when they land.
class ShellView {
public:
    void bind(Rml::DataModelConstructor &model);
    void setFirmware(const hx::Firmware *fw);  // nullptr: nothing loaded
    // What the connected unit runs (its answer to command 200); nullptr: no unit connected.
    void setUnit(const hx::FirmwareIdentity *id);

    // Unknown names keep the current page (e.g. an old settings.ini).
    void setPage(const std::string &page);
    void setRailOpen(bool open);
    // F1..F9: tab n (0-based); ignored past the last tab.
    void pickTab(int n);
    const std::string &page() const { return page_; }
    // The Firmware page's sub-tab: "unit", "file" or "flash".
    const std::string &firmwareTab() const { return fwTab_; }
    bool railOpen() const { return railOpen_; }
    // The Firmware tab's notice (risk of modified firmware, the recovery path): shown over the tab the first time it's
    // opened in a session, until dismissed. Off for good once dismissed with "Don't show again" (onChanged).
    void setFirmwareNoticeEnabled(bool on) { noticeEnabled_ = on; }
    bool firmwareNoticeEnabled() const { return noticeEnabled_; }

    // The user changed the page or the rail, so the app can remember it.
    std::function<void()> onChanged;

    struct InfoRow {
        std::string label, value;
        bool bad = false;
    };
    // The head of the Unit / File sub-tabs: what the firmware is, in big type, then its details.
    // tone: "ours", "stock", "bad" or "none" (the colour of the headline and of the sub-tab's status).
    struct Card {
        std::string headline = "Not connected";
        std::string detail = "Connect the Helix by USB and switch it on (Settings > Editor: Online). Close HX Edit and "
                             "the Line 6 Updater first: only one program can talk to it.";
        std::string tone = "none";
        std::vector<InfoRow> rows;
        // Our builds: every mod the registry knows (label = name, value = what it covers, bad = not in this
        // build), then any slot this editor doesn't know.
        std::vector<InfoRow> mods;
    };
    static Card noFileCard() { return {"No file open", "Open a .hxf, or drop it on the window.", "none", {}, {}}; }

private:
    struct RecordRow {
        std::string name, size, crc, checks;
        bool ok = true;
    };

    void changed();
    void setFirmwareTab(const std::string &tab);

    std::string page_ = "presets";
    bool railOpen_ = true;
    std::string fwPath_;
    std::vector<InfoRow> fwInfo_;
    std::vector<std::string> fwWarnings_;
    std::vector<RecordRow> fwRecords_;
    std::string fwTab_ = "unit";
    std::string helixTab_ = "eq";  // the Helix page's sub-tab: "eq" (Global EQ) or "settings" (Global Settings)
    Card unitCard_, fileCard_ = noFileCard();
    bool noticeEnabled_ = true;   // the setting
    bool noticeShown_ = false;    // over the Firmware tab now
    bool noticeDone_ = false;     // dismissed this session
    bool noticeNoMore_ = false;   // the "Don't show again" box
    void showNoticeIfDue();
    Rml::DataModelHandle handle_;
};

} // namespace editor

#pragma once

#include <string>

namespace editor {

// What the app remembers between runs, in settings.ini (key=value lines) in the writable directory.
struct Settings {
    std::string firmware;  // last .hxf opened (UTF-8 path)
    std::string base;      // the base (stock) firmware: Firmware tab > Base firmware
    int setlist = 0;
    int preset = 0;        // index within the setlist
    std::string page = "presets";  // rail tab shown last
    bool railOpen = true;          // rail with labels (true) or icons only
    bool online = true;            // connect to the unit (and keep looking for one); false = offline editing
    std::string unit = "helix-lt"; // the unit offline editing is for (kUnits in editorview.cpp)
    bool confirmSave = false;      // ask before Save writes a preset to the unit (off: saves at once, as HX Edit)
    bool firmwareNotice = true;    // the Firmware tab's risk / recovery notice ("Don't show again" turns it off)

    // Missing file or keys keep the defaults; unknown keys are ignored.
    void load(const std::string &path);
    bool save(const std::string &path) const;
};

} // namespace editor

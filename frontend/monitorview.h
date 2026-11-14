#pragma once

#include "core/midi.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <deque>
#include <string>
#include <vector>

namespace Rml {
class DataModelConstructor;
}

namespace editor {

// The Monitor tab. Two sub-tabs: "usb" = the editor protocol traffic with the unit and status lines (what went
// out, what came back, notices), in a text box so lines can be selected and copied; "midi" = MIDI input from one
// or every port (read-only) with the NAM / polymidi debug words decoded (core/midi.h), newest first.
class MonitorView {
public:
    // After ShellView::bind (it registers the string array type).
    void bind(Rml::DataModelConstructor &model);
    // One line for the USB log (kept: the last kUsbLines).
    void addLog(std::string line);
    // Every frame. visible: the Monitor tab is shown (lists are only rebuilt then).
    void update(bool visible);

private:
    struct MidiRow {
        std::string time, port, text;
        bool debug = false;  // one of the debug-word CCs
        bool realtime = false;  // clock / active sensing (not bound)
    };
    struct WordRow {
        std::string name, value, hex, age;
        bool seen = false;
    };

    void rebuildUsb();
    void rebuildMidi();
    void rebuildWords();
    void setTab(const std::string &tab);
    void dirty(const char *name);

    static constexpr std::size_t kUsbLines = 2000, kMidiLines = 2000, kShown = 400, kUsbShown = 600;
    Rml::DataModelHandle handle_;
    std::string tab_ = "usb";

    std::deque<std::string> usb_;
    // The text box: the last kShown lines, oldest first. Not refreshed while it has focus (selecting) or while
    // paused, so a selection holds still; after a refresh it scrolls to the end.
    std::string usbText_;
    bool usbDirty_ = false, usbFocused_ = false, usbPaused_ = false;
    bool guarded_ = false;  // the read-only guard is on the box's wrapper
    int scrollPending_ = 0;  // frames left to keep the box scrolled to the end (layout of the new text lags)
    std::string usbCount_ = "empty";

    hx::midi::Input midi_;
    hx::midi::DebugWords words_;
    std::deque<MidiRow> midiAll_;
    std::vector<MidiRow> midiShown_;
    bool midiDirty_ = false;
    bool hideRealtime_ = true, debugOnly_ = false;
    std::vector<std::string> ports_;  // the listened ports: "name" / "name  (can't open: why)"
    // The port dropdown: "all" or a portNames() index as text; the names are read again while not listening.
    std::string portChoice_ = "all";
    std::vector<std::string> portNames_;
    double lastPortScan_ = -10;
    void scanPorts();
    void listen(bool on);
    std::vector<WordRow> wordRows_;
    std::string midiState_ = "Off";
    double lastRebuild_ = 0, lastWords_ = 0;
};

} // namespace editor

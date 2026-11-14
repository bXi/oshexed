#pragma once

// The one owner of the unit's connection. Jobs run on their own thread (one at a time); the UI polls
// takeEvents() each frame. Later device features (editor protocol) become more jobs here.

#include "core/device/editorclient.h"
#include "core/device/link.h"
#include "core/device/updater.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hx {

struct DeviceEvent {
    enum class Kind {
        Log,         // text
        Info,        // info (read-only check finished)
        Record,      // record + step
        RecordBytes, // record + done / total
        SlotCheck,   // done = slot, total = count
        Done,        // text = summary
        Failed,      // text = reason; wrote = a slot had been touched (recovery needed)
    };
    Kind kind = Kind::Log;
    std::string text;
    DeviceInfo info;
    std::uint32_t record = 0;
    RecordStep step = RecordStep::Prepare;
    std::size_t done = 0, total = 0;
    bool wrote = false;
};

// What the editor session reports (takeSessionEvents()).
struct SessionEvent {
    enum class Kind {
        Searching,     // text = why no unit yet (retrying)
        Connected,     // text = USB description
        Library,       // setlists, presetNames
        Preset,        // current (setlist, preset, snapshot, name) + presetBlob (the l6-helix body)
        Notice,        // notice: something changed on the unit
        Copied,        // presetBlob: the unit's current preset, asked for with sendEdit(kCurrentPreset) (Copy)
        Log,           // text: one line of traffic (command sent + the unit's answer, or a notice)
        Irs,           // irs: the unit's filled IR slots (at connect and after refreshIrs)
        Info,          // current: the current preset's info alone (its modified flag after a change on the unit)
        Stored,        // text = the read's tag, presetBlob = what it returned (empty if refused)
        GlobalEq,      // numbers = the 13 global EQ values (cmd 76), flag = bypassed (setting 203); at connect
        Build,         // flag = the unit answered command 200, text = its version string (MODTAG.md); at connect
        Settings,      // numbers = id, value, id, value, ... (global settings asked for with readSettings)
        Refused,       // text: the unit answered a command with an error
        Disconnected,  // text = reason (retrying)
    };
    Kind kind = Kind::Searching;
    std::string text;
    std::vector<std::string> setlists;
    std::vector<std::pair<int, std::string>> presetNames;  // (slot = setlist * 128 + n, name)
    editor::CurrentInfo current;
    std::vector<std::uint8_t> presetBlob;
    EditorNotice notice;
    std::vector<editor::IrSlot> irs;
    std::vector<double> numbers;
    bool flag = false;
};

class DeviceLink {
public:
    using IoFactory = std::function<std::unique_ptr<LinkIo>(std::string *error)>;

    // statePath: where UnitState lives ("" = memory only). io: how to reach the unit (USB by default).
    explicit DeviceLink(std::string statePath, IoFactory io = {});
    // Waits for a running job: stopping a flash halfway would leave a wiped slot.
    ~DeviceLink();
    DeviceLink(const DeviceLink &) = delete;
    DeviceLink &operator=(const DeviceLink &) = delete;

    bool busy() const { return busy_; }

    // Each returns false (and does nothing) while another job runs.
    bool readInfo();
    // simulate: against the fake unit instead of USB (rehearsal, nothing leaves the PC; UnitState untouched).
    bool flash(std::vector<RecordFile> files, FlashOptions options, bool simulate = false);
    // Honoured only before the first slot is touched.
    void requestStop() { stop_ = true; }

    std::vector<DeviceEvent> takeEvents();

    // The editor session: connects to the unit (retrying every 2 s while none answers), reads the setlists, preset
    // names and current preset, sends queued edits and reports the unit's own changes. Stopped while a flash or
    // info job runs (they need the connection); start it again afterwards.
    void startSession();
    void stopSession();  // waits for it to close the connection
    bool sessionRunning() const { return sessionThread_.joinable(); }
    // An edit for the unit's current preset (editing channel). Knob changes for the same knob replace a queued
    // one, so a slider drag sends only the latest value.
    void sendEdit(int command, mp::Value args);
    // Reads the IR slot list again (library channel); answered with an Irs event.
    void refreshIrs() { irsWanted_ = true; }
    // Reads these global settings (cmd 24 each); answered with one Settings event.
    void readSettings(std::vector<int> ids) {
        std::lock_guard lock(sessionMutex_);
        settingsWanted_ = std::move(ids);
    }
    // A write to the unit's storage on the library channel (IR upload / rename / clear), wrapped as HX Edit does.
    // The caller has asked the user first (device safety). When it's done the IR list is read again (an Irs event).
    void sendLibraryWrite(int command, mp::Value args);
    // A library read (e.g. a stored preset, lib 4): answered with a Stored event carrying `tag` and the data.
    void sendLibraryRead(int command, mp::Value args, std::string tag);
    std::vector<SessionEvent> takeSessionEvents();

    // Only while !busy(): the flash thread updates it.
    const UnitState &unitState() const { return state_; }
    void assume(const std::vector<RecordFile> &files, const std::string &name);

private:
    bool start(std::function<void()> job);
    void post(DeviceEvent e);
    void log(const std::string &s);
    void sessionLoop();
    void sessionWork(EditorClient &c);
    void readCurrent(EditorClient &c);
    void readIrs(EditorClient &c);
    // What each filled slot holds (IR or NAM capture), from its first sample words (library cmd 11), cached by the
    // slot's hash so a slot is read once per content, kept on disk (link thread only).
    std::map<std::string, editor::IrSlot> irKinds_;
    bool irKindsLoaded_ = false;
    std::vector<editor::IrSlot> irList_;  // the last list, kinds filled in as they're read
    std::deque<std::size_t> irProbe_;     // its entries still to read
    void probeIrs(EditorClient &c, int count);
    void loadIrKinds();
    void saveIrKinds() const;
    void readGlobalEq(EditorClient &c);
    void readBuild(EditorClient &c);
    void postSession(SessionEvent e);
    void waitOrStop(double seconds);

    struct Edit {
        int command = 0;
        mp::Value args;
        EditorChannel channel = EditorChannel::Edit;
        std::string tag;    // a library read: handed back with its Stored event
        bool read = false;  // a library read (no wrap, nothing changes on the unit)
    };
    void libraryWrite(EditorClient &c, const Edit &e);
    void libraryDone(EditorClient &c, int command);
    void readLibrary(EditorClient &c);
    void logCall(int command, const mp::Value &args, const EditorClient::Reply &r);
    void refused(int command, const EditorClient::Reply &r);
    int libraryPending_ = 0;  // the library write that answered 1, finished at its kNoticeLibraryDone; 0 = none
    std::thread sessionThread_;
    std::atomic<bool> sessionStop_{false};
    std::atomic<bool> irsWanted_{false};
    std::mutex sessionMutex_;
    std::deque<Edit> edits_;
    std::vector<int> settingsWanted_;
    std::vector<SessionEvent> sessionEvents_;

    IoFactory io_;
    UnitState state_;
    std::thread thread_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::vector<DeviceEvent> events_;
};

} // namespace hx

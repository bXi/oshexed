#include "core/device/devicelink.h"

#include "core/device/fakeunit.h"
#include "core/device/usbio.h"

#include "core/appdata.h"
#include "core/nam.h"
#include "core/text.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

namespace hx {

DeviceLink::DeviceLink(std::string statePath, IoFactory io) : io_(std::move(io)), state_(std::move(statePath)) {
    if (!io_) io_ = [](std::string *error) { return openUsbLink(error); };
}

DeviceLink::~DeviceLink() {
    stopSession();
    if (thread_.joinable()) thread_.join();
}

bool DeviceLink::start(std::function<void()> job) {
    if (busy_) return false;
    stopSession();  // one program, one connection: the job needs it
    if (thread_.joinable()) thread_.join();
    stop_ = false;
    busy_ = true;
    thread_ = std::thread([this, job = std::move(job)] {
        job();
        busy_ = false;
    });
    return true;
}

void DeviceLink::post(DeviceEvent e) {
    std::lock_guard lock(mutex_);
    events_.push_back(std::move(e));
}

void DeviceLink::log(const std::string &s) {
    DeviceEvent e;
    e.text = s;
    post(std::move(e));
}

std::vector<DeviceEvent> DeviceLink::takeEvents() {
    std::lock_guard lock(mutex_);
    std::vector<DeviceEvent> out;
    out.swap(events_);
    return out;
}

void DeviceLink::assume(const std::vector<RecordFile> &files, const std::string &name) {
    if (!busy_) state_.assume(files, name);
}

// ---------------------------------------------------------------- editor session

void DeviceLink::startSession() {
    if (sessionThread_.joinable() || busy_) return;
    sessionStop_ = false;
    sessionThread_ = std::thread([this] { sessionLoop(); });
}

void DeviceLink::stopSession() {
    if (!sessionThread_.joinable()) return;
    sessionStop_ = true;
    sessionThread_.join();
    std::lock_guard lock(sessionMutex_);
    edits_.clear();
}

void DeviceLink::sendEdit(int command, mp::Value args) {
    std::lock_guard lock(sessionMutex_);
    if (command == editor::kSetKnob) {
        auto same = [&](const mp::Value &a) {
            for (int key : {98, 26, 28}) {
                const mp::Value *x = a.get(key), *y = args.get(key);
                if (!x || !y || x->asInt() != y->asInt()) return false;
            }
            return true;
        };
        for (Edit &e : edits_)
            if (e.command == command && same(e.args)) {
                e.args = std::move(args);
                return;
            }
    }
    edits_.push_back({command, std::move(args)});
}

std::vector<SessionEvent> DeviceLink::takeSessionEvents() {
    std::lock_guard lock(sessionMutex_);
    std::vector<SessionEvent> out;
    out.swap(sessionEvents_);
    return out;
}

void DeviceLink::postSession(SessionEvent e) {
    std::lock_guard lock(sessionMutex_);
    sessionEvents_.push_back(std::move(e));
}

void DeviceLink::waitOrStop(double seconds) {
    for (double t = 0; t < seconds && !sessionStop_; t += 0.05) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void DeviceLink::sessionLoop() {
    std::string lastReason;
    while (!sessionStop_) {
        std::string error;
        std::unique_ptr<LinkIo> io = io_(&error);
        if (!io) {
            // Not found / in use by HX Edit: say so once per reason, keep trying.
            if (error != lastReason) {
                SessionEvent e;
                e.kind = SessionEvent::Kind::Searching;
                e.text = error;
                postSession(std::move(e));
                lastReason = error;
            }
            waitOrStop(2.0);
            continue;
        }
        try {
            EditorClient c(*io);
            c.open();
            lastReason.clear();
            SessionEvent e;
            e.kind = SessionEvent::Kind::Connected;
            e.text = io->description();
            postSession(std::move(e));
            sessionWork(c);
            c.close();
        } catch (const std::exception &ex) {
            if (ex.what() != lastReason) {
                SessionEvent e;
                e.kind = SessionEvent::Kind::Disconnected;
                e.text = ex.what();
                postSession(std::move(e));
                lastReason = ex.what();
            }
            waitOrStop(2.0);
        }
    }
    SessionEvent e;
    e.kind = SessionEvent::Kind::Disconnected;
    e.text = "stopped";
    postSession(std::move(e));
}

void DeviceLink::readCurrent(EditorClient &c) {
    SessionEvent e;
    e.kind = SessionEvent::Kind::Preset;
    e.current = editor::currentInfo(c.call(EditorChannel::Edit, editor::kCurrentInfo, mp::nil()).result);
    const mp::Value body = c.call(EditorChannel::Edit, editor::kCurrentPreset, mp::nil()).result;
    e.presetBlob.assign(body.s.begin(), body.s.end());
    postSession(std::move(e));
}

// What a slot holds is cached by its hash (same hash = same samples) in memory and in <app data>/ir_kinds.txt
// ("hash kind loudness level" lines), so only new or changed slots are ever read.
namespace {
std::filesystem::path irKindsFile() {
    const std::string dir = appDataDir();
    return dir.empty() ? std::filesystem::path() : pathFromUtf8(dir) / "ir_kinds.txt";
}
} // namespace

void DeviceLink::loadIrKinds() {
    irKindsLoaded_ = true;
    const std::filesystem::path p = irKindsFile();
    if (p.empty()) return;
    std::ifstream in(p);
    std::string hash;
    int kind = 0;
    double loudness = 0, level = 0;
    while (in >> hash >> kind >> loudness >> level) {
        editor::IrSlot s;
        s.hash = hash;
        s.kind = kind == 2 ? editor::IrSlot::Kind::Nam : editor::IrSlot::Kind::Ir;
        s.namLoudness = loudness;
        s.namLevelDbu = level;
        irKinds_[hash] = s;
    }
}

void DeviceLink::saveIrKinds() const {
    const std::filesystem::path p = irKindsFile();
    if (p.empty()) return;
    std::ofstream out(p, std::ios::trunc);
    for (const auto &[hash, s] : irKinds_)
        out << hash << ' ' << (s.kind == editor::IrSlot::Kind::Nam ? 2 : 1) << ' ' << s.namLoudness << ' ' << s.namLevelDbu
            << '\n';
}

// The list at once, with what the cache knows; slots it doesn't know are read a few at a time by probeIrs between
// edits (library cmd 11: a slot's samples, of which the first words tell a NAM capture from an IR).
void DeviceLink::readIrs(EditorClient &c) {
    if (!irKindsLoaded_) loadIrKinds();
    irList_ = editor::irList(c.call(EditorChannel::Library, editor::kIrList, editor::irListArgs()).result);
    irProbe_.clear();
    for (std::size_t i = 0; i < irList_.size(); ++i) {
        editor::IrSlot &s = irList_[i];
        if (const auto it = irKinds_.find(s.hash); it != irKinds_.end()) {
            s.kind = it->second.kind;
            s.namLoudness = it->second.namLoudness;
            s.namLevelDbu = it->second.namLevelDbu;
        } else {
            irProbe_.push_back(i);
        }
    }
    SessionEvent e;
    e.kind = SessionEvent::Kind::Irs;
    e.irs = irList_;
    postSession(std::move(e));
}

// Up to `count` unknown slots read and cached; the list posted again once none are left.
void DeviceLink::probeIrs(EditorClient &c, int count) {
    for (int k = 0; k < count && !irProbe_.empty(); ++k) {
        const std::size_t i = irProbe_.front();
        irProbe_.pop_front();
        if (i >= irList_.size()) continue;
        editor::IrSlot &s = irList_[i];
        try {
            const mp::Value r = c.call(EditorChannel::Library, editor::kIrRead, editor::irReadArgs(s.slot)).result;
            float head[8] = {};
            const std::size_t n = std::min<std::size_t>(8, r.s.size() / 4);
            std::memcpy(head, r.s.data(), n * 4);  // LE float32, as the unit (and this program) stores them
            if (const std::optional<NamHeader> h = namHeader(head, n)) {
                s.kind = editor::IrSlot::Kind::Nam;
                s.namLoudness = h->loudness;
                s.namLevelDbu = h->levelDbu;
            } else if (n > 0) {
                s.kind = editor::IrSlot::Kind::Ir;
            }
        } catch (const LinkError &) {
            throw;  // the link is gone: the session loop reconnects
        } catch (const std::exception &) {
            continue;  // this slot stays unknown
        }
        if (s.kind != editor::IrSlot::Kind::Unknown && !s.hash.empty()) irKinds_[s.hash] = s;
    }
    saveIrKinds();  // what's known so far survives a disconnect midway
    // The list goes to the editor once, when all are read: rebuilding it every few slots made its rows (and the
    // editor's dropdowns) impossible to click while the check ran.
    if (!irProbe_.empty()) return;
    SessionEvent e;
    e.kind = SessionEvent::Kind::Irs;
    e.irs = irList_;
    postSession(std::move(e));
}

void DeviceLink::readGlobalEq(EditorClient &c) {
    SessionEvent e;
    e.kind = SessionEvent::Kind::GlobalEq;
    const mp::Value eq = c.call(EditorChannel::Edit, editor::kGlobalEq, mp::map()).result;
    if (const mp::Value *list = eq.get(55); list && list->isArray())
        for (const mp::Value &v : list->items) e.numbers.push_back(v.asNumber());
    const mp::Value bypass = c.call(EditorChannel::Edit, editor::kGlobal, mp::map({{118, mp::integer(editor::kGlobalEqBypass)}})).result;
    if (const mp::Value *v = bypass.get(119)) e.flag = v->asBool();
    postSession(std::move(e));
}

// Our builds answer with the version format string; stock answers an error (status -45).
void DeviceLink::readBuild(EditorClient &c) {
    SessionEvent e;
    e.kind = SessionEvent::Kind::Build;
    const mp::Value args = mp::map();
    try {
        const EditorClient::Reply r = c.call(EditorChannel::Edit, editor::kBuildTag, args, 2.0);
        logCall(editor::kBuildTag, args, r);
        e.flag = r.status == 0 && r.result.type == mp::Type::Str;
        if (e.flag) e.text = r.result.s;
    } catch (const LinkError &) {
        // No answer at all counts as "doesn't know the command"; the session goes on.
    }
    postSession(std::move(e));
}

void DeviceLink::logCall(int command, const mp::Value &args, const EditorClient::Reply &r) {
    SessionEvent ev;
    ev.kind = SessionEvent::Kind::Log;
    ev.text = "> cmd " + std::to_string(command) + " " + mp::toText(args) + "   < status " + std::to_string(r.status) +
              (r.result.isNil() ? "" : " " + mp::toText(r.result, 200));
    postSession(std::move(ev));
}

// The reply's 111 is the unit's error code (-306 for a model that didn't go in: DSP full?, -3 for a knob on an empty
// slot).
void DeviceLink::refused(int command, const EditorClient::Reply &r) {
    const mp::Value *code = r.result.get(111);
    SessionEvent ev;
    ev.kind = SessionEvent::Kind::Refused;
    ev.text = "the unit refused command " + std::to_string(command) + " (error " +
              std::to_string(code ? code->asInt() : r.status) + ")";
    if (command == editor::kSetModel && code && code->asInt() == -306)
        ev.text += ": probably not enough DSP left on that path";
    postSession(std::move(ev));
}

// As HX Edit: begin, the write, and (when it answered 0, or failed) the IR list and end at once; an answer of 1 is
// finished at the unit's kNoticeLibraryDone (sessionWork).
void DeviceLink::libraryWrite(EditorClient &c, const Edit &e) {
    logCall(editor::kLibraryBegin, mp::map(), c.call(EditorChannel::Library, editor::kLibraryBegin, mp::map()));
    const EditorClient::Reply r = c.call(EditorChannel::Library, e.command, e.args, 15.0);
    logCall(e.command, e.args, r);
    if (r.status == 1) {
        libraryPending_ = e.command;
        return;
    }
    if (r.status != 0) refused(e.command, r);
    libraryDone(c, e.command);
}

// After a library write: what it changed read again (the IR list, or the preset names and the current preset, whose
// slot a reorder may have moved), then the wrap closed.
void DeviceLink::libraryDone(EditorClient &c, int command) {
    if (command == editor::kIrUpload || command == editor::kIrRename || command == editor::kIrClear) {
        readIrs(c);
    } else {
        readLibrary(c);
        readCurrent(c);
    }
    logCall(editor::kLibraryEnd, mp::map(), c.call(EditorChannel::Library, editor::kLibraryEnd, mp::map()));
}

void DeviceLink::sendLibraryWrite(int command, mp::Value args) {
    std::lock_guard lock(sessionMutex_);
    edits_.push_back({command, std::move(args), EditorChannel::Library});
}

void DeviceLink::sendLibraryRead(int command, mp::Value args, std::string tag) {
    std::lock_guard lock(sessionMutex_);
    edits_.push_back({command, std::move(args), EditorChannel::Library, std::move(tag), true});
}

void DeviceLink::readLibrary(EditorClient &c) {
    SessionEvent lib;
    lib.kind = SessionEvent::Kind::Library;
    lib.setlists = editor::setlistNames(c.call(EditorChannel::Library, editor::kSetlistNames, mp::nil()).result);
    for (int s = 0; s < int(lib.setlists.size()) && !sessionStop_; ++s)
        for (auto &p : editor::presetNames(
                 c.call(EditorChannel::Library, editor::kPresetNames, editor::presetNamesArgs(s)).result))
            lib.presetNames.push_back(std::move(p));
    postSession(std::move(lib));
}

void DeviceLink::sessionWork(EditorClient &c) {
    libraryPending_ = 0;
    readBuild(c);
    readLibrary(c);
    readIrs(c);
    readGlobalEq(c);
    readCurrent(c);

    bool infoWanted = false;  // the modified flag, after a change made on the unit
    double lastInfo = 0;
    while (!sessionStop_) {
        std::deque<Edit> edits;
        {
            std::lock_guard lock(sessionMutex_);
            edits.swap(edits_);
        }
        bool reread = false;
        for (Edit &e : edits) {
            if (e.channel == EditorChannel::Library && e.read) {
                // A stored preset (or other library data) for the editor: Copy, backups.
                const EditorClient::Reply r = c.call(EditorChannel::Library, e.command, e.args);
                logCall(e.command, e.args, r);
                SessionEvent ev;
                ev.kind = SessionEvent::Kind::Stored;
                ev.text = e.tag;
                if (r.status == 0) ev.presetBlob.assign(r.result.s.begin(), r.result.s.end());
                else refused(e.command, r);
                postSession(std::move(ev));
                continue;
            }
            if (e.channel == EditorChannel::Library) {
                libraryWrite(c, e);
                continue;
            }
            const EditorClient::Reply r = c.call(EditorChannel::Edit, e.command, e.args);
            logCall(e.command, e.args, r);
            // Copy: the preset as the unit has it now, bytes untouched (Paste sends them back with kReplacePreset).
            if (e.command == editor::kCurrentPreset) {
                SessionEvent ev;
                ev.kind = SessionEvent::Kind::Copied;
                ev.presetBlob.assign(r.result.s.begin(), r.result.s.end());
                postSession(std::move(ev));
                continue;
            }
            // Loading or replacing a preset answers 1 and then does it; everything else answers 0.
            if (r.status != 0 && e.command != editor::kLoadPreset && e.command != editor::kReplacePreset) {
                // The editor already shows the edit: read the preset back so it matches the unit.
                refused(e.command, r);
                reread = true;
                // A refused model: what follows in this batch builds on it (its knobs, emptying the slot a moved
                // block came from), so none of it is sent; the re-read shows the unit's state.
                if (e.command == editor::kSetModel) break;
            }
            // A new model gets the unit's default knobs: read the preset back so the editor shows them.
            // After a save the unit may have the saved slot open (Save as): the re-read says which, and its flag.
            if (e.command == editor::kSetModel || e.command == editor::kClearSlot || e.command == editor::kSetSplitModel ||
                e.command == editor::kSavePreset)
                reread = true;
        }
        c.poll(0.01);  // returns as soon as the unit sends something; short so queued edits go out soon
        for (EditorNotice &n : c.takeNotices()) {
            // Knob events can stream (an expression pedal): not logged.
            if (n.event != editor::kNoticeKnob) {
                SessionEvent ev;
                ev.kind = SessionEvent::Kind::Log;
                ev.text = "< notice " + std::to_string(n.event) + " " + mp::toText(n.payload);
                postSession(std::move(ev));
            }
            // A library write finished (it answered 1 first): its result, then the IR list again and the wrap closed.
            if (n.event == editor::kNoticeLibraryDone && libraryPending_) {
                const int command = libraryPending_;
                libraryPending_ = 0;
                if (const mp::Value *st = n.payload.get(103); st && st->asInt() != 0) {
                    EditorClient::Reply r;
                    r.status = int(st->asInt());
                    if (const mp::Value *res = n.payload.get(104)) r.result = *res;
                    refused(command, r);
                }
                libraryDone(c, command);
                continue;
            }
            // A preset loaded (from the editor or on the unit): its whole state changed. Snapshot changes come as
            // single changes and need no re-read (one after a footswitch press showed the stored state, not the
            // held one).
            // A slot emptied, filled or moved, an input / output, footswitch or Command Center change, a snapshot
            // pasted (on the unit or by us): read the preset back rather than follow each change.
            const bool slotChanged = n.event == editor::kNoticeBypass && !n.payload.get(59);
            const bool structural = n.event == editor::kNoticeIo || n.event == editor::kNoticeBypassSwitch ||
                                    n.event == editor::kNoticeCommand || n.event == editor::kNoticeSnapshotPasted ||
                                    n.event == editor::kNoticePresetReplaced ||
                                    n.event == editor::kNoticeSwitchRemoved || n.event == editor::kNoticeSwitchChanged ||
                                    n.event == editor::kNoticeRouting;
            if (n.event == editor::kNoticePresetLoaded || slotChanged || structural) {
                reread = true;
                continue;
            }
            if (n.event == editor::kNoticeKnob || n.event == editor::kNoticeBypass) infoWanted = true;
            SessionEvent ev;
            ev.kind = SessionEvent::Kind::Notice;
            ev.notice = std::move(n);
            postSession(std::move(ev));
        }
        if (reread) {
            readCurrent(c);
            infoWanted = false;
        }
        // A change on the unit (knob, on / off): its modified flag, at most twice a second (an expression pedal
        // streams knob events).
        if (infoWanted && steadySeconds() - lastInfo >= 0.5) {
            infoWanted = false;
            lastInfo = steadySeconds();
            SessionEvent ev;
            ev.kind = SessionEvent::Kind::Info;
            ev.current = editor::currentInfo(c.call(EditorChannel::Edit, editor::kCurrentInfo, mp::nil()).result);
            postSession(std::move(ev));
        }
        if (irsWanted_.exchange(false)) readIrs(c);
        // Slots not known yet: a few per pass, so edits and preset loads never wait for all of them.
        if (!irProbe_.empty() && edits.empty() && !reread) probeIrs(c, 4);
        std::vector<int> settings;
        {
            std::lock_guard lock(sessionMutex_);
            settings.swap(settingsWanted_);
        }
        if (!settings.empty()) {
            SessionEvent ev;
            ev.kind = SessionEvent::Kind::Settings;
            for (int id : settings) {
                const mp::Value r =
                    c.call(EditorChannel::Edit, editor::kGlobal, mp::map({{118, mp::integer(id)}})).result;
                if (const mp::Value *v = r.get(119); v && !v->isNil()) {
                    ev.numbers.push_back(id);
                    ev.numbers.push_back(v->asNumber());
                }
            }
            postSession(std::move(ev));
        }
    }
}

bool DeviceLink::readInfo() {
    return start([this] {
        DeviceEvent result;
        result.kind = DeviceEvent::Kind::Failed;
        std::string error;
        if (std::unique_ptr<LinkIo> io = io_(&error)) {
            try {
                log("USB: " + io->description());
                Link link(*io);
                link.log = [this](const std::string &s) { log(s); };
                link.trace = link.log;  // few messages: show them all
                Flasher f(link);
                f.log = link.log;
                try {
                    Flasher::Opened o = f.open(/*withInfo=*/true, /*handover=*/false);
                    link.close();
                    result.kind = DeviceEvent::Kind::Info;
                    result.info = *o.device;
                } catch (const std::exception &e) {
                    link.closeQuietly();
                    result.text = e.what();
                }
            } catch (const std::exception &e) {
                result.text = e.what();
            }
        } else {
            result.text = error;
        }
        post(std::move(result));
    });
}

bool DeviceLink::flash(std::vector<RecordFile> files, FlashOptions options, bool simulate) {
    return start([this, files = std::move(files), options = std::move(options), simulate] {
        // Either the fake unit on simulated time or the real USB link.
        FakeClock clock;
        std::unique_ptr<LinkIo> io;
        std::string error;
        if (simulate) io = std::make_unique<FakeUnit>(clock);
        else io = io_(&error);
        if (!io) {
            DeviceEvent e;
            e.kind = DeviceEvent::Kind::Failed;
            e.text = error;
            post(std::move(e));
            return;
        }
        if (!simulate) log("USB: " + io->description());
        Link link(*io, simulate ? clock.clock() : Clock(steadySeconds));
        link.log = [this](const std::string &s) { log(s); };
        if (!simulate) link.trace = link.log;  // the first messages, then quiet
        Flasher f(link, simulate ? nullptr : &state_);
        f.log = link.log;
        f.stopRequested = [this] { return stop_.load(); };
        f.onRecord = [this](std::uint32_t ri, RecordStep st) {
            DeviceEvent e;
            e.kind = DeviceEvent::Kind::Record;
            e.record = ri;
            e.step = st;
            post(std::move(e));
        };
        f.onRecordBytes = [this](std::uint32_t ri, std::size_t done, std::size_t total) {
            DeviceEvent e;
            e.kind = DeviceEvent::Kind::RecordBytes;
            e.record = ri;
            e.done = done;
            e.total = total;
            post(std::move(e));
        };
        f.onSlotCheck = [this](std::size_t slot, std::size_t count) {
            DeviceEvent e;
            e.kind = DeviceEvent::Kind::SlotCheck;
            e.done = slot;
            e.total = count;
            post(std::move(e));
        };
        DeviceEvent result;
        try {
            f.flash(files, options);
            result.kind = DeviceEvent::Kind::Done;
            result.text = options.keepPresets ? "Done. Presets kept: power-cycle the unit."
                                              : "Done. The unit reboots and rebuilds its presets.";
        } catch (const std::exception &e) {
            link.closeQuietly();
            result.kind = DeviceEvent::Kind::Failed;
            result.text = e.what();
            result.wrote = f.writing();
        }
        post(std::move(result));
    });
}

} // namespace hx

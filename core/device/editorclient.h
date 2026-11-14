#pragma once

// The editor protocol HX Edit speaks with a Helix in normal mode (NOTES.md "Editor protocol"):
// three link channels over one USB connection, each a stream of frames carrying msgpack requests, replies and the
// unit's notifications. UI-free; DeviceLink runs it on its own thread.

#include "core/device/link.h"
#include "core/msgpack.h"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace hx {

// The channels: our address, the unit's, and the service ("port") the first frame selects.
enum class EditorChannel { Library = 0, Edit = 1, Notify = 2 };
struct ChannelAddress {
    std::uint16_t pc, unit, port;
};
constexpr std::array<ChannelAddress, 3> kEditorChannels = {{{0x1001, 0x03ef, 2}, {0x1080, 0x03ed, 6}, {0x1002, 0x03f0, 4}}};

// Frames on a channel's byte stream: u16 kind (1 from the PC, 0 from the unit), u16 port, u32 length, the body.
// No padding between frames (the zeros after one in a capture are the link message's own padding).
std::vector<std::uint8_t> encodeFrame(std::uint16_t port, Bytes body, std::uint16_t kind = 1);
class FrameReader {
public:
    void feed(Bytes data);
    // The next complete frame's body, if one is buffered.
    std::optional<std::vector<std::uint8_t>> next();

private:
    std::vector<std::uint8_t> buf_;
};

// Splits one USB connection into per-channel LinkIos: a message goes to the channel whose PC address it names.
class ChannelMux {
public:
    explicit ChannelMux(LinkIo &io) : io_(io) {}
    // Valid while the mux lives.
    std::unique_ptr<LinkIo> channel(std::uint16_t pc, std::uint16_t unit);
    // Waits up to timeoutS for a message on any channel (sorting what arrives); true when one is queued.
    bool wait(double timeoutS);
    bool pending(std::uint16_t pc) const;

private:
    friend class ChannelIo;
    std::optional<std::vector<std::uint8_t>> read(std::uint16_t pc, double timeoutS);
    void route(std::vector<std::uint8_t> m);
    LinkIo &io_;
    std::map<std::uint16_t, std::deque<std::vector<std::uint8_t>>> queues_;
};

// One unasked message from the unit (Notify channel): event number and its payload.
struct EditorNotice {
    int event = 0;
    mp::Value payload;
};

class EditorClient {
public:
    explicit EditorClient(LinkIo &io, Clock clock = steadySeconds);
    ~EditorClient();

    std::function<void(const std::string &)> log;

    // Connects the three channels and selects their services. Throws LinkError.
    void open();
    struct Reply {
        int status = 0;    // 103: 0 ok (loading a preset answers 1)
        mp::Value result;  // 104
    };
    // Sends {102: id, 100: command, 101: args} and waits for the reply with that id. Notifications that arrive
    // meanwhile are kept for takeNotices(). Throws LinkError on timeout or a dead link.
    Reply call(EditorChannel ch, int command, const mp::Value &args, double timeoutS = 5.0);
    // Keeps the channels alive and collects what arrived; waits up to timeoutS in total.
    void poll(double timeoutS);
    std::vector<EditorNotice> takeNotices();
    void close();

private:
    void pump(int ch);  // link payloads -> frames -> replies / notices

    ChannelMux mux_;
    Clock clock_;
    std::array<std::unique_ptr<LinkIo>, 3> io_;
    std::array<std::unique_ptr<Link>, 3> link_;
    std::array<FrameReader, 3> frames_;
    std::array<int, 3> nextId_ = {1000, 1000, 1000};
    std::array<std::deque<mp::Value>, 3> replies_;
    std::deque<EditorNotice> notices_;
    bool open_ = false;
};

// Request builders and reply readers (NOTES.md "Editor protocol" tables).
namespace editor {
// Library channel. Writes (HX Edit wraps each in kLibraryBegin / kLibraryEnd) answer status 1, then a notice
// kNoticeLibraryDone {102: request id, 103: status} when done; some answer 0 at once.
constexpr int kSetlistNames = 0, kPresetNames = 1, kIrList = 13, kIrUpload = 9, kIrRename = 10, kIrClear = 15, kIrRead = 11,
              kLibraryBegin = 255, kLibraryEnd = 254, kReorderSetlist = 2, kReadPreset = 4, kRenamePreset = 6,
              kWritePreset = 8;
constexpr int kNoticeLibraryDone = 1;
// Edit channel.
constexpr int kCurrentPreset = 22, kCurrentInfo = 23, kGlobal = 24, kLoadPreset = 20, kSelectSnapshot = 88,
              kBypass = 41, kSetKnob = 30, kSetModel = 39, kSelectBlock = 78, kClearSlot = 28, kMoveBlock = 43,
              kSetIo = 42, kSetSetting = 25, kSetSplitModel = 40, kRenameSnapshot = 89, kSnapshotColour = 98,
              kCopySnapshot = 90, kPasteSnapshot = 91, kAssignController = 37, kControllerMin = 65,
              kControllerMax = 66, kAssignBypassSwitch = 56, kSwitchLabel = 59, kCommandType = 67,
              kCommandField = 68, kReplacePreset = 21, kSavePreset = 71, kGlobalEq = 76,
              // hx-edit-ops-2 (2026-11-08): MIDI CC number of a CC controller, remove a bypass switch, LED colour,
              // momentary / latching. Move the split / merge (selected first): slotArgs(path * 20 + position + 1),
              // or path * 20 + kSlotInputB / kSlotOutputB to make it lane B's own input / output.
              kControllerCc = 64, kRemoveBypassSwitch = 57, kSwitchColour = 61, kSwitchMomentary = 58,
              kMoveSplit = 46, kMoveMerge = 47;
// Ours only (MODTAG.md "Reading the tag over USB"): the version format string with the build tag; stock refuses it.
constexpr int kBuildTag = 200;
// Global EQ (HX Edit's EQLowFreq ... EQBypass table): settings 190 + index for the 13 values cmd 76 returns
// (low / mid / high freq, Q, gain; low cut, high cut, level, apply), 203 = bypass. Read with kGlobal, set with
// kSetSetting; the unit reports changes as kNoticeGlobal.
constexpr int kGlobalEqFirst = 190, kGlobalEqCount = 13, kGlobalEqBypass = 203;
// Controller assign (kAssignController): 74 = the source, numbered as in the preset (hx::controller: 0 none = clear,
// 1 EXP 1, 2 EXP 2, 6 + n footswitch n, 18 MIDI CC, 19 Snapshot); 71 = the MIDI CC number, which HX Edit sends as 4
// (0 when clearing) and sets with kControllerCc.
constexpr int kDefaultControllerCc = 4;
// Lane B's own input / output (kMoveSplit / kMoveMerge to these, then kSetIo on these slots); 9 = output A.
constexpr int kSlotInputB = 10, kSlotOutputB = 19;
// Adding a block = kSetModel on an empty slot (HX Edit selects it with kSelectBlock first).
// Notify events. kNoticeBypass without key 59 (on / off) means the slot's contents changed (block added or
// removed). A snapshot change sends 42 (the new number) and then a bypass / knob event for each change; 46 also
// comes after a footswitch press (snapshot modified?), so it is no reason to read the preset again.
constexpr int kNoticeIo = 27, kNoticeBypassSwitch = 31, kNoticeCommand = 35, kNoticeSnapshotPasted = 44,
              kNoticeSnapshotColour = 48, kNoticePresetReplaced = 21, kNoticePresetSaved = 5, kNoticePresetNamed = 6,
              kNoticeKnob = 30, kNoticeBypass = 49, kNoticePresetLoaded = 4, kNoticePresetChanged = 8,
              kNoticeSnapshotSelected = 42, kNoticeSnapshot46 = 46, kNoticeSelect = 39, kNoticeGlobal = 22,
              kNoticeFootswitch = 41,
              // A switch lost a function {98: slot, 70: switch}; a switch's settings changed {70: switch} (LED,
              // momentary, a controller put on it).
              kNoticeSwitchRemoved = 33, kNoticeSwitchChanged = 37,
              // A path's routing changed {56: path, 121: mode, 53: split position + 1, 54: merge position + 1}.
              kNoticeRouting = 26;

mp::Value presetNamesArgs(int setlist);
mp::Value loadPresetArgs(int setlist, int preset);
// Stores the preset being edited into (setlist, preset) under `name` (WRITES the unit's storage; notices 5 and 6).
mp::Value savePresetArgs(int setlist, int preset, const std::string &name);
mp::Value snapshotArgs(int snapshot);
mp::Value bypassArgs(int slot, bool on);
mp::Value slotArgs(int slot);  // {98: slot}: kClearSlot
mp::Value moveArgs(int from, int to);
mp::Value ioArgs(int slot, bool output, int port);  // slot 0 input A (key 51) / 9 output A (key 52)
mp::Value settingArgs(int id, const mp::Value &value);  // preset / global setting by id (16 = tempo)
mp::Value splitModelArgs(int slot, int model);
mp::Value snapshotNameArgs(int snapshot, const std::string &name);
mp::Value snapshotColourArgs(int snapshot, int colour);
mp::Value controllerArgs(int slot, int paramSet, int knob, int source);  // source 0 = clear
mp::Value controllerCcArgs(int slot, int paramSet, int knob, int cc);
mp::Value bypassSwitchArgs(int slot, int fs);  // kAssignBypassSwitch and kRemoveBypassSwitch
mp::Value switchColourArgs(int fs, int colour);  // colour: index into the LED colour names (0 Auto .. 11 Off)
mp::Value switchMomentaryArgs(int fs, bool momentary);
mp::Value switchLabelArgs(int fs, const std::string &label);
mp::Value commandTypeArgs(int commandSlot, int type);
mp::Value commandFieldArgs(int commandSlot, int type, int field, int value);
// {110: l6-helix blob}: the blob as a str, bytes as they are (no NUL added), as HX Edit sends it.
mp::Value replacePresetArgs(const std::vector<std::uint8_t> &blob);
mp::Value selectArgs(int slot, int paramSet);  // the block selected on the unit's screen
// knob = the value's index (preset 11.4); paramSet 0 = the block's own knobs.
mp::Value knobArgs(int slot, int paramSet, int knob, const mp::Value &value);
// slotType 6 block / 7 looper; cab -1 = none.
mp::Value modelArgs(int slot, int slotType, int model, int cab, int blockType, bool on);

std::vector<std::string> setlistNames(const mp::Value &result);
// [(slot number, name)] of one setlist's presets (slot = setlist * 128 + n).
std::vector<std::pair<int, std::string>> presetNames(const mp::Value &result);
struct CurrentInfo {
    int setlist = -1, preset = -1, snapshot = 0;
    std::string name;
    bool modified = false;  // 117: edited since loaded / saved (false right after a load, true after any change)
};
CurrentInfo currentInfo(const mp::Value &result);
// Library cmd 13 ({101: 2}): the filled IR slots only, [{112: slot, 109: name, 104: 32-hex hash, 123, 124, 125}].
struct IrSlot {
    int slot = -1;
    std::string name, hash;  // hash = what presets store in key 12 to name the IR they use
    // What the slot holds, from its first sample words (library cmd 11, DeviceLink): not read yet, an ordinary IR, or
    // one of our NAM captures (core/nam.h namHeader) with its loudness / input level.
    enum class Kind { Unknown, Ir, Nam } kind = Kind::Unknown;
    double namLoudness = 0, namLevelDbu = 0;
};
constexpr int kIrSlots = 128;
std::vector<IrSlot> irList(const mp::Value &result);
mp::Value irListArgs();
// Library cmd 11 {112: slot}: the slot's samples read back (HX Edit's IR export); the result is the 8192 bytes
// themselves (2048 LE float32), not a map (NOTES "lib 11").
mp::Value irReadArgs(int slot);
// Library cmd 9 as HX Edit sends it (key order too): data = 2048 LE float32 (core/ir.h), sum = irSum(data).
mp::Value irUploadArgs(int slot, const std::string &name, const std::vector<std::uint8_t> &data, std::uint32_t sum);
mp::Value irRenameArgs(int slot, const std::string &name);
mp::Value irClearArgs(int slot);
// Stored presets (library channel; slot = index within the setlist). Reading answers the l6-helix blob.
mp::Value readPresetArgs(int setlist, int preset);
mp::Value renamePresetArgs(int setlist, int preset, const std::string &name);
// A whole preset into a slot (blob as read with kReadPreset / kCurrentPreset, sent as a str without NUL).
mp::Value writePresetArgs(int setlist, int preset, const std::string &name, const std::vector<std::uint8_t> &blob);
// A setlist in a new order: for each position, the preset that goes there by its current library number
// (setlist * 128 + n) and name; sent as one msgpack array inside a str, as HX Edit does.
mp::Value reorderSetlistArgs(int setlist, const std::vector<std::pair<int, std::string>> &order);
// The notification's knob / bypass change: slot, param set, knob, value; nullopt for other events.
struct KnobChange {
    int slot = -1, paramSet = 0, knob = -1;
    mp::Value value;
};
std::optional<KnobChange> knobChange(const EditorNotice &n);
std::optional<std::pair<int, bool>> bypassChange(const EditorNotice &n);
} // namespace editor

} // namespace hx

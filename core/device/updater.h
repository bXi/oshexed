#pragma once

// The unit's update server, spoken over a Link (port of tools/hxflash.py: commands, Flasher, UnitState,
// presets_decision; NOTES.md "USB update protocol").
//
// Commands are little-endian u32s in the link's data stream, the first one `aa 00 00 op`:
//   session open: 000003000100000003 / 01000000020000000000 (raw, before the commands)
//   01 hello, 03 slot table, 02 start
//   04 n            read-only slot check (slow), not needed to flash
//   per record: 05 r 0 words hash8 0 0 / 06 (wipes the slot header) / 07 i hash8 nwords data... / 09 (verify)
//   0a 1 0          finish: the unit deletes its compiled presets and reboots
// A record's "file" = its slot sector (record header +0x24..+0x224) + payload, both in native (LE) order.
// hash8 = Fletcher pair over LE words, sent as two LE u32. Chunks are 0x1000 words.

#include "core/bytes.h"
#include "core/device/link.h"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hx {

struct HxfImage;

constexpr std::size_t kChunkWords = 0x1000;
constexpr std::size_t kPieceBytes = 512;  // the updater's write size (only for comparing with captures)

struct RecordFile {
    std::uint32_t index = 0;
    std::vector<std::uint8_t> data;
    std::string sha256;  // hex, of data; filled by recordFiles(); recompute after changing data
};

// What the updater sends for each record, in record order. Checks the word count and hash8 of each; throws
// FormatError when one doesn't match (a damaged or badly packed .hxf never reaches the unit).
std::vector<RecordFile> recordFiles(const HxfImage &img);

std::array<std::uint8_t, 8> fletcherLE(Bytes data);

// `aa 00 00 op` + little-endian u32 arguments.
std::vector<std::uint8_t> updaterCommand(std::uint8_t op, std::initializer_list<std::uint32_t> args = {});

// Every write the PC makes into the data stream for a full flash, in order (hxflash.py `commands`). Chunks
// are built one at a time, so the whole stream never sits in memory.
void forEachFlashWrite(const std::vector<RecordFile> &files,
                       const std::function<void(const std::string &name, Bytes data)> &fn);

// Service 5 of the editor server, read before the update handover.
struct DeviceInfo {
    std::string product;
    std::uint32_t runtime = 0, version = 0, build = 0;
    std::string raw;  // hex
    std::string versionText() const;  // "3.80"
};

// What this program last wrote to each slot (SHA-256 of the record file), so a fast flash knows what the unit
// holds. The unit can't tell: its op 4 only says valid / invalid. A slot is cleared before its 06 and set after
// its 09, so an interrupted flash leaves it unknown. Flashes by other tools are invisible; after one, flash
// everything or call assume().
class UnitState {
public:
    explicit UnitState(std::string path = {});  // empty path: in memory only
    bool known(std::uint32_t record) const { return records_.count(record) != 0; }
    bool same(const RecordFile &f) const;
    std::string fileName(std::uint32_t record) const;  // "" when unknown
    void clear(std::uint32_t record);
    void set(const RecordFile &f, const std::string &name);
    void assume(const std::vector<RecordFile> &files, const std::string &name);
    bool empty() const { return records_.empty(); }

private:
    void save() const;
    struct Entry {
        std::string sha256, name;
    };
    std::string path_;
    std::map<std::uint32_t, Entry> records_;
};

// Whether to skip the finish step and keep the unit's compiled presets. The finish deletes them all; they stay
// valid only while the knob definitions (rec 4) and the DSP modules (rec 5) on the unit are unchanged.
struct PresetsChoice {
    bool keep = false;
    std::string reason;
};
enum class PresetsMode { Auto, Keep, Rebuild };
PresetsChoice presetsDecision(PresetsMode mode, const std::vector<RecordFile> &files, const UnitState &state);

struct FlashOptions {
    std::set<std::uint32_t> skip;   // records to leave alone (already on the unit)
    bool checkSlots = false;        // the updater's slow read-only op 4 pass first (~3 min)
    bool keepPresets = false;       // no finish: the unit shows "Update failed", a power cycle boots the new one
    int window = 2;                 // chunks in flight
    std::string sourceName;         // stored in UnitState
};

enum class RecordStep { Prepare, Writing, Verify, Done };

class Flasher {
public:
    explicit Flasher(Link &link, UnitState *state = nullptr) : link_(link), state_(state) {}

    std::function<void(const std::string &)> log;
    std::function<void(std::uint32_t record, RecordStep)> onRecord;
    std::function<void(std::uint32_t record, std::size_t done, std::size_t total)> onRecordBytes;
    std::function<void(std::size_t slot, std::size_t count)> onSlotCheck;
    // Asked between steps; honoured only before the first slot is touched. After that a stop would leave a
    // wiped slot, so the flash runs to the end.
    std::function<bool()> stopRequested;

    // Service 5, then back to service selection. Right after connect, before the update handover.
    DeviceInfo info();
    struct Opened {
        std::vector<std::uint8_t> hello, slots;
        std::optional<DeviceInfo> device;
    };
    // withInfo: read service 5 first. handover=false: stop there, the unit keeps running normally. After a
    // handover the update server only ends cleanly with 0a (reboot); any other end shows "Update failed".
    Opened open(bool withInfo = false, bool handover = true);
    void flash(const std::vector<RecordFile> &files, const FlashOptions &options);

    // Set at the first 06 (slot header wiped): from then on a failure needs the recovery path.
    bool writing() const { return writing_; }

private:
    std::vector<std::uint8_t> request(int key, Bytes data, const std::string &label);
    std::vector<std::uint8_t> reply(int key, const std::string &label);
    std::pair<std::uint16_t, std::vector<std::uint8_t>> editorRequest(std::uint16_t type, Bytes payload,
                                                                       double timeoutS = 10);
    void say(const std::string &s) const {
        if (log) log(s);
    }
    void checkStop() const;

    Link &link_;
    UnitState *state_;
    bool writing_ = false;
};

} // namespace hx

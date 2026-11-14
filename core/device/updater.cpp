#include "core/device/updater.h"

#include "core/checksums.h"
#include "core/hxf.h"
#include "core/text.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace hx {

namespace {

constexpr std::size_t kSectorOff = 0x24;  // slot sector inside the record header

const std::uint8_t kSession0[] = {0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03};
const std::uint8_t kSession1[] = {0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
const std::uint8_t kSessionReply[] = {0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr int kSessionKey = -1;

std::optional<std::uint32_t> word(Bytes r, std::size_t i) {
    if (r.size() < 4 * i + 4) return std::nullopt;
    return readLE32(r, 4 * i);
}

bool allZero(Bytes r, std::size_t n) {
    return r.size() == n && std::all_of(r.begin(), r.end(), [](std::uint8_t c) { return c == 0; });
}

// Reply check and timeout per command. Timeouts: the 3.80 capture's longest wait x ~4.
struct ReplyRule {
    bool (*ok)(Bytes);
    double timeoutS;
};

ReplyRule replyRule(int key) {
    switch (key) {
    case kSessionKey:
        return {[](Bytes r) { return std::equal(r.begin(), r.end(), std::begin(kSessionReply), std::end(kSessionReply)); }, 10};
    case 1: return {[](Bytes r) { return r.size() == 4; }, 10};
    case 3: return {[](Bytes r) { return r.size() >= 4; }, 10};
    case 2: return {[](Bytes r) { return allZero(r, 4); }, 30};
    case 4: return {[](Bytes r) { return r.size() == 28 && word(r, 0) == 1u; }, 600};
    case 5: return {[](Bytes r) { return word(r, 0) == 1u; }, 300};
    case 6: return {[](Bytes r) { return allZero(r, 4); }, 30};
    case 7: return {[](Bytes r) { return allZero(r, 8); }, 30};
    case 9: return {[](Bytes r) { return allZero(r, 4); }, 120};
    case 10: return {[](Bytes r) { return allZero(r, 12); }, 30};
    }
    return {[](Bytes) { return false; }, 10};
}

void append32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(std::uint8_t(v >> s));
}

// 05 r 0 words hash8 0 0
std::vector<std::uint8_t> fileCommand(const RecordFile &f) {
    std::vector<std::uint8_t> c = updaterCommand(5, {f.index, 0, std::uint32_t(f.data.size() / 4)});
    const std::array<std::uint8_t, 8> h = fletcherLE(f.data);
    c.insert(c.end(), h.begin(), h.end());
    append32(c, 0);
    append32(c, 0);
    return c;
}

// 07 i hash8 nwords data...
std::vector<std::uint8_t> chunkCommand(std::size_t index, Bytes d) {
    std::vector<std::uint8_t> c = updaterCommand(7, {std::uint32_t(index)});
    const std::array<std::uint8_t, 8> h = fletcherLE(d);
    c.insert(c.end(), h.begin(), h.end());
    append32(c, std::uint32_t(d.size() / 4));
    c.insert(c.end(), d.begin(), d.end());
    return c;
}

std::string hexU32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%#010x", unsigned(v));
    return buf;
}

std::string mmss(double t) {
    const int s = std::max(0, int(t));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

} // namespace

std::array<std::uint8_t, 8> fletcherLE(Bytes data) {
    Fletcher f;
    f.add(data);
    std::array<std::uint8_t, 8> out{};
    for (int k = 0; k < 4; ++k) {
        out[k] = std::uint8_t(f.sum1 >> (8 * k));
        out[4 + k] = std::uint8_t(f.sum2 >> (8 * k));
    }
    return out;
}

std::vector<std::uint8_t> updaterCommand(std::uint8_t op, std::initializer_list<std::uint32_t> args) {
    std::vector<std::uint8_t> c = {0xaa, 0x00, 0x00, op};
    for (std::uint32_t a : args) append32(c, a);
    return c;
}

std::vector<RecordFile> recordFiles(const HxfImage &img) {
    std::vector<RecordFile> out;
    out.reserve(img.records.size());
    for (const HxfRecord &r : img.records) {
        if (r.header.size() < kSectorOff) throw FormatError("record " + std::to_string(r.index) + ": short header");
        RecordFile f;
        f.index = r.index;
        f.data = swapWords(r.header.subspan(kSectorOff));
        f.data.insert(f.data.end(), r.payload.begin(), r.payload.end());
        if (f.data.size() % 4 != 0 || f.data.size() / 4 != r.words)
            throw FormatError("record " + std::to_string(r.index) + ": size does not match its word count");
        std::array<std::uint8_t, 8> stored = r.hash8;  // big-endian as in the header
        swapWordsInPlace(stored);
        if (fletcherLE(f.data) != stored)
            throw FormatError("record " + std::to_string(r.index) + ": hash8 does not match");
        f.sha256 = toHex(sha256(f.data));
        out.push_back(std::move(f));
    }
    return out;
}

void forEachFlashWrite(const std::vector<RecordFile> &files,
                       const std::function<void(const std::string &, Bytes)> &fn) {
    fn("session", kSession0);
    fn("session", kSession1);
    fn("hello 01", updaterCommand(1));
    fn("slots 03", updaterCommand(3));
    fn("02", updaterCommand(2));
    for (std::size_t n = 0; n < files.size(); ++n) fn("erase " + std::to_string(n), updaterCommand(4, {std::uint32_t(n)}));
    const std::size_t step = 4 * kChunkWords;
    for (const RecordFile &f : files) {
        const std::string ri = std::to_string(f.index);
        fn("file " + ri, fileCommand(f));
        fn("06", updaterCommand(6));
        for (std::size_t i = 0; i < f.data.size(); i += step) {
            const Bytes d = Bytes(f.data).subspan(i, std::min(step, f.data.size() - i));
            fn("chunk " + ri + "." + std::to_string(i / step), chunkCommand(i / step, d));
        }
        fn("09", updaterCommand(9));
    }
    fn("finish 0a", updaterCommand(10, {1, 0}));
}

std::string DeviceInfo::versionText() const {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u.%02x", unsigned(version >> 24), unsigned((version >> 16) & 0xff));
    return buf;
}

// ---------------------------------------------------------------- unit state

UnitState::UnitState(std::string path) : path_(std::move(path)) {
    if (path_.empty()) return;
    std::ifstream in(pathFromUtf8(path_));
    std::string line;
    // "record<TAB>sha256<TAB>file name" per line.
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        try {
            records_[std::uint32_t(std::stoul(line.substr(0, a)))] = {line.substr(a + 1, b - a - 1), line.substr(b + 1)};
        } catch (const std::exception &) {
        }
    }
}

bool UnitState::same(const RecordFile &f) const {
    const auto it = records_.find(f.index);
    return it != records_.end() && it->second.sha256 == f.sha256;
}

std::string UnitState::fileName(std::uint32_t record) const {
    const auto it = records_.find(record);
    return it == records_.end() ? std::string() : it->second.name;
}

void UnitState::clear(std::uint32_t record) {
    records_.erase(record);
    save();
}

void UnitState::set(const RecordFile &f, const std::string &name) {
    records_[f.index] = {f.sha256, name};
    save();
}

void UnitState::assume(const std::vector<RecordFile> &files, const std::string &name) {
    for (const RecordFile &f : files) records_[f.index] = {f.sha256, name};
    save();
}

// Written beside the target and renamed over it: a crash mid-flash never leaves half a file.
void UnitState::save() const {
    if (path_.empty()) return;
    const std::filesystem::path target = pathFromUtf8(path_);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return;
        for (const auto &[ri, e] : records_) out << ri << '\t' << e.sha256 << '\t' << e.name << '\n';
        if (!out.flush()) return;
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
}

PresetsChoice presetsDecision(PresetsMode mode, const std::vector<RecordFile> &files, const UnitState &state) {
    if (mode == PresetsMode::Keep) return {true, "kept on request"};
    if (mode == PresetsMode::Rebuild) return {false, "rebuilt on request"};
    // Compiled presets may hold module data too (a kept build with a changed rec 5 left the unit silent until a
    // rebuild, 2026-11-04), so keep them only when records 4 AND 5 are unchanged.
    const std::pair<std::uint32_t, const char *> needed[] = {{4, "knob definitions"}, {5, "DSP modules"}};
    for (const auto &need : needed) {
        const std::uint32_t ri = need.first;
        if (!state.known(ri)) return {false, "record " + std::to_string(ri) + " on the unit unknown"};
        const auto f = std::find_if(files.begin(), files.end(), [ri](const RecordFile &x) { return x.index == ri; });
        if (f == files.end() || !state.same(*f)) return {false, std::string(need.second) + " changed"};
    }
    return {true, "knobs and DSP modules unchanged"};
}

// ---------------------------------------------------------------- flasher

std::vector<std::uint8_t> Flasher::request(int key, Bytes data, const std::string &label) {
    link_.send(data);
    return reply(key, label);
}

std::vector<std::uint8_t> Flasher::reply(int key, const std::string &label) {
    const ReplyRule rule = replyRule(key);
    std::vector<std::uint8_t> r = link_.recv(rule.timeoutS);
    if (!rule.ok(r)) throw LinkError(label + ": unexpected reply " + toHex(r));
    return r;
}

// Editor-server message: u16 type, u16 channel 3, u32 length + payload. The reply may span link messages.
std::pair<std::uint16_t, std::vector<std::uint8_t>> Flasher::editorRequest(std::uint16_t type, Bytes payload,
                                                                           double timeoutS) {
    std::vector<std::uint8_t> m = {std::uint8_t(type), std::uint8_t(type >> 8), 3, 0};
    append32(m, std::uint32_t(payload.size()));
    m.insert(m.end(), payload.begin(), payload.end());
    link_.send(m);
    std::vector<std::uint8_t> r = link_.recv(timeoutS);
    while (r.size() < 8 || r.size() < 8 + std::size_t(readLE32(r, 4))) {
        const std::vector<std::uint8_t> more = link_.recv(timeoutS);
        r.insert(r.end(), more.begin(), more.end());
    }
    const std::size_t n = readLE32(r, 4);
    return {readLE16(r, 0), std::vector<std::uint8_t>(r.begin() + 8, r.begin() + std::ptrdiff_t(8 + n))};
}

DeviceInfo Flasher::info() {
    const std::uint8_t service5[] = {5};
    auto [type, r] = editorRequest(0, service5);
    if (r.size() != 0x18 || r[0] != 5) throw LinkError("service 5: unexpected reply " + toHex(r));
    DeviceInfo d;
    for (int k = 1; k < 5 && r[std::size_t(k)]; ++k) d.product += char(r[std::size_t(k)]);
    d.runtime = readLE32(r, 12);
    d.version = readLE32(r, 16);
    d.build = readLE32(r, 20);
    d.raw = toHex(r);
    auto [atype, a] = editorRequest(3, {});
    if (atype != 3 || a.size() != 1 || a[0] != 0xff)
        throw LinkError("abort: unexpected reply type " + std::to_string(atype) + " " + toHex(a));
    return d;
}

Flasher::Opened Flasher::open(bool withInfo, bool handover) {
    Opened o;
    link_.connect();
    if (withInfo) {
        o.device = info();
        link_.waitUnitClose();  // the request ends with the unit closing the stream
        const DeviceInfo &d = *o.device;
        char buf[64];
        std::snprintf(buf, sizeof buf, "runtime word %#x", unsigned(d.runtime));
        say("device " + d.product + "  version " + d.versionText() + " (" + hexU32(d.version) + ")  build " +
            hexU32(d.build) + "  " + buf);
        if (!handover) return o;
        link_.connect();
    }
    link_.send(kSession0);
    request(kSessionKey, kSession1, "session");
    o.hello = request(1, updaterCommand(1), "01");
    o.slots = request(3, updaterCommand(3), "03");
    say("01 -> " + toHex(o.hello));
    say("03 -> " + toHex(o.slots));
    return o;
}

void Flasher::checkStop() const {
    if (!writing_ && stopRequested && stopRequested()) throw LinkError("stopped before anything was written");
}

void Flasher::flash(const std::vector<RecordFile> &all, const FlashOptions &opt) {
    std::vector<const RecordFile *> files;
    for (const RecordFile &f : all)
        if (!opt.skip.count(f.index)) files.push_back(&f);
    const double t0 = steadySeconds();
    auto elapsed = [&] {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%6.1f s", steadySeconds() - t0);
        return std::string(buf);
    };
    auto step = [&](std::uint32_t ri, RecordStep s) {
        if (onRecord) onRecord(ri, s);
    };

    checkStop();
    open();
    request(2, updaterCommand(2), "02");
    if (!opt.skip.empty()) {
        std::string list;
        for (std::uint32_t ri : opt.skip) list += (list.empty() ? "" : ", ") + std::to_string(ri);
        say("skipping records [" + list + "] (already on the unit)");
    }
    if (opt.checkSlots) {
        for (std::size_t n = 0; n < all.size(); ++n) {
            checkStop();
            if (onSlotCheck) onSlotCheck(n, all.size());
            const std::vector<std::uint8_t> r =
                request(4, updaterCommand(4, {std::uint32_t(n)}), "slot check " + std::to_string(n));
            char buf[64];
            std::snprintf(buf, sizeof buf, "slot %2zu status %u (type %u)  ", n, unsigned(*word(r, 0)),
                          unsigned(*word(r, 2)));
            say(buf + elapsed());
        }
    }
    const std::size_t chunkBytes = 4 * kChunkWords;
    for (const RecordFile *f : files) {
        checkStop();
        const std::uint32_t ri = f->index;
        const std::string rs = "record " + std::to_string(ri);
        step(ri, RecordStep::Prepare);
        request(5, fileCommand(*f), rs + " 05");
        writing_ = true;  // 06 wipes the slot header: from here on, failure = recovery
        if (state_) state_->clear(ri);
        request(6, updaterCommand(6), rs + " 06");
        step(ri, RecordStep::Writing);
        std::vector<std::size_t> pending;
        for (std::size_t i = 0; i < f->data.size(); i += chunkBytes) {
            const Bytes d = Bytes(f->data).subspan(i, std::min(chunkBytes, f->data.size() - i));
            // Chunks are pipelined: the next goes out before the reply to the previous one.
            while (int(pending.size()) >= std::max(1, opt.window)) {
                reply(7, rs + " chunk " + std::to_string(pending.front()));
                pending.erase(pending.begin());
            }
            link_.send(chunkCommand(i / chunkBytes, d));
            pending.push_back(i / chunkBytes);
            if (onRecordBytes) onRecordBytes(ri, i + d.size(), f->data.size());
        }
        for (std::size_t c : pending) reply(7, rs + " chunk " + std::to_string(c));
        step(ri, RecordStep::Verify);
        request(9, updaterCommand(9), rs + " 09");
        if (state_) state_->set(*f, opt.sourceName);
        step(ri, RecordStep::Done);
        char buf[48];
        std::snprintf(buf, sizeof buf, "record %2u ok (%9zu B)  ", unsigned(ri), f->data.size());
        say(buf + elapsed());
    }
    if (opt.keepPresets) {
        // No finish: its callback deletes every compiled preset and reboots, so the next boot rebuilds them all.
        // Closing instead leaves the unit on "Update failed"; a power cycle boots the new firmware with the
        // compiled presets kept.
        link_.close();
        say("done in " + mmss(steadySeconds() - t0) + "; finish skipped (presets kept): power-cycle the unit");
        return;
    }
    request(10, updaterCommand(10, {1, 0}), "finish 0a");
    say("done in " + mmss(steadySeconds() - t0) + "; the unit should reboot");
}

} // namespace hx

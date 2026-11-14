#include "core/device/editorclient.h"

#include "core/bytes.h"

#include <algorithm>

namespace hx {

std::vector<std::uint8_t> encodeFrame(std::uint16_t port, Bytes body, std::uint16_t kind) {
    std::vector<std::uint8_t> f;
    f.reserve(8 + body.size() + 3);
    for (std::uint16_t v : {kind, port}) {
        f.push_back(std::uint8_t(v));
        f.push_back(std::uint8_t(v >> 8));
    }
    for (int s = 0; s < 32; s += 8) f.push_back(std::uint8_t(body.size() >> s));
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

void FrameReader::feed(Bytes data) { buf_.insert(buf_.end(), data.begin(), data.end()); }

std::optional<std::vector<std::uint8_t>> FrameReader::next() {
    if (buf_.size() < 8) return std::nullopt;
    const std::size_t n = readLE32(buf_, 4);
    if (buf_.size() < 8 + n) return std::nullopt;
    std::vector<std::uint8_t> body(buf_.begin() + 8, buf_.begin() + std::ptrdiff_t(8 + n));
    buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(8 + n));
    return body;
}

// One channel of a ChannelMux.
class ChannelIo : public LinkIo {
public:
    ChannelIo(ChannelMux &mux, LinkIo &base, std::uint16_t pc, std::uint16_t unit)
        : mux_(mux), base_(base), pc_(pc), unit_(unit) {}
    void write(Bytes message) override { base_.write(message); }
    std::optional<std::vector<std::uint8_t>> read(double timeoutS) override { return mux_.read(pc_, timeoutS); }
    std::string description() const override { return base_.description(); }
    std::uint16_t pcAddress() const override { return pc_; }
    std::uint16_t unitAddress() const override { return unit_; }

private:
    ChannelMux &mux_;
    LinkIo &base_;
    std::uint16_t pc_, unit_;
};

std::unique_ptr<LinkIo> ChannelMux::channel(std::uint16_t pc, std::uint16_t unit) {
    queues_[pc];
    return std::make_unique<ChannelIo>(*this, io_, pc, unit);
}

std::optional<std::vector<std::uint8_t>> ChannelMux::read(std::uint16_t pc, double timeoutS) {
    const double end = steadySeconds() + std::max(0.0, timeoutS);
    for (;;) {
        auto &q = queues_[pc];
        if (!q.empty()) {
            std::vector<std::uint8_t> m = std::move(q.front());
            q.pop_front();
            return m;
        }
        // Past the deadline this still takes what is already waiting (read(0) returns at once when nothing is).
        std::optional<std::vector<std::uint8_t>> m = io_.read(std::max(0.0, end - steadySeconds()));
        if (!m) return std::nullopt;
        route(std::move(*m));
    }
}

// Messages for an address nobody opened are dropped.
void ChannelMux::route(std::vector<std::uint8_t> m) {
    if (const std::optional<LinkMessage> h = decodeMessage(m))
        if (auto it = queues_.find(h->dst); it != queues_.end()) it->second.push_back(std::move(m));
}

bool ChannelMux::wait(double timeoutS) {
    const double end = steadySeconds() + std::max(0.0, timeoutS);
    for (;;) {
        for (const auto &[pc, q] : queues_)
            if (!q.empty()) return true;
        std::optional<std::vector<std::uint8_t>> m = io_.read(std::max(0.0, end - steadySeconds()));
        if (!m) return false;
        route(std::move(*m));
    }
}

bool ChannelMux::pending(std::uint16_t pc) const {
    const auto it = queues_.find(pc);
    return it != queues_.end() && !it->second.empty();
}

EditorClient::EditorClient(LinkIo &io, Clock clock) : mux_(io), clock_(std::move(clock)) {}

EditorClient::~EditorClient() {
    try {
        close();
    } catch (const std::exception &) {
    }
}

// The order HX Edit uses: editing, notifications, library. Each channel's first frame names its service and the
// unit echoes it.
void EditorClient::open() {
    for (int ch : {1, 2, 0}) {
        const ChannelAddress &a = kEditorChannels[std::size_t(ch)];
        io_[std::size_t(ch)] = mux_.channel(a.pc, a.unit);
        link_[std::size_t(ch)] = std::make_unique<Link>(*io_[std::size_t(ch)], clock_);
        Link &l = *link_[std::size_t(ch)];
        l.log = log;
        l.connect();
        const std::uint8_t port[1] = {std::uint8_t(a.port)};
        l.send(encodeFrame(a.port, port));
        const double t = clock_();
        std::optional<std::vector<std::uint8_t>> echo;
        while (!(echo = frames_[std::size_t(ch)].next())) {
            if (clock_() - t > 3.0) throw LinkError("the unit did not answer the service request on channel " +
                                                     std::to_string(a.unit));
            l.poll(0.05);
            while (std::optional<std::vector<std::uint8_t>> p = l.take()) frames_[std::size_t(ch)].feed(*p);
        }
    }
    open_ = true;
}

void EditorClient::pump(int ch) {
    Link &l = *link_[std::size_t(ch)];
    while (std::optional<std::vector<std::uint8_t>> p = l.take()) frames_[std::size_t(ch)].feed(*p);
    while (std::optional<std::vector<std::uint8_t>> body = frames_[std::size_t(ch)].next()) {
        mp::Value v;
        try {
            v = mp::parse(*body);
        } catch (const FormatError &) {
            continue;  // not msgpack: ignore
        }
        if (ch == int(EditorChannel::Notify) && v.get(105)) {
            EditorNotice n;
            n.event = int(v.get(105)->asInt());
            if (const mp::Value *outer = v.get(106)) n.payload = outer->get(106) ? *outer->get(106) : *outer;
            notices_.push_back(std::move(n));
        } else {
            replies_[std::size_t(ch)].push_back(std::move(v));
        }
    }
}

void EditorClient::poll(double timeoutS) {
    if (!open_) return;
    // Wait once for anything, then take everything queued on every channel: the unit sends bursts (a snapshot
    // change is one event per block), and one message per channel per pass made them trickle in.
    mux_.wait(timeoutS);
    for (int ch = 0; ch < 3; ++ch) {
        Link &l = *link_[std::size_t(ch)];
        const std::uint16_t pc = kEditorChannels[std::size_t(ch)].pc;
        do l.poll(0);
        while (mux_.pending(pc));
        pump(ch);
    }
}

EditorClient::Reply EditorClient::call(EditorChannel channel, int command, const mp::Value &args, double timeoutS) {
    if (!open_) throw LinkError("not connected");
    const int ch = int(channel);
    const int id = nextId_[std::size_t(ch)]++;
    const mp::Value req = mp::map({{102, mp::integer(id)}, {100, mp::integer(command)}, {101, args}});
    const std::vector<std::uint8_t> body = mp::encode(req);
    link_[std::size_t(ch)]->send(encodeFrame(kEditorChannels[std::size_t(ch)].port, body));
    const double t = clock_();
    for (;;) {
        auto &q = replies_[std::size_t(ch)];
        for (auto it = q.begin(); it != q.end(); ++it)
            if (const mp::Value *rid = it->get(102); rid && rid->asInt() == id) {
                Reply r;
                if (const mp::Value *s = it->get(103)) r.status = int(s->asInt());
                if (const mp::Value *res = it->get(104)) r.result = *res;
                q.erase(it);
                return r;
            }
        // Replies nobody waits for any more: keep only a few.
        while (q.size() > 16) q.pop_front();
        if (clock_() - t > timeoutS)
            throw LinkError("no reply to command " + std::to_string(command) + " within " +
                            std::to_string(int(timeoutS)) + " s");
        poll(0.03);
    }
}

std::vector<EditorNotice> EditorClient::takeNotices() {
    std::vector<EditorNotice> out(std::make_move_iterator(notices_.begin()), std::make_move_iterator(notices_.end()));
    notices_.clear();
    return out;
}

void EditorClient::close() {
    if (!open_) return;
    open_ = false;
    for (auto &l : link_)
        if (l) l->closeQuietly();
}

namespace editor {

mp::Value presetNamesArgs(int setlist) { return mp::map({{107, mp::integer(setlist)}, {101, mp::integer(2)}}); }

mp::Value loadPresetArgs(int setlist, int preset) {
    return mp::map({{107, mp::integer(setlist)}, {108, mp::integer(preset)}});
}

mp::Value savePresetArgs(int setlist, int preset, const std::string &name) {
    return mp::map({{107, mp::integer(setlist)}, {108, mp::integer(preset)}, {109, mp::str(name)}});
}

mp::Value snapshotArgs(int snapshot) { return mp::map({{92, mp::integer(snapshot)}}); }

mp::Value bypassArgs(int slot, bool on) { return mp::map({{98, mp::integer(slot)}, {59, mp::boolean(on)}}); }

mp::Value slotArgs(int slot) { return mp::map({{98, mp::integer(slot)}}); }

mp::Value replacePresetArgs(const std::vector<std::uint8_t> &blob) {
    mp::Value body;
    body.type = mp::Type::Str;
    body.s.assign(blob.begin(), blob.end());
    return mp::map({{110, std::move(body)}});
}

mp::Value moveArgs(int from, int to) { return mp::map({{75, mp::integer(from)}, {76, mp::integer(to)}}); }

mp::Value ioArgs(int slot, bool output, int port) {
    return mp::map({{98, mp::integer(slot)}, {output ? 52 : 51, mp::integer(port)}});
}

mp::Value settingArgs(int id, const mp::Value &value) { return mp::map({{118, mp::integer(id)}, {119, value}}); }

mp::Value splitModelArgs(int slot, int model) {
    return mp::map({{98, mp::integer(slot)},
                    {100, mp::map({{23, mp::boolean(false)}, {25, mp::integer(model)}, {26, mp::integer(-1)}})}});
}

mp::Value snapshotNameArgs(int snapshot, const std::string &name) {
    return mp::map({{92, mp::integer(snapshot)}, {109, mp::str(name)}});
}

mp::Value snapshotColourArgs(int snapshot, int colour) {
    return mp::map({{92, mp::integer(snapshot)}, {66, mp::integer(colour)}});
}

mp::Value controllerArgs(int slot, int paramSet, int knob, int source) {
    return mp::map({{98, mp::integer(slot)},
                    {29, mp::boolean(true)},
                    {26, mp::integer(paramSet)},
                    {28, mp::integer(knob)},
                    {74, mp::integer(source)},
                    {71, mp::integer(source ? kDefaultControllerCc : 0)},
                    {129, mp::boolean(false)}});
}

mp::Value controllerCcArgs(int slot, int paramSet, int knob, int cc) {
    return mp::map({{98, mp::integer(slot)},
                    {29, mp::boolean(true)},
                    {26, mp::integer(paramSet)},
                    {28, mp::integer(knob)},
                    {71, mp::integer(cc)}});
}

mp::Value switchColourArgs(int fs, int colour) { return mp::map({{102, mp::integer(fs)}, {66, mp::integer(colour)}}); }

mp::Value switchMomentaryArgs(int fs, bool momentary) {
    return mp::map({{102, mp::integer(fs)}, {65, mp::boolean(momentary)}});
}

mp::Value bypassSwitchArgs(int slot, int fs) { return mp::map({{98, mp::integer(slot)}, {102, mp::integer(fs)}}); }

mp::Value switchLabelArgs(int fs, const std::string &label) {
    return mp::map({{102, mp::integer(fs)}, {109, mp::str(label)}});
}

mp::Value commandTypeArgs(int commandSlot, int type) {
    return mp::map({{24, mp::integer(commandSlot)}, {77, mp::integer(type)}});
}

mp::Value commandFieldArgs(int commandSlot, int type, int field, int value) {
    return mp::map({{24, mp::integer(commandSlot)}, {77, mp::integer(type)}, {28, mp::integer(field)},
                    {81, mp::integer(value)}});
}

mp::Value selectArgs(int slot, int paramSet) {
    return mp::map({{98, mp::integer(slot)}, {26, mp::integer(paramSet)}});
}

mp::Value knobArgs(int slot, int paramSet, int knob, const mp::Value &value) {
    return mp::map({{98, mp::integer(slot)},
                    {29, mp::boolean(true)},
                    {26, mp::integer(paramSet)},
                    {28, mp::integer(knob)},
                    {119, value}});
}

mp::Value modelArgs(int slot, int slotType, int model, int cab, int blockType, bool on) {
    mp::Value modelRef = mp::map({{23, mp::boolean(cab >= 0)}, {25, mp::integer(model)}, {26, mp::integer(cab)}});
    mp::Value content = mp::map({{24, std::move(modelRef)}, {9, mp::integer(blockType)}, {10, mp::boolean(on)}});
    return mp::map({{98, mp::integer(slot)},
                    {99, mp::map({{19, mp::integer(slotType)}, {20, std::move(content)}})}});
}

std::vector<std::string> setlistNames(const mp::Value &result) {
    // [{index: name}, ...]
    std::vector<std::string> out;
    if (!result.isArray()) return out;
    for (const mp::Value &e : result.items)
        if (e.isMap() && e.items.size() >= 2) {
            const std::size_t i = std::size_t(std::max<std::int64_t>(0, e.items[0].asInt()));
            if (out.size() <= i) out.resize(i + 1);
            out[i] = e.items[1].asText();
        }
    return out;
}

std::vector<std::pair<int, std::string>> presetNames(const mp::Value &result) {
    // [{slot: {109: name, ...}}, ...]
    std::vector<std::pair<int, std::string>> out;
    if (!result.isArray()) return out;
    for (const mp::Value &e : result.items)
        if (e.isMap() && e.items.size() >= 2) {
            const mp::Value *name = e.items[1].get(109);
            out.emplace_back(int(e.items[0].asInt()), name ? name->asText() : std::string());
        }
    return out;
}

CurrentInfo currentInfo(const mp::Value &result) {
    CurrentInfo c;
    if (const mp::Value *v = result.get(107)) c.setlist = int(v->asInt(-1));
    if (const mp::Value *v = result.get(108)) c.preset = int(v->asInt(-1));
    if (const mp::Value *v = result.get(92)) c.snapshot = int(v->asInt());
    if (const mp::Value *v = result.get(109)) c.name = v->asText();
    if (const mp::Value *v = result.get(117)) c.modified = v->asBool();
    return c;
}

mp::Value irListArgs() { return mp::map({{101, mp::integer(2)}}); }

mp::Value irReadArgs(int slot) { return mp::map({{112, mp::integer(slot)}}); }

mp::Value irUploadArgs(int slot, const std::string &name, const std::vector<std::uint8_t> &data, std::uint32_t sum) {
    mp::Value samples;  // a str of the raw bytes, no NUL (as the preset blob in cmd 21)
    samples.type = mp::Type::Str;
    samples.s.assign(data.begin(), data.end());
    return mp::map({{112, mp::integer(slot)},
                    {113, mp::integer(std::int64_t(sum))},
                    {109, mp::str(name)},
                    {114, mp::integer(1)},
                    {115, mp::integer(3)},
                    {123, mp::boolean(false)},
                    {124, mp::boolean(false)},
                    {125, mp::integer(0)},
                    {110, std::move(samples)}});
}

mp::Value irRenameArgs(int slot, const std::string &name) {
    return mp::map({{112, mp::integer(slot)}, {109, mp::str(name)}});
}

mp::Value irClearArgs(int slot) { return mp::map({{112, mp::integer(slot)}}); }

namespace {
mp::Value rawStr(const std::vector<std::uint8_t> &bytes) {
    mp::Value v;
    v.type = mp::Type::Str;
    v.s.assign(bytes.begin(), bytes.end());
    return v;
}
// Fields 123 / 124 / 125 of library entries: false / false / 0 in every capture (meaning unknown).
void libraryFlags(std::vector<std::pair<std::int64_t, mp::Value>> &m) {
    m.push_back({123, mp::boolean(false)});
    m.push_back({124, mp::boolean(false)});
    m.push_back({125, mp::integer(0)});
}
} // namespace

mp::Value readPresetArgs(int setlist, int preset) {
    return mp::map({{107, mp::integer(setlist)}, {108, mp::integer(preset)}, {101, mp::integer(2)}});
}

mp::Value renamePresetArgs(int setlist, int preset, const std::string &name) {
    return mp::map({{107, mp::integer(setlist)}, {108, mp::integer(preset)}, {109, mp::str(name)}});
}

mp::Value writePresetArgs(int setlist, int preset, const std::string &name, const std::vector<std::uint8_t> &blob) {
    std::vector<std::pair<std::int64_t, mp::Value>> m = {
        {107, mp::integer(setlist)}, {108, mp::integer(preset)}, {109, mp::str(name)}};
    libraryFlags(m);
    m.push_back({110, rawStr(blob)});
    return mp::map(std::move(m));
}

mp::Value reorderSetlistArgs(int setlist, const std::vector<std::pair<int, std::string>> &order) {
    std::vector<mp::Value> entries;
    for (const auto &[number, name] : order) {
        std::vector<std::pair<std::int64_t, mp::Value>> info = {{109, mp::str(name)}};
        libraryFlags(info);
        entries.push_back(mp::map({{number, mp::map(std::move(info))}}));
    }
    return mp::map({{107, mp::integer(setlist)},
                    {110, rawStr(mp::encode(mp::array(std::move(entries))))},
                    {101, mp::integer(2)}});
}

std::vector<IrSlot> irList(const mp::Value &result) {
    std::vector<IrSlot> out;
    if (!result.isArray()) return out;
    for (const mp::Value &e : result.items) {
        IrSlot s;
        if (const mp::Value *v = e.get(112)) s.slot = int(v->asInt(-1));
        if (const mp::Value *v = e.get(109)) s.name = v->asText();
        if (const mp::Value *v = e.get(104)) s.hash = v->asText();
        if (s.slot >= 0) out.push_back(std::move(s));
    }
    return out;
}

std::optional<KnobChange> knobChange(const EditorNotice &n) {
    if (n.event != kNoticeKnob) return std::nullopt;
    const mp::Value *slot = n.payload.get(98), *knob = n.payload.get(28), *value = n.payload.get(119);
    if (!slot || !knob || !value) return std::nullopt;
    KnobChange k;
    k.slot = int(slot->asInt(-1));
    k.knob = int(knob->asInt(-1));
    if (const mp::Value *set = n.payload.get(26)) k.paramSet = int(set->asInt());
    k.value = *value;
    return k;
}

std::optional<std::pair<int, bool>> bypassChange(const EditorNotice &n) {
    if (n.event != kNoticeBypass) return std::nullopt;
    const mp::Value *slot = n.payload.get(98), *on = n.payload.get(59);
    if (!slot || !on) return std::nullopt;
    return std::pair{int(slot->asInt(-1)), on->asBool()};
}

} // namespace editor

} // namespace hx

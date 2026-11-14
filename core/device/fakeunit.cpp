#include "core/device/fakeunit.h"

#include "core/device/updater.h"

#include <algorithm>
#include <string>

namespace hx {

namespace {

const std::uint8_t kSession0[] = {0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03};
const std::uint8_t kSession1[] = {0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
const std::uint8_t kSessionReply[] = {0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
// 03 reply as the 3.80 unit sent it.
const std::uint8_t kSlotTable[] = {0x10, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0};

void le32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(std::uint8_t(v >> s));
}

void expect(bool ok, const std::string &what) {
    if (!ok) throw FakeUnitError(what);
}

} // namespace

void FakeUnit::send(std::uint8_t flags, Bytes payload, std::uint8_t type, std::int64_t pos) {
    const std::uint32_t p = pos < 0 ? std::uint32_t(0x200 + rx_) : std::uint32_t(pos);
    out_.push_back(encodeMessage(flags, seq_, p, payload, type, kUnitId, kPcId));
    if (type == kTypeNormal) granted_ = std::max<std::uint64_t>(granted_, p);
    seq_ = std::uint8_t(seq_ + 1);
}

void FakeUnit::write(Bytes message) {
    const std::optional<LinkMessage> m = decodeMessage(message);
    expect(m && m->src == kPcId && m->dst == kUnitId, "message not from the PC to the unit");
    if (m->type == kTypeConnect) {
        const std::uint8_t window[] = {0x00, 0x02, 0x00, 0x00};
        send(kFlagConnect, window, kTypeConnect, 0x01000100);
        seq_ = 2;
        return;
    }
    if (!(m->flags & kFlagData)) return;  // ack, keepalive, close
    const std::vector<std::uint8_t> &p = m->payload;
    expect(!p.empty() && p.size() <= kMsgMax, "payload size " + std::to_string(p.size()));
    rx_ += p.size();
    expect(rx_ <= granted_, "credit exceeded");
    buf_.insert(buf_.end(), p.begin(), p.end());
    if (!parse() && rx_ >= granted_) send(kFlagAck);  // window full
}

bool FakeUnit::parse() {
    bool replied = false;
    while (true) {
        if (session_ < 2) {
            const Bytes want = session_ == 0 ? Bytes(kSession0) : Bytes(kSession1);
            if (buf_.size() < want.size()) return replied;
            expect(std::equal(want.begin(), want.end(), buf_.begin()), "session open");
            if (session_ == 1) {
                send(kFlagData, kSessionReply);
                replied = true;
            }
            ++session_;
            buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(want.size()));
            continue;
        }
        if (buf_.size() < 4) return replied;
        expect(buf_[0] == 0xaa && buf_[1] == 0 && buf_[2] == 0, "command word");
        const std::uint8_t op = buf_[3];
        std::size_t n = op == 4 ? 8 : op == 5 ? 32 : op == 10 ? 12 : 4;
        if (op == 7) {
            if (buf_.size() < 20) return replied;
            n = 20 + 4 * std::size_t(readLE32(buf_, 16));
        }
        if (buf_.size() < n) return replied;
        command(op, Bytes(buf_).first(n));
        replied = true;
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(n));
    }
}

void FakeUnit::command(std::uint8_t op, Bytes b) {
    std::vector<std::uint8_t> r;
    switch (op) {
    case 1:
        r = {0x01, 0x04, 0x04, 0x04};
        break;
    case 3:
        r.assign(std::begin(kSlotTable), std::end(kSlotTable));
        break;
    case 2:
    case 6:
        r.assign(4, 0);
        break;
    case 4:
        for (std::uint32_t v : {1u, 0x03800000u, 6u, 4u, 0u, 0u, 0u}) le32(r, v);
        break;
    case 5: {
        Current c;
        c.index = readLE32(b, 4);
        c.words = readLE32(b, 12);
        c.hash8.assign(b.begin() + 16, b.begin() + 24);
        record_ = std::move(c);
        for (std::uint32_t v : {1u, 0x1000u, 4u}) le32(r, v);
        break;
    }
    case 7: {
        expect(record_.has_value(), "07 before 05");
        const std::size_t k = readLE32(b, 4);
        const Bytes d = b.subspan(20);
        expect(k * 4 * kChunkWords == record_->data.size(), "chunk order");
        const std::array<std::uint8_t, 8> h = fletcherLE(d);
        expect(std::equal(h.begin(), h.end(), b.begin() + 8), "chunk " + std::to_string(k) + " hash");
        record_->data.insert(record_->data.end(), d.begin(), d.end());
        r.assign(8, 0);
        break;
    }
    case 9: {
        expect(record_.has_value(), "09 before 05");
        const std::array<std::uint8_t, 8> h = fletcherLE(record_->data);
        expect(record_->data.size() == 4 * std::size_t(record_->words) &&
                   std::equal(h.begin(), h.end(), record_->hash8.begin()),
               "record " + std::to_string(record_->index));
        records_[record_->index] = std::move(record_->data);
        record_.reset();
        r.assign(4, 0);
        break;
    }
    case 10:
        finished_ = true;
        r.assign(12, 0);
        break;
    default:
        throw FakeUnitError("unknown command " + std::to_string(op));
    }
    send(kFlagData, r);
}

std::optional<std::vector<std::uint8_t>> FakeUnit::read(double timeoutS) {
    if (outHead_ == out_.size() && rx_ + 0x200 > granted_) send(kFlagAck);  // idle: ack what is there
    if (outHead_ == out_.size()) {
        clock_.t += timeoutS;
        if (clock_.t - lastKeepalive_ >= 1) {
            lastKeepalive_ = clock_.t;
            send(kFlagKeepalive);
        }
        return std::nullopt;
    }
    std::vector<std::uint8_t> m = std::move(out_[outHead_++]);
    if (outHead_ == out_.size()) {
        out_.clear();
        outHead_ = 0;
    }
    return m;
}

} // namespace hx

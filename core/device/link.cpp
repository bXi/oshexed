#include "core/device/link.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace hx {

namespace {

void put16(std::vector<std::uint8_t> &b, std::uint16_t v) {
    b.push_back(std::uint8_t(v));
    b.push_back(std::uint8_t(v >> 8));
}

void put32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(std::uint8_t(v >> s));
}

std::string hexHead(Bytes d, std::size_t max = 16) {
    std::string s;
    char buf[3];
    for (std::size_t i = 0; i < d.size() && i < max; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", d[i]);
        s += buf;
    }
    if (d.size() > max) s += "... (" + std::to_string(d.size()) + " B)";
    return s;
}

} // namespace

std::vector<std::uint8_t> encodeMessage(std::uint8_t flags, std::uint8_t seq, std::uint32_t pos, Bytes payload,
                                        std::uint8_t type, std::uint16_t src, std::uint16_t dst) {
    std::vector<std::uint8_t> m;
    m.reserve(16 + payload.size() + 3);
    put16(m, std::uint16_t(8 + payload.size()));
    m.push_back(0);
    m.push_back(type);
    put16(m, src);
    put16(m, dst);
    m.push_back(0);
    m.push_back(seq);
    m.push_back(0);
    m.push_back(flags);
    put32(m, pos);
    m.insert(m.end(), payload.begin(), payload.end());
    m.resize((m.size() + 3) & ~std::size_t(3), 0);
    return m;
}

std::optional<LinkMessage> decodeMessage(Bytes d) {
    if (d.size() < 16) return std::nullopt;
    LinkMessage m;
    const std::size_t n = readLE16(d, 0);
    m.type = d[3];
    m.src = readLE16(d, 4);
    m.dst = readLE16(d, 6);
    m.seq = d[9];
    m.flags = d[11];
    m.pos = readLE32(d, 12);
    const std::size_t end = std::min(d.size(), 8 + n);
    if (end > 16) m.payload.assign(d.begin() + 16, d.begin() + std::ptrdiff_t(end));
    return m;
}

void Link::ioWrite(Bytes m) {
    if (trace && traceLimit > 0) {
        --traceLimit;
        trace("> " + hexHead(m, 48));
    }
    io_.write(m);
}

std::optional<std::vector<std::uint8_t>> Link::ioRead(double timeoutS) {
    std::optional<std::vector<std::uint8_t>> d = io_.read(timeoutS);
    if (d && trace && traceLimit > 0) {
        --traceLimit;
        trace("< " + hexHead(*d, 48));
    }
    return d;
}

double steadySeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Link::Link(LinkIo &io, Clock clock)
    : io_(io), clock_(std::move(clock)), pc_(io.pcAddress()), unit_(io.unitAddress()), strict_(io.strictAddresses()) {
    lastTx_ = lastRx_ = clock_();
}

void Link::sendMessage(std::uint8_t flags, Bytes payload) {
    const std::uint32_t pos = std::uint32_t(kPcWindow + rx_);
    ioWrite(encodeMessage(flags, seq_, pos, payload, kTypeNormal, pc_, unit_));
    seq_ = std::uint8_t(seq_ + 1);
    sentPos_ = pos;
    lastTx_ = clock_();
    if (flags & kFlagData) tx_ += payload.size();
}

void Link::handle(Bytes d) {
    const std::optional<LinkMessage> m = decodeMessage(d);
    if (!m || m->type != kTypeNormal || (strict_ && (m->src != unit_ || m->dst != pc_))) {
        say("  ignoring message " + hexHead(d));
        return;
    }
    if (m->seq != ((unitSeq_ + 1) & 0xff))
        throw LinkError("lost a unit message (seq " + std::to_string(m->seq) + " after " + std::to_string(unitSeq_) +
                        ")");
    unitSeq_ = m->seq;
    lastRx_ = clock_();
    credit_ = std::max<std::uint64_t>(credit_, m->pos);
    if ((m->flags & kFlagData) && !m->payload.empty()) {
        rx_ += m->payload.size();
        inbox_.push_back(m->payload);
    }
    // The unit ends the stream after each editor-server request and waits for our close before it accepts a
    // new open.
    if ((m->flags & kFlagClose) && !unitClosed_) {
        unitClosed_ = true;
        sendMessage(kFlagClose);
    }
}

void Link::poll(double timeoutS) {
    if (std::optional<std::vector<std::uint8_t>> d = ioRead(timeoutS)) handle(*d);
    if (tick) tick();
    if (unitClosed_) return;
    const double now = clock_();
    if (sentPos_ != std::int64_t(kPcWindow + rx_) && now - lastRx_ >= kAckDelayS) sendMessage(kFlagAck);
    else if (now - lastTx_ >= kKeepaliveS) sendMessage(kFlagKeepalive);
    if (now - lastRx_ > kDeadS) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "no message from the unit for %.1f s", now - lastRx_);
        throw LinkError(buf);
    }
}

// The unit takes a new open only after the previous session was closed from the PC side. Under WSL the usbipd
// attach re-enumerates the unit, which resets that; on Windows the unit stays connected, so a session left
// open (a crashed or interrupted program) blocks every later open. So: if the open gets no reply, send a close
// for the stale session and try again.
void Link::connect() {
    std::uint8_t window[4];
    for (int k = 0; k < 4; ++k) window[k] = std::uint8_t(kPcWindow >> (8 * k));
    std::optional<LinkMessage> reply;
    constexpr int kTries = 3;
    for (int attempt = 1; attempt <= kTries && !reply; ++attempt) {
        while (ioRead(0.05)) {
        }  // drop stale input
        tx_ = rx_ = 0;
        inbox_.clear();
        unitClosed_ = false;
        ioWrite(encodeMessage(kFlagConnect, 0, 0x21000100, window, kTypeConnect, pc_, unit_));
        const double t = clock_();
        const double wait = attempt == kTries ? 3.0 : 1.5;
        while (clock_() - t <= wait) {
            std::optional<std::vector<std::uint8_t>> d = ioRead(0.25);
            if (!d) continue;
            std::optional<LinkMessage> m = decodeMessage(*d);
            if (m && m->type == kTypeConnect && m->flags == kFlagConnect && (!strict_ || m->src == unit_)) {
                reply = std::move(m);
                break;
            }
            say("  connect: ignoring " + hexHead(*d));
        }
        if (!reply && attempt < kTries) {
            say("no connect reply: closing a session the unit may still hold, then trying again");
            ioWrite(encodeMessage(kFlagClose, 0, kPcWindow, {}, kTypeNormal, pc_, unit_));
        }
    }
    // A unit left with a half-open session refuses every open (Line 6's Updater too) until it restarts.
    if (!reply) throw LinkError("no connect reply: switch the Helix off and on, then try again");
    if (reply->payload.size() < 4) throw LinkError("connect reply without a window");
    credit_ = readLE32(reply->payload, 0);
    unitSeq_ = (reply->seq + 1) & 0xff;  // both sides skip seq 1 after connect
    seq_ = 2;
    sentPos_ = -1;
    lastRx_ = lastTx_ = clock_();
    open_ = true;
    char buf[48];
    std::snprintf(buf, sizeof buf, "connected (unit window %#llx)", static_cast<unsigned long long>(credit_));
    say(buf);
}

void Link::send(Bytes data) {
    std::size_t i = 0;
    while (i < data.size()) {
        if (credit_ <= tx_) {
            poll(0.2);
            continue;
        }
        const std::size_t room = std::size_t(credit_ - tx_);
        const std::size_t n = std::min({room, kMsgMax, data.size() - i});
        sendMessage(kFlagData, data.subspan(i, n));
        i += n;
    }
}

std::vector<std::uint8_t> Link::recv(double timeoutS) {
    const double t = clock_();
    while (inbox_.empty()) {
        poll(0.05);
        if (clock_() - t > timeoutS) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "no reply within %g s", timeoutS);
            throw LinkError(buf);
        }
    }
    std::vector<std::uint8_t> r = std::move(inbox_.front());
    inbox_.pop_front();
    return r;
}

std::optional<std::vector<std::uint8_t>> Link::take() {
    if (inbox_.empty()) return std::nullopt;
    std::vector<std::uint8_t> r = std::move(inbox_.front());
    inbox_.pop_front();
    return r;
}

void Link::waitUnitClose(double timeoutS) {
    const double t = clock_();
    while (!unitClosed_) {
        poll(0.05);
        if (clock_() - t > timeoutS) throw LinkError("the unit did not close the stream");
    }
}

void Link::closeQuietly() {
    if (!open_ || unitClosed_) return;
    try {
        close(0.5);
    } catch (const std::exception &) {
    }
}

void Link::close(double timeoutS) {
    open_ = false;
    if (unitClosed_) return;
    sendMessage(kFlagClose);
    const double t = clock_();
    while (!unitClosed_ && clock_() - t < timeoutS) {
        if (std::optional<std::vector<std::uint8_t>> d = ioRead(0.05)) {
            const std::optional<LinkMessage> m = decodeMessage(*d);
            if (m && (m->flags & kFlagClose)) unitClosed_ = true;
        }
    }
}

} // namespace hx

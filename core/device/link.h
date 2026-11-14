#pragma once

// PC side of the Helix message link (port of tools/hxflash.py `Link`; NOTES.md "USB update protocol").
//
// Every USB packet is one message: a 16-byte little-endian header, the payload, zero padding to 4 bytes.
//   u16 len (header bytes 8.. + payload, no padding), u8 0, u8 type (0x28 connect, 0x18 normal)
//   u16 src, u16 dst (PC 0x1001, unit 0x03ef)
//   u8 0, u8 seq (per sender, every message), u8 0, u8 flags (0x02 connect/close, 0x04 data, 0x08 ack,
//   0x10 keepalive)
//   u32 position = credit: the sender's window plus the payload bytes it has received. Each side may send
//   payload up to the other's latest position.
// Payloads are a byte stream; the commands on top of it are in updater.h. No USB here: the link talks to a
// LinkIo, which is the USB endpoint pair in the app and a fake unit in tests.

#include "core/bytes.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hx {

constexpr std::uint16_t kPcId = 0x1001;
constexpr std::uint16_t kUnitId = 0x03ef;
constexpr std::uint32_t kPcWindow = 0x1000;
constexpr std::size_t kMsgMax = 496;        // payload bytes per message
constexpr std::uint8_t kTypeConnect = 0x28, kTypeNormal = 0x18;
constexpr std::uint8_t kFlagConnect = 0x02, kFlagClose = 0x02, kFlagData = 0x04, kFlagAck = 0x08,
                       kFlagKeepalive = 0x10;

struct LinkMessage {
    std::uint8_t type = 0;
    std::uint16_t src = 0, dst = 0;
    std::uint8_t seq = 0, flags = 0;
    std::uint32_t pos = 0;
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encodeMessage(std::uint8_t flags, std::uint8_t seq, std::uint32_t pos, Bytes payload = {},
                                        std::uint8_t type = kTypeNormal, std::uint16_t src = kPcId,
                                        std::uint16_t dst = kUnitId);
// nullopt when shorter than a header.
std::optional<LinkMessage> decodeMessage(Bytes d);

// Anything that ends the link: lost message, timeout, unexpected reply, USB error.
struct LinkError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One message per call each way. read() waits up to timeoutS and returns nullopt when nothing came.
class LinkIo {
public:
    virtual ~LinkIo() = default;
    virtual void write(Bytes message) = 0;
    virtual std::optional<std::vector<std::uint8_t>> read(double timeoutS) = 0;
    // For logs: which device and endpoints.
    virtual std::string description() const { return {}; }
    // Our address on the unit's message router. Through Line 6's driver it carries the app id the driver gave us.
    virtual std::uint16_t pcAddress() const { return kPcId; }
    // The unit's address this link talks to (the editor protocol uses three: 0x03ef, 0x03ed, 0x03f0).
    virtual std::uint16_t unitAddress() const { return kUnitId; }
    // False when a driver in between rewrites the address fields: then only the message type is checked.
    virtual bool strictAddresses() const { return true; }
};

// Seconds from some fixed point; injectable so the fake unit can run on simulated time.
using Clock = std::function<double()>;
double steadySeconds();

class Link {
public:
    static constexpr double kKeepaliveS = 1.0;
    static constexpr double kAckDelayS = 0.005;
    static constexpr double kDeadS = 5.0;   // nothing from the unit for this long: link dead

    explicit Link(LinkIo &io, Clock clock = steadySeconds);

    std::function<void(const std::string &)> log;
    std::function<void()> tick;  // every poll (progress display)
    // Every raw message both ways ("> " sent, "< " received), for diagnosing the USB side. Off when empty.
    std::function<void(const std::string &)> trace;
    int traceLimit = 60;  // messages traced before it goes quiet (a flash sends thousands)

    void connect();
    // Sends payload bytes as messages, each within the unit's credit.
    void send(Bytes data);
    // Next unit data payload; keeps the link alive while waiting.
    std::vector<std::uint8_t> recv(double timeoutS);
    // A data payload that already arrived (poll() collects them), without waiting.
    std::optional<std::vector<std::uint8_t>> take();
    // Reads at most one message; sends a pending ack or a keepalive when due.
    void poll(double timeoutS = kAckDelayS);
    void waitUnitClose(double timeoutS = 3.0);
    // Our close; the unit answers with its own.
    void close(double timeoutS = 1.0);
    // After a failure: close if a session is open, ignore errors. Leaving it open blocks the next connect.
    void closeQuietly();
    bool unitClosed() const { return unitClosed_; }

private:
    void sendMessage(std::uint8_t flags, Bytes payload = {});
    void ioWrite(Bytes m);
    std::optional<std::vector<std::uint8_t>> ioRead(double timeoutS);
    void handle(Bytes d);
    void say(const std::string &s) const {
        if (log) log(s);
    }

    LinkIo &io_;
    Clock clock_;
    std::uint16_t pc_ = kPcId;
    std::uint16_t unit_ = kUnitId;
    bool strict_ = true;
    std::uint8_t seq_ = 0;
    std::uint64_t tx_ = 0;          // payload bytes sent
    std::uint64_t rx_ = 0;          // payload bytes received
    std::uint64_t credit_ = 0;      // unit's latest position: we may send while tx < credit
    int unitSeq_ = -1;
    std::int64_t sentPos_ = -1;     // position in our last message
    double lastTx_ = 0, lastRx_ = 0;
    std::deque<std::vector<std::uint8_t>> inbox_;
    bool unitClosed_ = false;       // the unit sent close and we answered
    bool open_ = false;             // connected and not closed by us
};

} // namespace hx

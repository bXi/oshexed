#pragma once

// A pretend unit behind a LinkIo (port of tools/hxflash.py `FakeUnit`): credit window 512 (acks when full or
// idle), parses the update commands, checks every hash, canned replies, simulated time. Replies come at once,
// so it checks the protocol, not the timing. Used by core_tests and `oshexed --flash-sim`.

#include "core/device/link.h"

#include <map>
#include <stdexcept>
#include <vector>

namespace hx {

struct FakeClock {
    double t = 0;
    Clock clock() {
        return [this] { return t; };
    }
};

// Thrown when the PC side breaks the protocol (wrong order, bad hash, credit exceeded).
struct FakeUnitError : std::logic_error {
    using std::logic_error::logic_error;
};

class FakeUnit : public LinkIo {
public:
    explicit FakeUnit(FakeClock &clock) : clock_(clock) {}

    void write(Bytes message) override;
    std::optional<std::vector<std::uint8_t>> read(double timeoutS) override;

    // Records written and verified (09 passed), by index.
    const std::map<std::uint32_t, std::vector<std::uint8_t>> &records() const { return records_; }
    bool finished() const { return finished_; }

private:
    void send(std::uint8_t flags, Bytes payload = {}, std::uint8_t type = kTypeNormal, std::int64_t pos = -1);
    bool parse();
    void command(std::uint8_t op, Bytes b);

    FakeClock &clock_;
    std::vector<std::vector<std::uint8_t>> out_;
    std::size_t outHead_ = 0;
    std::uint8_t seq_ = 0;
    std::uint64_t rx_ = 0;
    std::uint64_t granted_ = 0x200;
    std::vector<std::uint8_t> buf_;
    int session_ = 0;
    struct Current {
        std::uint32_t index = 0;
        std::uint32_t words = 0;
        std::vector<std::uint8_t> hash8, data;
    };
    std::optional<Current> record_;
    std::map<std::uint32_t, std::vector<std::uint8_t>> records_;
    double lastKeepalive_ = 0;
    bool finished_ = false;
};

} // namespace hx

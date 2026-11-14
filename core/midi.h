#pragma once

// MIDI input for the Monitor tab (read-only: nothing is ever sent).
//
// Windows: the WinMM MIDI API (ships with Windows; same calls as C:\Users\Public\midimon.ps1). Every input port
// is opened while listening. Note: USB MIDI out of the Helix is broken on Windows 11 24H2+ with the new MIDI
// service, so the Helix's own port may stay silent there; a DIN MIDI interface works.
// Linux / macOS: no ports yet.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hx::midi {

struct Message {
    int port = 0;
    double time = 0;  // seconds() when it arrived
    std::uint8_t status = 0, data1 = 0, data2 = 0;
    std::size_t sysexBytes = 0;  // a SysEx message: its length (status 0xF0, data unused); not captured yet
};

// The clock Message::time uses (seconds since its first use).
double seconds();

// One line, e.g. "ch 16  CC 40 = 12", "ch 1  Note on C4 (60) vel 100", "Clock".
std::string describe(const Message& m);
// Clock, active sensing: the monitor hides them unless asked.
bool isRealtime(const Message& m);

struct Port {
    std::string name;
    bool open = false;
    std::string error;  // why it didn't open
};

class Input {
public:
    Input();
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    static std::vector<std::string> portNames();
    // Opens input port `only` (portNames() index), or every port for -1; ports() says which worked.
    void start(int only = -1);
    void stop();
    bool listening() const { return listening_; }
    const std::vector<Port>& ports() const { return ports_; }
    // Messages since the last call (from WinMM's callback thread).
    std::vector<Message> take();

    struct Impl;  // platform part; public so the callback can reach push()
    void push(const Message& m);

private:
    std::unique_ptr<Impl> impl_;
    bool listening_ = false;
    std::vector<Port> ports_;
    std::mutex mutex_;
    std::vector<Message> queue_;
};

// The NAM / polymidi debug words (TESTS.md "Reporting"): word i (0-3) arrives as four 5-bit chunks on channel 16,
// CC 40 + 4i + c (c = 0..3): word = CC(40+4i) + 32 CC(41+4i) + 1024 CC(42+4i) + 32768 CC(43+4i).
class DebugWords {
public:
    static constexpr int kWords = 4;
    // True when m was one of the chunks.
    bool feed(const Message& m);
    bool seen(int word) const { return seen_[word] == 0xF; }  // all four chunks arrived at least once
    std::uint32_t value(int word) const;
    double updated(int word) const { return updated_[word]; }  // Message::time of its last chunk
    void clear();

private:
    std::uint8_t chunks_[kWords][4] = {};
    std::uint8_t seen_[kWords] = {};
    double updated_[kWords] = {};
};

} // namespace hx::midi

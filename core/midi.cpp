#include "core/midi.h"

#include <chrono>
#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace hx::midi {

namespace {

std::string noteName(int n) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

} // namespace

double seconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool isRealtime(const Message& m) {
    return m.status == 0xF8 || m.status == 0xFE;
}

std::string describe(const Message& m) {
    char buf[96];
    if (m.sysexBytes) {
        std::snprintf(buf, sizeof buf, "SysEx, %zu bytes", m.sysexBytes);
        return buf;
    }
    const int ch = (m.status & 0x0F) + 1;
    switch (m.status & 0xF0) {
    case 0x80: std::snprintf(buf, sizeof buf, "ch %d  Note off %s (%d) vel %d", ch, noteName(m.data1).c_str(), m.data1, m.data2); break;
    case 0x90:
        std::snprintf(buf, sizeof buf, "ch %d  Note %s %s (%d) vel %d", ch, m.data2 ? "on" : "off", noteName(m.data1).c_str(),
                      m.data1, m.data2);
        break;
    case 0xA0: std::snprintf(buf, sizeof buf, "ch %d  Aftertouch %s = %d", ch, noteName(m.data1).c_str(), m.data2); break;
    case 0xB0: std::snprintf(buf, sizeof buf, "ch %d  CC %d = %d", ch, m.data1, m.data2); break;
    case 0xC0: std::snprintf(buf, sizeof buf, "ch %d  Program %d", ch, m.data1); break;
    case 0xD0: std::snprintf(buf, sizeof buf, "ch %d  Channel pressure %d", ch, m.data1); break;
    case 0xE0: std::snprintf(buf, sizeof buf, "ch %d  Pitch bend %d", ch, (m.data1 | (m.data2 << 7)) - 8192); break;
    default:
        switch (m.status) {
        case 0xF8: return "Clock";
        case 0xFA: return "Start";
        case 0xFB: return "Continue";
        case 0xFC: return "Stop";
        case 0xFE: return "Active sensing";
        case 0xFF: return "Reset";
        default: std::snprintf(buf, sizeof buf, "%02X %02X %02X", m.status, m.data1, m.data2);
        }
    }
    return buf;
}

void Input::push(const Message& m) {
    std::lock_guard lock(mutex_);
    if (queue_.size() < 20000) queue_.push_back(m);  // nobody reading: don't grow without end
}

std::vector<Message> Input::take() {
    std::lock_guard lock(mutex_);
    std::vector<Message> out;
    out.swap(queue_);
    return out;
}

#if defined(_WIN32)

struct Input::Impl {
    struct Open {
        Input* owner;
        int port;
        HMIDIIN handle = nullptr;
    };
    std::vector<std::unique_ptr<Open>> open;
};

namespace {

// Short messages only: SysEx needs buffers handed back from here, and WinMM forbids MIDI calls in its callback.
void CALLBACK onMidi(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
    if (msg != MIM_DATA) return;
    auto* o = reinterpret_cast<Input::Impl::Open*>(inst);
    Message m;
    m.port = o->port;
    m.time = seconds();
    m.status = std::uint8_t(p1);
    m.data1 = std::uint8_t(p1 >> 8);
    m.data2 = std::uint8_t(p1 >> 16);
    o->owner->push(m);
}

std::string portName(UINT i) {
    MIDIINCAPSW caps{};
    if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) return "port " + std::to_string(i);
    std::string s;
    for (const wchar_t* p = caps.szPname; *p; ++p) s += *p < 128 ? char(*p) : '?';
    return s;
}

} // namespace

Input::Input() : impl_(std::make_unique<Impl>()) {}
Input::~Input() { stop(); }

std::vector<std::string> Input::portNames() {
    std::vector<std::string> out;
    for (UINT i = 0, n = midiInGetNumDevs(); i < n; ++i) out.push_back(portName(i));
    return out;
}

void Input::start(int only) {
    if (listening_) return;
    ports_.clear();
    for (UINT i = 0, n = midiInGetNumDevs(); i < n; ++i) {
        Port p;
        if (only >= 0 && int(i) != only) {
            p.name = portName(i);
            p.error = "not selected";
            ports_.push_back(std::move(p));  // kept so Message::port still indexes ports()
            continue;
        }
        p.name = portName(i);
        auto o = std::make_unique<Impl::Open>();
        o->owner = this;
        o->port = int(i);
        const MMRESULT r = midiInOpen(&o->handle, i, DWORD_PTR(&onMidi), DWORD_PTR(o.get()), CALLBACK_FUNCTION);
        if (r == MMSYSERR_NOERROR) {
            midiInStart(o->handle);
            p.open = true;
            impl_->open.push_back(std::move(o));
        } else {
            wchar_t text[MAXERRORLENGTH] = {};
            midiInGetErrorTextW(r, text, MAXERRORLENGTH);
            for (const wchar_t* c = text; *c; ++c) p.error += *c < 128 ? char(*c) : '?';
            if (p.error.empty()) p.error = "error " + std::to_string(r);
        }
        ports_.push_back(std::move(p));
    }
    listening_ = true;
}

void Input::stop() {
    for (auto& o : impl_->open) {
        midiInStop(o->handle);
        midiInReset(o->handle);
        midiInClose(o->handle);
    }
    impl_->open.clear();
    for (Port& p : ports_) p.open = false;
    listening_ = false;
}

#else

struct Input::Impl {};
Input::Input() : impl_(std::make_unique<Impl>()) {}
Input::~Input() = default;
std::vector<std::string> Input::portNames() { return {}; }
void Input::start(int) { listening_ = true; }
void Input::stop() { listening_ = false; }

#endif

bool DebugWords::feed(const Message& m) {
    if (m.sysexBytes || m.status != 0xBF || m.data1 < 40 || m.data1 > 55) return false;
    const int word = (m.data1 - 40) / 4, chunk = (m.data1 - 40) % 4;
    chunks_[word][chunk] = m.data2 & 0x1F;
    seen_[word] |= std::uint8_t(1 << chunk);
    updated_[word] = m.time;
    return true;
}

std::uint32_t DebugWords::value(int word) const {
    const std::uint8_t* c = chunks_[word];
    return std::uint32_t(c[0]) | std::uint32_t(c[1]) << 5 | std::uint32_t(c[2]) << 10 | std::uint32_t(c[3]) << 15;
}

void DebugWords::clear() {
    *this = DebugWords{};
}

} // namespace hx::midi

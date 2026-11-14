#include "core/ir.h"

#include "core/checksums.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace hx {

namespace {

constexpr std::uint16_t kFormatPcm = 1, kFormatFloat = 3, kFormatExtensible = 0xfffe;

struct WavFormat {
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
};

// One sample (frame `i`, channel 0) as a float in -1..1 (floats as stored).
float sampleAt(Bytes data, const WavFormat& f, std::size_t i) {
    const std::size_t bytes = f.bits / 8;
    const std::size_t off = i * bytes * f.channels;
    const std::uint8_t* p = data.data() + off;
    if (f.format == kFormatFloat) {
        if (f.bits == 32) {
            float v;
            std::uint32_t w = readLE32(data, off);
            std::memcpy(&v, &w, 4);
            return v;
        }
        std::uint64_t w = 0;
        for (int k = 0; k < 8; ++k) w |= std::uint64_t(p[k]) << (8 * k);
        double v;
        std::memcpy(&v, &w, 8);
        return float(v);
    }
    switch (f.bits) {
    case 8: return (float(p[0]) - 128.f) / 128.f;  // 8-bit WAV is unsigned
    case 16: return float(std::int16_t(p[0] | p[1] << 8)) / 32768.f;
    case 24: {
        std::int32_t v = p[0] | p[1] << 8 | p[2] << 16;
        if (v & 0x800000) v -= 0x1000000;
        return float(v) / 8388608.f;
    }
    default: return float(double(std::int32_t(readLE32(data, off))) / 2147483648.0);
    }
}

} // namespace

std::optional<IrData> irFromWav(Bytes wav, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<IrData> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
        return fail("not a WAV file");
    WavFormat f;
    Bytes data;
    bool haveFmt = false, haveData = false;
    for (std::size_t off = 12; off + 8 <= wav.size();) {
        const std::uint32_t size = readLE32(wav, off + 4);
        const std::size_t body = off + 8;
        const std::size_t n = std::min<std::size_t>(size, wav.size() - body);  // a truncated last chunk: what's there
        if (std::memcmp(wav.data() + off, "fmt ", 4) == 0 && n >= 16) {
            f.format = readLE16(wav, body);
            f.channels = readLE16(wav, body + 2);
            f.rate = readLE32(wav, body + 4);
            f.bits = readLE16(wav, body + 14);
            if (f.format == kFormatExtensible && n >= 26) f.format = readLE16(wav, body + 24);  // sub-format GUID's first word
            haveFmt = true;
        } else if (std::memcmp(wav.data() + off, "data", 4) == 0) {
            data = wav.subspan(body, n);
            haveData = true;
        }
        off = body + size + (size & 1);  // chunks are padded to even sizes
    }
    if (!haveFmt || !haveData) return fail("the WAV has no format or no data chunk");
    const bool pcm = f.format == kFormatPcm && (f.bits == 8 || f.bits == 16 || f.bits == 24 || f.bits == 32);
    const bool flt = f.format == kFormatFloat && (f.bits == 32 || f.bits == 64);
    if (!pcm && !flt) return fail("unsupported WAV encoding (format " + std::to_string(f.format) + ", " +
                                  std::to_string(f.bits) + " bit)");
    if (f.channels == 0 || f.rate == 0) return fail("the WAV header has no channels or no sample rate");
    const std::size_t frames = data.size() / (std::size_t(f.bits / 8) * f.channels);
    if (frames == 0) return fail("the WAV has no samples");

    IrData out;
    if (f.channels > 1) out.notes.push_back(std::to_string(f.channels) + " channels: the first (left) is used");
    std::vector<float> src(frames);
    for (std::size_t i = 0; i < frames; ++i) src[i] = sampleAt(data, f, i);

    // To 48 kHz; only as many output samples as the slot holds are computed.
    std::vector<float> s;
    if (f.rate == std::uint32_t(kIrRate)) {
        s = std::move(src);
    } else {
        out.notes.push_back("resampled from " + std::to_string(f.rate) + " Hz to 48000 Hz");
        const double step = double(f.rate) / kIrRate;
        const std::size_t n = std::min<std::size_t>(kIrSamples, std::size_t(double(frames - 1) / step) + 1);
        s.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double x = double(i) * step;
            const std::size_t k = std::size_t(x);
            const double t = x - double(k);
            const float a = src[k], b = k + 1 < frames ? src[k + 1] : 0.f;
            s[i] = float(a + (b - a) * t);
        }
    }
    if (s.size() > std::size_t(kIrSamples))
        out.notes.push_back("cut from " + std::to_string(s.size()) + " to 2048 samples (42.7 ms)");
    s.resize(kIrSamples, 0.f);
    for (int i = 0; i < kIrFade; ++i) s[std::size_t(kIrSamples - kIrFade + i)] *= float(kIrFade - 1 - i) / float(kIrFade);
    out.samples = std::move(s);
    return out;
}

std::vector<std::uint8_t> irBytes(const std::vector<float>& samples) {
    std::vector<std::uint8_t> out;
    out.reserve(samples.size() * 4);
    for (float v : samples) {
        const std::uint32_t w = std::bit_cast<std::uint32_t>(v);
        for (int k = 0; k < 32; k += 8) out.push_back(std::uint8_t(w >> k));
    }
    return out;
}

std::vector<std::uint8_t> irWav(const std::vector<float>& samples) {
    const std::vector<std::uint8_t> data = irBytes(samples);
    std::vector<std::uint8_t> o;
    auto u32 = [&](std::uint32_t v) {
        for (int k = 0; k < 32; k += 8) o.push_back(std::uint8_t(v >> k));
    };
    auto u16 = [&](std::uint16_t v) {
        o.push_back(std::uint8_t(v));
        o.push_back(std::uint8_t(v >> 8));
    };
    auto tag = [&](const char* t) { o.insert(o.end(), t, t + 4); };
    tag("RIFF");
    u32(std::uint32_t(4 + 8 + 16 + 8 + data.size()));
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(kFormatFloat);
    u16(1);
    u32(kIrRate);
    u32(kIrRate * 4);
    u16(4);
    u16(32);
    tag("data");
    u32(std::uint32_t(data.size()));
    o.insert(o.end(), data.begin(), data.end());
    return o;
}

std::uint32_t irSum(Bytes data) {
    std::uint32_t sum = 0;
    for (std::size_t off = 0; off + 4 <= data.size(); off += 4) sum += readLE32(data, off);
    return sum;
}

std::string irHash(Bytes data) {
    const auto h = md5(data);
    return toHex(Bytes(h.data(), h.size()));
}

std::string irName(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (out.size() >= 63) break;
        const bool printable = c >= 32 && c < 127;
        const bool lacking = c == '[' || c == ']' || c == '{' || c == '}' || c == '\\' || c == '|' || c == '~' ||
                             c == '^' || c == '`';
        out += printable && !lacking ? c : '?';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "IR" : out;
}

} // namespace hx

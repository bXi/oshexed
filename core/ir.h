#pragma once

// User IRs (NOTES.md "User IRs" and "Editor protocol"): a slot holds 2048 mono 32-bit float samples at 48 kHz.
// What HX Edit uploads (library cmd 9) is those samples as little-endian floats, their 32-bit word sum, and a name;
// the slot list then shows the MD5 of the same bytes as the IR's hash (the one presets use in key 12).

#include "core/bytes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hx {

constexpr int kIrSamples = 2048;
constexpr int kIrRate = 48000;
constexpr int kIrFade = 120;  // HX Edit fades the last 120 samples (NAM notes "N2"); 0..1927 pass bit-exact

struct IrData {
    std::vector<float> samples;      // kIrSamples
    std::vector<std::string> notes;  // what was changed to fit: resampled, first channel only, cut, ...
};

// A .wav file (PCM 8/16/24/32-bit or 32/64-bit float, WAVE_FORMAT_EXTENSIBLE too; any channel count: the first is
// used; any rate: resampled to 48 kHz by linear interpolation) as a slot's 2048 samples: cut or padded with zeros,
// then the last kIrFade samples faded out as HX Edit does. Nothing is normalised. nullopt + error for a file that
// isn't a readable WAV.
std::optional<IrData> irFromWav(Bytes wav, std::string* error);

// The samples as the upload carries them: little-endian float32.
std::vector<std::uint8_t> irBytes(const std::vector<float>& samples);
// A mono 48 kHz 32-bit float .wav of the samples (what nam2wav.py writes; HX Edit imports it unchanged).
std::vector<std::uint8_t> irWav(const std::vector<float>& samples);
// The upload's field 113: the 32-bit sum of the data's little-endian words.
std::uint32_t irSum(Bytes data);
// MD5 of the data as 32 lowercase hex digits: the hash the slot list will show.
std::string irHash(Bytes data);
// A name the unit takes: printable ASCII, '[' / ']' (and other characters its font lacks) as '?' like HX Edit does,
// at most 63 characters, "IR" if nothing is left.
std::string irName(std::string_view text);

} // namespace hx

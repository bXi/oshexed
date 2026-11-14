#pragma once

// NAM captures -> a user IR slot for our NAM block (port of ../tools/nam2wav.py; format v1 is our own, NOTES.md
// "NAM"). Only the small 3-channel WaveNet ("A2 Lite", 1871 weights) that the DSP kernel runs is accepted, the
// one a SlimmableContainer .nam carries next to the full model; anything else is refused with the reason.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hx {

struct NamIr {
    std::vector<float> samples;  // 2048, ready to upload as they are (words 1928+ are 0: HX Edit's fade can't hurt)
    int weights = 0;             // 1871
    int exponent = 0;            // weights are stored as w / 2^exponent
    double levelDbu = 0, loudness = 0;  // from the metadata (0 if absent)
    int flushed = 0;             // denormal weights set to 0
    std::string name;            // metadata name, "" if none
    std::string gearType;        // metadata gear_type ("amp", "amp_cab", "pedal", ...), "" if none
    std::string modeledBy, gearMake, gearModel, toneType;  // metadata, "" if none
    double sampleRate = 0;       // the file's sample_rate (0 if absent)
};

// The header our NAM slots start with (format v1, written by namToIr): the two marker words, then version, weight
// exponent, weight count, input level and loudness. nullopt: not one of our NAM captures (an ordinary IR).
// `words`: the slot's first sample words (at least 7), as read back from the unit (library command 11).
struct NamHeader {
    int version = 0, weights = 0, exponent = 0;
    double levelDbu = 0, loudness = 0;
};
std::optional<NamHeader> namHeader(const float* words, std::size_t count);

// A slot name for a capture: a short gear-type prefix (P pedal, A amp, AC amp + cab, PA pedal + amp, PAC pedal + amp
// + cab, Pre preamp) and its name, cut to what a slot holds (irName).
std::string namSlotName(const std::string& gearType, const std::string& name);

// The .nam file's text -> the slot's samples, bit for bit what nam2wav.py writes. nullopt + reason if the file
// isn't JSON, has no 3-channel WaveNet, or that model isn't the A2 Lite structure.
std::optional<NamIr> namToIr(std::string_view json, std::string* error);

// One line for the user about what kind of capture it is ("amp + cab: use without a cab block", ...); "" if unknown.
std::string namGearHint(const std::string& gearType);

} // namespace hx

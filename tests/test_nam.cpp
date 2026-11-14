#include "test.h"

#include "core/ir.h"
#include "core/nam.h"

#include <bit>
#include <cstdint>
#include <string>

namespace {

// A synthetic SlimmableContainer with the A2 Lite 3-channel WaveNet (structure only; weights are a ramp).
std::string a2Lite(double scale, const char* gear = "amp_cab", int weights = 1871) {
    const char* kernels = "[6,6,6,6,6,6,6,6,6,6,6,6,6,6,15,15,6,6,6,6,6,6,6]";
    const char* dilations = "[1,3,7,17,41,101,239,1,3,7,17,41,101,239,1,13,1,3,7,17,41,101,239]";
    std::string act, gating, secondary;
    for (int i = 0; i < 23; ++i) {
        act += std::string(i ? "," : "") + R"({"type":"LeakyReLU","negative_slope":0.01})";
        gating += std::string(i ? "," : "") + R"("none")";
        secondary += std::string(i ? "," : "") + "null";
    }
    std::string w;
    for (int i = 0; i < weights; ++i) w += (i ? "," : "") + std::to_string(scale * (i - 900) / 900.0);
    return std::string(R"({"version":"0.7.0","architecture":"SlimmableContainer","metadata":{"gear_type":")") + gear +
           R"(","input_level_dbu":12.5,"loudness":-17.25},"config":{"submodels":[{"max_value":0.5,"model":{
           "architecture":"WaveNet","sample_rate":48000,"config":{"head":null,"layers":[{"input_size":1,
           "condition_size":1,"channels":3,"bottleneck":3,"kernel_sizes":)" +
           kernels + R"(,"dilations":)" + dilations + R"(,"activation":[)" + act + R"(],"gating_mode":[)" + gating +
           R"(],"secondary_activation":[)" + secondary + R"(],"layer1x1":{"active":true,"groups":1},
           "head1x1":{"active":false},"head":{"out_channels":1,"kernel_size":16,"bias":true},"groups_input":1,
           "groups_input_mixin":1}]},"weights":[)" +
           w + "]}}]}}";
}

} // namespace

TEST("nam: an A2 Lite capture packs as nam2wav.py's format v1") {
    std::string error;
    const auto ir = hx::namToIr(a2Lite(10.0), &error);
    REQUIRE(ir.has_value());
    CHECK_EQ(ir->samples.size(), std::size_t(2048));
    CHECK_EQ(ir->weights, 1871);
    CHECK_EQ(ir->exponent, 2);  // peak ~10.8: / 4 = 2.7 <= 4
    CHECK(std::bit_cast<std::uint32_t>(ir->samples[0]) == std::bit_cast<std::uint32_t>(0.6180339887f));
    CHECK_EQ(ir->samples[2], 1.0f);
    CHECK_EQ(ir->samples[3], 2.f / 16);
    CHECK_EQ(ir->samples[4], 1871.f / 1024);
    CHECK_EQ(ir->samples[5], float(12.5 / 16));
    CHECK_EQ(ir->samples[6], float(-17.25 / 16));
    // Word 7: the low 16 bits of the sum of the stored weights' words, / 65536.
    std::uint64_t sum = 0;
    for (int i = 8; i < 8 + 1871; ++i) sum += std::bit_cast<std::uint32_t>(ir->samples[std::size_t(i)]);
    CHECK_EQ(ir->samples[7], float(double(sum & 0xffff) / 65536.0));
    // Weights scaled exactly by 2^-e; the words HX Edit fades stay 0.
    CHECK_EQ(ir->samples[8] * 4.f, float(10.0 * -900 / 900.0));
    CHECK_EQ(ir->samples[2047], 0.f);
    CHECK_EQ(ir->gearType, std::string("amp_cab"));
    CHECK(!hx::namGearHint(ir->gearType).empty());
    // As a .wav it reads back unchanged (bit-exact through the importer up to the fade).
    const auto back = hx::irFromWav(hx::irWav(ir->samples), nullptr);
    REQUIRE(back.has_value());
    CHECK(back->samples == ir->samples);
}

TEST("nam: other models are refused with a reason") {
    std::string error;
    CHECK(!hx::namToIr(a2Lite(1.0, "amp", 1870), &error));
    CHECK(error.find("1871") != std::string::npos);
    CHECK(!hx::namToIr("{\"architecture\":\"WaveNet\",\"config\":{\"layers\":[{\"channels\":8}]}}", &error));
    CHECK(error.find("3-channel") != std::string::npos);
    CHECK(!hx::namToIr("not json", &error));
}

TEST("nam: a slot's header words tell a capture from an IR") {
    // as read back from the unit's slot 128 (Randall SATAN 50, 2026-11-13): markers, v1, exponent 0, 1871 weights,
    // level 0, loudness -23.4 dB
    const float capture[8] = {0.6180339887f, -0.3819660113f, 1.0f, 0.0f, 1871.0f / 1024.0f, 0.0f, -1.46529f, 0.333282f};
    const auto h = hx::namHeader(capture, 8);
    REQUIRE(h.has_value());
    CHECK_EQ(h->version, 1);
    CHECK_EQ(h->weights, 1871);
    CHECK(h->loudness < -23.4 && h->loudness > -23.5);
    const float ir[8] = {0.5f, 0.25f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(!hx::namHeader(ir, 8).has_value());
    CHECK(!hx::namHeader(capture, 2).has_value());  // too short to tell
}

TEST("nam: slot names get the gear-type prefix") {
    CHECK_EQ(hx::namSlotName("amp_cab", "Satan 50"), hx::irName("AC Satan 50"));
    CHECK_EQ(hx::namSlotName("pedal", "Klon"), hx::irName("P Klon"));
    CHECK_EQ(hx::namSlotName("", "Plain"), hx::irName("Plain"));
}

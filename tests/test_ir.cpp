#include "test.h"

#include "core/device/editorclient.h"
#include "core/ir.h"
#include "core/msgpack.h"

#include <bit>
#include <cmath>
#include <cstring>

namespace {

// A minimal WAV: RIFF/WAVE, "fmt " (16 bytes), optional odd-sized junk chunk, "data".
std::vector<std::uint8_t> wav(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits,
                              const std::vector<std::uint8_t>& data, bool junk = false) {
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
    u32(0);  // not checked
    tag("WAVE");
    if (junk) {
        tag("junk");
        u32(3);
        o.insert(o.end(), {1, 2, 3, 0});  // padded to even
    }
    tag("fmt ");
    u32(16);
    u16(format);
    u16(channels);
    u32(rate);
    u32(rate * channels * bits / 8);
    u16(std::uint16_t(channels * bits / 8));
    u16(bits);
    tag("data");
    u32(std::uint32_t(data.size()));
    o.insert(o.end(), data.begin(), data.end());
    return o;
}

std::vector<std::uint8_t> floats(const std::vector<float>& v) { return hx::irBytes(v); }

} // namespace

TEST("ir: 48 kHz float mono passes bit-exact up to the fade, padded with zeros") {
    std::vector<float> in(3000);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = float(std::sin(double(i) * 0.37)) * 3.5f;  // peaks > 1 stay
    const auto ir = hx::irFromWav(wav(3, 1, 48000, 32, floats(in)), nullptr);
    REQUIRE(ir.has_value());
    CHECK_EQ(ir->samples.size(), std::size_t(2048));
    for (int i = 0; i < 2048 - hx::kIrFade; ++i)
        CHECK(std::bit_cast<std::uint32_t>(ir->samples[std::size_t(i)]) == std::bit_cast<std::uint32_t>(in[std::size_t(i)]));
    CHECK_EQ(ir->samples[2047], 0.f);  // faded out
    CHECK(!ir->notes.empty());         // cut from 3000
    const auto shortIr = hx::irFromWav(wav(3, 1, 48000, 32, floats({0.5f, -0.25f}), true), nullptr);
    REQUIRE(shortIr.has_value());
    CHECK_EQ(shortIr->samples[1], -0.25f);
    CHECK_EQ(shortIr->samples[2], 0.f);
    CHECK(shortIr->notes.empty());
}

TEST("ir: PCM 16 / 24 bit, stereo uses the first channel, other rates resampled") {
    // 16-bit stereo: left 16384 (0.5), right -32768.
    const auto a = hx::irFromWav(wav(1, 2, 48000, 16, {0x00, 0x40, 0x00, 0x80}), nullptr);
    REQUIRE(a.has_value());
    CHECK_EQ(a->samples[0], 0.5f);
    CHECK_EQ(a->notes.size(), std::size_t(1));
    // 24-bit: -0.5.
    const auto b = hx::irFromWav(wav(1, 1, 48000, 24, {0x00, 0x00, 0xc0}), nullptr);
    REQUIRE(b.has_value());
    CHECK_EQ(b->samples[0], -0.5f);
    // 24 kHz -> 48 kHz: every other output sample is between two inputs.
    const auto c = hx::irFromWav(wav(3, 1, 24000, 32, floats({0.f, 1.f, 0.f})), nullptr);
    REQUIRE(c.has_value());
    CHECK_EQ(c->samples[1], 0.5f);
    CHECK_EQ(c->samples[2], 1.f);
    std::string error;
    CHECK(!hx::irFromWav(std::vector<std::uint8_t>{'R', 'I', 'F', 'F'}, &error));
    CHECK(!error.empty());
}

TEST("ir: checksum, hash, name and the upload request as HX Edit sends it") {
    // Field 113 is the 32-bit sum of the data words (checked against hx-edit-ops: 0x29f316a5 for its upload).
    const std::vector<std::uint8_t> d = {1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 2, 0, 0, 0};
    CHECK_EQ(hx::irSum(d), std::uint32_t(2));
    CHECK_EQ(hx::irHash(std::vector<std::uint8_t>{}), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(hx::irName("[AMP] Zoota - DI"), std::string("?AMP? Zoota - DI"));
    CHECK_EQ(hx::irName("  "), std::string("IR"));
    // Key order and types: 112, 113 (u32), 109 (str + NUL), 114, 115, 123, 124, 125, 110 (str16 without NUL).
    const std::vector<std::uint8_t> data(8192, 0);
    const auto bytes = hx::mp::encode(hx::editor::irUploadArgs(100, "Test", data, 0x29f316a5));
    const std::uint8_t head[] = {0x89, 0x70, 0x64, 0x71, 0xce, 0x29, 0xf3, 0x16, 0xa5, 0x6d, 0xa5, 'T', 'e', 's', 't', 0,
                                 0x72, 0x01, 0x73, 0x03, 0x7b, 0xc2, 0x7c, 0xc2, 0x7d, 0x00, 0x6e, 0xda, 0x20, 0x00};
    REQUIRE(bytes.size() == sizeof head + 8192);
    CHECK(std::memcmp(bytes.data(), head, sizeof head) == 0);
}

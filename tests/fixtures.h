#pragma once

// Synthetic rec 4 (DSP metadata) for the tests, laid out as NOTES.md "Model tables" describes.

#include "builder.h"

#include <array>
#include <bit>
#include <string>
#include <vector>

namespace test {

struct MetaFixture {
    // Symbol indices.
    enum : std::uint32_t {
        kAmpA, kAmpB, kDrive, kDisplayName, kAmpAName, kCategory, kAmp, kSubcategory, kGuitar, kGain, kBass, kCount
    };
    static std::vector<std::string> symbols() {
        return {"HD2_AmpA", "HD2_AmpB", "Drive", "displayName", "Amp A Deluxe", "category",
                "amp",      "subcategory", "guitar", "Gain", "bass"};
    }

    // Two entries: AmpA (attrs category/subcategory/displayName, one float knob "Drive" shown as
    // "Gain", one state param) and AmpB (category amp, subcategory bass, no display name).
    static std::vector<std::uint8_t> build() {
        Buf m;
        m.zeros(40);  // header, patched below

        const std::size_t symOff = m.size();
        const std::vector<std::string> syms = symbols();
        m.le32(std::uint32_t(syms.size()));
        m.le32(0);
        std::uint32_t poolPos = 0;
        for (const std::string& s : syms) {
            m.le32(poolPos);
            m.le32(0);
            m.le32(std::uint32_t(s.size()));
            poolPos += std::uint32_t(s.size());
        }
        for (const std::string& s : syms) m.bytes(s);
        m.padTo((m.size() + 3) & ~std::size_t(3));

        auto attrs = [&](std::vector<std::array<std::uint32_t, 3>> list) {
            const std::size_t at = m.size();
            for (auto& a : list) {
                m.le32(a[0]);
                m.le32(a[1]);
                m.le32(a[2]);
            }
            return std::uint32_t(at);
        };
        const std::uint32_t modelA = attrs({{kCategory, 7, kAmp}, {kSubcategory, 7, kGuitar}, {kDisplayName, 7, kAmpAName}});
        const std::uint32_t modelB = attrs({{kCategory, 7, kAmp}, {kSubcategory, 7, kBass}});
        const std::uint32_t knobAttrs = attrs({{kDisplayName, 7, kGain}});

        const std::uint32_t knobs = std::uint32_t(m.size());
        for (std::uint32_t w : {1u, knobAttrs, std::uint32_t(kDrive), 3u, 0u, std::bit_cast<std::uint32_t>(0.0f),
                                std::bit_cast<std::uint32_t>(1.0f), std::bit_cast<std::uint32_t>(0.5f)})
            m.le32(w);
        const std::uint32_t states = std::uint32_t(m.size());
        for (std::uint32_t w : {0u, 0u, std::uint32_t(kDrive), 1u, 4u}) m.le32(w);

        const std::size_t entOff = m.size();
        // [nModelAttr, modelAttrOff, symId, index, ?, n5, n6, n7, n8, off9, off10, off11, off12, rec5]
        for (std::uint32_t w : {3u, modelA, std::uint32_t(kAmpA), 0u, 0u, 0u, 1u, 1u, 0u, 0u, states, knobs, 0u, 17u})
            m.le32(w);
        for (std::uint32_t w : {2u, modelB, std::uint32_t(kAmpB), 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 18u})
            m.le32(w);

        m.putLE32(0, 0x4023245E);
        m.putLE32(12, std::uint32_t(symOff));
        m.putLE32(24, std::uint32_t(entOff));
        m.putLE32(28, 2);
        return m.b;
    }
};

} // namespace test

#pragma once

// A synthetic .hxf for the tests: an MCU app record and a plain resource record, every check field filled in.

#include "builder.h"
#include "core/checksums.h"

#include <miniz.h>

#include <string_view>
#include <vector>

namespace hxtest {

inline constexpr std::uint32_t kAppMagic = 0x230B0C5E;

// A minimal MCU app payload (native LE): header, then one segment at 0x28000000.
inline std::vector<std::uint8_t> makeApp() {
    test::Buf p;
    p.le32(kAppMagic);
    p.le32(0);           // total size, patched below
    p.le32(0);           // CRC32 of [0x34:], patched below
    p.le32(2);
    p.le32(4);
    p.le32(0x100000e5);  // entry
    p.padTo(0x34);
    const std::size_t seg = p.size();
    p.le32(8);           // len
    p.le32(0x28000000);  // load address
    p.le32(0);           // next, patched below
    for (std::uint32_t w : {0x11223344u, 0x55667788u}) p.le32(w);
    p.putLE32(seg + 8, std::uint32_t(p.size()));
    p.putLE32(4, std::uint32_t(p.size()));
    p.putLE32(8, hx::crc32(hx::Bytes(p.b).subspan(0x34)));
    return p.b;
}

// One L6IA record around a native payload, with every check field filled in.
inline void appendRecord(test::Buf& img, std::uint32_t index, std::uint32_t type, const std::vector<std::uint8_t>& native) {
    test::Buf h;
    h.be32(index);
    h.be32(0x03800000);
    h.be32(type);
    h.be32(4);
    h.be32(std::uint32_t((native.size() + 0x200) / 4));
    h.zeros(8);          // hash8, patched below
    h.zeros(8);
    h.be32(0x56112603);  // sector magic
    h.be32(0x03800000);
    h.be32(hx::crc32(native));
    h.be32(std::uint32_t(native.size()));
    h.padTo(0x224);

    std::vector<std::uint8_t> sector(h.b.begin() + 0x24, h.b.end());
    hx::swapWordsInPlace(sector);
    hx::Fletcher f;
    f.add(sector);
    f.add(native);
    h.putBE32(0x14, f.sum1);
    h.putBE32(0x18, f.sum2);

    img.b.insert(img.b.end(), h.b.begin(), h.b.end());
    const std::vector<std::uint8_t> be = hx::swapWords(native);
    img.b.insert(img.b.end(), be.begin(), be.end());
}

inline std::vector<std::uint8_t> makeHxf(const std::vector<std::uint8_t>& image) {
    std::vector<std::uint8_t> z(mz_compressBound(mz_ulong(image.size())));
    mz_ulong zlen = mz_ulong(z.size());
    mz_compress2(z.data(), &zlen, image.data(), mz_ulong(image.size()), 9);
    z.resize(zlen);

    test::Buf head;
    head.be32(0x00010000);
    head.be32(0x02010100);
    head.be32(0x00210004);
    head.be32(1);
    head.be32(std::uint32_t(image.size()));
    const auto digest = hx::md5(image);
    head.b.insert(head.b.end(), digest.begin(), digest.end());
    head.zeros(20);

    test::Buf f;
    f.bytes("FORM");
    f.be32(0);
    f.bytes("L6FF");
    auto chunk = [&](std::string_view id, const std::vector<std::uint8_t>& body) {
        f.bytes(id);
        f.be32(std::uint32_t(body.size()));
        f.b.insert(f.b.end(), body.begin(), body.end());
    };
    chunk("HEAD", head.b);
    chunk("data", z);
    f.putBE32(4, std::uint32_t(f.size() - 8));
    return f.b;
}

inline test::Buf makeImage(const std::vector<std::uint8_t>& app, const std::vector<std::uint8_t>& res) {
    test::Buf img;
    img.bytes("L6IA");
    for (std::uint32_t w : {2u, 1u, 1u, 0x40u, 0x10u}) img.be32(w);
    appendRecord(img, 0, 2, app);
    appendRecord(img, 1, 6, res);
    return img;
}

} // namespace hxtest

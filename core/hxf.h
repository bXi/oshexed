#pragma once

#include "core/bytes.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hx {

// One L6IA record (NOTES.md "Inner: L6IA image"). The spans point into HxfImage::image.
struct HxfRecord {
    std::size_t offset = 0;     // of the record header inside the L6IA image
    std::uint32_t index = 0;    // hdr +0x00
    std::uint32_t type = 0;     // hdr +0x08: 2 = app, 3 = app copy, 6 = resource
    std::uint32_t words = 0;    // hdr +0x10: (size + 0x200) / 4
    std::array<std::uint8_t, 8> hash8{};  // hdr +0x14
    std::uint32_t crc = 0;      // hdr +0x2c: CRC32 of the native payload
    std::uint32_t size = 0;     // hdr +0x30
    Bytes header;               // 0x224 bytes, big-endian as stored
    Bytes payload;              // device-native (LE) form
};

struct HxfChunk {
    std::string id;
    std::size_t offset = 0;     // of the chunk header in the file
    std::size_t length = 0;
    std::vector<std::uint8_t> body;  // kept for HEAD and dinf only (data is inflated instead)
};

// A loaded .hxf: the outer chunks and the inflated L6IA image with every payload already
// swapped to native byte order.
struct HxfImage {
    std::size_t fileSize = 0;
    std::uint32_t formSize = 0;
    std::vector<HxfChunk> chunks;
    std::uint32_t productId = 0;    // HEAD +0x08 (0x00210004 = LT)
    bool md5Ok = false;             // HEAD +0x14 vs MD5 of the inflated image
    std::array<std::uint32_t, 6> l6iaHeader{};
    std::vector<std::uint8_t> image;
    std::array<std::uint8_t, 32> imageSha256{};  // of the inflated image as in the file (before the swap below)
    std::vector<HxfRecord> records;

    const HxfRecord* record(std::uint32_t index) const;
};

enum class InnerKind { None, App, Ui };

// The checks hxf.py `info` runs on each record.
struct RecordChecks {
    bool crc = false;
    bool words = false;
    bool hash8 = false;
    InnerKind inner = InnerKind::None;
    bool innerOk = false;
    bool all() const { return crc && words && hash8 && (inner == InnerKind::None || innerOk); }
};

RecordChecks checkRecord(const HxfRecord& r);

// Our working names for the records, as in tools/hxf.py.
const char* recordName(std::uint32_t index);

// Returned in a unique_ptr because the records point into `image`.
std::unique_ptr<HxfImage> loadHxf(Bytes file, std::string* error = nullptr);
std::unique_ptr<HxfImage> loadHxfFile(const std::string& path, std::string* error = nullptr);

} // namespace hx

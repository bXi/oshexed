#include "core/hxf.h"

#include "core/checksums.h"
#include "core/iff.h"
#include "core/text.h"

#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace hx {

namespace {

constexpr std::size_t kRecHdr = 0x224;
constexpr std::size_t kSectorOff = 0x24;  // the slot sector starts here inside the record header
constexpr std::size_t kL6iaHdr = 0x18;
constexpr std::size_t kHeadMd5 = 0x14;
constexpr std::uint32_t kAppMagic = 0x230B0C5E;
constexpr std::uint32_t kUiMagic = 0x31323334;

std::unique_ptr<HxfImage> fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return nullptr;
}

} // namespace

const HxfRecord* HxfImage::record(std::uint32_t index) const {
    for (const HxfRecord& r : records)
        if (r.index == index) return &r;
    return nullptr;
}

const char* recordName(std::uint32_t index) {
    static const char* const names[] = {"m4_app",   "m4_app_copy", "unknown_2",      "ui_mcu",
                                        "dsp_meta", "dsp_code",    "dsp_meta_audio", "dsp_code_small",
                                        "gfx_meta", "gfx_data",    "msgpack_10",     "model_db",
                                        "model_names", "setlist_names", "presets_db", "cabpkg"};
    return index < std::size(names) ? names[index] : "";
}

RecordChecks checkRecord(const HxfRecord& r) {
    RecordChecks c;
    c.crc = crc32(r.payload) == r.crc;
    c.words = r.words == (std::uint64_t(r.size) + 0x200) / 4;

    // hash8 covers the slot sector in native order, then the payload.
    std::vector<std::uint8_t> sector(r.header.begin() + kSectorOff, r.header.begin() + kRecHdr);
    swapWordsInPlace(sector);
    Fletcher f;
    f.add(sector);
    f.add(r.payload);
    std::array<std::uint8_t, 8> want{};
    for (int k = 0; k < 4; ++k) {
        want[k] = std::uint8_t(f.sum1 >> (24 - 8 * k));
        want[4 + k] = std::uint8_t(f.sum2 >> (24 - 8 * k));
    }
    c.hash8 = want == r.hash8;

    const Bytes p = r.payload;
    if (p.size() >= 4) {
        const std::uint32_t magic = readLE32(p, 0);
        if (magic == kAppMagic && p.size() >= 0x34) {
            c.inner = InnerKind::App;
            c.innerOk = readLE32(p, 8) == crc32(p.subspan(0x34));
        } else if (magic == kUiMagic && p.size() >= 16) {
            c.inner = InnerKind::Ui;
            std::uint32_t sum = 0;
            for (std::size_t i = 16; i + 4 <= p.size(); i += 4) sum += readLE32(p, i);
            c.innerOk = readLE32(p, 12) == sum;
        }
    }
    return c;
}

std::unique_ptr<HxfImage> loadHxf(Bytes file, std::string* error) {
    std::string iffError;
    const std::optional<IffForm> form = parseIff(file, &iffError);
    if (!form) return fail(error, iffError);
    if (form->type != "L6FF") return fail(error, "not an L6FF file (FORM type " + form->type + ")");

    auto img = std::make_unique<HxfImage>();
    img->fileSize = file.size();
    img->formSize = readBE32(file, 4);

    Bytes head, data;
    bool haveData = false;
    for (const IffChunk& c : form->chunks) {
        HxfChunk out{c.id, c.offset - 8, c.body.size(), {}};
        if (c.id == "data") {
            data = c.body;
            haveData = true;
        } else {
            out.body.assign(c.body.begin(), c.body.end());
            if (c.id == "HEAD") head = c.body;
        }
        img->chunks.push_back(std::move(out));
    }
    if (head.size() < kHeadMd5 + 16) return fail(error, "HEAD chunk missing or too short");
    if (!haveData) return fail(error, "no data chunk");
    img->productId = readBE32(head, 8);
    const std::uint32_t inflated = readBE32(head, 0x10);

    img->image.resize(inflated);
    mz_ulong outLen = inflated;
    const int rc = mz_uncompress(img->image.data(), &outLen, data.data(), mz_ulong(data.size()));
    if (rc != MZ_OK) return fail(error, std::string("data chunk does not inflate: ") + mz_error(rc));
    if (outLen != inflated) return fail(error, "inflated size differs from HEAD");

    const std::array<std::uint8_t, 16> digest = md5(img->image);
    img->md5Ok = std::equal(digest.begin(), digest.end(), head.begin() + kHeadMd5);
    // Identifies stock builds (buildtag.cpp); taken here because the payloads are byte-swapped in place below.
    img->imageSha256 = sha256(img->image);

    const Bytes image = img->image;
    if (image.size() < kL6iaHdr || std::memcmp(image.data(), "L6IA", 4) != 0)
        return fail(error, "payload is not an L6IA image");
    for (int k = 0; k < 6; ++k) img->l6iaHeader[k] = readBE32(image, std::size_t(k) * 4);

    std::size_t p = kL6iaHdr;
    while (p < image.size()) {
        if (image.size() - p < kRecHdr) return fail(error, "record header truncated at offset " + std::to_string(p));
        HxfRecord r;
        r.offset = p;
        r.header = image.subspan(p, kRecHdr);
        r.index = readBE32(r.header, 0x00);
        r.type = readBE32(r.header, 0x08);
        r.words = readBE32(r.header, 0x10);
        std::copy_n(r.header.begin() + 0x14, 8, r.hash8.begin());
        r.crc = readBE32(r.header, 0x2c);
        r.size = readBE32(r.header, 0x30);
        if (r.size > image.size() - p - kRecHdr)
            return fail(error, "record " + std::to_string(r.index) + " truncated");
        r.payload = image.subspan(p + kRecHdr, r.size);
        img->records.push_back(r);
        p += kRecHdr + r.size;
    }

    // Payloads to native order, in place. The headers stay big-endian.
    for (const HxfRecord& r : img->records)
        swapWordsInPlace(std::span<std::uint8_t>(img->image.data() + r.offset + kRecHdr, r.size));
    return img;
}

std::unique_ptr<HxfImage> loadHxfFile(const std::string& path, std::string* error) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) return fail(error, "cannot open " + path);
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return fail(error, "cannot read " + path);
    in.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(buf.data()), size)) return fail(error, "cannot read " + path);
    return loadHxf(buf, error);
}

} // namespace hx

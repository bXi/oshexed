#include "core/hxfwrite.h"

#include "core/checksums.h"
#include "core/deflate.h"
#include "core/dspmeta.h"
#include "core/text.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace hx {

namespace {

constexpr std::size_t kRecHdr = 0x224;
constexpr std::size_t kSectorOff = 0x24;
constexpr std::size_t kHeadMd5 = 0x14;
constexpr std::uint32_t kAppMagic = 0x230B0C5E;
constexpr std::uint32_t kUiMagic = 0x31323334;

void putLE32(std::uint8_t* p, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) p[k] = std::uint8_t(v >> (8 * k));
}

void putBE32(std::uint8_t* p, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) p[k] = std::uint8_t(v >> (24 - 8 * k));
}

void appendBE32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.resize(out.size() + 4);
    putBE32(out.data() + out.size() - 4, v);
}

// fwedit.safe: letters, digits and "-_." kept, the rest "_", outer "_" stripped.
std::string safeName(const std::string& s) {
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') ? c : '_';
    const std::size_t a = out.find_first_not_of('_');
    if (a == std::string::npos) return "unnamed";
    return out.substr(a, out.find_last_not_of('_') - a + 1);
}

std::optional<FirmwareParts> failParts(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return std::nullopt;
}

std::vector<std::uint8_t> toVec(Bytes b) { return {b.begin(), b.end()}; }

} // namespace

std::optional<DspModules> splitDspModules(Bytes rec5, std::string* error) {
    try {
        DspModules m;
        need(rec5, 0, 16, "rec 5 header");
        std::copy_n(rec5.begin(), 16, m.header.begin());
        const std::uint32_t n = readLE32(rec5, 8), toff = readLE32(rec5, 12);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::size_t at = (std::size_t(toff) + std::size_t(i) * 2) * 4;
            const std::uint32_t size = le32(rec5, at, "rec 5 table"), off = le32(rec5, at + 4, "rec 5 table");
            m.modules.push_back(toVec(slice(rec5, std::size_t(off) * 4, std::size_t(size) * 4, "rec 5 module")));
        }
        return m;
    } catch (const FormatError& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::vector<std::uint8_t> joinDspModules(const DspModules& m) {
    for (std::size_t i = 0; i < m.modules.size(); ++i)
        if (m.modules[i].size() % 4)
            throw FormatError("DSP module " + std::to_string(i) + ": size must be a multiple of 4 bytes");
    const std::uint32_t n = std::uint32_t(m.modules.size());
    const std::uint32_t dataStart = 6 + n * 2;
    std::vector<std::uint8_t> out(m.header.begin(), m.header.end());
    out.resize(24);
    putLE32(out.data() + 8, n);  // modules may have been added
    putLE32(out.data() + 16, dataStart);
    std::uint32_t off = dataStart;
    for (const auto& b : m.modules) {
        out.resize(out.size() + 8);
        putLE32(out.data() + out.size() - 8, std::uint32_t(b.size() / 4));
        putLE32(out.data() + out.size() - 4, off);
        off += std::uint32_t(b.size() / 4);
    }
    putLE32(out.data() + 20, off);
    for (const auto& b : m.modules) out.insert(out.end(), b.begin(), b.end());
    return out;
}

std::optional<FirmwareParts> explodeParts(const HxfImage& img, std::string* error) {
    const HxfRecord *r0 = img.record(0), *r1 = img.record(1), *r2 = img.record(2), *r4 = img.record(4),
                    *r5 = img.record(5);
    if (!r0 || !r1 || !r2 || !r4 || !r5) return failParts(error, "firmware lacks one of records 0, 1, 2, 4, 5");
    FirmwareParts p;
    p.app = toVec(r0->payload);
    p.appShared = std::equal(r0->payload.begin(), r0->payload.end(), r1->payload.begin(), r1->payload.end());
    if (!p.appShared) p.appCopy = toVec(r1->payload);
    p.bootLdr = toVec(r2->payload);
    p.moduleMeta = toVec(r4->payload);
    std::string why;
    std::optional<DspModules> mods = splitDspModules(r5->payload, &why);
    if (!mods) return failParts(error, "rec 5: " + why);
    p.modules = std::move(*mods);

    // Each module named after the first rec 4 entry that uses it, as fwedit does; "res" when none does.
    std::map<std::uint32_t, std::string> names;
    if (const std::optional<DspMeta> meta = DspMeta::parse(r4->payload, &why))
        for (const MetaEntry& e : meta->entries()) names.emplace(e.rec5, meta->symbol(e.symId));
    else
        return failParts(error, "rec 4: " + why);
    for (std::size_t i = 0; i < p.modules.modules.size(); ++i) {
        const auto it = names.find(std::uint32_t(i));
        char num[16];
        std::snprintf(num, sizeof num, "%03zu", i);
        p.moduleFiles.push_back(std::string("dsp/modules/") + num + "_" + safeName(it == names.end() ? "res" : it->second) +
                                ".bin");
    }
    return p;
}

RecordMap assembleParts(const FirmwareParts& p) {
    RecordMap out;
    out[0] = p.app;
    out[1] = p.appShared ? p.app : p.appCopy;
    out[2] = p.bootLdr;
    out[4] = p.moduleMeta;
    out[5] = joinDspModules(p.modules);
    return out;
}

void fixInner(std::vector<std::uint8_t>& b) {
    if (b.size() < 4) return;
    const std::uint32_t magic = readLE32(b, 0);
    if (magic == kAppMagic && b.size() >= 0x34) {
        putLE32(b.data() + 4, std::uint32_t(b.size()));
        putLE32(b.data() + 8, crc32(Bytes(b).subspan(0x34)));
    } else if (magic == kUiMagic && b.size() >= 16) {
        const std::size_t words = (b.size() - 16) / 4;
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < words; ++i) sum += readLE32(b, 16 + i * 4);
        putLE32(b.data() + 8, std::uint32_t(words));
        putLE32(b.data() + 12, sum);
    }
}

PackedHxf packHxf(const HxfImage& base, const RecordMap& replaced, const std::function<void(double)>& progress) {
    PackedHxf res;
    std::vector<std::uint8_t> img;
    for (std::uint32_t w : base.l6iaHeader) appendBE32(img, w);
    for (const HxfRecord& r : base.records) {
        const auto it = replaced.find(r.index);
        std::vector<std::uint8_t> native = it != replaced.end() ? it->second : toVec(r.payload);
        native.resize((native.size() + 3) / 4 * 4, 0);
        fixInner(native);
        std::vector<std::uint8_t> hdr = toVec(r.header);
        const std::uint32_t crc = crc32(native);
        if (crc != r.crc) res.changed.push_back(r.index);
        putBE32(hdr.data() + 0x10, std::uint32_t((native.size() + 0x200) / 4));
        putBE32(hdr.data() + 0x2c, crc);
        putBE32(hdr.data() + 0x30, std::uint32_t(native.size()));
        // hash8: Fletcher over the slot sector in native order, then the payload.
        std::vector<std::uint8_t> sector(hdr.begin() + kSectorOff, hdr.end());
        swapWordsInPlace(sector);
        Fletcher f;
        f.add(sector);
        f.add(native);
        putBE32(hdr.data() + 0x14, f.sum1);
        putBE32(hdr.data() + 0x18, f.sum2);
        img.insert(img.end(), hdr.begin(), hdr.end());
        const std::size_t at = img.size();
        img.insert(img.end(), native.begin(), native.end());
        swapWordsInPlace(std::span<std::uint8_t>(img.data() + at, native.size()));
    }
    res.imageSize = img.size();

    std::vector<std::uint8_t>& out = res.file;
    out = {'F', 'O', 'R', 'M', 0, 0, 0, 0, 'L', '6', 'F', 'F'};
    for (const HxfChunk& c : base.chunks) {
        std::vector<std::uint8_t> body;
        if (c.id == "data") {
            body = zlibCompress(img, 9, progress);
        } else {
            body = c.body;
            if (c.id == "HEAD" && body.size() >= kHeadMd5 + 16) {
                putBE32(body.data() + 0x10, std::uint32_t(img.size()));
                const std::array<std::uint8_t, 16> d = md5(img);
                std::copy(d.begin(), d.end(), body.begin() + kHeadMd5);
            }
            if (c.id == "dinf" && body.size() >= 0x10) putBE32(body.data() + 0x0c, std::uint32_t(img.size()));
        }
        out.insert(out.end(), c.id.begin(), c.id.end());
        appendBE32(out, std::uint32_t(body.size()));
        out.insert(out.end(), body.begin(), body.end());  // Line 6 writes no IFF pad byte
    }
    putBE32(out.data() + 4, std::uint32_t(out.size() - 8));
    return res;
}

bool writeFile(const std::string& path, Bytes data, std::string* error) {
    const std::filesystem::path target = pathFromUtf8(path);
    std::filesystem::path tmp = target;
    tmp += ".part";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
            if (error) *error = "cannot write " + path;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

} // namespace hx

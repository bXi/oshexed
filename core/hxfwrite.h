#pragma once

// Writing .hxf files: the C++ side of tools/hxf.py `pack` and the parts of tools/fwedit.py `explode` / `build` the
// mod builder edits. Unchanged records come back byte for byte, so stock -> parts -> pack gives the stock file.

#include "core/bytes.h"
#include "core/hxf.h"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hx {

// Native (little-endian) payloads by record index.
using RecordMap = std::map<std::uint32_t, std::vector<std::uint8_t>>;

// rec 5: a header, a table of (size, offset) in words, then the DSP modules back to back (fwedit ex/bd_dsp_modules).
struct DspModules {
    std::array<std::uint8_t, 16> header{};  // words 0..3 (word 2 = count, rewritten on join)
    std::vector<std::vector<std::uint8_t>> modules;
};
std::optional<DspModules> splitDspModules(Bytes rec5, std::string* error = nullptr);
// Every module must be a whole number of words (throws FormatError).
std::vector<std::uint8_t> joinDspModules(const DspModules& m);

// The records the mod builder changes, in fwedit's folder layout (main_app/, dsp/).
struct FirmwareParts {
    std::vector<std::uint8_t> app;         // rec 0: main_app/m4_app.bin
    bool appShared = true;                 // rec 1 is a copy of rec 0 (always so in stock files)
    std::vector<std::uint8_t> appCopy;     // rec 1 when not shared: main_app/m4_app_copy.bin
    std::vector<std::uint8_t> bootLdr;     // rec 2: dsp/boot_ldr.bin
    std::vector<std::uint8_t> moduleMeta;  // rec 4: dsp/module_meta.bin
    DspModules modules;                    // rec 5: dsp/modules/*.bin
    std::vector<std::string> moduleFiles;  // "dsp/modules/NNN_name.bin", named as fwedit names them
};
std::optional<FirmwareParts> explodeParts(const HxfImage& img, std::string* error = nullptr);
// Records 0, 1, 2, 4 and 5 from the parts.
RecordMap assembleParts(const FirmwareParts& p);

// hxf.py fix_inner: the app's length and CRC32 (magic 0x230b0c5e), the UI chip's word count and sum ("4321").
void fixInner(std::vector<std::uint8_t>& native);

struct PackedHxf {
    std::vector<std::uint8_t> file;
    std::size_t imageSize = 0;           // inflated L6IA image
    std::vector<std::uint32_t> changed;  // records whose CRC32 differs from the base file's
};
// hxf.py pack: the base file's records, with `replaced` swapped in (native payloads, padded to words), every
// record header, inner checksum, HEAD (size, MD5) and dinf size recomputed, the image deflated at zlib level 9.
// progress (optional): the deflate's share done (0..1); it's most of the time a pack takes.
PackedHxf packHxf(const HxfImage& base, const RecordMap& replaced = {},
                  const std::function<void(double)>& progress = {});

bool writeFile(const std::string& path, Bytes data, std::string* error = nullptr);

} // namespace hx

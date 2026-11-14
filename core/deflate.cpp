// Kept apart from the rest of core: with Z_PREFIX, zlib.h #defines names like crc32 and compress, which would
// rename our own functions in any file that includes it.
#include "core/deflate.h"

#include <zlib.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace hx {

// compress2's own loop, in 1 MB pieces so the caller can show progress: Z_NO_FLUSH until the last piece, then
// Z_FINISH. Without flushes, deflate's output doesn't depend on how the input is split.
std::vector<std::uint8_t> zlibCompress(Bytes data, int level, const std::function<void(double)>& progress) {
    if (data.size() > 0xFFFFFFFFu) throw std::runtime_error("zlibCompress: input too big");
    std::vector<std::uint8_t> out(compressBound(uLong(data.size())));
    z_stream s{};
    if (const int rc = deflateInit(&s, level); rc != Z_OK)
        throw std::runtime_error("zlib deflateInit failed: " + std::to_string(rc));
    s.next_out = out.data();
    s.avail_out = uInt(out.size());
    constexpr std::size_t kPiece = 1u << 20;
    std::size_t at = 0;
    int rc = Z_OK;
    do {
        const std::size_t n = std::min(kPiece, data.size() - at);
        s.next_in = const_cast<Bytef*>(data.data() + at);
        s.avail_in = uInt(n);
        at += n;
        const int flush = at == data.size() ? Z_FINISH : Z_NO_FLUSH;
        rc = deflate(&s, flush);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            deflateEnd(&s);
            throw std::runtime_error("zlib deflate failed: " + std::to_string(rc));
        }
        if (progress && !data.empty()) progress(double(at) / double(data.size()));
    } while (rc != Z_STREAM_END);
    out.resize(s.total_out);
    deflateEnd(&s);
    return out;
}

} // namespace hx

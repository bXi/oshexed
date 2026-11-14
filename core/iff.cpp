#include "core/iff.h"

namespace hx {

namespace {

std::string fourcc(Bytes b, std::size_t off) { return std::string(reinterpret_cast<const char*>(&b[off]), 4); }

std::optional<IffForm> fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return std::nullopt;
}

} // namespace

std::optional<IffForm> parseIff(Bytes data, std::string* error) {
    if (data.size() < 12) return fail(error, "file too short for a FORM header");
    if (fourcc(data, 0) != "FORM") return fail(error, "no FORM header");
    const std::uint64_t formLen = readBE32(data, 4);
    if (formLen < 4 || 8 + formLen > data.size()) return fail(error, "FORM length runs past the end of the file");

    IffForm form;
    form.type = fourcc(data, 8);
    const std::size_t end = std::size_t(8 + formLen);
    std::size_t pos = 12;
    while (pos < end) {
        if (end - pos < 8) return fail(error, "truncated chunk header at offset " + std::to_string(pos));
        const std::string id = fourcc(data, pos);
        const std::uint64_t len = readBE32(data, pos + 4);
        if (len > end - pos - 8) return fail(error, "chunk " + id + " runs past the end of the FORM");
        form.chunks.push_back({id, pos + 8, data.subspan(pos + 8, std::size_t(len))});
        pos += 8 + std::size_t(len);
    }
    return form;
}

} // namespace hx

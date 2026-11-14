#include "test.h"

#include "core/iff.h"

#include <string_view>

namespace {

// Synthetic data only: no firmware bytes in the repo.
struct Builder {
    std::vector<std::uint8_t> out;
    void be32(std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) out.push_back(std::uint8_t(v >> s));
    }
    void id(std::string_view s) { out.insert(out.end(), s.begin(), s.end()); }
    void chunk(std::string_view name, std::vector<std::uint8_t> body) {
        id(name);
        be32(std::uint32_t(body.size()));
        out.insert(out.end(), body.begin(), body.end());
    }
    // Wraps everything so far in FORM <type>.
    std::vector<std::uint8_t> form(std::string_view type) {
        Builder f;
        f.id("FORM");
        f.be32(std::uint32_t(4 + out.size()));
        f.id(type);
        f.out.insert(f.out.end(), out.begin(), out.end());
        return f.out;
    }
};

} // namespace

TEST("iff: splits chunks, no pad after odd lengths") {
    Builder b;
    b.chunk("HEAD", {1, 2, 3});  // odd length: next chunk starts right after it
    b.chunk("dinf", {});
    b.chunk("data", {9, 8});
    const auto file = b.form("L6FF");

    std::string error;
    const auto form = hx::parseIff(file, &error);
    REQUIRE(form.has_value());
    CHECK_EQ(form->type, std::string("L6FF"));
    REQUIRE(form->chunks.size() == 3);
    CHECK_EQ(form->chunks[0].id, std::string("HEAD"));
    CHECK_EQ(form->chunks[0].offset, std::size_t(20));
    CHECK_EQ(form->chunks[0].body.size(), std::size_t(3));
    CHECK_EQ(form->chunks[1].id, std::string("dinf"));
    CHECK_EQ(form->chunks[1].body.size(), std::size_t(0));
    CHECK_EQ(form->chunks[2].id, std::string("data"));
    CHECK_EQ(int(form->chunks[2].body[1]), 8);
}

TEST("iff: rejects bad input") {
    std::string error;
    const std::uint8_t tiny[] = {'F', 'O', 'R', 'M'};
    CHECK(!hx::parseIff(tiny, &error));

    Builder notForm;
    notForm.id("LIST");
    notForm.be32(4);
    notForm.id("L6FF");
    CHECK(!hx::parseIff(notForm.out, &error));

    Builder b;
    b.chunk("HEAD", {1, 2, 3, 4});
    auto file = b.form("L6FF");
    file[19] = 0x40;  // HEAD claims 64 bytes
    CHECK(!hx::parseIff(file, &error));
    CHECK(!error.empty());

    auto shortForm = b.form("L6FF");
    shortForm[7] += 1;  // FORM length one past the end of the file
    CHECK(!hx::parseIff(shortForm, &error));
}

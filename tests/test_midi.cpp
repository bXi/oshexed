#include "test.h"

#include "core/midi.h"

namespace {

hx::midi::Message cc(int channel, int number, int value, double t = 0) {
    hx::midi::Message m;
    m.status = std::uint8_t(0xB0 | (channel - 1));
    m.data1 = std::uint8_t(number);
    m.data2 = std::uint8_t(value);
    m.time = t;
    return m;
}

} // namespace

TEST("midi: debug words from CC 40-55 on channel 16") {
    hx::midi::DebugWords w;
    // 454 = 6 + 32 * 14 (TESTS.md: W1 SDRAM tloop 454 cycles); word 1 = 6174 = 30 + 32 * 0 + 1024 * 6
    const int word0[4] = {454 & 31, (454 >> 5) & 31, (454 >> 10) & 31, (454 >> 15) & 31};
    for (int c = 0; c < 4; ++c) CHECK(w.feed(cc(16, 40 + c, word0[c], 1.5)));
    CHECK(w.seen(0));
    CHECK(w.value(0) == 454);
    CHECK(w.updated(0) == 1.5);
    CHECK(!w.seen(1));
    for (int c = 0; c < 3; ++c) w.feed(cc(16, 44 + c, (6174 >> (5 * c)) & 31));
    CHECK(!w.seen(1));  // one chunk still missing
    w.feed(cc(16, 47, 0));
    CHECK(w.seen(1) && w.value(1) == 6174);
    CHECK(!w.feed(cc(15, 40, 3)));  // other channel
    CHECK(!w.feed(cc(16, 56, 3)));  // past the last chunk
    CHECK(!w.feed(cc(16, 39, 3)));
    w.clear();
    CHECK(!w.seen(0));
}

TEST("midi: message text") {
    CHECK(hx::midi::describe(cc(16, 40, 12)) == "ch 16  CC 40 = 12");
    hx::midi::Message n;
    n.status = 0x90;
    n.data1 = 60;
    n.data2 = 100;
    CHECK(hx::midi::describe(n) == "ch 1  Note on C4 (60) vel 100");
    n.data2 = 0;
    CHECK(hx::midi::describe(n) == "ch 1  Note off C4 (60) vel 0");
    hx::midi::Message clock;
    clock.status = 0xF8;
    CHECK(hx::midi::describe(clock) == "Clock" && hx::midi::isRealtime(clock));
    hx::midi::Message pc;
    pc.status = 0xC2;
    pc.data1 = 5;
    CHECK(hx::midi::describe(pc) == "ch 3  Program 5");
}

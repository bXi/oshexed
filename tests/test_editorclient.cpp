#include "test.h"

#include "core/device/editorclient.h"

#include <deque>
#include <map>

namespace {

// A unit that speaks the editor protocol on the three channels, with canned answers.
class FakeEditorUnit : public hx::LinkIo {
public:
    void write(hx::Bytes message) override {
        const auto m = hx::decodeMessage(message);
        if (!m) return;
        Chan &c = chans_[m->dst];
        c.pc = m->src;
        if (m->type == hx::kTypeConnect) {
            const std::uint8_t window[] = {0x00, 0x02, 0x00, 0x00};
            out_.push_back(hx::encodeMessage(hx::kFlagConnect, 0, 0x01000100, window, hx::kTypeConnect, m->dst, m->src));
            c.seq = 2;
            return;
        }
        if (!(m->flags & hx::kFlagData)) return;
        c.rx += m->payload.size();
        c.frames.feed(m->payload);
        while (auto body = c.frames.next()) answer(m->dst, c, *body);
    }
    std::optional<std::vector<std::uint8_t>> read(double) override {
        if (out_.empty()) return std::nullopt;
        auto m = std::move(out_.front());
        out_.pop_front();
        return m;
    }
    std::vector<int> commands;  // what the client asked, in order

private:
    struct Chan {
        std::uint16_t pc = 0;
        std::uint8_t seq = 2;
        std::uint64_t rx = 0;
        bool selected = false;
        hx::FrameReader frames;
    };

    void send(std::uint16_t unit, Chan &c, const std::vector<std::uint8_t> &frame) {
        out_.push_back(hx::encodeMessage(hx::kFlagData, c.seq, std::uint32_t(0x200 + c.rx), frame, hx::kTypeNormal, unit, c.pc));
        c.seq = std::uint8_t(c.seq + 1);
    }

    void answer(std::uint16_t unit, Chan &c, const std::vector<std::uint8_t> &body) {
        const std::uint16_t port = unit == 0x03ef ? 2 : unit == 0x03ed ? 6 : 4;
        if (!c.selected) {  // the service request: echo the port
            c.selected = true;
            send(unit, c, hx::encodeFrame(port, body, 0));
            return;
        }
        const hx::mp::Value req = hx::mp::parse(body);
        const int id = int(req.get(102)->asInt()), cmd = int(req.get(100)->asInt());
        commands.push_back(cmd);
        hx::mp::Value result;
        if (cmd == hx::editor::kSetlistNames)
            result = hx::mp::array({hx::mp::map({{0, hx::mp::str("FACTORY 1")}}), hx::mp::map({{1, hx::mp::str("USER 1")}})});
        if (cmd == hx::editor::kCurrentInfo)
            result = hx::mp::map({{107, hx::mp::integer(4)}, {108, hx::mp::integer(2)}, {109, hx::mp::str("Test")}});
        const hx::mp::Value reply = hx::mp::map({{102, hx::mp::integer(id)}, {103, hx::mp::integer(0)}, {104, result}});
        send(unit, c, hx::encodeFrame(port, hx::mp::encode(reply), 0));
        if (cmd == hx::editor::kBypass) {  // the unit also tells the notification channel
            Chan &n = chans_[0x03f0];
            const hx::mp::Value inner = hx::mp::map({{98, hx::mp::integer(4)}, {59, hx::mp::boolean(false)}});
            const hx::mp::Value note = hx::mp::map(
                {{105, hx::mp::integer(49)}, {106, hx::mp::map({{82, hx::mp::integer(0)}, {106, inner}})}});
            send(0x03f0, n, hx::encodeFrame(4, hx::mp::encode(note), 0));
        }
    }

    std::map<std::uint16_t, Chan> chans_;
    std::deque<std::vector<std::uint8_t>> out_;
};

} // namespace

TEST("editor protocol: a request encodes as HX Edit's") {
    // HX Edit's first request on the editing channel, from the capture: frame header + {102: 1000, 100: 76, 101: {}}.
    const hx::mp::Value req = hx::mp::map({{102, hx::mp::integer(1000)}, {100, hx::mp::integer(76)}, {101, hx::mp::map()}});
    const std::vector<std::uint8_t> frame = hx::encodeFrame(6, hx::mp::encode(req));
    const std::vector<std::uint8_t> want = {0x01, 0x00, 0x06, 0x00, 0x09, 0x00, 0x00, 0x00, 0x83,
                                            0x66, 0xcd, 0x03, 0xe8, 0x64, 0x4c, 0x65, 0x80};
    CHECK(frame == want);

    // Frames split across messages come out whole.
    hx::FrameReader r;
    r.feed(hx::Bytes(frame).first(5));
    CHECK(!r.next().has_value());
    r.feed(hx::Bytes(frame).subspan(5));
    const auto body = r.next();
    REQUIRE(body.has_value());
    CHECK_EQ(body->size(), std::size_t(9));
    CHECK(!r.next().has_value());
}

TEST("editor protocol: msgpack writer round trip") {
    const hx::mp::Value v = hx::mp::map({{1, hx::mp::integer(-200)},
                                         {2, hx::mp::real(0.5)},
                                         {300, hx::mp::str("Name")},
                                         {4, hx::mp::array({hx::mp::boolean(true), hx::mp::nil(), hx::mp::integer(70000)})}});
    const hx::mp::Value back = hx::mp::parse(hx::mp::encode(v));
    CHECK_EQ(back.get(1)->asInt(), std::int64_t(-200));
    CHECK_EQ(back.get(2)->asNumber(), 0.5);
    CHECK_EQ(back.get(300)->asText(), std::string("Name"));
    CHECK(back.get(4)->at(0)->asBool());
    CHECK(back.get(4)->at(1)->isNil());
    CHECK_EQ(back.get(4)->at(2)->asInt(), std::int64_t(70000));
}

TEST("editor protocol: open, requests, notifications") {
    FakeEditorUnit unit;
    hx::EditorClient client(unit);
    client.open();

    const auto names = hx::editor::setlistNames(client.call(hx::EditorChannel::Library, hx::editor::kSetlistNames, hx::mp::nil()).result);
    REQUIRE(names.size() == 2);
    CHECK_EQ(names[1], std::string("USER 1"));

    const auto info = hx::editor::currentInfo(client.call(hx::EditorChannel::Edit, hx::editor::kCurrentInfo, hx::mp::nil()).result);
    CHECK_EQ(info.setlist, 4);
    CHECK_EQ(info.preset, 2);
    CHECK_EQ(info.name, std::string("Test"));

    client.call(hx::EditorChannel::Edit, hx::editor::kBypass, hx::editor::bypassArgs(4, false));
    client.poll(0.01);
    const auto notices = client.takeNotices();
    REQUIRE(notices.size() == 1);
    const auto b = hx::editor::bypassChange(notices[0]);
    REQUIRE(b.has_value());
    CHECK_EQ(b->first, 4);
    CHECK(!b->second);
    CHECK_EQ(unit.commands.size(), std::size_t(3));
}

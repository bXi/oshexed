#include "test.h"

#include "core/checksums.h"
#include "core/device/devicelink.h"
#include "core/device/fakeunit.h"
#include "core/device/link.h"
#include "core/device/updater.h"
#include "core/hxf.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <chrono>
#include <thread>

namespace {

// Synthetic L6IA-like image: records with a 0x224-byte big-endian header (slot sector from +0x24) and a
// native payload of made-up words. Only what recordFiles() reads is filled in.
struct TestImage {
    std::vector<std::vector<std::uint8_t>> headers, payloads;
    hx::HxfImage img;

    explicit TestImage(const std::vector<std::size_t> &payloadWords) {
        headers.resize(payloadWords.size());
        payloads.resize(payloadWords.size());
        for (std::size_t i = 0; i < payloadWords.size(); ++i) {
            std::vector<std::uint8_t> &h = headers[i], &p = payloads[i];
            h.assign(0x224, 0);
            for (std::size_t k = 0x24; k < 0x224; ++k) h[k] = std::uint8_t(k * 7 + i);
            p.resize(payloadWords[i] * 4);
            for (std::size_t k = 0; k < p.size(); ++k) p[k] = std::uint8_t(k * 13 + i * 5 + (k >> 9));
            hx::HxfRecord r;
            r.index = std::uint32_t(i);
            r.words = std::uint32_t((0x200 + p.size()) / 4);
            r.size = std::uint32_t(p.size());
            std::vector<std::uint8_t> file = hx::swapWords(hx::Bytes(h).subspan(0x24));
            file.insert(file.end(), p.begin(), p.end());
            const std::array<std::uint8_t, 8> le = hx::fletcherLE(file);
            for (int w = 0; w < 2; ++w)
                for (int k = 0; k < 4; ++k) r.hash8[std::size_t(4 * w + k)] = le[std::size_t(4 * w + 3 - k)];
            r.header = h;
            r.payload = p;
            img.records.push_back(r);
        }
    }
};

struct Sim {
    hx::FakeClock clock;
    hx::FakeUnit unit{clock};
    hx::Link link{unit, clock.clock()};
};

} // namespace

TEST("device: link messages encode, pad and decode") {
    const std::uint8_t payload[] = {1, 2, 3, 4, 5};
    const std::vector<std::uint8_t> m = hx::encodeMessage(hx::kFlagData, 7, 0x1234, payload);
    CHECK_EQ(m.size(), std::size_t(24));  // 16 + 5, padded to 4
    CHECK_EQ(int(m[0]), 13);              // 8 + payload
    CHECK_EQ(int(m[3]), int(hx::kTypeNormal));
    const std::optional<hx::LinkMessage> d = hx::decodeMessage(m);
    CHECK(d.has_value());
    CHECK_EQ(int(d->seq), 7);
    CHECK_EQ(int(d->flags), int(hx::kFlagData));
    CHECK_EQ(d->pos, 0x1234u);
    CHECK_EQ(d->src, hx::kPcId);
    CHECK_EQ(d->payload.size(), std::size_t(5));
    CHECK(!hx::decodeMessage(hx::Bytes(m).first(10)).has_value());
}

TEST("device: updater commands are aa0000op plus LE words") {
    const std::vector<std::uint8_t> c = hx::updaterCommand(10, {1, 0});
    const std::vector<std::uint8_t> want = {0xaa, 0, 0, 10, 1, 0, 0, 0, 0, 0, 0, 0};
    CHECK(c == want);
}

TEST("device: record files are sector + payload, checked against hash8") {
    TestImage t({3, 0x1800});
    const std::vector<hx::RecordFile> files = hx::recordFiles(t.img);
    CHECK_EQ(files.size(), std::size_t(2));
    CHECK_EQ(files[1].data.size(), std::size_t(0x200 + 0x1800 * 4));
    // The sector goes out word-swapped (native), the payload as is.
    CHECK_EQ(int(files[0].data[0]), int(t.headers[0][0x27]));
    CHECK_EQ(int(files[0].data[0x200]), int(t.payloads[0][0]));

    t.img.records[1].hash8[0] ^= 1;
    bool threw = false;
    try {
        hx::recordFiles(t.img);
    } catch (const hx::FormatError &) {
        threw = true;
    }
    CHECK(threw);
}

TEST("device: the write list matches the updater's order") {
    TestImage t({4, 0x1001});  // second record: 0x1081 words = 2 chunks
    std::vector<std::string> names;
    hx::forEachFlashWrite(hx::recordFiles(t.img), [&](const std::string &n, hx::Bytes) { names.push_back(n); });
    const std::vector<std::string> want = {"session", "session", "hello 01", "slots 03", "02", "erase 0", "erase 1",
                                           "file 0",  "06",      "chunk 0.0", "09",      "file 1", "06",
                                           "chunk 1.0", "chunk 1.1", "09", "finish 0a"};
    CHECK(names == want);
}

TEST("device: full flash against the fake unit writes every record") {
    TestImage t({5, 0x2100, 0x40});
    const std::vector<hx::RecordFile> files = hx::recordFiles(t.img);
    Sim s;
    hx::UnitState state;
    hx::Flasher f(s.link, &state);
    std::vector<std::uint32_t> done;
    f.onRecord = [&](std::uint32_t ri, hx::RecordStep st) {
        if (st == hx::RecordStep::Done) done.push_back(ri);
    };
    hx::FlashOptions opt;
    opt.checkSlots = true;
    opt.sourceName = "test.hxf";
    f.flash(files, opt);
    CHECK(s.unit.finished());
    CHECK(f.writing());
    CHECK_EQ(s.unit.records().size(), files.size());
    for (const hx::RecordFile &rf : files) {
        CHECK(s.unit.records().count(rf.index) == 1 && s.unit.records().at(rf.index) == rf.data);
        CHECK(state.same(rf));
    }
    CHECK_EQ(done.size(), std::size_t(3));
}

TEST("device: skipped records stay untouched; keep presets skips the finish") {
    TestImage t({5, 0x900, 0x40});
    const std::vector<hx::RecordFile> files = hx::recordFiles(t.img);
    Sim s;
    hx::Flasher f(s.link);
    hx::FlashOptions opt;
    opt.skip = {0, 2};
    opt.keepPresets = true;
    f.flash(files, opt);
    CHECK(!s.unit.finished());
    CHECK_EQ(s.unit.records().size(), std::size_t(1));
    CHECK(s.unit.records().count(1) == 1);
}

TEST("device: a stop before writing sends nothing to the slots") {
    TestImage t({5});
    Sim s;
    hx::Flasher f(s.link);
    f.stopRequested = [] { return true; };
    bool threw = false;
    try {
        f.flash(hx::recordFiles(t.img), {});
    } catch (const hx::LinkError &) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!f.writing());
    CHECK(s.unit.records().empty());
}

TEST("device: unit state survives a reload and drives the presets choice") {
    TestImage t({1, 2, 3, 4, 5, 6});
    const std::vector<hx::RecordFile> files = hx::recordFiles(t.img);
    const std::string path = (std::filesystem::temp_directory_path() / "hxed_test_unit_state.txt").string();
    std::filesystem::remove(path);
    {
        hx::UnitState s(path);
        CHECK(s.empty());
        CHECK(!hx::presetsDecision(hx::PresetsMode::Auto, files, s).keep);  // unknown
        s.assume(files, "a.hxf");
    }
    hx::UnitState s(path);
    CHECK(s.same(files[4]));
    CHECK_EQ(s.fileName(5), std::string("a.hxf"));
    CHECK(hx::presetsDecision(hx::PresetsMode::Auto, files, s).keep);
    CHECK(!hx::presetsDecision(hx::PresetsMode::Rebuild, files, s).keep);

    std::vector<hx::RecordFile> changed = files;
    changed[5].data[0] ^= 1;
    changed[5].sha256 = hx::toHex(hx::sha256(changed[5].data));
    CHECK(!hx::presetsDecision(hx::PresetsMode::Auto, changed, s).keep);
    s.clear(4);
    CHECK(!s.known(4));
    CHECK(!hx::presetsDecision(hx::PresetsMode::Auto, files, s).keep);
    std::filesystem::remove(path);
}

namespace {

std::vector<hx::DeviceEvent> waitForJob(hx::DeviceLink &d) {
    std::vector<hx::DeviceEvent> all;
    for (int i = 0; i < 20000 && d.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (hx::DeviceEvent &e : d.takeEvents()) all.push_back(std::move(e));
    return all;
}

} // namespace

TEST("device: the device thread runs a simulated flash and reports it") {
    TestImage t({5, 0x1100});
    hx::DeviceLink d("", [](std::string *error) -> std::unique_ptr<hx::LinkIo> {
        *error = "no unit in tests";
        return nullptr;
    });
    CHECK(d.flash(hx::recordFiles(t.img), {}, /*simulate=*/true));
    const std::vector<hx::DeviceEvent> events = waitForJob(d);
    CHECK(!d.busy());
    CHECK(!events.empty() && events.back().kind == hx::DeviceEvent::Kind::Done);
    int done = 0;
    for (const hx::DeviceEvent &e : events)
        done += e.kind == hx::DeviceEvent::Kind::Record && e.step == hx::RecordStep::Done;
    CHECK_EQ(done, 2);
    CHECK(d.unitState().empty());  // a rehearsal doesn't touch the unit state

    CHECK(d.readInfo());
    const std::vector<hx::DeviceEvent> info = waitForJob(d);
    CHECK(info.size() == 1 && info[0].kind == hx::DeviceEvent::Kind::Failed && info[0].text == "no unit in tests");
}

#include "test.h"

#include "core/presets.h"
#include "core/unitprofile.h"

#include <string_view>

namespace {

int inputIndex(std::string_view label) {
    const auto ports = hx::ioInputs();
    for (std::size_t i = 0; i < ports.size(); ++i)
        if (ports[i].label == label) return int(i);
    return -1;
}

int outputIndex(std::string_view label) {
    const auto ports = hx::ioOutputs();
    for (std::size_t i = 0; i < ports.size(); ++i)
        if (ports[i].label == label) return int(i);
    return -1;
}

} // namespace

TEST("unitprofile: the Helix LT has no Send / Return 3/4, no mic, no LCD brightness or scribble strips") {
    using hx::UnitModel;
    const UnitModel lt = UnitModel::HelixLt;
    CHECK(!hx::outputAvailable(lt, outputIndex("Send 3/4")));
    CHECK(hx::outputAvailable(lt, outputIndex("Send 1/2")));
    CHECK(!hx::inputAvailable(lt, inputIndex("Return 3/4")));
    CHECK(!hx::inputAvailable(lt, inputIndex("Return 3")));
    CHECK(!hx::inputAvailable(lt, inputIndex("Mic")));
    CHECK(!hx::inputAvailable(lt, inputIndex("Aux")));
    CHECK(!hx::inputAvailable(lt, inputIndex("S/PDIF")));
    CHECK(hx::inputAvailable(lt, inputIndex("Guitar")));
    CHECK(hx::inputAvailable(lt, inputIndex("Variax Mags")));
    CHECK(hx::inputAvailable(lt, inputIndex("Return 1/2")));
    CHECK(hx::inputAvailable(lt, inputIndex("USB 7/8")));
    CHECK(hx::outputAvailable(lt, outputIndex("Digital")));
    for (int id : {6, 19, 22, 23, 24, 70, 72, 84, 102, 126}) CHECK(!hx::globalSettingAvailable(lt, id));
    for (int id : {25, 26, 66, 71, 68, 69, 75}) CHECK(hx::globalSettingAvailable(lt, id));
}

TEST("unitprofile: an unknown or other model hides nothing; ids and product IDs map") {
    using hx::UnitModel;
    CHECK(hx::outputAvailable(UnitModel::Unknown, outputIndex("Send 3/4")));
    CHECK(hx::outputAvailable(UnitModel::HelixFloor, outputIndex("Send 3/4")));
    CHECK(hx::globalSettingAvailable(UnitModel::HelixFloor, 22));
    CHECK(hx::unitFromProductId(0x00210004) == UnitModel::HelixLt);
    CHECK(hx::unitFromProductId(0x12345678) == UnitModel::Unknown);
    CHECK(hx::unitFromId("helix-lt") == UnitModel::HelixLt);
    CHECK(hx::unitId(UnitModel::HelixLt) == "helix-lt");
    CHECK(hx::unitName(UnitModel::HelixLt) == "Helix LT");
    CHECK(!hx::inputAvailable(UnitModel::HelixLt, -1));
}

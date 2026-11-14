#pragma once

// Which Helix model the editor is working for, and what that model's hardware has. Everything the editor lists
// from its own tables (I/O ports, global settings) is filtered by it, so a Helix LT never offers Send 3/4 or the
// Main LCD brightness. Lists read from the user's firmware (model browser) are already the model's own.
//
// The model comes from (in this order): the connected unit, the open .hxf's product ID, the offline unit picker.

#include <cstdint>
#include <string>
#include <string_view>

namespace hx {

enum class UnitModel { Unknown, HelixFloor, HelixLt, HelixRack, HxStomp, HxStompXl, HxEffects };

// The settings key ("helix-lt", ...) and display name ("Helix LT").
std::string_view unitId(UnitModel m);
std::string_view unitName(UnitModel m);
UnitModel unitFromId(std::string_view id);  // Unknown for anything else
// The .hxf HEAD product ID (0x00210004 = LT); Unknown for the ones not mapped yet.
UnitModel unitFromProductId(std::uint32_t productId);

// Hardware the model has. Unknown models get everything (nothing is hidden on a guess).
bool inputAvailable(UnitModel m, int source);  // index into ioInputs()
bool outputAvailable(UnitModel m, int dest);   // index into ioOutputs()
bool globalSettingAvailable(UnitModel m, int id);

} // namespace hx

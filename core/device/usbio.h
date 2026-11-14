#pragma once

// The unit's message endpoints as a LinkIo.
//
// Windows: Line 6's driver package binds the OS's WinUSB driver to interface 0 of each unit (L6WinUsb.inf,
// `USB\VID_0E41&PID_424A&MI_00` for the LT; the audio driver takes interface 1). So we open that interface
// through the WinUSB API, exactly like the Line 6 Updater does, with no driver changes and no libusb (LGPL).
// One program at a time: while HX Edit or the Updater holds the unit, opening fails with a clear message.
// Linux / macOS: not yet (later: usbfs / IOKit, or libusb as a shared library).

#include "core/device/link.h"

#include <memory>
#include <string>

namespace hx {

constexpr std::uint16_t kLine6Vid = 0x0e41;
constexpr std::uint16_t kHelixLtPid = 0x424a;  // normal and update mode alike

// Opens the first Helix LT found. nullptr with a one-line reason in *error when it can't.
std::unique_ptr<LinkIo> openUsbLink(std::string *error = nullptr);

} // namespace hx

// SPDX-License-Identifier: GPL-3.0-or-later
// What freshVision does when it can't open a capture device: which device to
// start with when none is given, and a help window instead of a bare error
// when nothing can be opened.
#pragma once

#include <functional>
#include <string>

namespace fv {

// The device to open when the command line names none: the last device that
// worked (if it is still a Vision capture node), else the first Vision node
// that has a signal, else the first Vision node; empty if there is none.
std::string chooseStartDevice();
// Remembers `path` as the last device that worked (~/.config/freshvision/last-device).
void rememberDevice(const std::string& path);
// Prints every capture node: path, driver, input name, signal (--list-devices).
void printDevices();

// A window explaining that no capture device could be opened, listing the
// capture nodes found (refreshed every 2 s) with a "Use" button each, and
// buttons for freshCoder (works without a card) and Quit. `tried`/`error`:
// what was tried and why it failed (may be empty). Returns true with
// `chosen` set when the user picks a device, false to quit.
bool runNoDeviceScreen(const std::string& tried, const std::string& error, std::string& chosen,
                       const std::function<void()>& openFreshCoder);

}  // namespace fv

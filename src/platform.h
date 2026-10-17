// SPDX-License-Identifier: GPL-3.0-or-later
// Folders the programs use, per platform (Linux and Windows).
#pragma once

#include <string>

namespace fv {

// The folder of the running program.
std::string programDir();
// The settings folder: Linux $XDG_CONFIG_HOME/freshvision or
// ~/.config/freshvision; Windows %APPDATA%\freshVision, or <program
// folder>\settings when a file "portable.txt" lies next to the program (a
// zip unpacked to a USB stick keeps its settings with it). Not created here.
std::string settingsBaseDir();

// Windows: a console program started from Explorer gets a console window of
// its own; the GUI closes it (one started from a command prompt keeps it).
// Elsewhere nothing.
void hideOwnConsole();

}  // namespace fv

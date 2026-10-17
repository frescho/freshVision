// SPDX-License-Identifier: GPL-3.0-or-later
// Machine files (two layers since 0.1.0): built-in machine files shipped
// with the program (`<prefix>/share/freshvision/machines/`, read-only, replaced
// by every upgrade) and the user's "My modes" (`~/.config/freshvision/machines/`,
// never touched by an upgrade). One human-readable file per machine, each
// with one section per video mode. A user mode with the same machine and
// mode name as a built-in one replaces it ("replaced by yours"); the
// built-in mode stays available. Each built-in machine and mode has a
// `revision`; a user mode saved from a built-in one records `based_on`, so a
// newer built-in revision can be offered after an upgrade.
// A mode says when it applies (match conditions on what the driver reports
// at runtime and what freshVision measures) and what it sets (the same keys
// as a preset, presets.h). freshVision picks the mode whose stated
// conditions all hold and that satisfies the most of them (the way VCS
// picks its video presets), and can switch to it by itself when the source
// changes mode.
//
// File format (INI-like, '#' starts a comment line; a value may end in the
// marker "  # default": the value equalled the program default when saved):
//   [machine]
//   format_version=2                      (missing = 1, the 0.1.0 files)
//   group=Commodore                       (maker/family: the Machines section groups by it)
//   revision=1                            (built-in files)
//   name=Commodore 64 (PAL)
//   note=free text shown in the app
//   sound_source=hw:CARD=PCH,DEV=0,left   (optional: added when the machine is applied)
//   alias=768x576 -> 384x298              (optional: treat a detected size as another)
//   [mode Composite 8x]
//   match_input=Analog Sync On Green      (substring of the input's name)
//   match_signal=Video                    (the driver's "Signal Type")
//   match_size=384x298                    (the detected active area)
//   match_line_total=524 +-2              (the detected samples per line)
//   match_refresh=50.125 +-0.02           (measured, Hz)
//   match_cycles_per_line=283.5 +-0.1     (composite: colour-carrier cycles per line, measured)
//   match_device=/dev/video2
//   match_manual=1                        (only applied by a click, never automatically)
//   match_colour_on=blue                  (a colour burst on that input; S-Video's colour wire)
//   match_no_colour_on=blue               (no burst on that input)
//   match_priority=1                      (wins a tie: tried first while a condition can't be measured yet)
//   hint=text shown when this mode is active
//   revision=1                            (built-in modes)
//   based_on=1 / kept_revision=2          (user modes saved from / kept against a built-in one)
//   hidden=1                              (user file: hide the built-in mode of this name)
//   ...preset keys (format=g8, width=2048, pal_multiple=8, control.0.name=..., ...)
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "presets.h"

namespace fv {

// What the source looks like right now, from the driver's runtime reports
// (clean-room: controls matched by name) and freshVision's own measurements.
struct DetectedMode {
    std::string device;       // /dev/videoN
    std::string input;        // ENUMINPUT name
    std::string signal;       // "Signal Type" label ("Video", "DVI", ...)
    uint32_t width = 0, height = 0;  // detected active area: the defaults of "Horizontal/Vertical Timing"
    int lineTotal = 0;        // detected samples per line: the default of "Horizontal Size"
    double refreshHz = 0;     // measured from the buffer timestamps, 0 = not yet
    double cyclesPerLine = 0; // composite: carrier cycles per line (PAL decoder), 0 = not measured
    // A colour burst on the red / green / blue input: 1 yes, 0 no, -1 not
    // measured (only the inputs the running decoder looks at are measured;
    // S-Video decoding sees its colour input).
    int colourOn[3] = {-1, -1, -1};
    bool noSignal = false;

    // The part that changes when the source changes mode (refresh and
    // cycles are measured and settle over seconds, so they are left out).
    std::string signature() const;
    std::string describe() const;
    // The same as a few fixed lines (the Machines section: nothing reflows
    // when a measured number changes; "-" while not measured yet).
    std::string describeLines() const;
};

struct ModeMatch {
    std::string device, input, signal;
    uint32_t width = 0, height = 0;           // 0 = any
    int lineTotal = 0, lineTotalTol = 2;      // 0 = any
    double refreshHz = 0, refreshTol = 0.05;  // 0 = any
    double cyclesPerLine = 0, cyclesTol = 0.1;// 0 = any
    bool manual = false;  // match_manual=1: never picked by itself, only by a click (e.g. not yet verified)
    int colourOn = -1;    // match_colour_on=red|green|blue: a burst must be there (0-2; -1 = any)
    int noColourOn = -1;  // match_no_colour_on=...: no burst may be there
    int priority = 0;     // match_priority=N: wins a tie against modes with a lower one
};

// Where a mode comes from: shipped, the user's own, or the user's copy that
// replaces a shipped mode of the same name.
enum class ModeOrigin { BuiltIn, Mine, MineReplacing };

struct MachineMode {
    std::string name;
    ModeMatch match;
    std::string hint;
    Preset settings;
    ModeOrigin origin = ModeOrigin::Mine;
    int revision = 0;       // built-in: this mode's revision
    int basedOn = -1;       // a user mode saved from a built-in one: that revision (-1 = none)
    int keptRevision = -1;  // "keep mine" chosen against this built-in revision
    bool hidden = false;    // user file: hides the built-in mode of this name
    // The setting keys as read (including keys this version doesn't know:
    // they are written back unchanged).
    std::map<std::string, std::string> raw;
};

struct Machine {
    std::string path;  // the user's file (may not exist yet)
    std::string id;    // file name without .conf
    std::string name, note;
    std::string group;          // maker/family, e.g. "Commodore"
    int revision = 0;           // built-in machine's revision
    bool builtIn = false;       // a shipped file exists for this machine
    bool mine = false;          // the user has a file for this machine
    std::vector<MachineMode> replacedBuiltIn;  // built-in modes a user mode replaces (for "differences" / "back")
    std::vector<std::string> hiddenModes;      // built-in modes the user hid
    std::string soundSource;  // "DEVICE[,left|right|stereo]", empty = none
    std::vector<std::pair<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>>> aliases;
    std::vector<MachineMode> modes;
};

// The user's folder (~/.config/freshvision/machines, created if missing).
std::string machinesDir();
// The shipped built-in machine files: $FRESHVISION_BUILTIN_MACHINES, else
// <program>/../share/freshvision/machines, else the source tree's
// data/machines (development builds). Empty if none is found.
std::string builtInMachinesDir();
// Built-in machines combined with the user's (user modes replace built-in
// ones of the same name, hidden ones are left out); default last.
// Unreadable lines are reported in `errors`.
std::vector<Machine> loadMachines(std::vector<std::string>& errors);
// Writes the user's part of `m` (its own modes, replacements, hidden marks)
// to the user's file; a file left with nothing in it is removed.
bool saveMachine(const Machine& m, std::string& err);
// Saving over an existing mode: the old mode's settings with only what
// changed since it was applied (`baseline` = the settings then, `now` = the
// settings at saving): keys compared one by one, card controls by name.
// `changes` lists what was taken over ("key: old -> new").
Preset mergeModeChanges(const Preset& old, const Preset& baseline, const Preset& now, std::vector<std::string>& changes);
// The keys that differ between two modes' settings and conditions ("key: a -> b").
std::vector<std::string> modeDifferences(const MachineMode& mine, const MachineMode& builtIn);
// A user mode whose built-in base has a newer revision than it was saved
// from (and the user hasn't chosen "keep mine" for that revision).
struct ModeUpdate {
    int machine = -1;
    std::string mode;
    int builtInRevision = 0;
};
std::vector<ModeUpdate> pendingModeUpdates(const std::vector<Machine>& machines);
// One-time conversion of 0.1.0 settings (built-in copies in the user's folder,
// presets): see machines.cpp. Returns false if nothing needed converting;
// `report` lists what was done.
bool migrateSettings(std::string& report);
// A file name for a new machine's name (letters, digits, '-').
std::string machineIdFor(const std::string& name);
// Newest modification time of the directory's files (to reload on change).
int64_t machinesDirStamp();

struct MatchResult {
    int machine = -1, mode = -1;
    int score = -1;
    int priority = 0;
    std::string why;  // which conditions held
};
// -1: a stated condition fails; otherwise the number of satisfied conditions
// (a measurement that isn't there yet neither counts nor fails).
// `allowManual`: also score modes marked match_manual (the user named this
// machine, e.g. --machine NAME or its button); otherwise they never match.
int scoreMode(const Machine& m, const MachineMode& mode, const DetectedMode& d, std::string* why = nullptr,
              bool allowManual = false);
MatchResult bestMatch(const std::vector<Machine>& machines, const DetectedMode& d);

}  // namespace fv

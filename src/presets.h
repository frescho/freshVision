// SPDX-License-Identifier: GPL-3.0-or-later
// Named, savable/restorable snapshots of the app's video + audio + recorder
// settings ("presets") — the
// base of the machine modes.
//
//
// Storage: one small hand-rolled key=value text file per preset under
// ~/.config/freshvision/presets/<name>.txt (ungrouped) or
// ~/.config/freshvision/presets/<group>/<name>.txt (grouped, e.g. "C64") —
// not JSON. No JSON library exists in this codebase yet, and the shape here
// is fixed and not deeply nested, so a small hand-rolled writer/parser
// avoids adding one — the same choice this project already made for the
// WAV header (src/audio.cpp). Groups are one level deep only (a group
// holding sub-groups isn't supported — not asked for, and flat listing/
// deletion is simpler without it).
//
// Controls are matched by *name*, never a hardcoded ID: control IDs are
// runtime-scanned per driver, so applying a preset must
// ask the currently-open device "do you have a control called X", not
// assume an ID — the same "ask the system" pattern as controls.cpp's own
// enumeration.
#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "audio.h"
#include "capture.h"
#include "display.h"
#include "recorder.h"

namespace fv {

struct PresetControl {
    std::string name;
    int64_t value = 0;
};

struct PresetAudioSource {
    std::string deviceId;
    ChannelMode mode = ChannelMode::Stereo;
    OutputRoute route[2] = {OutputRoute::Stereo, OutputRoute::Stereo};
    float gain = 1.0f;
    float balance = 0.0f;
    int delayMs = 0;
    // Hardware mixer state — only meaningful when the device still has one
    // (AudioCapture::hasHwControl()) at apply time; saved regardless so a
    // preset made on hardware where it's usable still carries the values.
    float hwVolume = 1.0f;
    float hwBalance = 0.0f;
    bool hwMuted = false;
    std::string hwInputSource;  // empty: no selectable input source, or none saved
};

struct Preset {
    std::string name;
    std::string group;  // empty = ungrouped/top-level (e.g. "C64")

    // Video
    std::string devicePath;  // e.g. "/dev/video3"
    uint32_t formatFourcc = 0;
    uint32_t width = 0, height = 0;
    Rect crop;
    std::vector<PresetControl> controls;
    DisplayOptions display;

    // Audio
    std::vector<PresetAudioSource> audioSources;
    std::string monitorPlaybackId;

    // Recorder
    VideoEncoderKind encoderKind = VideoEncoderKind::Auto;

    // Which parts a file actually contained (a hand-written machine mode may
    // leave them out; applying it then keeps what is running). A saved
    // preset always has all of them.
    bool audioSpecified = true;
    bool encoderSpecified = true;
    bool displaySpecified = true;
};

// The settings part of a preset file (every key except name/group), shared
// with the machine files (machines.h), whose modes use the same keys.
void writePresetSettings(std::ostream& f, const Preset& p);
// Parses those keys; missing keys get the defaults (see the *Specified flags).
Preset parsePresetSettings(std::map<std::string, std::string> kv);

// ~/.config/freshvision/presets, created if it doesn't exist yet. Empty on
// failure (e.g. $HOME unset).
std::string presetsDir();

// One saved preset's identity, as returned by listPresets() — group empty
// means ungrouped/top-level.
struct PresetEntry {
    std::string group;
    std::string name;
};

bool savePreset(const Preset& p, std::string& err);  // uses p.group and p.name
bool loadPreset(const std::string& group, const std::string& name, Preset& out, std::string& err);
// Every saved preset's (group, name), sorted by group then name (ungrouped
// ones sort first, since "" < any non-empty group name).
std::vector<PresetEntry> listPresets();
bool deletePreset(const std::string& group, const std::string& name, std::string& err);

// freshVision's settings folder: $XDG_CONFIG_HOME/freshvision, else
// ~/.config/freshvision (inside a Flatpak XDG_CONFIG_HOME points into the
// sandbox's own folder). Empty if neither is known.
std::string configDir();

}  // namespace fv

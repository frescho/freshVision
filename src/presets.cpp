// SPDX-License-Identifier: GPL-3.0-or-later
#include "presets.h"
#include "platform.h"

#include "formats.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fv {

namespace fs = std::filesystem;

namespace {

// Compact, stable tokens for the text format — deliberately separate from
// each type's own display-name function (e.g. channelModeName() returns
// "Separate L/R", which has a space and a slash and isn't a nice bare
// key=value token, and could change for UI reasons independently of the
// file format).
std::string channelModeToken(ChannelMode m) {
    switch (m) {
        case ChannelMode::Left: return "Left";
        case ChannelMode::Right: return "Right";
        case ChannelMode::Stereo: return "Stereo";
        case ChannelMode::SeparateLR: return "SeparateLR";
    }
    return "Stereo";
}
ChannelMode channelModeFromToken(const std::string& s) {
    if (s == "Left") return ChannelMode::Left;
    if (s == "Right") return ChannelMode::Right;
    if (s == "SeparateLR") return ChannelMode::SeparateLR;
    return ChannelMode::Stereo;
}

std::string outputRouteToken(OutputRoute r) {
    switch (r) {
        case OutputRoute::Left: return "Left";
        case OutputRoute::Right: return "Right";
        case OutputRoute::Stereo: return "Stereo";
    }
    return "Stereo";
}
OutputRoute outputRouteFromToken(const std::string& s) {
    if (s == "Left") return OutputRoute::Left;
    if (s == "Right") return OutputRoute::Right;
    return OutputRoute::Stereo;
}

std::string scaleModeToken(ScaleMode m) {
    switch (m) {
        case ScaleMode::Integer: return "Integer";
        case ScaleMode::Fit: return "Fit";
        case ScaleMode::Stretch: return "Stretch";
    }
    return "Integer";
}
ScaleMode scaleModeFromToken(const std::string& s) {
    if (s == "Fit") return ScaleMode::Fit;
    if (s == "Stretch") return ScaleMode::Stretch;
    return ScaleMode::Integer;
}

std::string encoderKindToken(VideoEncoderKind k) {
    switch (k) {
        case VideoEncoderKind::Auto: return "Auto";
        case VideoEncoderKind::Nvenc: return "Nvenc";
        case VideoEncoderKind::Vaapi: return "Vaapi";
        case VideoEncoderKind::X264: return "X264";
        case VideoEncoderKind::Ffv1: return "Ffv1";
        case VideoEncoderKind::Raw: return "Raw";
    }
    return "Auto";
}
VideoEncoderKind encoderKindFromToken(const std::string& s) {
    if (s == "Nvenc") return VideoEncoderKind::Nvenc;
    if (s == "Vaapi") return VideoEncoderKind::Vaapi;
    if (s == "X264") return VideoEncoderKind::X264;
    if (s == "Ffv1") return VideoEncoderKind::Ffv1;
    if (s == "Raw") return VideoEncoderKind::Raw;
    return VideoEncoderKind::Auto;
}

// A line's value may itself contain '=' (a control name won't, in
// practice, but don't assume) — split on the *first* '=' only. Values are
// written/read as plain UTF-8 text; a newline inside a name would corrupt
// the format, but V4L2/ALSA names are single-line in every case this
// codebase has ever seen, so that's not guarded against further.
bool splitKeyValue(const std::string& line, std::string& key, std::string& value) {
    auto pos = line.find('=');
    if (pos == std::string::npos) return false;
    key = line.substr(0, pos);
    value = line.substr(pos + 1);
    return true;
}

// A single path component (no '/', no "." / "..", no control characters).
// Used for both the group and the name — but a group may additionally be
// empty (ungrouped); a name never is.
bool isValidPathComponent(const std::string& s) {
    if (s == "." || s == "..") return false;
    if (s.find('/') != std::string::npos) return false;
    for (char c : s)
        if (static_cast<unsigned char>(c) < 0x20) return false;  // no control characters
    return true;
}
bool isValidPresetName(const std::string& name) { return !name.empty() && isValidPathComponent(name); }
bool isValidGroup(const std::string& group) { return isValidPathComponent(group); }  // empty = ungrouped, valid

// Where a preset's file lives — one level of subdirectory when grouped,
// directly under presetsDir() when not. Doesn't create the group
// subdirectory; savePreset() does that itself, the read paths (load/
// delete) don't need to.
fs::path presetFilePath(const std::string& dir, const std::string& group, const std::string& name) {
    fs::path p = fs::path(dir);
    if (!group.empty()) p /= group;
    return p / (name + ".txt");
}

}  // namespace

void writePresetSettings(std::ostream& f, const Preset& p) {
    f << "device_path=" << p.devicePath << "\n";
    // The command-line name (g8, rgb4, ...) when there is one, so a
    // hand-edited machine file stays readable; the number otherwise.
    if (const PixelFormat* pf = p.formatFourcc ? findFormat(p.formatFourcc) : nullptr)
        f << "format=" << pf->name << "\n";
    else
        f << "format_fourcc=" << p.formatFourcc << "\n";
    f << "width=" << p.width << "\n";
    f << "height=" << p.height << "\n";
    f << "crop_set=" << (p.crop.set ? 1 : 0) << "\n";
    f << "crop_x=" << p.crop.x << "\n";
    f << "crop_y=" << p.crop.y << "\n";
    f << "crop_w=" << p.crop.w << "\n";
    f << "crop_h=" << p.crop.h << "\n";
    f << "display_scale_mode=" << scaleModeToken(p.display.scaleMode) << "\n";
    f << "display_linear_filter=" << (p.display.linearFilter ? 1 : 0) << "\n";
    f << "display_pixel_aspect=" << p.display.pixelAspect << "\n";
    f << "display_integer_factor=" << p.display.integerFactor << "\n";  // 0 = Auto
    // PAL composite decoding (Sync-on-Green): a preset is also a machine
    // profile.
    const PalDecodeUi& u = p.display.palUi;
    f << "rgb_autotune=" << (p.display.rgbAutoTune ? 1 : 0) << "\n";
    f << "rgb_autocrop=" << (p.display.rgbAutoCrop ? 1 : 0) << "\n";
    f << "rgb_screen_size=" << p.display.rgbScreenW << "x" << p.display.rgbScreenH << "\n";
    f << "pal_decode=" << (p.display.palDecode ? 1 : 0) << "\n";
    f << "pal_multiple=" << p.display.palMultiple << "\n";
    f << "pal_svideo=" << (p.display.palSvideo ? 1 : 0) << "\n";
    f << "pal_svideo_channels=" << "RGB"[std::clamp(p.display.svideoLuma, 0, 2)] << "," << "RGB"[std::clamp(p.display.svideoChroma, 0, 2)] << "\n";
    f << "pal_line=" << p.display.palLine << "\n";
    f << "video_standard=" << (p.display.videoStandard == 1 ? "ntsc" : "pal") << "\n";
    f << "pal_autotune=" << (p.display.palAutoTune ? 1 : 0) << "\n";
    f << "pal_luma=" << u.luma << "\n";
    f << "pal_average_lines=" << (u.averageLines ? 1 : 0) << "\n";
    f << "pal_clock_stripes=" << (u.removeStripes ? 1 : 0) << "\n";
    f << "pal_relock=" << (u.relock ? 1 : 0) << "\n";
    f << "pal_chroma_cycles=" << u.chromaCycles << "\n";
    f << "pal_edge_desat=" << u.edgeDesat << "\n";
    f << "pal_soft_text=" << (u.softText ? 1 : 0) << "\n";
    f << "pal_mono=" << (u.mono ? 1 : 0) << "\n";
    f << "pal_brightness=" << u.picture.brightness << "\n";
    f << "pal_contrast=" << u.picture.contrast << "\n";
    f << "pal_saturation=" << u.picture.saturation << "\n";
    f << "pal_hue=" << u.picture.hue << "\n";
    f << "pal_gamma=" << u.picture.gamma << "\n";
    f << "pal_profile=" << u.profile << "\n";
    f << "pal_pixel_grid=" << (u.pixelGrid ? 1 : 0) << "\n";
    f << "pal_grid_core=" << u.gridCore << "\n";
    f << "pal_snap=" << (u.snap ? 1 : 0) << "\n";
    f << "pal_colours=" << u.colours << "\n";
    f << "pal_palette_source=" << u.paletteSource << "\n";
    f << "pal_c64_cells=" << (u.c64Cells ? 1 : 0) << "\n";
    f << "pal_tms_cells=" << (u.tmsCells ? 1 : 0) << "\n";
    f << "pal_pll=" << (u.pll ? 1 : 0) << "\n";
    f << "pal_pll_speed=" << u.pllBandwidth << "\n";
    {
        std::string hex;
        char b[8];
        for (const auto& c : u.customPalette) {
            std::snprintf(b, sizeof b, "%02X%02X%02X", c[0], c[1], c[2]);
            hex += (hex.empty() ? "" : ",") + std::string(b);
        }
        f << "pal_custom_palette=" << hex << "\n";
    }
    if (p.encoderSpecified) f << "recorder_encoder=" << encoderKindToken(p.encoderKind) << "\n";
    if (p.audioSpecified) f << "monitor_playback_id=" << p.monitorPlaybackId << "\n";

    f << "control_count=" << p.controls.size() << "\n";
    for (size_t i = 0; i < p.controls.size(); ++i) {
        f << "control." << i << ".name=" << p.controls[i].name << "\n";
        f << "control." << i << ".value=" << p.controls[i].value << "\n";
    }

    if (!p.audioSpecified) return;  // a machine mode that leaves the sound alone
    f << "audio_source_count=" << p.audioSources.size() << "\n";
    for (size_t i = 0; i < p.audioSources.size(); ++i) {
        const PresetAudioSource& s = p.audioSources[i];
        std::string k = "audio_source." + std::to_string(i) + ".";
        f << k << "device_id=" << s.deviceId << "\n";
        f << k << "mode=" << channelModeToken(s.mode) << "\n";
        f << k << "route0=" << outputRouteToken(s.route[0]) << "\n";
        f << k << "route1=" << outputRouteToken(s.route[1]) << "\n";
        f << k << "gain=" << s.gain << "\n";
        f << k << "balance=" << s.balance << "\n";
        f << k << "delay_ms=" << s.delayMs << "\n";
        f << k << "hw_volume=" << s.hwVolume << "\n";
        f << k << "hw_balance=" << s.hwBalance << "\n";
        f << k << "hw_muted=" << (s.hwMuted ? 1 : 0) << "\n";
        f << k << "hw_input_source=" << s.hwInputSource << "\n";
    }
}

Preset parsePresetSettings(std::map<std::string, std::string> kv) {
    Preset p;
    p.devicePath = kv["device_path"];
    p.formatFourcc = kv.count("format_fourcc") ? uint32_t(std::stoul(kv["format_fourcc"])) : 0;
    if (kv.count("format"))
        if (const PixelFormat* pf = findFormat(kv["format"])) p.formatFourcc = pf->fourcc;
    p.width = kv.count("width") ? uint32_t(std::stoul(kv["width"])) : 0;
    p.height = kv.count("height") ? uint32_t(std::stoul(kv["height"])) : 0;
    p.crop.set = kv.count("crop_set") && kv["crop_set"] == "1";
    p.crop.x = kv.count("crop_x") ? std::stoi(kv["crop_x"]) : 0;
    p.crop.y = kv.count("crop_y") ? std::stoi(kv["crop_y"]) : 0;
    p.crop.w = kv.count("crop_w") ? uint32_t(std::stoul(kv["crop_w"])) : 0;
    p.crop.h = kv.count("crop_h") ? uint32_t(std::stoul(kv["crop_h"])) : 0;
    p.display.scaleMode = scaleModeFromToken(kv["display_scale_mode"]);
    p.display.linearFilter = kv.count("display_linear_filter") && kv["display_linear_filter"] == "1";
    p.display.pixelAspect = kv.count("display_pixel_aspect") ? std::stod(kv["display_pixel_aspect"]) : 1.0;
    // Older presets have no factor: Auto. Out-of-range values also fall back to Auto.
    p.display.integerFactor = kv.count("display_integer_factor") ? std::atoi(kv["display_integer_factor"].c_str()) : 0;
    if (p.display.integerFactor < 0 || p.display.integerFactor > kMaxIntegerFactor) p.display.integerFactor = 0;
    {
        // PAL settings; older presets have none and get the defaults.
        auto num = [&](const char* k, double def) { return kv.count(k) ? std::atof(kv[k].c_str()) : def; };
        PalDecodeUi& u = p.display.palUi;
        p.display.rgbAutoTune = num("rgb_autotune", 0) != 0;
        p.display.rgbAutoCrop = num("rgb_autocrop", 0) != 0;
        if (kv.count("rgb_screen_size")) {
            int w = 0, h = 0;
            if (std::sscanf(kv["rgb_screen_size"].c_str(), "%dx%d", &w, &h) == 2 && w >= 0 && h >= 0)
                p.display.rgbScreenW = w, p.display.rgbScreenH = h;
        }
        p.display.palDecode = num("pal_decode", 0) != 0;
        p.display.palSvideo = num("pal_svideo", 0) != 0;
        if (kv.count("pal_svideo_channels") && kv["pal_svideo_channels"].size() == 3) {
            auto chIdx = [](char c) { c = char(std::toupper(static_cast<unsigned char>(c))); return c == 'R' ? 0 : c == 'G' ? 1 : c == 'B' ? 2 : -1; };
            const int l = chIdx(kv["pal_svideo_channels"][0]), c = chIdx(kv["pal_svideo_channels"][2]);
            if (l >= 0 && c >= 0 && l != c) p.display.svideoLuma = l, p.display.svideoChroma = c;
        }
        p.display.palMultiple = int(num("pal_multiple", 4));
        if (p.display.palMultiple == 16) p.display.palMultiple = 8;  // 16x removed
        {
            const int m = p.display.palMultiple;
            const bool ntsc = kv.count("video_standard") && kv["video_standard"] == "ntsc";
            if (m != 4 && m != 8 && m != 10 && !(ntsc && (m == 6 || m == 12))) p.display.palMultiple = 4;
        }
        p.display.palLine = int(num("pal_line", 1135));
        {
            auto it = kv.find("video_standard");
            p.display.videoStandard = it != kv.end() && it->second == "ntsc" ? 1 : 0;
        }
        p.display.palAutoTune = num("pal_autotune", 0) != 0;
        u.luma = std::clamp(int(num("pal_luma", 1)), 0, 2);
        u.averageLines = num("pal_average_lines", 0) != 0;
        u.removeStripes = num("pal_clock_stripes", 0) != 0;
        u.relock = num("pal_relock", 0) != 0;
        u.chromaCycles = std::clamp(int(num("pal_chroma_cycles", 2)), 1, 12);
        u.edgeDesat = float(std::clamp(num("pal_edge_desat", 0), 0.0, 1.0));
        u.softText = num("pal_soft_text", 0) != 0;
        u.mono = num("pal_mono", 0) != 0;
        u.picture.brightness = float(num("pal_brightness", 0));
        u.picture.contrast = float(num("pal_contrast", 1));
        u.picture.saturation = float(num("pal_saturation", 1));
        u.picture.hue = float(num("pal_hue", 0));
        u.picture.gamma = float(num("pal_gamma", 1));
        u.profile = int(num("pal_profile", 1));
        u.pixelGrid = num("pal_pixel_grid", 0) != 0;
        u.gridCore = float(num("pal_grid_core", 0.5));
        u.snap = num("pal_snap", 0) != 0;
        u.colours = std::max(2, int(num("pal_colours", 16)));
        u.paletteSource = std::clamp(int(num("pal_palette_source", 0)), 0, 2);
        u.c64Cells = num("pal_c64_cells", 0) != 0;
        u.tmsCells = num("pal_tms_cells", 0) != 0;
        u.pll = num("pal_pll", 1) != 0;
        u.pllBandwidth = float(num("pal_pll_speed", 0.2));
        u.customPalette.clear();
        const std::string hex = kv.count("pal_custom_palette") ? kv["pal_custom_palette"] : "";
        for (size_t i = 0; i + 6 <= hex.size(); i += 7) {
            unsigned r, g, b;
            if (std::sscanf(hex.c_str() + i, "%02x%02x%02x", &r, &g, &b) == 3)
                u.customPalette.push_back({uint8_t(r), uint8_t(g), uint8_t(b)});
        }
    }
    p.encoderKind = encoderKindFromToken(kv["recorder_encoder"]);
    p.monitorPlaybackId = kv["monitor_playback_id"];

    size_t controlCount = kv.count("control_count") ? std::stoul(kv["control_count"]) : 0;
    for (size_t i = 0; i < controlCount; ++i) {
        std::string base = "control." + std::to_string(i) + ".";
        if (!kv.count(base + "name")) continue;  // tolerate a truncated/edited file, don't crash on it
        PresetControl c;
        c.name = kv[base + "name"];
        c.value = kv.count(base + "value") ? std::stoll(kv[base + "value"]) : 0;
        p.controls.push_back(std::move(c));
    }

    size_t sourceCount = kv.count("audio_source_count") ? std::stoul(kv["audio_source_count"]) : 0;
    for (size_t i = 0; i < sourceCount; ++i) {
        std::string base = "audio_source." + std::to_string(i) + ".";
        if (!kv.count(base + "device_id")) continue;
        PresetAudioSource s;
        s.deviceId = kv[base + "device_id"];
        s.mode = channelModeFromToken(kv[base + "mode"]);
        s.route[0] = outputRouteFromToken(kv[base + "route0"]);
        s.route[1] = outputRouteFromToken(kv[base + "route1"]);
        s.gain = kv.count(base + "gain") ? std::stof(kv[base + "gain"]) : 1.0f;
        s.balance = kv.count(base + "balance") ? std::stof(kv[base + "balance"]) : 0.0f;
        s.delayMs = kv.count(base + "delay_ms") ? std::stoi(kv[base + "delay_ms"]) : 0;
        s.hwVolume = kv.count(base + "hw_volume") ? std::stof(kv[base + "hw_volume"]) : 1.0f;
        s.hwBalance = kv.count(base + "hw_balance") ? std::stof(kv[base + "hw_balance"]) : 0.0f;
        s.hwMuted = kv.count(base + "hw_muted") && kv[base + "hw_muted"] == "1";
        s.hwInputSource = kv[base + "hw_input_source"];
        p.audioSources.push_back(std::move(s));
    }
    p.audioSpecified = kv.count("audio_source_count") != 0;
    p.encoderSpecified = kv.count("recorder_encoder") != 0;
    p.displaySpecified = kv.count("display_scale_mode") != 0 || kv.count("pal_decode") != 0;
    return p;
}

std::string configDir() { return settingsBaseDir(); }

std::string presetsDir() {
    if (configDir().empty()) return {};
    fs::path dir = fs::path(configDir()) / "presets";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {};
    return dir.string();
}

bool savePreset(const Preset& p, std::string& err) {
    if (!isValidPresetName(p.name)) {
        err = "invalid preset name (must be non-empty, no '/' or control characters)";
        return false;
    }
    if (!isValidGroup(p.group)) {
        err = "invalid group name (no '/' or control characters)";
        return false;
    }
    std::string dir = presetsDir();
    if (dir.empty()) {
        err = "could not determine/create the presets directory (is $HOME set?)";
        return false;
    }
    fs::path path = presetFilePath(dir, p.group, p.name);
    if (!p.group.empty()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            err = "could not create the group directory for '" + p.group + "'";
            return false;
        }
    }
    std::ofstream f(path);
    if (!f) {
        err = "could not open " + path.string() + " for writing";
        return false;
    }

    f << "name=" << p.name << "\n";
    f << "group=" << p.group << "\n";
    writePresetSettings(f, p);

    if (!f) {
        err = "write error while saving " + path.string();
        return false;
    }
    return true;
}

bool loadPreset(const std::string& group, const std::string& name, Preset& out, std::string& err) {
    if (!isValidPresetName(name) || !isValidGroup(group)) {
        err = "invalid preset name or group";
        return false;
    }
    std::string dir = presetsDir();
    if (dir.empty()) {
        err = "could not determine the presets directory (is $HOME set?)";
        return false;
    }
    fs::path path = presetFilePath(dir, group, name);
    std::ifstream f(path);
    if (!f) {
        err = "no such preset: " + (group.empty() ? name : group + "/" + name);
        return false;
    }

    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        std::string k, v;
        if (splitKeyValue(line, k, v)) kv[k] = v;
    }

    Preset p = parsePresetSettings(kv);
    p.name = kv.count("name") ? kv["name"] : name;
    p.group = kv.count("group") ? kv["group"] : group;
    out = std::move(p);
    return true;
}

std::vector<PresetEntry> listPresets() {
    std::vector<PresetEntry> out;
    std::string dir = presetsDir();
    if (dir.empty()) return out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file()) {
            fs::path p = entry.path();
            if (p.extension() == ".txt") out.push_back({"", p.stem().string()});
        } else if (entry.is_directory()) {
            // One level of group subdirectory — see presets.h's "one level
            // deep only" note.
            std::string group = entry.path().filename().string();
            std::error_code ec2;
            for (const auto& sub : fs::directory_iterator(entry.path(), ec2)) {
                if (ec2) break;
                if (!sub.is_regular_file()) continue;
                fs::path sp = sub.path();
                if (sp.extension() == ".txt") out.push_back({group, sp.stem().string()});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const PresetEntry& a, const PresetEntry& b) {
        if (a.group != b.group) return a.group < b.group;
        return a.name < b.name;
    });
    return out;
}

bool deletePreset(const std::string& group, const std::string& name, std::string& err) {
    if (!isValidPresetName(name) || !isValidGroup(group)) {
        err = "invalid preset name or group";
        return false;
    }
    std::string dir = presetsDir();
    if (dir.empty()) {
        err = "could not determine the presets directory";
        return false;
    }
    fs::path path = presetFilePath(dir, group, name);
    std::error_code ec;
    if (!fs::remove(path, ec) || ec) {
        err = "could not delete " + path.string();
        return false;
    }
    // Tidy up an empty group directory left behind, best-effort — not an
    // error if it's not empty (other presets still in it) or this fails.
    if (!group.empty()) {
        std::error_code ec2;
        fs::remove(fs::path(dir) / group, ec2);
    }
    return true;
}

}  // namespace fv

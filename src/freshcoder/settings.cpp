// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace fc {

namespace {

bool validName(const std::string& n) {
    if (n.empty() || n.size() > 80 || n == "." || n == "..") return false;
    return n.find('/') == std::string::npos && n.find('\0') == std::string::npos;
}

std::string pathFor(const std::string& name) { return settingsFolder() + "/" + name + ".conf"; }

}  // namespace

std::string settingsFolder() {
    std::string dir = (std::filesystem::u8path(fv::settingsBaseDir()) / "freshcoder").u8string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::vector<std::string> listSettings() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(settingsFolder(), ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".conf") continue;
        const std::string n = e.path().stem().string();
        if (n != kLastSettings) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool saveSettings(const std::string& name, const CoderSettings& s, std::string& err) {
    if (!validName(name)) {
        err = "a name without '/' is needed";
        return false;
    }
    std::ofstream f(pathFor(name));
    if (!f) {
        err = "cannot write " + pathFor(name);
        return false;
    }
    const fv::PalDecodeUi& u = s.ui;
    f << "# freshCoder settings\n";
    f << "luma=" << u.luma << "\naverage_lines=" << u.averageLines << "\nclock_stripes=" << u.removeStripes << "\nrelock=" << u.relock << "\nchroma_cycles=" << u.chromaCycles << "\nedge_desat=" << u.edgeDesat << "\nsoft_text=" << u.softText << "\nmono=" << u.mono << "\n";
    f << "brightness=" << u.picture.brightness << "\ncontrast=" << u.picture.contrast << "\nsaturation=" << u.picture.saturation
      << "\nhue=" << u.picture.hue << "\ngamma=" << u.picture.gamma << "\n";
    f << "pll=" << u.pll << "\npll_speed=" << u.pllBandwidth << "\n";
    f << "standard_mode=" << u.standardMode << "\n";
    f << "svideo_mode=" << u.svideoMode << "\nsvideo_luma=" << u.svideoLuma << "\nsvideo_chroma=" << u.svideoChroma << "\n";
    f << "profile=" << u.profile << "\npixel_grid=" << u.pixelGrid << "\ngrid_core=" << u.gridCore << "\nsnap=" << u.snap
      << "\ncolours=" << u.colours << "\npalette_source=" << u.paletteSource << "\nc64_cells=" << u.c64Cells << "\ntms_cells=" << u.tmsCells << "\n";
    std::string hex;
    char b[8];
    for (const auto& c : u.customPalette) {
        std::snprintf(b, sizeof b, "%02X%02X%02X", c[0], c[1], c[2]);
        hex += (hex.empty() ? "" : ",") + std::string(b);
    }
    f << "custom_palette=" << hex << "\n";
    const OutputGeometry& g = s.geom;
    f << "crop=" << g.cropX << "," << g.cropY << "," << g.cropW << "," << g.cropH << "\n";
    f << "size_mode=" << int(g.mode) << "\nfactor=" << g.factor << "\ncustom=" << g.customW << "x" << g.customH
      << "\ntarget_h=" << g.targetH << "\ninteger_scale=" << g.integerScale << "\nint_x=" << g.intX << "\nint_y=" << g.intY
      << "\nint_same=" << g.intSame << "\nint_frame_h=" << g.intFrameH << "\nscaler=" << int(g.scaler) << "\n";
    std::snprintf(b, sizeof b, "%02X%02X%02X", g.pad[0], g.pad[1], g.pad[2]);
    f << "pad=" << b << "\n";
    const EncodeSettings& e = s.enc;
    f << "codec=" << e.codec << "\nquality=" << e.quality << "\npreset=" << e.preset << "\nchroma444=" << e.chroma444
      << "\naudio=" << e.audio << "\naudio_kbps=" << e.audioKbps << "\n";
    f << "preview_scale_mode=" << int(s.preview.scaleMode) << "\npreview_integer_factor=" << s.preview.integerFactor
      << "\npreview_linear_filter=" << s.preview.linearFilter << "\npreview_pixel_aspect=" << s.preview.pixelAspect
      << "\npreview_aspect_auto=" << s.previewAspectAuto << "\n";
    return bool(f);
}

bool loadSettings(const std::string& name, CoderSettings& s, std::string& err) {
    if (!validName(name)) {
        err = "invalid name";
        return false;
    }
    std::ifstream f(pathFor(name));
    if (!f) {
        err = "no saved settings named " + name;
        return false;
    }
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    // Missing keys keep their current values, so older files still load.
    auto num = [&](const char* k, double& v) {
        if (kv.count(k)) v = std::atof(kv[k].c_str());
    };
    auto getI = [&](const char* k, int& v) {
        if (kv.count(k)) v = std::atoi(kv[k].c_str());
    };
    auto getB = [&](const char* k, bool& v) {
        if (kv.count(k)) v = std::atoi(kv[k].c_str()) != 0;
    };
    auto getF = [&](const char* k, float& v) {
        if (kv.count(k)) v = float(std::atof(kv[k].c_str()));
    };
    fv::PalDecodeUi& u = s.ui;
    getI("luma", u.luma), getB("average_lines", u.averageLines), getB("clock_stripes", u.removeStripes), getB("relock", u.relock), getF("edge_desat", u.edgeDesat), getB("soft_text", u.softText), getB("mono", u.mono), getI("chroma_cycles", u.chromaCycles), u.chromaCycles = std::clamp(u.chromaCycles, 1, 12);
    getF("brightness", u.picture.brightness), getF("contrast", u.picture.contrast), getF("saturation", u.picture.saturation);
    getF("hue", u.picture.hue), getF("gamma", u.picture.gamma);
    getB("pll", u.pll), getF("pll_speed", u.pllBandwidth);
    getI("standard_mode", u.standardMode), u.standardMode = std::clamp(u.standardMode, 0, 2);
    getI("svideo_mode", u.svideoMode), getI("svideo_luma", u.svideoLuma), getI("svideo_chroma", u.svideoChroma);
    u.svideoMode = std::clamp(u.svideoMode, 0, 2), u.svideoLuma = std::clamp(u.svideoLuma, 0, 2);
    u.svideoChroma = std::clamp(u.svideoChroma, 0, 2);
    getI("profile", u.profile), getB("pixel_grid", u.pixelGrid), getF("grid_core", u.gridCore), getB("snap", u.snap);
    getI("colours", u.colours), getI("palette_source", u.paletteSource), getB("c64_cells", u.c64Cells), getB("tms_cells", u.tmsCells);
    if (kv.count("custom_palette")) {
        u.customPalette.clear();
        const std::string& hex = kv["custom_palette"];
        for (size_t i = 0; i + 6 <= hex.size(); i += 7) {
            unsigned r, g, b;
            if (std::sscanf(hex.c_str() + i, "%02x%02x%02x", &r, &g, &b) == 3)
                u.customPalette.push_back({uint8_t(r), uint8_t(g), uint8_t(b)});
        }
    }
    OutputGeometry& g = s.geom;
    if (kv.count("crop")) std::sscanf(kv["crop"].c_str(), "%d,%d,%d,%d", &g.cropX, &g.cropY, &g.cropW, &g.cropH);
    int mode = int(g.mode), scaler = int(g.scaler);
    getI("size_mode", mode), getI("scaler", scaler);
    g.mode = SizeMode(std::clamp(mode, 0, 4)), g.scaler = Scaler(std::clamp(scaler, 0, 3));
    getI("factor", g.factor);
    if (kv.count("custom")) std::sscanf(kv["custom"].c_str(), "%dx%d", &g.customW, &g.customH);
    getI("target_h", g.targetH), getB("integer_scale", g.integerScale), getI("int_x", g.intX), getI("int_y", g.intY);
    getB("int_same", g.intSame), getI("int_frame_h", g.intFrameH);
    if (kv.count("pad")) {
        unsigned r, gg, b;
        if (std::sscanf(kv["pad"].c_str(), "%02x%02x%02x", &r, &gg, &b) == 3) g.pad[0] = uint8_t(r), g.pad[1] = uint8_t(gg), g.pad[2] = uint8_t(b);
    }
    EncodeSettings& e = s.enc;
    if (kv.count("codec")) e.codec = kv["codec"];
    getI("quality", e.quality);
    if (kv.count("preset")) e.preset = kv["preset"];
    getB("chroma444", e.chroma444);
    if (kv.count("audio")) e.audio = kv["audio"];
    getI("audio_kbps", e.audioKbps);
    int sm = int(s.preview.scaleMode);
    getI("preview_scale_mode", sm);
    s.preview.scaleMode = fv::ScaleMode(std::clamp(sm, 0, 2));
    getI("preview_integer_factor", s.preview.integerFactor);
    getB("preview_linear_filter", s.preview.linearFilter);
    num("preview_pixel_aspect", s.preview.pixelAspect);
    getB("preview_aspect_auto", s.previewAspectAuto);
    return true;
}

bool deleteSettings(const std::string& name, std::string& err) {
    std::error_code ec;
    if (!validName(name) || !std::filesystem::remove(pathFor(name), ec)) {
        err = "could not delete " + name;
        return false;
    }
    return true;
}

}  // namespace fc

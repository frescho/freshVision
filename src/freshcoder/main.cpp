// SPDX-License-Identifier: GPL-3.0-or-later
// freshCoder: turns a freshVision raw composite recording (the card's samples
// plus sound) into a colour video. GUI by default; `--encode` for batch use.
//
#ifdef _WIN32
#define SDL_MAIN_HANDLED  // our own main(), no SDL2main
#endif
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "desktop.h"
#include "display.h"
#include "encode.h"
#include "palui.h"
#include "platform.h"
#include "process.h"
#include "recording.h"
#include "settings.h"

using namespace fc;

namespace {

struct Options {
    std::string in, out;
    bool encode = false;      // batch: encode and exit, no window
    bool listCodecs = false;
    bool info = false;
    fv::PalDecodeUi ui;
    OutputGeometry geom;
    EncodeSettings enc;
    bool codecSet = false, qualitySet = false, presetSet = false;
    bool touched = false;     // any setting given on the command line: don't restore the last settings
};

void usage() {
    std::printf(
        "usage: freshcoder [IN.mkv] [options]\n"
        "  Opens a freshVision raw composite recording (g8 samples + sound) in the GUI, or with\n"
        "  --encode converts it without a window.\n"
        "  --version                print the version and exit\n"
        "  --encode                 batch mode: encode IN to --out and exit\n"
        "  --out FILE               output file (.mkv; .mp4/.mov also work for their codecs)\n"
        "  --codec ID               video encoder (see --list-codecs; default libx264, else the first available)\n"
        "  --quality N              CRF/CQ/q of the encoder, lower = better (default per codec)\n"
        "  --preset NAME            encoder preset (default per codec)\n"
        "  --chroma 444|420         colour resolution (default 444 where the encoder can)\n"
        "  --audio auto|pcm|flac|aac|opus|none   sound (auto: PCM for FFV1/ProRes, FLAC otherwise)\n"
        "  --size 43xN|native|WxH|720p|1080p|1440p|2160p   output size (default 43x2: 4:3, 2x the lines)\n"
        "  --integer                with WxH/720p..: whole multiples of the line count only\n"
        "  --pixel-exact N|NXxNY|720p..2160p   integer upscale: every pixel repeated N x N (or NX x NY)\n"
        "                           times with no filtering, or the largest N that fits the frame (padded)\n"
        "  --scaler nearest|bilinear|bicubic|lanczos     (default bicubic)\n"
        "  --crop X,Y,W,H           crop on the decoded picture (samples, lines)\n"
        "  --pad RRGGBB             padding colour (default 000000)\n"
        "  --luma notch|box|off     brightness filter (default notch; not used for S-Video)\n"
        "  --standard auto|pal|ntsc the colour standard (auto: the recording's FRESHVISION_STANDARD tag, else PAL)\n"
        "  --svideo auto|L|L,C      RGB recordings: auto = as recorded (S-Video with the recording's inputs;\n"
        "                           without its S-Video tag the green input as composite), L,C = S-Video with\n"
        "                           brightness on L and colour on C, L = input L as composite (r, g, b)\n"
        "  --no-average             no line-pair colour averaging\n"
        "  --remove-stripes         subtract the fixed clock stripes locked to the line (learned from flat areas)\n"
        "  --relock                 re-align rolling frames at their vertical sync, black per line (Apple IIc)\n"
        "  --colour-smoothing N     colour averaged over N carrier cycles (default 2; more fades fringes)\n"
        "  --edge-colour F          reduce colour where brightness has fine detail (0-1, default 0)\n"
        "  --mono                   black and white source (no colour burst); also from the tag FRESHVISION_MONO\n"
        "  --soft-text              two-colour text screens: blends of the two palette colours (no fringes)\n"
        "  --no-pll                 no carrier tracking; --pll-speed F (0..1, default 0.2)\n"
        "  --brightness F --contrast F --saturation F --hue DEG --gamma F   picture controls\n"
        "  --grid                   sample the machine's own pixels (pixel grid; crop is then in those pixels)\n"
        "  --snap                   ... and snap them to the machine's palette (implies --grid)\n"
        "  --c64-cells              ... and decide colours per 8x8 cell, the VIC-II's rule (C64 profile)\n"
        "  --tms-cells              ... and two colours per 8-pixel segment, the TMS9918's rule (MSX1 profiles)\n"
        "  --profile N|NAME         machine profile: 0 generic, 1 C64 PAL (default), 2 VIC-20, 3 C16/Plus/4,\n"
        "                           4 ZX Spectrum, 5 MSX1, 6 Amiga lores,\n"
        "                           7 BBC Micro Model B, 8 C64 NTSC, 9 Apple II, 10 MSX1 NTSC\n"
        "  --core F                 part of each pixel averaged (0.1..1, default 0.5); --colours N palette size\n"
        "  --palette machine|learned|FILE   snapping colours: the machine's documented palette (default),\n"
        "                           learned from the picture, or a palette file (.gpl or RRGGBB lines)\n"
        "  --settings NAME          load saved settings (saved in the GUI; later options override them)\n"
        "  --list-codecs            show the encoders that work on this machine\n"
        "  --info                   print what the recording says about itself\n");
}

bool parseSize(const std::string& v, OutputGeometry& g) {
    int a = 0, b = 0;
    if (v == "native") g.mode = SizeMode::Native;
    else if (v == "720p" || v == "1080p" || v == "1440p" || v == "2160p") {
        g.mode = SizeMode::Target;
        g.targetH = std::atoi(v.c_str());
    } else if (std::sscanf(v.c_str(), "43x%d", &a) == 1 && a > 0) {
        g.mode = SizeMode::Aspect43;
        g.factor = a;
    } else if (std::sscanf(v.c_str(), "%dx%d", &a, &b) == 2 && a > 0 && b > 0) {
        g.mode = SizeMode::Custom;
        g.customW = a;
        g.customH = b;
    } else {
        return false;
    }
    return true;
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& v) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                return false;
            }
            v = argv[++i];
            return true;
        };
        std::string v;
        if (a != "--encode" && a != "--out" && a != "--info" && a != "--list-codecs" && a != "--version" && !a.empty() && a[0] == '-')
            o.touched = true;
        if (a == "--settings") {
            // Loaded where it stands, so options after it override it.
            if (!next(v)) return false;
            CoderSettings cs{o.ui, o.geom, o.enc, fv::DisplayOptions{}, true};
            std::string e;
            if (!loadSettings(v, cs, e)) { std::fprintf(stderr, "--settings: %s\n", e.c_str()); return false; }
            o.ui = cs.ui, o.geom = cs.geom, o.enc = cs.enc;
            continue;
        }
        if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else if (a == "--version") {
            std::printf("freshCoder %s (freshVision)\n", FRESHVISION_VERSION);
            std::exit(0);
        } else if (a == "--encode") o.encode = true;
        else if (a == "--list-codecs") o.listCodecs = true;
        else if (a == "--info") o.info = true;
        else if (a == "--no-average") o.ui.averageLines = false;
        else if (a == "--remove-stripes") o.ui.removeStripes = true;
        else if (a == "--relock") o.ui.relock = true;
        else if (a == "--soft-text") o.ui.softText = true;
        else if (a == "--mono") o.ui.mono = true;
        else if (a == "--edge-colour") { if (!next(v)) return false; o.ui.edgeDesat = float(std::clamp(std::atof(v.c_str()), 0.0, 1.0)); }
        else if (a == "--colour-smoothing") { if (!next(v)) return false; o.ui.chromaCycles = std::clamp(std::atoi(v.c_str()), 1, 12); }
        else if (a == "--no-pll") o.ui.pll = false;
        else if (a == "--grid") o.ui.pixelGrid = true;
        else if (a == "--snap") o.ui.pixelGrid = true, o.ui.snap = true;
        else if (a == "--c64-cells") o.ui.pixelGrid = o.ui.snap = o.ui.c64Cells = true;
        else if (a == "--tms-cells") o.ui.pixelGrid = o.ui.snap = o.ui.tmsCells = true;
        else if (a == "--profile") {
            if (!next(v)) return false;
            const auto& profs = pal::machineProfiles();
            // A number is that profile; otherwise the first name containing the text
            // ("--profile 1" used to end on "MSX1", whose name contains a 1).
            int found = -1;
            for (size_t k = 0; k < profs.size() && found < 0; ++k)
                if (std::to_string(k) == v) found = int(k);
            for (size_t k = 0; k < profs.size() && found < 0; ++k)
                if (std::string(profs[k].name).find(v) != std::string::npos) found = int(k);
            if (found < 0) { std::fprintf(stderr, "--profile: a number or part of a name (see --help)\n"); return false; }
            o.ui.profile = found;
            if (profs[size_t(found)].colours > 0) o.ui.colours = profs[size_t(found)].colours;
        }
        else if (a == "--core") { if (!next(v)) return false; o.ui.gridCore = float(std::atof(v.c_str())); }
        else if (a == "--palette") {
            if (!next(v)) return false;
            if (v == "machine") o.ui.paletteSource = 0;
            else if (v == "learned") o.ui.paletteSource = 2;
            else {
                std::string e;
                if (!fv::loadPaletteFile(v, o.ui.customPalette, e)) { std::fprintf(stderr, "--palette: %s\n", e.c_str()); return false; }
                o.ui.paletteSource = 1;
            }
        }
        else if (a == "--colours") { if (!next(v)) return false; o.ui.colours = std::max(2, std::atoi(v.c_str())); }
        else if (a == "--integer") o.geom.integerScale = true;
        else if (a == "--pixel-exact") {
            if (!next(v)) return false;
            o.geom.mode = SizeMode::Integer;
            int nx = 0, ny = 0;
            if (std::sscanf(v.c_str(), "%dx%d", &nx, &ny) == 2 && nx > 0 && ny > 0) o.geom.intX = nx, o.geom.intY = ny, o.geom.intSame = false;
            else if (v == "720p" || v == "1080p" || v == "1440p" || v == "2160p") o.geom.intFrameH = std::atoi(v.c_str());
            else o.geom.intX = std::max(1, std::atoi(v.c_str()));
        }
        else if (a == "--out") { if (!next(o.out)) return false; }
        else if (a == "--codec") { if (!next(o.enc.codec)) return false; o.codecSet = true; }
        else if (a == "--quality") { if (!next(v)) return false; o.enc.quality = std::atoi(v.c_str()); o.qualitySet = true; }
        else if (a == "--preset") { if (!next(o.enc.preset)) return false; o.presetSet = true; }
        else if (a == "--chroma") { if (!next(v)) return false; o.enc.chroma444 = v != "420"; }
        else if (a == "--audio") { if (!next(o.enc.audio)) return false; }
        else if (a == "--size") {
            if (!next(v) || !parseSize(v, o.geom)) { std::fprintf(stderr, "--size: 43xN, native, WxH or 720p/1080p/1440p/2160p\n"); return false; }
        } else if (a == "--scaler") {
            if (!next(v)) return false;
            o.geom.scaler = v == "nearest" ? Scaler::Nearest : v == "bilinear" ? Scaler::Bilinear : v == "lanczos" ? Scaler::Lanczos : Scaler::Bicubic;
        } else if (a == "--crop") {
            if (!next(v) || std::sscanf(v.c_str(), "%d,%d,%d,%d", &o.geom.cropX, &o.geom.cropY, &o.geom.cropW, &o.geom.cropH) != 4) {
                std::fprintf(stderr, "--crop expects X,Y,W,H\n");
                return false;
            }
        } else if (a == "--pad") {
            if (!next(v)) return false;
            unsigned rgb = unsigned(std::strtoul(v.c_str(), nullptr, 16));
            o.geom.pad[0] = uint8_t(rgb >> 16), o.geom.pad[1] = uint8_t(rgb >> 8), o.geom.pad[2] = uint8_t(rgb);
        } else if (a == "--standard") {
            if (!next(v)) return false;
            if (v == "auto") o.ui.standardMode = 0;
            else if (v == "pal") o.ui.standardMode = 1;
            else if (v == "ntsc") o.ui.standardMode = 2;
            else {
                std::fprintf(stderr, "--standard: auto, pal or ntsc\n");
                return false;
            }
        } else if (a == "--svideo") {
            if (!next(v)) return false;
            auto ch = [](char c) { return c == 'r' || c == 'R' ? 0 : c == 'g' || c == 'G' ? 1 : c == 'b' || c == 'B' ? 2 : -1; };
            if (v == "auto") {
                o.ui.svideoMode = kSvideoFromRecording;
            } else if (v.size() == 1 && ch(v[0]) >= 0) {
                o.ui.svideoMode = kSvideoOff, o.ui.svideoLuma = ch(v[0]);
            } else if (v.size() == 3 && v[1] == ',' && ch(v[0]) >= 0 && ch(v[2]) >= 0 && v[0] != v[2]) {
                o.ui.svideoMode = kSvideoOn, o.ui.svideoLuma = ch(v[0]), o.ui.svideoChroma = ch(v[2]);
            } else {
                std::fprintf(stderr, "--svideo: auto, one input (r, g, b) or two different inputs L,C (e.g. g,b)\n");
                return false;
            }
        } else if (a == "--luma") {
            if (!next(v)) return false;
            o.ui.luma = v == "box" ? 0 : v == "off" ? 2 : 1;
        } else if (a == "--pll-speed") { if (!next(v)) return false; o.ui.pllBandwidth = float(std::atof(v.c_str())); }
        else if (a == "--brightness") { if (!next(v)) return false; o.ui.picture.brightness = float(std::atof(v.c_str())); }
        else if (a == "--contrast") { if (!next(v)) return false; o.ui.picture.contrast = float(std::atof(v.c_str())); }
        else if (a == "--saturation") { if (!next(v)) return false; o.ui.picture.saturation = float(std::atof(v.c_str())); }
        else if (a == "--hue") { if (!next(v)) return false; o.ui.picture.hue = float(std::atof(v.c_str())); }
        else if (a == "--gamma") { if (!next(v)) return false; o.ui.picture.gamma = float(std::atof(v.c_str())); }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        } else {
            o.in = a;
        }
    }
    // No --codec and the default (x264) isn't in this system's FFmpeg (Fedora's
    // and openSUSE's own builds leave it out): the first encoder that is.
    if (!o.codecSet && !findCodec(o.enc.codec)) {
        for (const char* id : {"libx264", "h264_nvenc", "libx265", "hevc_nvenc", "libsvtav1", "ffv1"})
            if (findCodec(id)) {
                std::printf("freshcoder: %s isn't available here, using %s (see --list-codecs)\n", o.enc.codec.c_str(), id);
                o.enc.codec = id;
                o.codecSet = true;
                break;
            }
    }
    // A chosen codec brings its own defaults for what wasn't given.
    if (o.codecSet && !o.qualitySet) o.enc.quality = -1;
    if (o.codecSet && !o.presetSet) o.enc.preset = "\x01";
    return true;
}

// Fills in a codec's own defaults for quality/preset the user didn't set.
void settleCodecDefaults(EncodeSettings& e) {
    const VideoCodec* c = findCodec(e.codec);
    if (!c) return;
    if (e.quality < 0 || (!c->lossless && (e.quality < c->qualityMin || e.quality > c->qualityMax))) e.quality = c->qualityDefault;
    if (e.preset == "\x01" || (!c->presets.empty() && std::find(c->presets.begin(), c->presets.end(), e.preset) == c->presets.end()))
        e.preset = c->presetDefault;
    if (c->presets.empty()) e.preset.clear();
}

// Output goes to the desktop's Videos folder (like freshVision's recordings),
// named after the recording.
std::string defaultOutPath(const std::string& in) {
    std::filesystem::path p(in);
    return (std::filesystem::path(fv::defaultVideoFolder()) / (p.stem().string() + "-freshcoder.mkv")).string();
}

void printInfo(const RecordingInfo& i) {
    std::printf("%s: %dx%d samples, %.2f fps, %.1f s (~%lld frames), video %s, audio %s\n", i.path.c_str(), i.width,
                i.height, i.fps, i.durationSec, (long long)i.frames, i.videoCodec.c_str(),
                i.hasAudio ? (std::to_string(i.audioRate) + " Hz, " + std::to_string(i.audioChannels) + " ch").c_str() : "none");
    std::printf("  Horizontal Size %d, Position %d -> %d samples per subcarrier cycle%s, active picture from sample %d, %d samples\n",
                i.horizontalSize, i.horizontalPosition, i.samplesPerCycle,
                i.samplesPerCycle ? "" : " (NOT a PAL sampling rate)", i.activeStart, i.activeLen);
    std::printf("  standard %s%s\n", pal::standardName(i.standard),
                i.standardTagged ? (i.standard == i.taggedStandard ? " (from the recording)" : " (chosen; the recording says otherwise)")
                                 : " (no standard tag: PAL unless chosen)");
    static const char* chName[] = {"red", "green", "blue"};
    if (i.rgb && i.svideoTagged)
        std::printf("  RGB recording, S-Video tag: brightness on %s, colour on %s\n", chName[i.tagLuma], chName[i.tagChroma]);
    else if (i.rgb)
        std::printf("  RGB recording without S-Video tag\n");
    if (i.svideo)
        std::printf("  decoded as S-Video: brightness from %s, colour from %s\n", chName[std::clamp(i.lumaChannel, 0, 2)],
                    chName[std::clamp(i.chromaChannel, 0, 2)]);
    else if (i.rgb)
        std::printf("  decoded as composite from the %s input (S-Video: --svideo L,C)\n", chName[std::clamp(i.lumaChannel, 0, 2)]);
}

int runBatch(Options& o) {
    Recording rec;
    std::string err;
    if (!rec.open(o.in, err)) {
        std::fprintf(stderr, "freshcoder: %s\n", err.c_str());
        return 1;
    }
    applyUiChoices(rec, o.ui);
    printInfo(rec.info());
    if (o.info) return 0;
    rec.close();
    if (o.out.empty()) o.out = defaultOutPath(o.in);
    o.enc.outPath = o.out;
    settleCodecDefaults(o.enc);
    EncodeProgress prog;
    std::atomic<bool> running{true};
    std::thread reporter([&] {
        int64_t last = -1;
        while (running) {
            int64_t d = prog.framesDone, t = prog.framesTotal;
            if (d != last && t > 0) {
                std::fprintf(stderr, "\r%s %lld/%lld frames", prog.pass == 1 ? "pass 1 (carrier)" : "pass 2 (encoding)",
                             (long long)d, (long long)t);
                last = d;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    bool ok = transcode(o.in, o.enc, o.ui, o.geom, prog, err);
    running = false;
    reporter.join();
    std::fprintf(stderr, "\n");
    if (!ok) {
        std::fprintf(stderr, "freshcoder: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s (%s, quality %d%s%s)\n", o.out.c_str(), o.enc.codec.c_str(), o.enc.quality,
                o.enc.preset.empty() ? "" : (", preset " + o.enc.preset).c_str(), o.enc.chroma444 ? ", 4:4:4 if possible" : ", 4:2:0");
    return 0;
}

// ---------------------------------------------------------------- GUI ---

struct Gui {
    Options& o;
    fv::Display disp;
    fv::DisplayOptions dopts;
    Recording rec;
    RecordingInfo info;
    bool loaded = false;
    std::string loadError;
    std::vector<uint8_t> samples, rgb;
    int64_t index = 0;
    bool playing = false;
    uint64_t playStartTicks = 0;  // playback clock: frame = playStartIndex + elapsed * fps
    int64_t playStartIndex = 0;
    Processor proc;
    std::string status;
    bool dirty = true;  // settings changed: process the current frame again
    bool previewAspectAuto = true;  // pixel aspect of the preview follows the output (Native: 4:3 shape)
    char settingsName[80] = "";
    std::string settingsMsg;
    char inBuf[1024] = "", outBuf[1024] = "";
    fv::FolderPicker picker;
    int pickerFor = 0;  // 1 = input file, 2 = output file
    // Encoding.
    std::thread worker;
    EncodeProgress prog;
    std::mutex encMu;                 // encError/encDone are written by the worker
    std::string encError, encDone;
    std::atomic<bool> encoding{false};
    explicit Gui(Options& opt) : o(opt) {}
};

// RGB recordings: S-Video or one input as composite (the recording's S-Video
// tag says which inputs carry what; without it the choice is the user's).
void drawRgbInputs(Gui& g) {
    static const char* inputs[] = {"red", "green", "blue"};
    const RecordingInfo& i = g.info;
    fv::PalDecodeUi& u = g.o.ui;
    if (i.svideoTagged) {
        ImGui::TextWrapped("RGB recording, S-Video: brightness on %s, colour on %s (from the recording).", inputs[i.tagLuma],
                           inputs[i.tagChroma]);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.8f, 0.3f, 1));
        ImGui::TextWrapped("RGB recording without S-Video tag: choose how its inputs are read.");
        ImGui::PopStyleColor();
    }
    const char* modes[] = {"As recorded", "S-Video", "Composite (one input)"};
    bool changed = false;
    const int before = u.svideoMode;
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Composite (one input)").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                            ImGui::GetFrameHeight());
    changed |= ImGui::Combo("RGB inputs", &u.svideoMode, modes, 3);
    if (changed && before == kSvideoFromRecording && i.svideoTagged)
        u.svideoLuma = i.tagLuma, u.svideoChroma = i.tagChroma;  // start from what the recording says
    if (u.svideoMode != kSvideoFromRecording) {
        ImGui::SetNextItemWidth(90);
        changed |= ImGui::Combo("brightness input", &u.svideoLuma, inputs, 3);
        if (u.svideoMode == kSvideoOn) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            changed |= ImGui::Combo("colour input", &u.svideoChroma, inputs, 3);
            if (u.svideoLuma == u.svideoChroma) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Brightness and colour need two different inputs.");
        }
    }
    if (changed) {
        applyUiChoices(g.rec, u);
        g.info = g.rec.info();
        g.rec.seek(g.index);  // read the frame again with the new inputs
        int64_t idx = 0;
        if (g.rec.next(g.samples, idx)) g.index = idx;
        g.dirty = true;
    }
    if (g.info.svideo)
        ImGui::TextDisabled("Decoding: S-Video, brightness from %s, colour from %s", inputs[g.info.lumaChannel],
                            inputs[g.info.chromaChannel]);
    else
        ImGui::TextDisabled("Decoding: composite from the %s input", inputs[g.info.lumaChannel]);
}

bool loadRecording(Gui& g, const std::string& path) {
    g.loadError.clear();
    if (!g.rec.open(path, g.loadError)) {
        g.loaded = false;
        return false;
    }
    applyUiChoices(g.rec, g.o.ui);
    g.info = g.rec.info();
    g.loaded = true;
    g.index = 0;
    std::snprintf(g.inBuf, sizeof g.inBuf, "%s", path.c_str());
    std::snprintf(g.outBuf, sizeof g.outBuf, "%s", defaultOutPath(path).c_str());
    if (!g.info.samplesPerCycle) g.loadError = "Horizontal Size " + std::to_string(g.info.horizontalSize) + " is not a " + std::string(pal::standardName(g.info.standard)) + " sampling rate";
    int64_t idx = 0;
    if (!g.rec.next(g.samples, idx)) g.loadError = "no frames";
    g.index = idx;
    g.dirty = true;
    g.disp.setTitle("freshCoder - " + std::filesystem::path(path).filename().string());
    return true;
}

void showFrame(Gui& g) {
    if (!g.loaded || g.samples.empty() || !g.info.samplesPerCycle) return;
    g.proc.configure(g.info, g.o.ui, g.o.geom, std::max(1, int(std::thread::hardware_concurrency() / 2)), false);
    if (!g.proc.process(g.samples.data(), g.index, nullptr, g.rgb, &g.status)) return;
    const OutputSize& s = g.proc.size();
    static const fv::PixelFormat rgb24{"rgb24", 0, fv::Layout::Rgb24, 3, true};
    std::string e;
    g.disp.setFrame(rgb24, uint32_t(s.canvasW), uint32_t(s.canvasH), 0, e);
    g.disp.upload(g.rgb.data(), size_t(s.canvasW) * 3);
    // Native mode keeps one pixel per sample (or per machine pixel): show it
    // 4:3 in the preview.
    const fv::PalGridStatus gst = g.proc.gridStatus();
    const double pa = sampleAspect(g.info.activeLen) * (g.o.ui.pixelGrid && gst.valid ? gst.period : 1.0);
    if (g.previewAspectAuto) g.dopts.pixelAspect = g.o.geom.mode == SizeMode::Native ? pa : 1.0;
    g.dirty = false;
}

CoderSettings collectSettings(const Gui& g) {
    CoderSettings cs;
    cs.ui = g.o.ui;
    cs.ui.requestRedetect = cs.ui.requestRelearn = false;
    cs.geom = g.o.geom;
    cs.enc = g.o.enc;
    cs.enc.outPath.clear();
    cs.preview = g.dopts;
    cs.previewAspectAuto = g.previewAspectAuto;
    return cs;
}

void applySettings(Gui& g, const CoderSettings& cs) {
    const std::string outPath = g.o.enc.outPath;
    g.o.ui = cs.ui;
    g.o.geom = cs.geom;
    g.o.enc = cs.enc;
    g.o.enc.outPath = outPath;
    settleCodecDefaults(g.o.enc);
    const fv::DisplayOptions keep = g.dopts;
    g.dopts.scaleMode = cs.preview.scaleMode;
    g.dopts.integerFactor = cs.preview.integerFactor;
    g.dopts.linearFilter = cs.preview.linearFilter;
    g.dopts.pixelAspect = cs.preview.pixelAspect;
    g.dopts.areaX = keep.areaX, g.dopts.areaY = keep.areaY, g.dopts.areaW = keep.areaW, g.dopts.areaH = keep.areaH;
    g.previewAspectAuto = cs.previewAspectAuto;
    g.o.ui.requestRedetect = g.o.ui.requestRelearn = true;
    g.dirty = true;
}

void startEncode(Gui& g) {
    if (g.encoding) return;
    {
        std::string e;
        saveSettings(kLastSettings, collectSettings(g), e);  // an encode's settings are kept for next time
    }
    if (g.worker.joinable()) g.worker.join();
    g.o.enc.outPath = g.outBuf;
    settleCodecDefaults(g.o.enc);
    {
        std::lock_guard<std::mutex> lk(g.encMu);
        g.encError.clear();
        g.encDone.clear();
    }
    g.prog.cancel = false;
    g.prog.framesDone = 0;
    g.prog.pass = 0;
    g.encoding = true;
    const std::string in = g.info.path;
    const EncodeSettings st = g.o.enc;
    const fv::PalDecodeUi ui = g.o.ui;
    const OutputGeometry geom = g.o.geom;
    // The encode uses the preview's pixel grid and palette, so the file
    // looks like what was seen.
    const fv::PalGridStatus gst = g.proc.gridStatus();
    pal::GridEstimate grid;
    grid.valid = gst.valid, grid.period = gst.period, grid.phase = gst.phase, grid.score = gst.score;
    std::vector<pal::Colour> palette;
    for (const auto& c : gst.palette) palette.push_back(c);
    const pal::Calibration cal = g.proc.calibration();
    g.worker = std::thread([&g, in, st, ui, geom, grid, palette, cal] {
        std::string err;
        bool ok = transcode(in, st, ui, geom, g.prog, err, &grid, &palette, &cal);
        {
            std::lock_guard<std::mutex> lk(g.encMu);
            g.encError = ok ? "" : err;
            g.encDone = ok ? "Written: " + st.outPath : "";
        }
        g.encoding = false;
    });
}

void drawControls(Gui& g) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float panelW = 460;
    ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, vp->WorkSize.y), ImGuiCond_Always);
    ImGui::Begin("freshCoder", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    if (ImGui::CollapsingHeader("Settings")) {
        ImGui::TextDisabled("Everything below (decoding, picture, grid, palette, output, encoding, preview).");
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##sname", g.settingsName, sizeof g.settingsName);
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            std::string e;
            g.settingsMsg = saveSettings(g.settingsName, collectSettings(g), e) ? std::string("saved: ") + g.settingsName : e;
        }
        ImGui::SameLine();
        if (ImGui::Button("Defaults")) {
            applySettings(g, CoderSettings{});
            g.settingsMsg = "defaults";
        }
        for (const std::string& n : listSettings()) {
            ImGui::PushID(n.c_str());
            ImGui::TextUnformatted(n.c_str());
            ImGui::SameLine(220);
            if (ImGui::SmallButton("Load")) {
                CoderSettings cs = collectSettings(g);
                std::string e;
                if (loadSettings(n, cs, e)) {
                    applySettings(g, cs);
                    std::snprintf(g.settingsName, sizeof g.settingsName, "%s", n.c_str());
                    g.settingsMsg = "loaded: " + n;
                } else {
                    g.settingsMsg = e;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                std::string e;
                g.settingsMsg = deleteSettings(n, e) ? "deleted: " + n : e;
            }
            ImGui::PopID();
        }
        if (!g.settingsMsg.empty()) ImGui::TextDisabled("%s", g.settingsMsg.c_str());
        ImGui::TextDisabled("The last settings are kept automatically (on exit and at every Encode).");
    }
    if (ImGui::CollapsingHeader("Recording", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-140);
        ImGui::InputText("##in", g.inBuf, sizeof g.inBuf);
        ImGui::SameLine();
        if (ImGui::Button("Open")) loadRecording(g, g.inBuf);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            std::string e;
            const std::string start = g.inBuf[0] ? std::string(g.inBuf) : fv::defaultVideoFolder() + "/";
            if (g.picker.startFile(start, "Open a freshVision raw recording", "Matroska | *.mkv", false, e)) g.pickerFor = 1;
        }
        if (g.loaded) {
            const RecordingInfo& i = g.info;
            ImGui::TextWrapped("%dx%d samples, %.2f fps, %.1f s, %s%s", i.width, i.height, i.fps, i.durationSec,
                               i.videoCodec.c_str(), i.hasAudio ? ", with sound" : ", no sound");
            ImGui::TextWrapped("Horizontal Size %d = %dx the %s colour subcarrier; picture from sample %d", i.horizontalSize,
                               i.samplesPerCycle, pal::standardName(i.standard), i.activeStart);
            {
                // The standard: as recorded (tag), or chosen (older recordings carry no tag).
                const char* modes[] = {"As recorded", "PAL", "NTSC"};
                ImGui::SetNextItemWidth(ImGui::CalcTextSize("As recorded").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                                        ImGui::GetFrameHeight());
                if (ImGui::Combo("standard", &g.o.ui.standardMode, modes, 3)) {
                    applyUiChoices(g.rec, g.o.ui);
                    g.info = g.rec.info();
                    g.loadError = g.info.samplesPerCycle ? "" : "Horizontal Size " + std::to_string(g.info.horizontalSize) +
                                                                     " is not a " + pal::standardName(g.info.standard) + " sampling rate";
                    g.rec.seek(g.index);
                    int64_t idx = 0;
                    if (g.rec.next(g.samples, idx)) g.index = idx;
                    g.dirty = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled(i.standardTagged ? "(recording: %s)" : "(no tag: %s assumed)",
                                    pal::standardName(i.standardTagged ? i.taggedStandard : pal::Standard::Pal));
            }
            // What was fixed when recording (freshVision's sampling and levels): shown, not settable here.
            auto tag = [&](const char* k) {
                auto it = i.tags.find(k);
                return it == i.tags.end() ? std::string("?") : it->second;
            };
            ImGui::TextDisabled("Set at capture time: line %s, window %s/%s, Green Brightness %s, Green Contrast %s",
                                tag("CTRL_HORIZONTAL_SIZE").c_str(), tag("CTRL_HORIZONTAL_POSITION").c_str(),
                                tag("CTRL_HORIZONTAL_TIMING").c_str(), tag("CTRL_GREEN_BRIGHTNESS").c_str(),
                                tag("CTRL_GREEN_CONTRAST").c_str());
            if (i.rgb) drawRgbInputs(g);
        }
        if (!g.loadError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g.loadError.c_str());
    }
    if (ImGui::CollapsingHeader("Decoding", ImGuiTreeNodeFlags_DefaultOpen)) {
        const fv::PalGridStatus gst = g.proc.gridStatus();
        g.o.ui.lumaFilterUnused = g.loaded && g.info.svideo;
        if (fv::drawPalDecodeControls(g.o.ui, true, &gst)) g.dirty = true;
        if (g.o.ui.requestRedetect) g.proc.redetectGrid(), g.o.ui.requestRedetect = false, g.dirty = true;
        if (g.o.ui.requestRelearn) g.proc.relearnPalette(), g.o.ui.requestRelearn = false, g.dirty = true;
        if (ImGui::Button("Auto levels")) {
            if (g.proc.autoLevels(g.o.ui.picture)) g.dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sets brightness/contrast from this frame: its darkest 0.5%% to black, brightest 0.1%% to 95%%.\n"
                              "(What freshVision's auto-tune does at capture time, done digitally here.)");
        if (!g.status.empty()) ImGui::TextDisabled("%s", g.status.c_str());
        ImGui::TextDisabled("(The preview measures the carrier per frame; encoding measures the whole recording first.)");
    }
    if (ImGui::CollapsingHeader("Preview")) {
        // freshVision's Display options, for the preview only (the file gets the output size below).
        int sm = int(g.dopts.scaleMode);
        const char* smodes[] = {"Integer", "Fit", "Stretch"};
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("scale", &sm, smodes, 3);
        g.dopts.scaleMode = fv::ScaleMode(sm);
        if (g.dopts.scaleMode == fv::ScaleMode::Integer) {
            ImGui::SameLine();
            const char* fs[] = {"Auto", "1x", "2x", "3x", "4x"};
            ImGui::SetNextItemWidth(80);
            ImGui::Combo("##pf", &g.dopts.integerFactor, fs, 5);
        }
        ImGui::Checkbox("linear filter", &g.dopts.linearFilter);
        ImGui::Checkbox("automatic pixel aspect", &g.previewAspectAuto);
        ImGui::BeginDisabled(g.previewAspectAuto);
        float par = float(g.dopts.pixelAspect);
        ImGui::SetNextItemWidth(160);
        if (ImGui::DragFloat("pixel aspect", &par, 0.005f, 0.2f, 3.0f, "%.4f")) g.dopts.pixelAspect = par;
        if (ImGui::Button("1:1")) g.dopts.pixelAspect = 1.0;
        ImGui::SameLine();
        if (ImGui::Button("4:3") && g.proc.size().canvasH > 0)
            g.dopts.pixelAspect = (4.0 / 3.0) / (double(g.proc.size().canvasW) / double(g.proc.size().canvasH));
        ImGui::EndDisabled();
    }
    if (ImGui::CollapsingHeader("Output picture", ImGuiTreeNodeFlags_DefaultOpen)) {
        OutputGeometry& geo = g.o.geom;
        const char* modes[] = {"4:3, N x the lines", "Native samples", "Custom size", "16:9 frame (padded)",
                               "Integer (pixel-exact)"};
        int m = int(geo.mode);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("size", &m, modes, 5)) {
            geo.mode = SizeMode(m), g.dirty = true;
            if (geo.mode == SizeMode::Integer) g.dopts.linearFilter = false;  // keep the blocks sharp in the preview too
        }
        if (geo.mode == SizeMode::Integer) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::SliderInt(geo.intSame ? "N (both ways)" : "N across", &geo.intX, 1, 12)) g.dirty = true;
            if (!geo.intSame) {
                ImGui::SetNextItemWidth(120);
                if (ImGui::SliderInt("N down", &geo.intY, 1, 12)) g.dirty = true;
            }
            if (ImGui::Checkbox("same both ways (square blocks)", &geo.intSame)) g.dirty = true;
            const char* frames[] = {"picture size", "720p frame", "1080p frame", "1440p frame", "2160p frame"};
            const int fh[] = {0, 720, 1080, 1440, 2160};
            int fi = 0;
            for (int k = 0; k < 5; ++k)
                if (geo.intFrameH == fh[k]) fi = k;
            ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("into", &fi, frames, 5)) geo.intFrameH = fh[fi], g.dirty = true;
            if (g.loaded) {
                const double pa = g.proc.pictureAspect();
                const int nx = geo.intX, ny = geo.intSame ? geo.intX : geo.intY;
                const double err = (pa * ny / nx - 1.0) * 100.0;  // how much wider/narrower than the 4:3 shape
                ImGui::TextDisabled("Blocks are %s; the picture is %.1f%% %s than true 4:3%s.", nx == ny ? "square" : "unequal",
                                    std::abs(err), err > 0 ? "narrower" : "wider",
                                    geo.intSame ? " (unequal factors can correct it)" : "");
            }
        }
        if (geo.mode == SizeMode::Aspect43) {
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderInt("N", &geo.factor, 1, 8)) g.dirty = true;
        } else if (geo.mode == SizeMode::Custom) {
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("width", &geo.customW, 0, 0)) g.dirty = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("height", &geo.customH, 0, 0)) g.dirty = true;
        } else if (geo.mode == SizeMode::Target) {
            const char* ts[] = {"720p", "1080p", "1440p", "2160p"};
            const int th[] = {720, 1080, 1440, 2160};
            int ti = geo.targetH == 720 ? 0 : geo.targetH == 1440 ? 2 : geo.targetH == 2160 ? 3 : 1;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("frame", &ti, ts, 4)) geo.targetH = th[ti], g.dirty = true;
        }
        if (geo.mode == SizeMode::Custom || geo.mode == SizeMode::Target)
            if (ImGui::Checkbox("whole multiples of the lines only (crisp lines)", &geo.integerScale)) g.dirty = true;
        const char* scalers[] = {"nearest", "bilinear", "bicubic", "lanczos"};
        int sc = int(geo.scaler);
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("scaler", &sc, scalers, 4)) geo.scaler = Scaler(sc), g.dirty = true;
        float pad[3] = {geo.pad[0] / 255.f, geo.pad[1] / 255.f, geo.pad[2] / 255.f};
        if (ImGui::ColorEdit3("padding", pad, ImGuiColorEditFlags_NoInputs)) {
            for (int k = 0; k < 3; ++k) geo.pad[k] = uint8_t(std::lround(pad[k] * 255));
            g.dirty = true;
        }
        ImGui::Text(g.o.ui.pixelGrid ? "Crop (machine pixels, lines; 0 size = to the edge):"
                                     : "Crop (samples, lines; 0 size = to the edge):");
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("x", &geo.cropX, 8, 64)) g.dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("y", &geo.cropY, 1, 8)) g.dirty = true;
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("w", &geo.cropW, 8, 64)) g.dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("h", &geo.cropH, 1, 8)) g.dirty = true;
        geo.cropX = std::max(0, geo.cropX), geo.cropY = std::max(0, geo.cropY);
        geo.cropW = std::max(0, geo.cropW), geo.cropH = std::max(0, geo.cropH);
        if (ImGui::Button("Reset crop")) geo.cropX = geo.cropY = geo.cropW = geo.cropH = 0, g.dirty = true;
        const OutputSize& s = g.proc.size();
        if (g.loaded)
            ImGui::Text("Decoded %dx%d%s -> output %dx%d", g.proc.decodedW(), g.proc.decodedH(),
                        g.o.ui.pixelGrid ? (" -> " + std::to_string(g.proc.pictureW()) + " machine pixels").c_str() : "",
                        s.canvasW, s.canvasH);
    }
    if (ImGui::CollapsingHeader("Encoding", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& codecs = availableCodecs();
        EncodeSettings& e = g.o.enc;
        const VideoCodec* cur = findCodec(e.codec);
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo("video", cur ? cur->label.c_str() : e.codec.c_str())) {
            for (const auto& c : codecs)
                if (ImGui::Selectable(c.label.c_str(), c.id == e.codec)) {
                    e.codec = c.id;
                    e.quality = -1;
                    e.preset = "\x01";
                    settleCodecDefaults(e);
                }
            ImGui::EndCombo();
        }
        cur = findCodec(e.codec);
        if (cur && !cur->lossless && !cur->qualityName.empty()) {
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt(cur->qualityName.c_str(), &e.quality, cur->qualityMin, cur->qualityMax);
            ImGui::SameLine();
            ImGui::TextDisabled("(lower = better)");
        } else if (cur && cur->lossless) {
            ImGui::TextDisabled("Lossless: no quality setting.");
        }
        if (cur && !cur->presets.empty()) {
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("preset", e.preset.c_str())) {
                for (const auto& p : cur->presets)
                    if (ImGui::Selectable(p.c_str(), p == e.preset)) e.preset = p;
                ImGui::EndCombo();
            }
        }
        if (cur && cur->can444 && cur->can420) ImGui::Checkbox("4:4:4 colour (no colour subsampling)", &e.chroma444);
        const char* audios[] = {"auto", "pcm", "flac", "aac", "opus", "none"};
        int ai = 0;
        for (int k = 0; k < 6; ++k)
            if (e.audio == audios[k]) ai = k;
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("sound", &ai, audios, 6)) e.audio = audios[ai];
        ImGui::SetNextItemWidth(-140);
        ImGui::InputText("##out", g.outBuf, sizeof g.outBuf);
        ImGui::SameLine();
        if (ImGui::Button("Save as...")) {
            std::string err;
            if (g.picker.startFile(g.outBuf, "Output file", "Video | *.mkv *.mp4 *.mov", true, err)) g.pickerFor = 2;
        }
        ImGui::BeginDisabled(!g.loaded || g.encoding || !g.info.samplesPerCycle);
        if (ImGui::Button("Encode")) startEncode(g);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Open folder")) {
            std::string e;
            std::filesystem::path folder = std::filesystem::path(g.outBuf).parent_path();
            if (folder.empty()) folder = fv::defaultVideoFolder();
            if (!fv::openInFileBrowser(folder.string(), e)) {
                std::lock_guard<std::mutex> lk(g.encMu);
                g.encError = e;
            }
        }
        if (g.encoding) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) g.prog.cancel = true;
            const int64_t t = std::max<int64_t>(1, g.prog.framesTotal);
            const float f = float(g.prog.framesDone) / float(t);
            char label[96];
            std::snprintf(label, sizeof label, "%s %lld / %lld", g.prog.pass == 1 ? "pass 1: carrier" : "pass 2: encoding",
                          (long long)g.prog.framesDone, (long long)t);
            ImGui::ProgressBar(std::min(1.0f, f), ImVec2(-1, 0), label);
        }
        {
            std::lock_guard<std::mutex> lk(g.encMu);
            if (!g.encError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g.encError.c_str());
            if (!g.encDone.empty()) ImGui::TextWrapped("%s", g.encDone.c_str());
        }
    }
    ImGui::End();

    // Player bar under the picture.
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + panelW, vp->WorkPos.y + vp->WorkSize.y - 60), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - panelW, 60), ImGuiCond_Always);
    ImGui::Begin("Player", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    ImGui::BeginDisabled(!g.loaded);
    auto seekTo = [&](int64_t f) {
        g.rec.seek(f);
        int64_t idx = 0;
        if (g.rec.next(g.samples, idx)) g.index = idx;
        g.dirty = true;
    };
    if (ImGui::Button("|<")) seekTo(0);
    ImGui::SameLine();
    if (ImGui::Button("<")) seekTo(std::max<int64_t>(0, g.index - 1));
    ImGui::SameLine();
    if (ImGui::Button(g.playing ? "Pause" : "Play ")) g.playing = !g.playing;
    ImGui::SameLine();
    if (ImGui::Button(">")) {
        int64_t idx = 0;
        if (g.rec.next(g.samples, idx)) g.index = idx, g.dirty = true;
    }
    ImGui::SameLine();
    int frame = int(g.index);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##seek", &frame, 0, int(std::max<int64_t>(0, g.info.frames - 1)), "frame %d")) seekTo(frame);
    ImGui::EndDisabled();
    ImGui::End();
}

int runGui(Options& o) {
    Gui g(o);
    std::string err;
    if (!g.disp.create(1600, 900, false, true, err)) {
        std::fprintf(stderr, "freshcoder: %s\n", err.c_str());
        return 1;
    }
    g.disp.setTitle("freshCoder");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(g.disp.window(), g.disp.glContext());
    ImGui_ImplOpenGL3_Init("#version 150");
    settleCodecDefaults(o.enc);
    g.dopts.scaleMode = fv::ScaleMode::Fit;
    g.dopts.linearFilter = true;
    if (!o.touched) {
        // The last session's settings, unless the command line set some.
        CoderSettings cs = collectSettings(g);
        std::string e;
        if (loadSettings(kLastSettings, cs, e)) applySettings(g, cs);
    }
    if (!o.in.empty()) loadRecording(g, o.in);

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_KEYDOWN && !ImGui::GetIO().WantCaptureKeyboard) {
                if (ev.key.keysym.sym == SDLK_SPACE) g.playing = !g.playing;
                if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) quit = true;
            }
        }
        std::string chosen, e;
        if (g.picker.poll(chosen, e) && !chosen.empty()) {
            if (g.pickerFor == 1) loadRecording(g, chosen);
            else if (g.pickerFor == 2) std::snprintf(g.outBuf, sizeof g.outBuf, "%s", chosen.c_str());
        }
        if (g.playing && g.loaded) {
            // Frames follow a clock (the screen's 60 Hz loop and the
            // recording's 50 fps don't divide: advancing one frame per 20 ms
            // tick played at ~30 fps); behind, it skips ahead.
            const uint64_t now = SDL_GetTicks64();
            if (g.playStartTicks == 0) g.playStartTicks = now, g.playStartIndex = g.index;
            const int64_t target = g.playStartIndex + int64_t(double(now - g.playStartTicks) * g.info.fps / 1000.0);
            int64_t idx = g.index;
            bool ok = true;
            while (ok && idx < target) ok = g.rec.next(g.samples, idx);
            if (idx != g.index) g.index = idx, g.dirty = true;
            if (!ok) g.playing = false;
        } else {
            g.playStartTicks = 0;
        }
        if (g.dirty) showFrame(g);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        drawControls(g);
        ImGui::Render();

        int ww = 0, wh = 0;
        SDL_GL_GetDrawableSize(g.disp.window(), &ww, &wh);
        g.dopts.areaX = 460;
        g.dopts.areaY = 0;
        g.dopts.areaW = std::max(1, ww - 460);
        g.dopts.areaH = std::max(1, wh - 60);
        g.disp.beginFrame();
        if (g.loaded) g.disp.drawVideo(g.dopts);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        g.disp.endFrame();
        if (!g.playing) SDL_Delay(10);
    }
    if (g.encoding) g.prog.cancel = true;
    if (g.worker.joinable()) g.worker.join();
    {
        std::string e;
        saveSettings(kLastSettings, collectSettings(g), e);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) {
        usage();
        return 64;
    }
    if (o.listCodecs) {
        for (const auto& c : availableCodecs())
            std::printf("%-12s %-36s %s%s%s\n", c.id.c_str(), c.label.c_str(),
                        c.lossless ? "lossless" : (c.qualityName + " " + std::to_string(c.qualityMin) + "-" + std::to_string(c.qualityMax) +
                                                   " (default " + std::to_string(c.qualityDefault) + ")").c_str(),
                        c.can444 ? ", 4:4:4" : "", c.can420 ? ", 4:2:0" : "");
        return 0;
    }
    if (o.encode || o.info) {
        if (o.in.empty()) {
            std::fprintf(stderr, "freshcoder: --encode/--info need an input file\n");
            return 64;
        }
        return runBatch(o);
    }
#ifdef _WIN32
    SDL_SetMainReady();
#endif
    fv::hideOwnConsole();
    return runGui(o);
}

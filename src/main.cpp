// SPDX-License-Identifier: GPL-3.0-or-later
// freshvision: capture-to-display program. The first version was
// command-line only; then came the ImGui shell, live
// control/geometry panels and the shader/PBO display path on top of it.
#include <algorithm>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <future>
#include <vector>

#include <unistd.h>

#include <SDL.h>

#include "audio.h"
#include "capture.h"
#include "controls.h"
#include "desktop.h"
#include "display.h"
#include "formats.h"
#include "gui.h"
#include "grid.hpp"
#include "pal.hpp"
#include "presets.h"
#include "machines.h"
#include "nodevice.h"
#include "rgbtune.h"
#include "recorder.h"
#include "safety.h"
#include "v4l2_util.h"

using namespace fv;

namespace {

volatile std::sig_atomic_t g_quit = 0;
volatile std::sig_atomic_t g_quitSignal = 0;  // which signal ended the run (0: not a signal)
void onSignal(int sig) {
    g_quit = 1;
    g_quitSignal = sig;
}

struct Options {
    std::string device = "/dev/video2";
    bool deviceGiven = false;  // --device on the command line; else chosen at start (nodevice.h)
    bool listDevices = false;
    std::string format = "auto";
    uint32_t width = 0, height = 0;
    std::vector<std::string> ctrls;
    bool info = false;
    bool noStream = false;
    bool vsync = true;
    bool fullscreen = false;
    bool pollSource = true;
    bool overrideGuard = false;
    bool swapRb = false;
    bool noGui = false;
    bool palDecode = false;  // start with PAL composite decoding on (g8 only)
    int palTiming = -1;      // 4/8/10: set that PAL sampling once streaming; 0: the baseline; -1: leave the timing alone
    int palLine = 1135;      // samples per line at 4x (1134 for a C64; NTSC 910, Apple II/MSX1 912)
    bool palLineGiven = false;
    int standard = 0;        // --standard pal|ntsc: 0 PAL, 1 NTSC
    bool palAverageLines = false;
    bool palStripes = false;     // --pal-stripes
    bool palRelock = false;      // --pal-relock
    int buffers = 0;             // --buffers N (0 = the default 4)
    double frameRate = 0;        // --frame-rate F: VIDIOC_S_PARM before streaming (0 = not set)
    bool palAutoTune = false;
    bool syncPictureToSound = false;
    std::vector<std::string> audioSources;  // --audio-source DEVICE[,left|right|stereo], added at start
    bool palGrid = false, palSnap = false;
    int palProfile = 1;      // pal::machineProfiles() index (1 = C64 PAL)
    int palLuma = 1;         // 0 box, 1 notch (default), 2 off
    int palThreads = 0;      // 0 = half the cores
    std::string palDumpPath; // save one raw g8 frame (rows of samples) 3 s after the timing settles
    std::string dumpFramePath; // --dump-frame: one raw frame of any format, same moment
    Rect crop;
    std::string recordAudioPath;  // empty = don't auto-start recording
    std::string recordVideoPath;  // empty = don't auto-start recording
    std::string recordEncoder;    // empty = the GUI's default (Auto)
    std::string savePresetName;   // empty = don't auto-save a preset at startup
    std::string loadPresetName;   // empty = don't auto-load a preset at startup
    std::string presetGroup;      // optional group for --save-preset/--load-preset; empty = ungrouped
    std::string machine;          // --machine NAME[/MODE]: apply at start
    int autoSwitch = -1;          // --auto-switch / --no-auto-switch; -1 = the saved choice
    bool listMachines = false;
    bool restoreMachines = false;
    bool rgbAutoTune = false;
    bool rgbAutoCrop = false;
    int svideo = -1;         // --svideo [L,C]: -1 = not given; else luma channel * 3 + chroma channel
};

void usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --version                print the version and exit\n"
        "  --device PATH            capture device (default: the last one that worked, else the\n"
        "                           first Vision card input with a signal; see --list-devices)\n"
        "  --list-devices           list the capture devices, their inputs and signal, then exit\n"
        "  --format NAME            auto (default), rgb32, rgb24, bgr24, uyvy, or an explicit\n"
        "                           name: xbgr32 abgr32 bgr4 xrgb32 argb32 rgb4\n"
        "  --size WxH               request a size (default: the driver's current size)\n"
        "  --ctrl \"NAME=VALUE\"      set a driver control by name (repeatable); see --info\n"
        "  --info                   print device info and all controls, then exit\n"
        "  --no-stream              apply --ctrl and exit without streaming\n"
        "  --no-vsync               start with vsync off\n"
        "  --fullscreen             start fullscreen\n"
        "  --no-poll                do not poll DV timings / G_FMT for mode changes\n"
        "  --crop X,Y,W,H           capture only this region of the source (selection API)\n"
        "  --swap-rb                swap red and blue of the RGB formats\n"
        "  --no-gui                 start with the ImGui panels hidden (Tab shows them)\n"
        "  --pal-decode             show a g8 Sync-on-Green capture decoded as PAL colour\n"
        "  --pal-timing 4x|8x|10x|sog  once streaming, sample at that multiple of the PAL colour\n"
        "                           subcarrier (width 1024/2048/2560), or put the driver's\n"
        "                           Sync-on-Green baseline timing back (524/113/384).\n"
        "                           (12x-16x are left out: the card doesn't deliver a fully\n"
        "                           correct frame there)\n"
        "  --pal-line N             samples per line at 4x (default 1135 = standard PAL; 1134 = C64)\n"
        "  --standard pal|ntsc      the colour TV standard for decoding (default pal); NTSC: 910 samples per line\n"
        "                           at 4x unless --pal-line says otherwise (912 for an Apple II or MSX1)\n"
        "  --pal-average-lines      average the colour of line pairs (PAL delay-line method)\n"
        "  --pal-stripes            remove the fixed clock stripes locked to the line (C64: character columns)\n"
        "  --pal-relock             re-align rolling frames at their vertical sync, black per line (Apple IIc)\n"
        "  --buffers N              V4L2 capture buffers (2-32, default 4)\n"
        "  --frame-rate F           ask the driver for F frames per second (VIDIOC_S_PARM) before streaming\n"
        "  --pal-autotune           auto-tune: line length, black level and swing (g8 composite)\n"
        "  --sync-picture-to-sound  hold the picture back to line up with the monitor sound (adds latency)\n"
        "  --audio-source DEV[,left|right|stereo]  add an ALSA capture device as a sound source at start\n"
        "                           (e.g. hw:CARD=PCH,DEV=0,left; repeatable; like ticking it in the Audio section)\n"
        "  --pal-grid / --pal-snap  sample the machine's pixels / and snap them to its palette\n"
        "  --pal-profile N          machine profile: 0 generic, 1 C64 PAL (default), 2 VIC-20, 3 C16/Plus/4,\n"
        "                           4 ZX Spectrum, 5 MSX1, 6 Amiga lores,\n"
        "                           7 BBC Micro Model B, 8 C64 NTSC, 9 Apple II, 10 MSX1 NTSC\n"
        "  --pal-luma notch|box|off brightness filter: notch (default, sharp), box (soft), off (raw)\n"
        "  --pal-threads N          decoder threads (default: half the cores)\n"
        "  --pal-dump FILE          save one raw g8 frame to FILE (for paltool decode)\n"
        "  --svideo [L,C]           S-Video on an RGB capture: brightness on input L, colour on C (r, g or b;\n"
        "                           default g,b: Y on Green, C on Blue); decoded like composite (--pal-decode, --pal-timing)\n"
        "  --dump-frame FILE        save one raw frame of any format to FILE (rows of width x bytes per\n"
        "                           pixel, no padding; the byte order is printed), 3 s after the timing settles\n"
        "  --record-audio FILE      start recording the audio mix to a WAV file immediately\n"
        "  --record-video FILE      start recording video (+ audio mix, if any) to an .mkv immediately\n"
        "  --record-encoder NAME    auto (default), x264, nvenc, ffv1 or raw (ffv1/raw: lossless;\n"
        "                           a g8 capture is kept as one channel of raw samples)\n"
        "  --save-preset NAME       save the current settings as a click-only mode NAME, then exit\n"
        "  --load-preset NAME       apply the mode NAME at startup, before anything else runs\n"
        "  --preset-group GROUP     the machine for --save-preset/--load-preset (e.g. \"C64\"); default \"My setups\"\n"
        "  --machine NAME[/MODE]    apply a machine file's mode at start (file name or machine name;\n"
        "                           without /MODE the best-matching mode). Files: ~/.config/freshvision/machines\n"
        "  --auto-switch, --no-auto-switch\n"
        "                           switch to the matching machine mode when the source changes mode\n"
        "                           (default: the choice saved from the Machines section, on at first)\n"
        "  --list-machines          print the machine files, their modes and any errors, then exit\n"
        "  --restore-machines       write the built-in machine files that are missing (never overwrites), then exit\n"
        "  --rgb-autotune           analog RGB: tune Phase and centre the picture\n"
        "  --rgb-autocrop           ... and crop to the picture (off by default: with a black border\n"
        "                           the crop follows what is drawn, not the machine's screen)\n"
        "  --i-know-it-may-freeze   allow RGB capture without intel_iommu=off\n"
        "keys: F/F11 fullscreen, V vsync, Tab show/hide panels, Q/Esc quit\n",
        argv0);
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string val;
        bool inlineVal = false;
        size_t eq = a.find('=');
        if (a.compare(0, 2, "--") == 0 && eq != std::string::npos) {
            val = a.substr(eq + 1);
            a = a.substr(0, eq);
            inlineVal = true;
        }
        auto need = [&](std::string& out) {
            if (inlineVal) {
                out = val;
                return true;
            }
            if (i + 1 >= argc) {
                std::fprintf(stderr, "option %s needs a value\n", a.c_str());
                return false;
            }
            out = argv[++i];
            return true;
        };

        std::string v;
        if (a == "--device") {
            if (!need(o.device)) return false;
            o.deviceGiven = true;
        } else if (a == "--list-devices") {
            o.listDevices = true;
        } else if (a == "--format") {
            if (!need(o.format)) return false;
            std::transform(o.format.begin(), o.format.end(), o.format.begin(), [](unsigned char c) { return std::tolower(c); });
        } else if (a == "--size") {
            if (!need(v)) return false;
            unsigned w = 0, h = 0;
            if (std::sscanf(v.c_str(), "%ux%u", &w, &h) != 2 || !w || !h) {
                std::fprintf(stderr, "--size expects WxH\n");
                return false;
            }
            o.width = w;
            o.height = h;
        } else if (a == "--crop") {
            if (!need(v)) return false;
            int x = 0, y = 0;
            unsigned w = 0, h = 0;
            if (std::sscanf(v.c_str(), "%d,%d,%u,%u", &x, &y, &w, &h) != 4 || !w || !h) {
                std::fprintf(stderr, "--crop expects X,Y,W,H\n");
                return false;
            }
            o.crop = Rect{x, y, w, h, true};
        } else if (a == "--swap-rb") {
            o.swapRb = true;
        } else if (a == "--no-gui") {
            o.noGui = true;
        } else if (a == "--pal-dump") {
            if (!need(o.palDumpPath)) return false;
        } else if (a == "--svideo") {
            o.svideo = 1 * 3 + 2;  // Y on Green, C on Blue (the default wiring)
            // An optional L,C right after it.
            std::string next = inlineVal ? val : (i + 1 < argc ? std::string(argv[i + 1]) : std::string());
            auto chIdx = [](char c) { return c == 'r' ? 0 : c == 'g' ? 1 : c == 'b' ? 2 : -1; };
            if (next.size() == 3 && next[1] == ',' && chIdx(next[0]) >= 0 && chIdx(next[2]) >= 0 && next[0] != next[2]) {
                o.svideo = chIdx(next[0]) * 3 + chIdx(next[2]);
                if (!inlineVal) ++i;
            } else if (inlineVal) {
                std::fprintf(stderr, "--svideo expects L,C with two different channels of r, g, b (e.g. g,r)\n");
                return false;
            }
        } else if (a == "--dump-frame") {
            if (!need(o.dumpFramePath)) return false;
        } else if (a == "--pal-decode") {
            o.palDecode = true;
        } else if (a == "--pal-timing") {
            if (!need(v)) return false;
            if (v == "4x") o.palTiming = 4;
            else if (v == "8x") o.palTiming = 8;
            else if (v == "10x") o.palTiming = 10;
            else if (v == "6x") o.palTiming = 6;    // NTSC only (an MSX pixel = 4 samples)
            else if (v == "12x") o.palTiming = 12;  // NTSC only (42.95 MHz; an MSX pixel = 8 samples)
            else if (v == "sog") o.palTiming = 0;
            else if (v == "16x") {
                // Removed until the reason is known: the card
                // doesn't really sample at 16x on this input.
                std::fprintf(stderr, "--pal-timing 16x has been removed: the card delivers unusable samples at 16x; "
                                     "10x is the maximum\n");
                return false;
            } else {
                std::fprintf(stderr, "--pal-timing expects 4x, 8x, 10x, 6x/12x (NTSC) or sog\n");
                return false;
            }
        } else if (a == "--pal-line") {
            if (!need(v)) return false;
            o.palLine = std::atoi(v.c_str());
            o.palLineGiven = true;
            if (o.palLine < 850 || o.palLine > 1300) {
                std::fprintf(stderr, "--pal-line expects samples per line at 4x, e.g. 1135 or 1134 (PAL), 910 or 912 (NTSC)\n");
                return false;
            }
        } else if (a == "--standard") {
            if (!need(v)) return false;
            if (v == "pal") o.standard = 0;
            else if (v == "ntsc") o.standard = 1;
            else {
                std::fprintf(stderr, "--standard expects pal or ntsc\n");
                return false;
            }
        } else if (a == "--pal-average-lines") {
            o.palAverageLines = true;
        } else if (a == "--pal-stripes") {
            o.palStripes = true;
        } else if (a == "--buffers") {
            if (!need(v)) return false;
            o.buffers = std::atoi(v.c_str());
        } else if (a == "--frame-rate") {
            if (!need(v)) return false;
            o.frameRate = std::atof(v.c_str());
        } else if (a == "--pal-relock") {
            o.palRelock = true;
        } else if (a == "--pal-autotune") {
            o.palAutoTune = true;
        } else if (a == "--sync-picture-to-sound") {
            o.syncPictureToSound = true;
        } else if (a == "--audio-source") {
            if (!need(v)) return false;
            o.audioSources.push_back(v);
        } else if (a == "--pal-grid") {
            o.palGrid = true;
        } else if (a == "--pal-snap") {
            o.palGrid = o.palSnap = true;
        } else if (a == "--pal-profile") {
            if (!need(v)) return false;
            o.palProfile = std::max(0, std::atoi(v.c_str()));
        } else if (a == "--pal-luma") {
            if (!need(v)) return false;
            if (v == "box") o.palLuma = 0;
            else if (v == "notch") o.palLuma = 1;
            else if (v == "off") o.palLuma = 2;
            else {
                std::fprintf(stderr, "--pal-luma expects notch, box or off\n");
                return false;
            }
        } else if (a == "--pal-threads") {
            if (!need(v)) return false;
            o.palThreads = std::max(1, std::atoi(v.c_str()));
        } else if (a == "--record-audio") {
            if (!need(o.recordAudioPath)) return false;
        } else if (a == "--record-video") {
            if (!need(o.recordVideoPath)) return false;
        } else if (a == "--record-encoder") {
            if (!need(o.recordEncoder)) return false;
            if (o.recordEncoder != "auto" && o.recordEncoder != "x264" && o.recordEncoder != "nvenc" &&
                o.recordEncoder != "ffv1" && o.recordEncoder != "raw") {
                std::fprintf(stderr, "--record-encoder expects auto, x264, nvenc, ffv1 or raw\n");
                return false;
            }
        } else if (a == "--save-preset") {
            if (!need(o.savePresetName)) return false;
        } else if (a == "--load-preset") {
            if (!need(o.loadPresetName)) return false;
        } else if (a == "--preset-group") {
            if (!need(o.presetGroup)) return false;
        } else if (a == "--machine") {
            if (!need(o.machine)) return false;
        } else if (a == "--auto-switch") {
            o.autoSwitch = 1;
        } else if (a == "--no-auto-switch") {
            o.autoSwitch = 0;
        } else if (a == "--list-machines") {
            o.listMachines = true;
        } else if (a == "--restore-machines") {
            o.restoreMachines = true;
        } else if (a == "--rgb-autotune") {
            o.rgbAutoTune = true;
        } else if (a == "--rgb-autocrop") {
            o.rgbAutoCrop = true;
        } else if (a == "--ctrl") {
            if (!need(v)) return false;
            o.ctrls.push_back(v);
        } else if (a == "--info") {
            o.info = true;
        } else if (a == "--no-stream") {
            o.noStream = true;
        } else if (a == "--no-vsync") {
            o.vsync = false;
        } else if (a == "--fullscreen") {
            o.fullscreen = true;
        } else if (a == "--no-poll") {
            o.pollSource = false;
        } else if (a == "--i-know-it-may-freeze") {
            o.overrideGuard = true;
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else if (a == "--version") {
            std::printf("freshVision %s\n", FRESHVISION_VERSION);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

// Formats to try, in order of preference, limited to what the driver offers.
std::vector<const PixelFormat*> pickCandidates(const std::string& want, const std::vector<uint32_t>& driver,
                                               std::string& err) {
    std::vector<const PixelFormat*> out;
    auto offered = [&](const PixelFormat* pf) {
        return std::find(driver.begin(), driver.end(), pf->fourcc) != driver.end();
    };
    auto add32 = [&] {
        for (uint32_t fcc : driver) {
            const PixelFormat* pf = findFormat(fcc);
            if (pf && pf->rgb && pf->bytesPerPixel == 4) out.push_back(pf);
        }
    };
    auto addName = [&](const char* n) {
        const PixelFormat* pf = findFormat(std::string(n));
        if (pf && offered(pf)) out.push_back(pf);
    };

    if (want == "auto") {
        add32();
        addName("bgr24");
        addName("rgb24");
        addName("uyvy");
    } else if (want == "rgb32") {
        add32();
    } else {
        const PixelFormat* pf = findFormat(want);
        if (!pf) {
            err = "unknown format '" + want + "'";
            return {};
        }
        if (!offered(pf)) {
            err = "the driver does not enumerate '" + want + "' (see --info)";
            return {};
        }
        out.push_back(pf);
    }
    if (out.empty()) err = "the driver offers no format freshVision can display (see --info)";
    return out;
}

double median(std::vector<double> v) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
}

double maxOf(const std::vector<double>& v) {
    return v.empty() ? NAN : *std::max_element(v.begin(), v.end());
}

double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return NAN;
    double m = 0;
    for (double x : v) m += x;
    m /= double(v.size());
    double s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / double(v.size()));
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return NAN;
    double s = 0;
    for (double x : v) s += x;
    return s / double(v.size());
}

// Per-second window of measurements (main thread); only updated for
// iterations that actually displayed a new captured frame.
struct Window {
    unsigned displayed = 0;
    std::vector<double> ageDequeue;
    std::vector<double> ageTake;
    std::vector<double> agePresent;
    std::vector<double> presentDt;
    std::vector<double> uploadMs;
    std::vector<double> swapMs;
};

}  // namespace

int main(int argc, char** argv) {
    // One line at a time even into a file: the line before a driver crash
    // must not be lost in the buffer.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        usage(argv[0]);
        return 64;
    }
    // 0.1.0 settings (built-in copies in the machine folder, presets) are
    // converted once into "My modes" (machines.cpp, migrateSettings).
    {
        std::string report;
        if (migrateSettings(report)) std::printf("settings converted to Built-in / My modes:\n%s", report.c_str());
    }
    if (opt.restoreMachines) {
        // Built-in machines ship with the program now; nothing to write.
        std::printf("built-in machines: %s (shipped with freshVision, always available)\nyour modes: %s\n",
                    builtInMachinesDir().empty() ? "(not found)" : builtInMachinesDir().c_str(), machinesDir().c_str());
        return builtInMachinesDir().empty() ? 1 : 0;
    }
    if (opt.listMachines) {
        std::vector<std::string> errors;
        std::vector<Machine> ms = loadMachines(errors);
        std::printf("built-in machines: %s\nyour modes: %s\n", builtInMachinesDir().c_str(), machinesDir().c_str());
        for (const Machine& mc : ms) {
            std::printf("  %s  (%s.conf%s%s)\n", mc.name.c_str(), mc.id.c_str(), mc.builtIn ? ", built-in" : "",
                        mc.mine ? ", yours" : "");
            for (const MachineMode& md : mc.modes) {
                const ModeMatch& c = md.match;
                std::printf("    %s mode \"%s\"%s: input %s, signal %s, size %ux%u, line %d, refresh %.3f +-%.3f, "
                            "cycles/line %.2f +-%.2f\n",
                            md.origin == ModeOrigin::BuiltIn ? "[built-in]" : md.origin == ModeOrigin::Mine ? "[mine]    " : "[replaced]",
                            md.name.c_str(), c.manual ? " (manual)" : "", c.input.empty() ? "any" : c.input.c_str(),
                            c.signal.empty() ? "any" : c.signal.c_str(), c.width, c.height, c.lineTotal, c.refreshHz,
                            c.refreshTol, c.cyclesPerLine, c.cyclesTol);
            }
        }
        for (const std::string& e : errors) std::printf("  error: %s\n", e.c_str());
        return errors.empty() ? 0 : 1;
    }

    if (opt.listDevices) {
        printDevices();
        return 0;
    }
    // Which device: --device, else the last one that worked, else the first
    // Vision input with a signal. Nothing to open: a help window (with the
    // devices found and freshCoder) instead of a bare error, unless the run
    // is a scripted one (--info, --no-stream).
    if (!opt.deviceGiven) opt.device = chooseStartDevice();
    Capture cap;
    std::string err;
    while (opt.device.empty() || !cap.open(opt.device, err)) {
        if (opt.device.empty()) err = "no Vision capture device found (see --list-devices)";
        if (opt.info || opt.noStream) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "%s%s%s\n", opt.device.c_str(), opt.device.empty() ? "" : ": ", err.c_str());
        std::string chosen;
        auto launchCoder = [] {
            std::string exe = "freshcoder", e;
            std::error_code ec;
            const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
            if (!ec && std::filesystem::exists(self.parent_path() / "freshcoder", ec)) exe = (self.parent_path() / "freshcoder").string();
            if (!launchDetached({exe}, e)) std::fprintf(stderr, "freshCoder: %s\n", e.c_str());
        };
        if (!runNoDeviceScreen(opt.device, err, chosen, launchCoder)) return 1;
        opt.device = chosen;
        err.clear();
    }
    cap.setOverrideGuard(opt.overrideGuard);
    if (opt.buffers > 0) cap.setBufferCount(uint32_t(opt.buffers));
    if (opt.frameRate > 0) cap.setFrameRate(opt.frameRate);
    cap.setSourcePolling(opt.pollSource);

    std::vector<ControlInfo> controls = enumerateControls(cap.fd());
    if (opt.info) {
        cap.printInfo();
        printControls(controls);
        return 0;
    }

    int failures = 0;
    for (const auto& c : opt.ctrls) {
        size_t eq = c.find('=');
        std::string msg;
        if (eq == std::string::npos) {
            std::fprintf(stderr, "--ctrl expects NAME=VALUE, got '%s'\n", c.c_str());
            ++failures;
            continue;
        }
        if (setControlByName(cap.fd(), controls, c.substr(0, eq), c.substr(eq + 1), msg)) {
            std::printf("control %s\n", msg.c_str());
        } else {
            std::fprintf(stderr, "control error: %s\n", msg.c_str());
            ++failures;
        }
    }
    if (opt.noStream) {
        // Vertical Flip belongs to the open handle that sets it
        // (measured): set from this short-lived
        // process it is gone as soon as the process exits.
        for (const std::string& c : opt.ctrls)
            if (c.rfind("Vertical Flip", 0) == 0)
                std::fprintf(stderr, "note: Vertical Flip only lasts while the handle that set it is open; set it in the "
                                     "streaming freshVision (Controls section, a preset, or --ctrl when starting it)\n");
        return failures ? 1 : 0;
    }
    if (failures) return 1;

    // Choose and start the capture format. A lambda so a later source switch
    // (GuiActions::selectSource) can redo exactly this on the new device.
    const PixelFormat* chosen = nullptr;
    PixelFormat shown{};
    auto pickAndStart = [&](uint32_t width, uint32_t height) -> bool {
        std::vector<const PixelFormat*> candidates = pickCandidates(opt.format, cap.enumFormats(), err);
        if (candidates.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        if (opt.format == "auto") {
            std::string why;
            if (!rgbCaptureAllowed(opt.overrideGuard, why)) {
                std::fprintf(stderr, "note: %s\nnote: falling back to a YUV format\n", why.c_str());
                candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                                [](const PixelFormat* p) { return p->rgb; }),
                                 candidates.end());
                if (candidates.empty()) {
                    std::fprintf(stderr, "no YUV format offered\n");
                    return false;
                }
            }
        }
        for (const PixelFormat* pf : candidates) {
            if (cap.start(pf->fourcc, width, height, err)) {
                chosen = pf;
                shown = *chosen;  // with the byte order this driver really delivers
                shown.layout = effectiveLayout(*chosen, cap.driverName(), opt.swapRb);
                return true;
            }
            std::fprintf(stderr, "format %s ('%s') failed: %s\n", pf->name, fourccString(pf->fourcc).c_str(), err.c_str());
        }
        return false;
    };
    cap.setCrop(opt.crop);
    // A crop without an explicit --size used to leave the capture format at
    // whatever size the device already had (crop only windows the sensor
    // region; it doesn't by itself change VIDIOC_S_FMT's width/height) --
    // the delivered frame silently stayed at the old, pre-crop size. Default
    // the capture size to the crop's own W,H so "--crop X,Y,W,H" alone does
    // what it looks like it should (found chasing a report that
    // cropping "doesn't scale up in steps as expected").
    uint32_t startW = opt.width;
    uint32_t startH = opt.height;
    if (!startW && !startH && opt.crop.set) { startW = opt.crop.w; startH = opt.crop.h; }
    if (!pickAndStart(startW, startH)) return 1;
    rememberDevice(opt.device);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Display disp;
    // Start window: the last size used, or about 1600x1000 (it
    // used to take the capture's shape, e.g. a 2048x298 "envelope").
    const std::string windowConf = [] {
        const char* cfg = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        std::string dir = (cfg && *cfg ? std::string(cfg) : std::string(home ? home : ".") + "/.config") + "/freshvision";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir + "/window.conf";
    }();
    int winW = 1600, winH = 1000;
    if (FILE* wf = std::fopen(windowConf.c_str(), "r")) {
        int w = 0, h = 0;
        if (std::fscanf(wf, "%dx%d", &w, &h) == 2 && w >= 400 && h >= 300) winW = w, winH = h;
        std::fclose(wf);
    } else {
        SDL_Rect usable;
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0 && SDL_GetDisplayUsableBounds(0, &usable) == 0)
            winW = std::min(winW, usable.w * 9 / 10), winH = std::min(winH, usable.h * 9 / 10);
    }
    if (!disp.create(cap.format().width, cap.format().height, opt.fullscreen, opt.vsync, err, winW, winH) ||
        !disp.setFrame(shown, cap.format().width, cap.format().height, cap.format().colorspace, err)) {
        std::fprintf(stderr, "display: %s\n", err.c_str());
        return 1;
    }
    Gui gui;
    bool guiOk = false;
    if (!opt.noGui) {
        std::string ge;
        guiOk = gui.init(disp.window(), disp.glContext(), ge);
        if (!guiOk) std::fprintf(stderr, "GUI disabled: %s\n", ge.c_str());
    }
    bool guiVisible = guiOk;

    std::printf("capturing %s (driver '%s'): %ux%u '%s' (%s), stride %u, buffers mapped, signal type: %s\n",
                opt.device.c_str(), cap.driverName().c_str(), cap.format().width, cap.format().height,
                fourccString(chosen->fourcc).c_str(), chosen->name, cap.format().stride,
                cap.signalTypeName().empty() ? "unknown" : cap.signalTypeName().c_str());

    Window win;
    std::vector<double> allAgePresent;
    Counters prev, total;
    unsigned long totalDisplayed = 0, restarts = 0;
    int64_t lastPresentNs = 0;
    const int64_t t0 = nowNs();
    int64_t lastFrameNs = t0, lastStatNs = t0, lastRestartNs = 0;
    int64_t lastLoopEndNs = 0, slowSinceNs = 0;  // recording safety net, see after disp.endFrame()
    double captureFps = 0;  // frames the driver delivered in the last stats second; the recorder's frame rate
    double lastRefreshHz = 0;  // the last refresh measured over 3+ s of buffer timestamps (kept across reopens)
    bool clockReported = false;
    std::string statsText = "(warming up)";
    std::string statsPanel;
    // Picture held back to line up with the monitor sound: a
    // queue of frame copies in front of the display; N from the measured
    // sound delay minus the picture's own age, once a second.
    struct DelayedFrame {
        std::vector<uint8_t> data;
        size_t stride = 0;
        uint32_t w = 0, h = 0;
        int layout = 0;
    };
    std::deque<DelayedFrame> pictureQueue;
    int pictureDelayFrames = 0;
    double monitorSoundMs = 0, pictureAgeMs = 0;
    DisplayOptions dispOpts;
    dispOpts.palDecode = opt.palDecode;
    dispOpts.palLine = opt.palLine;
    dispOpts.videoStandard = opt.standard;
    // The colour standard chosen in the Display section (PAL or NTSC):
    // subcarrier, line timing, levels (tools/pal/pal.hpp).
    auto videoStd = [&] { return dispOpts.videoStandard == 1 ? pal::Standard::Ntsc : pal::Standard::Pal; };
    auto stdInfo = [&]() -> const pal::StandardInfo& { return pal::standardInfo(videoStd()); };
    // The capture window per 4x of sampling: from just before the burst to
    // (nearly) the end of the line. PAL: 90 + 1024 samples (5.1 us on);
    // NTSC: 64 + 832 (4.5 us on; 896 of the 910-912 samples per line).
    auto windowAt4x = [&](int& position, int& width) {
        if (videoStd() == pal::Standard::Ntsc) position = 64, width = 832;
        else position = 90, width = 1024;
    };
    if (opt.standard == 1 && !opt.palLineGiven) dispOpts.palLine = 910;
    dispOpts.palUi.averageLines = opt.palAverageLines;
    dispOpts.palUi.removeStripes = opt.palStripes;
    dispOpts.palUi.relock = opt.palRelock;
    dispOpts.palUi.luma = opt.palLuma;
    dispOpts.palAutoTune = opt.palAutoTune;
    dispOpts.delayPictureToSound = opt.syncPictureToSound;
    dispOpts.palUi.pixelGrid = opt.palGrid;
    dispOpts.palUi.snap = opt.palSnap;
    dispOpts.palUi.profile = std::min<int>(opt.palProfile, int(pal::machineProfiles().size()) - 1);
    if (pal::machineProfiles()[size_t(dispOpts.palUi.profile)].colours > 0)
        dispOpts.palUi.colours = pal::machineProfiles()[size_t(dispOpts.palUi.profile)].colours;
    if (opt.palTiming > 0) dispOpts.palMultiple = opt.palTiming;
    if (opt.svideo >= 0) {
        dispOpts.palSvideo = true;
        dispOpts.svideoLuma = opt.svideo / 3;
        dispOpts.svideoChroma = opt.svideo % 3;
    }
    const int palThreads = opt.palThreads > 0 ? opt.palThreads : std::max(1, int(std::thread::hardware_concurrency() / 2));

    // PAL composite decoding of a g8 Sync-on-Green capture (tools/pal/pal.hpp).
    // The display shows the decoded RGB instead of the raw samples while it
    // is on; the burst position found once is reused, re-searched every
    // second or whenever colour is lost.
    const PixelFormat palShown{"pal-rgb", 0, Layout::Rgb24, 3, true};
    bool dispIsPal = false;            // the display texture currently holds decoded PAL
    int dispPalW = 0;                  // ... at this width
    double aspectBeforePal = 1.0;      // restored when decoding is switched off
    std::vector<uint8_t> palRgb, palRows, svChroma;
    pal::LinePattern palStripes;   // fixed clock stripes (learned while "Remove fixed clock stripes" is on)
    pal::VerticalRelock palRelock; // "Re-align rolling frames"
    int palLastOutW = 0;
    std::vector<double> palSoftState;  // "Two-colour text (soft)": its two levels over frames           // the last decoded picture's width (shown again for a frame that can't be placed)
    int64_t palStripesHsize = 0;   // the line length it was learned at
    pal::DecodeInfo palInfo;
    int palBurstStart = -1, palFramesSinceSearch = 0;
    // The V-switch parity, held from frame to frame: the decoder decides it
    // from the line length; if a frame can't be
    // decided that way, the last decided parity is used.
    int64_t palSyncedSize = -1;  // Horizontal Size the Display panel's fields were last synced to
    pal::GridStage palGrid;      // the machine's pixels and palette (Display panel "Pixel grid")
    // Auto-tune: per-frame signal levels, summed over a half second.
    struct AutoTuneAcc {
        int frames = 0;
        double blank = 0, burstMin = 0, clippedBurst = 0, white255 = 0, peak = 0, step = 0;
        int colourFrames = 0;
        std::complex<double> stepVec = 0;
    } autoAcc;
    std::string autoStatus;              // shown in the Display panel
    int64_t autoLastActionNs = 0;
    bool autoLineChecked = false;        // the line length was checked since the last timing change
    std::vector<uint8_t> palGridRgb;
    int palGridW = 0;
    int palParity = -1;
    bool palParityHeld = false;
    // Live software PLL: the carrier-frequency error per line,
    // smoothed from each frame's measured step error and applied to the next
    // frame (freshCoder does the same from a whole-recording pass).
    float palPllErr = 0;
    std::vector<float> palPllLines;
    double palMsSum = 0;
    int palMsCount = 0;
    std::string palStatus;
    // The Display section's copy: one item per line, fixed-width numbers,
    // renewed once a second (the one-line status reflowed 50 times a second
    // as its numbers changed length).
    std::string palPanel;
    bool palCompositeNow = false;  // this frame was decoded as composite (auto-tune applies)
    int64_t palPanelNs = 0;
    double palPanelMsSum = 0;
    int palPanelMsCount = 0;
    // Machines: the source's refresh rate from the buffer timestamps and
    // the composite carrier's cycles per line, both measured since the last
    // (re)start; `streamGeneration` counts (re)starts.
    bool palDumped = false, frameDumped = false;
    int64_t windowReopenAtNs = 0;  // a mode/preset changed the capture window: reopen then
    bool refBase = false;
    uint32_t refSeq0 = 0, refSeqN = 0;
    int64_t refTs0 = 0, refTsN = 0;
    std::complex<double> cplVec;
    int cplN = 0;
    double cyclesMeasured = 0;
    // Colour burst per input (red/green/blue) as the running decoder sees it:
    // frames and frames with colour in the current second; the last verdict
    // per input (-1 unknown) is kept until the source changes.
    int colourFrames[3] = {0, 0, 0}, colourSeen[3] = {0, 0, 0};
    int colourKnown[3] = {-1, -1, -1};
    uint64_t streamGeneration = 0;
    int palTimingMode = -1;            // 4 / 0: to be applied once frames flow; -1: nothing pending
    int palTimingPhase = 0;            // 0: nothing sent yet, 1: first controls sent, 2: all sent, restart pending
    int palTimingLine = 1135;          // samples per line at 4x for the pending request
    unsigned long palPhaseFrame = 0;   // framesSinceStart when the first phase was sent
    unsigned long framesSinceStart = 0;

    auto accumulate = [&] {
        Counters c = cap.counters();
        total.delivered += c.delivered - prev.delivered;
        total.overwritten += c.overwritten - prev.overwritten;
        total.driverGaps += c.driverGaps - prev.driverGaps;
        total.errorFrames += c.errorFrames - prev.errorFrames;
        total.shortFrames += c.shortFrames - prev.shortFrames;
        total.timeouts += c.timeouts - prev.timeouts;
        total.seqJumps += c.seqJumps - prev.seqJumps;
        prev = c;
        return c;
    };

    auto afterRestart = [&](const char* how) {
        prev = Counters{};
        // setFrame below puts the raw format back, and with it the aspect
        // from before decoding (it stayed at the decoded picture's before).
        if (dispIsPal) dispOpts.pixelAspect = aspectBeforePal;
        dispIsPal = false;
        framesSinceStart = 0;    // a pending PAL timing waits for frames again
        palBurstStart = -1;
        palParity = -1;          // a new stream can start on the other parity
        palPllErr = 0;           // and a new carrier estimate
        autoAcc = AutoTuneAcc{}; // auto-tune measures the new state afresh
        refBase = false;         // machines: refresh and carrier measured afresh
        cplVec = {};
        cplN = 0;
        cyclesMeasured = 0;
        palStripes.reset();      // a new window: the stripes sit elsewhere
        palRelock.reset();
        ++streamGeneration;
        const CaptureFormat& f = cap.format();
        std::string e;
        if (!disp.setFrame(shown, f.width, f.height, f.colorspace, e)) {
            std::printf("  display: %s\n", e.c_str());
            cap.stop();
            return;
        }
        std::printf("  %s: %ux%u '%s'\n", how, f.width, f.height, fourccString(f.fourcc).c_str());
        lastFrameNs = nowNs();
        clockReported = false;
    };

    // `deliberate`: a reopen the app asks for itself (new capture window or
    // size), which must not be skipped by the error-recovery rate limit.
    auto restart = [&](const std::string& reason, uint32_t newW = 0, uint32_t newH = 0, bool deliberate = false) {
        int64_t now = nowNs();
        if (!deliberate && now - lastRestartNs < 2000000000LL) return;  // at most one recovery restart per 2 s
        lastRestartNs = now;
        ++restarts;
        std::printf("restart #%lu: %s\n", restarts, reason.c_str());
        accumulate();
        std::string e;
        if (!cap.restart(e, newW, newH)) {  // stop, close, re-open, start (error-recovery path)
            prev = Counters{};
            std::printf("  restart failed: %s (retrying)\n", e.c_str());
            return;
        }
        afterRestart("streaming again");
    };

    // A deliberate geometry change (UI, preset, machine mode, RGB auto-crop):
    // always a full close/reopen at the new size. A stop/S_FMT/start with a
    // new width on the same open handle oopsed the driver in STREAMON twice:
    // g8 8x -> 16x and RGB 384 -> 2048 wide.
    auto applyGeometry = [&](uint32_t w, uint32_t h, bool changeCrop, Rect newCrop) {
        if (changeCrop) cap.setCrop(newCrop);
        restart("geometry change (full reopen)", w, h, true);
    };

    // A source switch: close the current device, open the new
    // one, and pick/start a format on it exactly as at startup. `opt.device`
    // becomes the new active path so the title bar, --info-style messages
    // and a later switch back all refer to the right one.
    auto selectSource = [&](const std::string& path) {
        if (path == opt.device) return;
        accumulate();
        cap.stop();
        cap.close();
        std::string e;
        if (!cap.open(path, e)) {
            std::printf("source switch: open %s failed: %s (reopening %s)\n", path.c_str(), e.c_str(),
                        opt.device.c_str());
            if (!cap.open(opt.device, e)) {
                std::printf("  could not reopen the previous device either: %s\n", e.c_str());
                return;
            }
        } else {
            opt.device = path;
        }
        cap.setOverrideGuard(opt.overrideGuard);
        cap.setSourcePolling(opt.pollSource);
        cap.setCrop(Rect{});  // a different source's old crop rarely still makes sense
        if (!pickAndStart(0, 0)) {  // 0,0: let the new source's own current size apply
            std::printf("source switch: no working format on %s\n", opt.device.c_str());
            return;
        }
        afterRestart("source switched");
    };

    // A new pixel format (a machine mode or preset asks for one): always
    // a full close/reopen, the path that is safe for g8, at the
    // requested size. RGB formats stay behind the IOMMU guard.
    auto switchFormat = [&](const PixelFormat* pf, uint32_t w, uint32_t h) -> bool {
        std::string why;
        if (needsIommuGuard(pf->fourcc) && !rgbCaptureAllowed(opt.overrideGuard, why)) {
            std::printf("format switch to %s refused: %s\n", pf->name, why.c_str());
            return false;
        }
        const PixelFormat* before = chosen;
        accumulate();
        cap.stop();
        cap.close();
        std::string e;
        if (!cap.open(opt.device, e)) {
            std::printf("format switch: reopen %s failed: %s\n", opt.device.c_str(), e.c_str());
            return false;
        }
        cap.setOverrideGuard(opt.overrideGuard);
        cap.setSourcePolling(opt.pollSource);
        if (!cap.start(pf->fourcc, w, h, e)) {
            std::printf("format switch to %s %ux%u failed: %s (back to %s)\n", pf->name, w, h, e.c_str(),
                        before ? before->name : "?");
            if (before && !cap.start(before->fourcc, 0, 0, e)) std::printf("  and that failed too: %s\n", e.c_str());
            if (cap.streaming()) afterRestart("format kept");
            return false;
        }
        chosen = pf;
        shown = *chosen;
        shown.layout = effectiveLayout(*chosen, cap.driverName(), opt.swapRb);
        afterRestart("format switched");
        return true;
    };

    // A small audio mixer/router, independent of the video
    // capture above (own ALSA devices, own threads).
    const int kAudioOutputChannels = 2;  // shared by the mixer, the recorder and the Audio panel below
    AudioMixer audioMixer;
    {
        std::string ae;
        if (!audioMixer.start(48000, kAudioOutputChannels, ae)) std::fprintf(stderr, "audio mixer: %s\n", ae.c_str());
    }
    std::vector<AudioDeviceInfo> audioDevices = enumerateAudioDevices();
    // "default" (ALSA's PCM name for "whatever PipeWire/Pulse currently
    // considers the system default output") goes first, ahead of the raw
    // hw: devices — this is the actual fix, corrected after an
    // earlier wrong guess: a first attempt hardcoded a preference for a
    // "PCH" device on the theory that "default" must be wrong because it
    // resolved to the HDMI/Nvidia output; the user corrected this directly
    // ("Monitor output must be the system default ... you overwritten the
    // proper sound output") — HDMI *is* their real output here. The
    // corrected fix is to not guess at all: keep using "default" and let
    // PipeWire/Pulse's own, already-correct default-output detection do
    // the job, live (so it keeps following the OS setting if that ever
    // changes) — the real hw: devices remain selectable as an override.
    std::vector<AudioDeviceInfo> playbackDevices;
    playbackDevices.push_back({"default", "System default output (follows OS audio settings)"});
    for (auto& d : enumeratePlaybackDevices()) playbackDevices.push_back(std::move(d));
    std::string monitorPlaybackId = "default";
    int64_t lastAudioScanNs = nowNs();
    std::future<std::vector<AudioDeviceInfo>> audioScan;  // a device scan running in the background
    // Remembers a device's RoutedSource across disable/re-enable (see
    // setAudioSourceEnabled below) — AudioMixer::removeSource() itself has
    // no memory of a removed source's settings, by design (it's a router
    // over currently-open devices, not a settings store), so main.cpp keeps
    // this alongside it.
    std::map<std::string, RoutedSource> audioSourceSettings;

    GuiActions actions;
    actions.setControl = [&](uint32_t id, int64_t value) { cap.requestControlChange(id, value); };
    actions.applySize = [&](uint32_t w, uint32_t h) { applyGeometry(w, h, false, Rect{}); };
    // Same fix as the startup path above: default the capture size to the
    // new crop's own W,H (a bare crop change carried no size before, so the
    // capture format silently stayed at whatever size it already was).
    actions.applyCrop = [&](Rect r) { applyGeometry(r.w, r.h, true, r); };
    actions.selectSource = selectSource;
    actions.setAudioSourceEnabled = [&](const std::string& deviceId, bool on) {
        if (on) {
            // Reuse this device's last known settings if it's been enabled
            // before this run, instead of always resetting to hard defaults
            // — found in testing ("If I enable/change settings/
            // disable/enable a device it is reset back to default"):
            // disabling used to fully discard the RoutedSource, so a
            // re-enable silently lost mode/route/gain/balance/delay.
            auto it = audioSourceSettings.find(deviceId);
            RoutedSource r = (it != audioSourceSettings.end()) ? it->second : RoutedSource{};
            r.deviceId = deviceId;
            // route[]/gain/balance default to Stereo/Stereo/1.0/0.0 (see
            // RoutedSource) so a source enabled for the first time is
            // audible on both output channels immediately, not landing
            // silently on a channel nothing is listening to.
            std::string ae;
            if (!audioMixer.addOrUpdateSource(r, ae))
                std::printf("audio: add %s failed: %s\n", deviceId.c_str(), ae.c_str());
        } else {
            for (const RoutedSource& r : audioMixer.sources()) {
                if (r.deviceId == deviceId) {
                    audioSourceSettings[deviceId] = r;  // remember for the next enable
                    break;
                }
            }
            audioMixer.removeSource(deviceId);
        }
    };
    actions.setAudioChannelMode = [&](const std::string& deviceId, ChannelMode mode) {
        for (RoutedSource r : audioMixer.sources()) {
            if (r.deviceId != deviceId) continue;
            r.mode = mode;
            std::string ae;
            audioMixer.addOrUpdateSource(r, ae);  // same deviceId: updates in place, does not reopen
            break;
        }
    };
    actions.setAudioOutputRoute = [&](const std::string& deviceId, int logicalIndex, OutputRoute route) {
        for (RoutedSource r : audioMixer.sources()) {
            if (r.deviceId != deviceId) continue;
            r.route[logicalIndex] = route;
            std::string ae;
            audioMixer.addOrUpdateSource(r, ae);
            break;
        }
    };
    actions.setAudioGain = [&](const std::string& deviceId, float gain) {
        for (RoutedSource r : audioMixer.sources()) {
            if (r.deviceId != deviceId) continue;
            r.gain = gain;
            std::string ae;
            audioMixer.addOrUpdateSource(r, ae);
            break;
        }
    };
    actions.setAudioBalance = [&](const std::string& deviceId, float balance) {
        for (RoutedSource r : audioMixer.sources()) {
            if (r.deviceId != deviceId) continue;
            r.balance = balance;
            std::string ae;
            audioMixer.addOrUpdateSource(r, ae);
            break;
        }
    };
    actions.setAudioHwVolume = [&](const std::string& deviceId, float v) { audioMixer.setHwVolume(deviceId, v); };
    actions.setAudioHwBalance = [&](const std::string& deviceId, float b) { audioMixer.setHwBalance(deviceId, b); };
    actions.setAudioHwMuted = [&](const std::string& deviceId, bool m) { audioMixer.setHwMuted(deviceId, m); };
    actions.setAudioInputSource = [&](const std::string& deviceId, const std::string& name) {
        if (!audioMixer.setHwInputSource(deviceId, name))
            std::printf("audio: could not set %s's input source to %s\n", deviceId.c_str(), name.c_str());
    };
    std::string monitorError;  // last monitor start/switch failure, shown in the Audio panel; cleared on success
    unsigned long long lastLoggedUnderruns = 0;  // for the per-second "(+N)" on the audio log line
    actions.toggleAudioMonitor = [&](bool on) {
        std::string ae;
        if (on) {
            if (monitorPlaybackId.empty()) {
                monitorError = "no playback device to monitor to (none found)";
                std::printf("audio: %s\n", monitorError.c_str());
                return;
            }
            int64_t t0 = nowNs();
            bool ok = audioMixer.startMonitor(monitorPlaybackId, ae);
            std::printf("audio: monitor start on %s %s in %.1f ms%s%s\n", monitorPlaybackId.c_str(),
                        ok ? "ok" : "FAILED", double(nowNs() - t0) / 1e6, ok ? "" : ": ", ok ? "" : ae.c_str());
            if (ok) monitorError.clear();
            else monitorError = monitorPlaybackId + ": " + ae;
        } else {
            audioMixer.stopMonitor();
            std::printf("audio: monitor stop (%s stays open, silent)\n", monitorPlaybackId.c_str());
        }
    };
    actions.setAudioPlaybackDevice = [&](const std::string& playbackId) {
        if (!audioMixer.monitoring()) {  // not playing: just remember the pick, it's opened at the next Start
            std::printf("audio: monitor output set to %s (monitor off, opened at next start)\n", playbackId.c_str());
            monitorPlaybackId = playbackId;
            monitorError.clear();
            return;
        }
        // Switch live. On failure the old device keeps playing (startMonitor
        // opens the new one before closing the old), so the selection stays
        // on the old device too, rather than showing a device that isn't playing.
        std::string ae;
        int64_t t0 = nowNs();
        bool ok = audioMixer.startMonitor(playbackId, ae);
        std::printf("audio: monitor switch %s -> %s %s in %.1f ms%s%s\n", monitorPlaybackId.c_str(), playbackId.c_str(),
                    ok ? "ok" : "FAILED", double(nowNs() - t0) / 1e6, ok ? "" : ": ", ok ? "" : ae.c_str());
        if (ok) {
            monitorPlaybackId = playbackId;
            monitorError.clear();
        } else {
            monitorError = playbackId + ": " + ae;
        }
    };
    actions.setMonitorHwVolume = [&](float v) { audioMixer.setMonitorHwVolume(v); };
    actions.setMonitorHwMuted = [&](bool mute) { audioMixer.setMonitorHwMuted(mute); };
    actions.setAudioDelay = [&](const std::string& deviceId, int delayMs) {
        for (RoutedSource r : audioMixer.sources()) {
            if (r.deviceId != deviceId) continue;
            r.delayMs = delayMs;
            std::string ae;
            audioMixer.addOrUpdateSource(r, ae);
            break;
        }
    };
    // Monitor on by default (user feedback: "The
    // default should be enabling the monitor") so a newly added source is
    // audible without a separate click; harmless if it fails (no playback
    // device, e.g. a headless box) — toggleAudioMonitor's own printf covers
    // that, and the Audio panel's Start/Stop button still works either way.
    actions.toggleAudioMonitor(true);
    // --record-audio gives the initial/default path; the GUI
    // button reuses it (adding a timestamp if it's still the default) so
    // repeated recordings during one run don't overwrite each other.
    std::string audioRecordPath = opt.recordAudioPath;
    actions.toggleAudioRecording = [&](bool on) {
        std::string ae;
        if (on) {
            std::string path = audioRecordPath;
            if (path.empty()) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "freshvision-audio-%lld.wav", (long long)(nowNs() / 1000000));
                path = buf;
            }
            if (!audioMixer.startRecording(path, ae)) std::printf("audio: start recording failed: %s\n", ae.c_str());
            else std::printf("audio: recording to %s\n", path.c_str());
        } else {
            audioMixer.stopRecording();
        }
    };
    if (!opt.recordAudioPath.empty()) actions.toggleAudioRecording(true);

    // Video (+ audio) recording. The audio mixer's sink feeds every
    // mixed tick into the recorder regardless of whether it's active right
    // now (Recorder::pushAudioFrame is a no-op when not recording) so
    // starting a recording never has to touch the audio wiring again.
    Recorder recorder;
    VideoEncoderKind videoEncoderKind = VideoEncoderKind::Auto;
    if (opt.recordEncoder == "x264") videoEncoderKind = VideoEncoderKind::X264;
    else if (opt.recordEncoder == "nvenc") videoEncoderKind = VideoEncoderKind::Nvenc;
    else if (opt.recordEncoder == "ffv1") videoEncoderKind = VideoEncoderKind::Ffv1;
    else if (opt.recordEncoder == "raw") videoEncoderKind = VideoEncoderKind::Raw;
    // --record-encoder wins over a mode's or preset's encoder applied later
    // (a deferred --record-video used to record with the mode's NVENC).
    const VideoEncoderKind cliEncoderKind = videoEncoderKind;
    // Probed once here (a handful of ms), not per frame -- see recorder.h.
    const std::vector<VideoEncoderKind> availableEncoderKinds = availableVideoEncoders();
    audioMixer.setSink([&](const int16_t* interleaved, size_t frames, int64_t ptsNs) {
        recorder.pushAudioFrame(interleaved, frames, ptsNs);
    });
    std::string videoRecordPath = opt.recordVideoPath;
    actions.setVideoEncoderKind = [&](VideoEncoderKind k) { videoEncoderKind = k; };
    std::string lastRecording;  // the last finished recording, for "Open in freshCoder"
    auto logRecordingStop = [&]() {
        RecorderCounters rc = recorder.counters();
        lastRecording = recorder.path();
        std::printf("video: recording stopped: %s, %llu frames, %llu dropped, %llu audio blocks dropped, %.1f MB\n",
                    recorder.path().c_str(), (unsigned long long)rc.videoFrames,
                    (unsigned long long)rc.droppedVideoFrames, (unsigned long long)rc.droppedAudioBlocks,
                    double(rc.bytesWritten) / (1024.0 * 1024.0));
    };
    std::string recError;         // last start failure, shown in the Recorder panel
    int64_t recStartNs = 0;       // for the panel's elapsed time
    // The desktop's Videos folder (~/Videos here), shared with freshCoder
    //; it was the directory freshVision was started in.
    std::string recDefaultFolder = defaultVideoFolder();
    auto defaultRecordName = []() {
        std::time_t t = std::time(nullptr);
        std::tm lt{};
        localtime_r(&t, &lt);
        char buf[64];
        std::strftime(buf, sizeof buf, "freshvision-%Y-%m-%d_%H-%M-%S.mkv", &lt);
        return std::string(buf);
    };
    auto startRecording = [&](const std::string& path) {
        const CaptureFormat& f = cap.format();
        std::string re;
        // Canvas = whatever the capture's current size is right now; a
        // later mode change letterboxes into this same size instead of
        // resizing the file (Recorder::start's doc comment).
        // The capture's real rate (rounded; 50 for the PAL C64), not a fixed 60.
        double recFps = captureFps >= 1.0 ? double(std::lround(captureFps)) : 50.0;
        // Right after a reopen the stats second is cut short (it said 40 fps):
        // the last longer measurement is better, the reopen doesn't
        // change the source's rate.
        if (lastRefreshHz > 10) recFps = double(std::lround(lastRefreshHz));
        // Right after a reopen the one-second count is low (a recording made
        // with --pal-timing said 14 fps, then 40 and 12):
        // the rate from the buffer timestamps since the (re)start, as soon as
        // 10 frames have come.
        if (refBase && refSeqN - refSeq0 >= 10 && refTsN > refTs0) {  // 10 frames give the whole-number rate
            const double hz = double(refSeqN - refSeq0) / (double(refTsN - refTs0) / 1e9);
            if (hz > 10 && hz < 250) recFps = double(std::lround(hz));
        }
        // Everything an offline decoder (freshCoder) needs to know about how
        // the samples were taken, as Matroska tags: the source, the format
        // and every driver control by the name the driver reports.
        {
            std::vector<std::pair<std::string, std::string>> tags;
            tags.emplace_back("FRESHVISION_BUILD", std::string(__DATE__) + " " + __TIME__);
            tags.emplace_back("SOURCE_DEVICE", opt.device);
            tags.emplace_back("SOURCE_DRIVER", cap.driverName());
            tags.emplace_back("SOURCE_SIGNAL_TYPE", cap.signalTypeName());
            tags.emplace_back("CAPTURE_FORMAT", fourccString(f.fourcc));
            tags.emplace_back("CAPTURE_SIZE", std::to_string(f.width) + "x" + std::to_string(f.height));
            tags.emplace_back("CAPTURE_FPS", std::to_string(recFps));
            // S-Video: which inputs carry brightness and colour (freshCoder decodes it as Y/C).
            if (dispOpts.palDecode) tags.emplace_back("FRESHVISION_STANDARD", stdInfo().name);
            if (dispOpts.palDecode && dispOpts.palSvideo && channelOffset(shown.layout, 0) >= 0)
                tags.emplace_back("FRESHVISION_SVIDEO", std::string(1, "RGB"[std::clamp(dispOpts.svideoLuma, 0, 2)]) + "," +
                                                            std::string(1, "RGB"[std::clamp(dispOpts.svideoChroma, 0, 2)]));
            for (const auto& c : cap.controlsSnapshot()) {
                if (!c.hasValue) continue;
                std::string key = "CTRL_";
                for (char ch : c.name) key += std::isalnum((unsigned char)ch) ? char(std::toupper((unsigned char)ch)) : '_';
                tags.emplace_back(key, std::to_string(c.value));
            }
            recorder.setTags(std::move(tags));
        }
        if (!recorder.start(path, f.width, f.height, shown.layout, recFps, audioMixer.sampleRate(),
                            kAudioOutputChannels, videoEncoderKind, re)) {
            recError = "start recording failed: " + re;
            std::printf("video: %s\n", recError.c_str());
            return;
        }
        recError.clear();
        recStartNs = nowNs();
        std::printf("video: recording to %s (%s, %.0f fps)\n", path.c_str(), recorder.activeEncoderName().c_str(), recFps);
    };
    actions.toggleVideoRecording = [&](bool on) {
        if (on) {
            startRecording(videoRecordPath.empty() ? defaultRecordName() : videoRecordPath);
        } else {
            logRecordingStop();
            recorder.stop();
        }
    };
    FolderPicker folderPicker;
    std::string pickedFolder;
    int pickedSeq = 0;
    actions.browseRecordFolder = [&](const std::string& startFolder) {
        std::string e;
        if (!folderPicker.start(startFolder, e)) recError = e;
    };
    // freshCoder lives next to this binary (same build folder or install
    // directory); otherwise it is looked up in PATH.
    actions.openInFreshCoder = [&](const std::string& path) {
        std::string exe = "freshcoder";
        std::error_code ec;
        std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec && std::filesystem::exists(self.parent_path() / "freshcoder", ec)) exe = (self.parent_path() / "freshcoder").string();
        std::vector<std::string> args = {exe};
        if (!path.empty()) args.push_back(path);
        std::string e;
        if (!launchDetached(args, e)) recError = "freshCoder: " + e;
        else std::printf("freshcoder started: %s %s\n", exe.c_str(), path.c_str());
    };
    actions.openRecordFolder = [&](const std::string& folder) {
        std::error_code ec;
        if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
            recError = "folder does not exist: " + folder;
            return;
        }
        std::string e;
        if (!openInFileBrowser(folder, e)) recError = e;
        else recError.clear();
    };
    actions.startVideoRecordingTo = [&](const std::string& folder, const std::string& nameIn) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (folder.empty() || !fs::is_directory(folder, ec)) {
            recError = "folder does not exist: " + folder;
            return;
        }
        if (access(folder.c_str(), W_OK) != 0) {
            recError = "folder is not writable: " + folder;
            return;
        }
        std::string name = nameIn.empty() ? defaultRecordName() : nameIn;
        if (name.find('/') != std::string::npos) {
            recError = "the file name must not contain '/': " + name;
            return;
        }
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".mkv") != 0) name += ".mkv";
        fs::path path = fs::path(folder) / name;
        if (fs::exists(path, ec)) {
            recError = "file already exists, not overwriting: " + path.string();
            return;
        }
        startRecording(path.string());
    };

    // Presets: capture the
    // app's current video+audio+recorder state into a Preset, or push a
    // loaded Preset back out — driving the same actions/local state every
    // panel already uses, so a preset does exactly what clicking through
    // those panels by hand would do.
    // "Signal Type" is what the driver *detected*; it reports it as a
    // writable menu but rejects every write (the driver-behaviour
    // findings call it read-only). Matched by the name the driver reports,
    // per the clean-room rule, and never saved or restored by presets.
    auto isDetectedOnlyControl = [](const std::string& name) { return name == "Signal Type"; };
    auto capturePresetState = [&](const std::string& group, const std::string& presetName) {
        Preset p;
        p.name = presetName;
        p.group = group;
        p.devicePath = opt.device;
        p.formatFourcc = chosen ? chosen->fourcc : 0;
        p.width = cap.format().width;
        p.height = cap.format().height;
        p.crop = cap.appliedCrop();
        for (const auto& c : cap.controlsSnapshot()) {
            // Only controls a write could ever succeed on — a disabled/
            // read-only/grabbed control (e.g. "Signal Type", which just
            // reports what the driver detected) still has a value to save,
            // but requestControlChange() on it at apply time fails, and on
            // this driver was observed to also cause a real stream restart
            // (a benign one, but pointless to
            // trigger). Real bug, found end-to-end-testing this
            // feature — controlIsSettable() is the same check
            // setControlByName() already uses, so both agree.
            if (!c.hasValue || !controlIsSettable(c) || isDetectedOnlyControl(c.name)) continue;
            PresetControl pc;
            pc.name = c.name;
            pc.value = c.value;
            p.controls.push_back(std::move(pc));
        }
        p.display = dispOpts;
        p.encoderKind = videoEncoderKind;
        p.monitorPlaybackId = monitorPlaybackId;
        for (const auto& r : audioMixer.sources()) {
            PresetAudioSource s;
            s.deviceId = r.deviceId;
            s.mode = r.mode;
            s.route[0] = r.route[0];
            s.route[1] = r.route[1];
            s.gain = r.gain;
            s.balance = r.balance;
            s.delayMs = r.delayMs;
            HwAudioInfo hw = audioMixer.hwInfo(r.deviceId);
            if (hw.available) {
                s.hwVolume = hw.volume;
                s.hwBalance = hw.balance;
                s.hwMuted = hw.muted;
            }
            s.hwInputSource = hw.currentInputSource;
            p.audioSources.push_back(std::move(s));
        }
        return p;
    };

    auto applyPresetState = [&](const Preset& p) {
        if (!p.devicePath.empty() && p.devicePath != opt.device && actions.selectSource) actions.selectSource(p.devicePath);
        if (chosen && p.formatFourcc != 0 && p.formatFourcc != chosen->fourcc) {
            // A different pixel format: one full reopen at the preset's size
            // (machine modes need it, e.g. RGB -> g8 composite).
            const PixelFormat* pf = findFormat(p.formatFourcc);
            if (!pf) {
                std::printf("preset '%s': format '%s' is not one freshVision can show, kept '%s'\n", p.name.c_str(),
                            fourccString(p.formatFourcc).c_str(), chosen->name);
            } else {
                if (p.crop.set) cap.setCrop(p.crop);
                if (switchFormat(pf, p.width, p.height))
                    std::printf("preset '%s': format %s %ux%u\n", p.name.c_str(), pf->name, cap.format().width,
                                cap.format().height);
            }
        }
        // Skip the restart entirely if size and crop already match — a
        // stop()+start() cycle always carries some chance of the usual
        // occasional post-restart DQBUF hiccup (self-recovering, already
        // documented), and the common case for loading a preset is
        // "the same setup that's already running", not an actual change.
        Rect curCrop = cap.appliedCrop();
        // A preset without a crop (a hand-written machine mode) leaves the crop alone.
        const bool sizeMatches = p.width == cap.format().width && p.height == cap.format().height;
        bool geometryMatches = sizeMatches && (!p.crop.set || (p.crop.set == curCrop.set && p.crop.x == curCrop.x &&
                                                               p.crop.y == curCrop.y && p.crop.w == curCrop.w &&
                                                               p.crop.h == curCrop.h));
        if (p.width && p.height && !geometryMatches) applyGeometry(p.width, p.height, p.crop.set, p.crop);
        // The capture window (Position/Timing/Size) only takes effect at the
        // next device open; a preset that changes it needs one.
        bool windowChanged = false;
        for (const auto& pc : p.controls) {
            if (pc.name != "Horizontal Position" && pc.name != "Horizontal Timing" && pc.name != "Horizontal Size" &&
                pc.name != "Vertical Position" && pc.name != "Vertical Timing")
                continue;
            for (const auto& c : cap.controlsSnapshot())
                if (c.name == pc.name && c.hasValue && c.value != pc.value) windowChanged = true;
        }
        for (const auto& pc : p.controls) {
            if (isDetectedOnlyControl(pc.name)) continue;  // older presets still contain it
            bool found = false;
            for (const auto& c : cap.controlsSnapshot()) {
                if (c.name == pc.name) {
                    cap.requestControlChange(c.id, pc.value);
                    found = true;
                    break;
                }
            }
            if (!found)
                std::printf("preset '%s': control '%s' not found on the current device, skipped\n", p.name.c_str(),
                            pc.name.c_str());
        }
        if (p.displaySpecified) {
            const DisplayOptions keep = dispOpts;
            dispOpts = p.display;
            dispOpts.areaX = keep.areaX, dispOpts.areaY = keep.areaY, dispOpts.areaW = keep.areaW, dispOpts.areaH = keep.areaH;
            if (dispIsPal) dispOpts.pixelAspect = keep.pixelAspect;  // the decoded picture's aspect is set by the decoder
        }
        // A PAL preset brings its sampling: set it through the crash-safe
        // phased path (the window controls only apply at a reopen).
        if (dispOpts.palDecode && (shown.layout == Layout::Grey8 || channelOffset(shown.layout, 0) >= 0) &&
            actions.setPalTiming)
            actions.setPalTiming(dispOpts.palMultiple);  // does its own reopen
        else if (windowChanged)
            windowReopenAtNs = nowNs() + 1500000000LL;  // once the writes are through
        if (p.encoderSpecified && actions.setVideoEncoderKind) actions.setVideoEncoderKind(p.encoderKind);
        if (!p.audioSpecified) {
            std::printf("preset '%s' applied (sound left as it is)\n", p.name.c_str());
            return;
        }

        // Audio: make the active sources match the preset exactly (remove
        // anything not in it, add/update everything that is) — a preset
        // load is a "go to this state", not a merge.
        std::vector<std::string> want;
        for (const auto& s : p.audioSources) want.push_back(s.deviceId);
        for (const auto& r : audioMixer.sources()) {
            if (std::find(want.begin(), want.end(), r.deviceId) == want.end()) audioMixer.removeSource(r.deviceId);
        }
        for (const auto& s : p.audioSources) {
            RoutedSource r;
            r.deviceId = s.deviceId;
            r.mode = s.mode;
            r.route[0] = s.route[0];
            r.route[1] = s.route[1];
            r.gain = s.gain;
            r.balance = s.balance;
            r.delayMs = s.delayMs;
            std::string ae;
            if (!audioMixer.addOrUpdateSource(r, ae)) {
                std::printf("preset '%s': could not add audio source %s: %s\n", p.name.c_str(), s.deviceId.c_str(),
                            ae.c_str());
                continue;
            }
            if (audioMixer.hwInfo(s.deviceId).available) {
                audioMixer.setHwVolume(s.deviceId, s.hwVolume);
                audioMixer.setHwBalance(s.deviceId, s.hwBalance);
                audioMixer.setHwMuted(s.deviceId, s.hwMuted);
                if (!s.hwInputSource.empty()) audioMixer.setHwInputSource(s.deviceId, s.hwInputSource);
            }
        }
        if (!p.monitorPlaybackId.empty() && actions.setAudioPlaybackDevice)
            actions.setAudioPlaybackDevice(p.monitorPlaybackId);
        std::printf("preset '%s' applied\n", p.name.c_str());
    };

    // Tracks the last successfully-loaded preset's identity, for the
    // Presets panel to pre-fill its Save fields with (PresetPanelState,
    // below) — user feedback.
    std::string lastLoadedGroup, lastLoadedName;

    actions.resetControl = [&](uint32_t id) {
        for (const auto& c : cap.controlsSnapshot())
            if (c.id == id) {
                cap.requestControlChange(id, c.def);
                break;
            }
    };
    actions.resetAllControls = [&]() {
        for (const auto& c : cap.controlsSnapshot())
            if (controlIsSettable(c)) cap.requestControlChange(c.id, c.def);
    };

    // Controls by the name the driver reports (clean-room rule: no fixed IDs).
    auto controlValue = [&](const char* name, int64_t fallback) {
        for (const auto& c : cap.controlsSnapshot())
            if (c.name == name && c.hasValue) return c.value;
        return fallback;
    };
    auto setControlNamed = [&](const char* name, int64_t value) {
        for (const auto& c : cap.controlsSnapshot())
            if (c.name == name) {
                cap.requestControlChange(c.id, value);
                std::printf("pal timing: %s -> %lld\n", name, (long long)value);
                return true;
            }
        std::printf("pal timing: no control named \"%s\" on this source\n", name);
        return false;
    };
    // The timing controls only take effect when changed while streaming
    // so the
    // request waits in palTimingMode until frames flow; see the main loop.
    // No size change here: the new capture width is applied inside the one
    // full reopen at the end (applyPendingPalTiming), never by a stop/start on
    // the open handle (that path crashed the driver).
    actions.switchToRgbForSvideo = [&] {
        const PixelFormat* pf = findFormat("rgb4");
        if (!pf || !chosen || chosen->layout != Layout::Grey8) return;
        const auto f = cap.format();
        if (switchFormat(pf, f.width, f.height))
            std::printf("S-Video: capture switched to %s %ux%u (two inputs needed)\n", pf->name, cap.format().width,
                        cap.format().height);
    };
    actions.setPalTiming = [&](int mode) {
        // PAL: 4x/8x/10x (12x-16x left out: the card delivers no fully correct frame there). NTSC also
        // 6x and 12x (42.95 MHz, below PAL 10x's 44.3; whole frames at 60 fps on
        // an Apple IIc and an MSX).
        const bool ntsc = videoStd() == pal::Standard::Ntsc;
        if (mode != 0 && mode != 4 && mode != 8 && mode != 10 && !(ntsc && (mode == 6 || mode == 12))) {
            std::printf("pal timing: %dx refused (%s)\n", mode, ntsc ? "NTSC: 4x, 6x, 8x, 10x or 12x" : "PAL: 4x, 8x or 10x");
            return;
        }
        palTimingMode = mode;
        palTimingLine = dispOpts.palLine;
        palTimingPhase = 0;
        if (mode > 0) std::printf("pal timing: %s %dxFsc, %d samples/line at 4x, requested\n", stdInfo().name, mode, palTimingLine);
        else std::printf("pal timing: Sync-on-Green baseline requested\n");
    };
    // In phases, measured: the line length takes effect
    // while streaming, but a new capture window (Position/Timing) only at the
    // next device open, and a window past the current line length is
    // ignored. So for 4x: the line length; then, once the driver reports it
    // and half a second of frames has passed, the window; then one stream
    // reopen of the device. For the baseline the window shrinks first and
    // the line length follows, then the same reopen.
    auto applyPendingPalTiming = [&] {
        if (palTimingMode < 0 || !cap.streaming() || framesSinceStart < 5) return;
        const int m = palTimingMode;
        const int64_t size = m > 0 ? int64_t(palTimingLine) * m / 4 : 524;
        if (palTimingPhase == 2) {
            if (framesSinceStart - palPhaseFrame < 25) return;
            palTimingMode = -1;
            palTimingPhase = 0;
            palBurstStart = -1;
            // A full close/reopen, at the width of the new window: after a
            // plain stop+start the driver fails the first DQBUF (EINVAL), and
            // a stop/S_FMT/start to a new width crashed it.
            int wPos = 0, wWidth = 0;
            windowAt4x(wPos, wWidth);
            const uint32_t newW = m > 0 ? uint32_t(wWidth * m / 4) : 384;
            restart("pal timing: reopen at " + std::to_string(newW) + " so the new capture window applies", newW, 0, true);
            return;
        }
        if (palTimingPhase == 0) {
            if (m > 0) {
                setControlNamed("Horizontal Size", size);
            } else {
                setControlNamed("Horizontal Timing", 384);
                setControlNamed("Horizontal Position", 113);
            }
            palTimingPhase = 1;
            palPhaseFrame = framesSinceStart;
            return;
        }
        if (framesSinceStart - palPhaseFrame < 25) return;
        if (m > 0) {
            if (controlValue("Horizontal Size", 0) != size) return;  // snapshot refreshes once a second
            int wPos = 0, wWidth = 0;
            windowAt4x(wPos, wWidth);
            setControlNamed("Horizontal Position", wPos * m / 4);
            setControlNamed("Horizontal Timing", wWidth * m / 4);
        } else {
            setControlNamed("Horizontal Size", size);
        }
        palTimingPhase = 2;
        palPhaseFrame = framesSinceStart;
    };
    // Auto-tune (Display panel checkbox, --pal-autotune): every half second,
    // from the levels measured on the raw samples, it moves the settings
    // toward a clean decode, one step at a time, logging each change.
    //  1. No PAL sampling rate yet: set the chosen multiple with the
    //     profile's (or the standard) line length.
    //  2. Line length: the measured line-to-line carrier step shows the
    //     source's carrier cycles per line (fraction, 180-degree ambiguous;
    //     it does not depend on our sampling). The candidate nearest the
    //     profile's value (or the PAL range 283.5-283.75) times the multiple
    //     is the right Horizontal Size (C64: 283.5 -> 1134 at 4x).
    //  3. Black level: Green Brightness until blanking sits near code 56 and
    //     the burst isn't cut off at 0; Green Contrast down while white clips
    //     at 255, back up while there is headroom.
    auto runAutoTune = [&]() {
        if (shown.layout != Layout::Grey8 || !cap.streaming() || palTimingMode >= 0) return;
        dispOpts.palDecode = true;  // its measurements come from the decoder
        const int64_t now = nowNs();
        auto log = [&](const std::string& msg) {
            autoStatus = msg;
            std::printf("auto-tune: %s\n", msg.c_str());
        };
        const int64_t hsize = controlValue("Horizontal Size", 0);
        const double q = double(hsize) / stdInfo().cyclesPerLine;
        const int K = (q >= 3.5 && std::abs(q - std::round(q)) < 0.01 * q) ? int(std::lround(q)) : 0;
        const auto& profs = pal::machineProfiles();
        const int profIdx = std::clamp(dispOpts.palUi.profile, 0, int(profs.size()) - 1);
        const auto& prof = profs[size_t(profIdx)];
        // A profile's line length counts only for its own standard.
        const int profLine = prof.lineAt4x > 0 && pal::isNtscProfile(profIdx) == (videoStd() == pal::Standard::Ntsc) ? prof.lineAt4x : 0;
        if (!K) {
            if (now - autoLastActionNs < 3000000000LL) return;
            autoLastActionNs = now;
            dispOpts.palDecode = true;
            if (profLine > 0) dispOpts.palLine = profLine;
            log("Horizontal Size " + std::to_string(hsize) + " is not a " + stdInfo().name + " sampling rate: setting " +
                std::to_string(dispOpts.palMultiple) + "x, line " + std::to_string(dispOpts.palLine));
            autoLineChecked = false;
            autoAcc = AutoTuneAcc{};
            actions.setPalTiming(dispOpts.palMultiple);
            return;
        }
        // After a change: 1.5 s for it to take effect (the control snapshot
        // refreshes once a second) before measuring again, so no step is
        // sent twice from a stale reading.
        if (now - autoLastActionNs < 1500000000LL) {
            autoAcc = AutoTuneAcc{};
            return;
        }
        if (autoAcc.frames < 25 || framesSinceStart < 50) return;
        const AutoTuneAcc a = autoAcc;
        autoAcc = AutoTuneAcc{};
        const double n = a.frames;
        const double blank = a.blank / n, burstMin = a.burstMin / n, clipped = a.clippedBurst / n;
        const double white = a.white255 / n, peak = a.peak / n;
        std::string state;
        // 2. Line length.
        if (!autoLineChecked && a.colourFrames >= a.frames / 2) {
            autoLineChecked = true;
            const double s = std::arg(a.stepVec) / (2 * pal::kPi);  // cycles, -0.5..0.5
            const double target = profLine > 0 ? profLine / 4.0 : stdInfo().cyclesPerLine;
            // PAL: the step is known only up to 180 deg (V-switch parity); NTSC: exactly.
            const bool ntsc = videoStd() == pal::Standard::Ntsc;
            double best = 0, bestD = 1e9;
            for (double base : {std::floor(target) - 1, std::floor(target), std::floor(target) + 1})
                for (double f : {s, ntsc ? s : s + 0.5}) {
                    double frac = f - std::floor(f);
                    double c = base + frac;
                    if (std::abs(c - target) < bestD) bestD = std::abs(c - target), best = c;
                }
            const int want = int(std::lround(best * K));
            const int line4 = int(std::lround(best * 4));
            if (want != hsize) {
                dispOpts.palLine = line4;
                log("line length: the carrier has " + std::to_string(best).substr(0, 7) + " cycles per line -> Horizontal Size " +
                    std::to_string(hsize) + " -> " + std::to_string(want));
                autoLineChecked = false;
                autoLastActionNs = now;
                actions.setPalTiming(K);
                return;
            }
            state += "line " + std::to_string(hsize) + " ok";
        } else if (autoLineChecked) {
            state += "line " + std::to_string(hsize) + " ok";
        } else {
            state += "line: waiting for colour";
        }
        // 3. Black level and swing, one control step per half second.
        const int64_t gb = controlValue("Green Brightness", -1), gc = controlValue("Green Contrast", -1);
        auto setNamed = [&](const char* name, int64_t v) {
            for (const auto& c : cap.controlsSnapshot())
                if (c.name == name) {
                    v = std::clamp<int64_t>(v, c.min, c.max);
                    cap.requestControlChange(c.id, v);
                    autoLastActionNs = now;
                    return v;
                }
            return int64_t(-1);
        };
        char buf[160];
        if (gb >= 0 && (clipped > 0.05 || blank < 44)) {
            const int64_t step = clipped > 0.05 ? 16 : std::clamp<int64_t>(int64_t(std::lround((56 - blank) / 0.93)), 2, 24);
            const int64_t v = setNamed("Green Brightness", gb + step);
            std::snprintf(buf, sizeof buf, "black level: blanking %.0f, burst cut at 0 on %.0f%% -> Green Brightness %lld -> %lld",
                          blank, clipped * 100, (long long)gb, (long long)v);
            log(buf);
            return;
        }
        if (gb >= 0 && blank > 70) {
            const int64_t step = std::clamp<int64_t>(int64_t(std::lround((blank - 56) / 0.93)), 2, 24);
            const int64_t v = setNamed("Green Brightness", gb - step);
            std::snprintf(buf, sizeof buf, "black level: blanking %.0f -> Green Brightness %lld -> %lld", blank, (long long)gb,
                          (long long)v);
            log(buf);
            return;
        }
        if (gc >= 0 && white > 0.0005) {
            const int64_t v = setNamed("Green Contrast", gc - (white > 0.005 ? 24 : 12));
            std::snprintf(buf, sizeof buf, "white clips (%.2f%% at 255) -> Green Contrast %lld -> %lld", white * 100,
                          (long long)gc, (long long)v);
            log(buf);
            return;
        }
        if (gc >= 0 && gc < 256 && peak < 215 && burstMin > 12) {
            const int64_t v = setNamed("Green Contrast", gc + 8);
            std::snprintf(buf, sizeof buf, "headroom (peak %.0f) -> Green Contrast %lld -> %lld", peak, (long long)gc, (long long)v);
            log(buf);
            return;
        }
        std::snprintf(buf, sizeof buf, "%s | blanking %.0f, burst low %.0f, white at 255 %.2f%%, peak %.0f: all in range",
                      state.c_str(), blank, burstMin, white * 100, peak);
        autoStatus = buf;
        if (dispOpts.palUi.pixelGrid && !palGrid.grid().valid) palGrid.redetect();
    };
    // --audio-source and a machine's sound_source: the same as ticking the
    // device in the Audio section. DEVICE[,left|right|stereo].
    auto addAudioSpec = [&](const std::string& spec) {
        std::string dev = spec, mode;
        const size_t comma = spec.rfind(',');
        if (comma != std::string::npos) {
            const std::string tail = spec.substr(comma + 1);
            if (tail == "left" || tail == "right" || tail == "stereo") dev = spec.substr(0, comma), mode = tail;
        }
        actions.setAudioSourceEnabled(dev, true);
        bool active = false;
        for (const RoutedSource& r : audioMixer.sources()) active |= r.deviceId == dev;
        if (!active) {
            std::printf("audio source NOT added: %s (see the line above; another program may hold the device)\n",
                        dev.c_str());
            return;
        }
        if (mode == "left") actions.setAudioChannelMode(dev, ChannelMode::Left);
        else if (mode == "right") actions.setAudioChannelMode(dev, ChannelMode::Right);
        else if (mode == "stereo") actions.setAudioChannelMode(dev, ChannelMode::Stereo);
        std::printf("audio source added: %s%s\n", dev.c_str(), mode.empty() ? "" : (" (" + mode + ")").c_str());
    };
    for (const std::string& spec : opt.audioSources) addAudioSpec(spec);

    // ---- Machines: files, matching, applying, automatic switching ----
    std::vector<Machine> machines;
    std::vector<std::string> machineErrors;
    int64_t machinesStamp = 0;
    std::string activeMachineId, activeModeName, machineHint, machineStatus;
    bool activeByUser = false;       // chosen by a click/--machine: the automatic switch keeps it for this source
    DetectedMode detected;
    MatchResult machineMatch;
    std::string stableSig, decidedSig;
    int stableCount = 0;
    int64_t lastMachineApplyNs = 0;
    const std::string machineStatePath = configDir().empty() ? std::string() : configDir() + "/machine-state.conf";
    bool autoSwitch = true;
    // A mode picked automatically while its colour-wire condition couldn't be
    // measured yet (S-Video, tried first by its match_priority): the wire
    // whose burst decides it; checked as soon as ~25 frames are measured with
    // the mode's timing in place, instead of waiting for the next steady
    // 5-second windows (a cold start with plain composite sat ~20 s in the
    // S-Video mode).
    int tentativeColourCh = -1, tentFrames = 0, tentSeen = 0;
    bool tentativeFromStart = false;
    // The settings shortly after a mode was applied (once things have settled):
    // saving over that mode then takes over only what changed since.
    Preset modeBaseline;
    bool haveModeBaseline = false;
    int64_t modeBaselineAtNs = 0;  // 0 = none pending
    std::string baselineMachine, baselineMode;
    std::vector<std::string> pendingChanges;
    int64_t lastChangesNs = 0;  // set by --machine NAME without a mode: may correct a mode the user didn't name
    int pendingColourMachine = -1, pendingColourMode = -1;  // applied at the next loop pass, not mid-frame
    {
        std::ifstream f(machineStatePath);
        std::string line;
        while (std::getline(f, line))
            if (line.rfind("auto_switch=", 0) == 0) autoSwitch = line.substr(12) != "0";
    }
    if (opt.autoSwitch >= 0) autoSwitch = opt.autoSwitch == 1;
    // Per machine, per mode: how a user's version differs from the built-in
    // one (computed once per reload, not per frame); newer built-in versions.
    std::vector<std::vector<std::vector<std::string>>> machineDiffs;
    std::vector<MachinePanelState::Update> machineUpdates;
    auto reloadMachines = [&] {
        machineErrors.clear();
        machines = loadMachines(machineErrors);
        machinesStamp = machinesDirStamp();
        for (const std::string& e : machineErrors) std::printf("machines: %s\n", e.c_str());
        machineDiffs.assign(machines.size(), {});
        for (size_t i = 0; i < machines.size(); ++i)
            for (const MachineMode& md : machines[i].modes) {
                std::vector<std::string> d;
                if (md.origin == ModeOrigin::MineReplacing)
                    for (const MachineMode& b : machines[i].replacedBuiltIn)
                        if (b.name == md.name) d = modeDifferences(md, b);
                machineDiffs[i].push_back(d);
            }
        machineUpdates.clear();
        for (const ModeUpdate& u : pendingModeUpdates(machines)) {
            MachinePanelState::Update up;
            up.machine = u.machine, up.mode = u.mode, up.revision = u.builtInRevision;
            const Machine& mc = machines[size_t(u.machine)];
            for (size_t j = 0; j < mc.modes.size(); ++j)
                if (mc.modes[j].name == u.mode) up.diff = machineDiffs[size_t(u.machine)][j];
            machineUpdates.push_back(up);
            std::printf("machines: a newer built-in version of %s / %s (revision %d) exists; yours is kept until you choose\n",
                        mc.name.c_str(), u.mode.c_str(), u.builtInRevision);
        }
    };
    reloadMachines();
    std::printf("machines: %zu machines (built-in: %s; yours: %s), automatic switching %s\n", machines.size(),
                builtInMachinesDir().c_str(), machinesDir().c_str(), autoSwitch ? "on" : "off");
    auto findMachine = [&](const std::string& id) {
        for (size_t i = 0; i < machines.size(); ++i)
            if (machines[i].id == id) return int(i);
        return -1;
    };
    bool rgbTunePending = opt.rgbAutoTune;
    bool rgbTuneStopRequest = false;  // a mode without the RGB tune was applied
    if (opt.rgbAutoTune) dispOpts.rgbAutoTune = true;
    if (opt.rgbAutoCrop) dispOpts.rgbAutoCrop = true;
    auto applyMachineMode = [&](int mi, int mo, bool byUser) {
        if (mi < 0 || mi >= int(machines.size())) return;
        tentativeColourCh = -1, tentFrames = tentSeen = 0, tentativeFromStart = false;
        const Machine& mc = machines[size_t(mi)];
        if (mc.modes.empty()) {
            activeMachineId = mc.id;
            activeModeName.clear();
            machineHint.clear();
            machineStatus = mc.name + " has no modes yet: set the source up and save one below.";
            return;
        }
        if (mo < 0) {  // the machine's best mode for what the source looks like now
            int best = -1;
            for (int j = 0; j < int(mc.modes.size()); ++j) {
                // The machine was named by the user (button, --machine NAME): its
                // click-only modes count too (the MSX1's are).
                // Ties go to the higher match_priority, as in the automatic pick (S-Video
                // before Composite while the colour wire isn't measured yet).
                const int sc = scoreMode(mc, mc.modes[size_t(j)], detected, nullptr, byUser);
                const int bsc = best < 0 ? -1 : scoreMode(mc, mc.modes[size_t(best)], detected, nullptr, byUser);
                if (sc >= 0 && (best < 0 || sc > bsc ||
                                (sc == bsc && mc.modes[size_t(j)].match.priority > mc.modes[size_t(best)].match.priority)))
                    best = j;
            }
            if (best < 0) {
                // None fits this source: don't force one (a click on the C64
                // button put its composite mode on the scan converter).
                // The active mode stays; the GUI makes the machine the target
                // for "Save current settings as this mode".
                machineStatus = "None of " + mc.name + "'s modes matches this source (" + detected.describe() +
                                "): nothing changed. Save the current setup as a mode, or click a mode to force it.";
                std::printf("machines: %s\n", machineStatus.c_str());
                return;
            }
            mo = best;
        }
        if (mo >= int(mc.modes.size())) return;
        const MachineMode& md = mc.modes[size_t(mo)];
        // A mode that names no device but an input (the C64's composite and
        // S-Video modes: "Sync On Green") goes to the device with that input
        // first; it ran on the scan converter after the RGB mode had switched
        // there.
        std::string modeDevice = md.settings.devicePath;
        if (modeDevice.empty() && !md.match.device.empty()) modeDevice = md.match.device;
        if (modeDevice.empty() && !md.match.input.empty() && cap.inputName().find(md.match.input) == std::string::npos) {
            for (const SourceInfo& s : probeVideoSources())
                if (s.isCaptureDevice && s.name.find(md.match.input) != std::string::npos) {
                    modeDevice = s.path;
                    break;
                }
            if (modeDevice.empty()) {
                machineStatus = mc.name + " / " + md.name + " needs the input \"" + md.match.input +
                                "\", and no capture device has it: nothing changed.";
                std::printf("machines: %s\n", machineStatus.c_str());
                return;
            }
            std::printf("machine: %s / %s needs the input \"%s\": switching to %s\n", mc.name.c_str(), md.name.c_str(),
                        md.match.input.c_str(), modeDevice.c_str());
        }
        std::printf("machine: applying %s / %s (%s)\n", mc.name.c_str(), md.name.c_str(),
                    byUser ? "chosen" : "automatic");
        if (!mc.soundSource.empty()) {
            std::string dev = mc.soundSource.substr(0, mc.soundSource.rfind(','));
            bool have = false;
            for (const RoutedSource& r : audioMixer.sources()) have |= r.deviceId == dev || r.deviceId == mc.soundSource;
            if (!have) addAudioSpec(mc.soundSource);
        }
        Preset p = md.settings;
        p.name = mc.name + " / " + md.name;
        if (p.devicePath.empty()) p.devicePath = modeDevice;
        applyPresetState(p);
        activeMachineId = mc.id;
        activeModeName = md.name;
        activeByUser = byUser;
        haveModeBaseline = false;
        modeBaselineAtNs = nowNs() + 1500000000LL;  // taken once the mode's reopen has finished
        baselineMachine = mc.id, baselineMode = md.name;
        machineHint = md.hint;
        machineStatus = std::string(byUser ? "Applied " : "Switched automatically to ") + mc.name + " / " + md.name;
        lastMachineApplyNs = nowNs();
        decidedSig = detected.signature();
        if (dispOpts.rgbAutoTune && !dispOpts.palDecode) {
            rgbTunePending = true;
        } else {  // a mode without it: a running or pending RGB tune is dropped
            rgbTunePending = false;
            rgbTuneStopRequest = true;  // the loop stops a running tune
        }
    };
    actions.applyMachineMode = [&](int mi, int mo) { applyMachineMode(mi, mo, true); };
    actions.reloadMachines = [&] {
        reloadMachines();
        machineStatus = "Reloaded " + std::to_string(machines.size()) + " machine files";
    };
    actions.setAutoSwitch = [&](bool on) {
        autoSwitch = on;
        if (!on) decidedSig.clear();
        std::ofstream f(machineStatePath);
        f << "auto_switch=" << (on ? 1 : 0) << "\n";
        std::printf("machines: automatic switching %s\n", on ? "on" : "off");
    };
    actions.openMachinesFolder = [&] {
        std::string e;
        if (!openInFileBrowser(machinesDir(), e)) machineStatus = "Open folder: " + e;
    };
    actions.editMachine = [&](int mi) {
        if (mi < 0 || mi >= int(machines.size())) return;
        std::string e;
        if (!launchDetached({"xdg-open", machines[size_t(mi)].path}, e)) machineStatus = "Edit: " + e;
        else machineStatus = "Opened " + machines[size_t(mi)].path + " (Reload after saving it; it reloads by itself too)";
    };
    actions.newMachine = [&](const std::string& name, const std::string& group) {
        Machine mc;
        mc.id = machineIdFor(name);
        mc.name = name;
        mc.group = group;
        for (const Machine& m : machines)
            if (m.id == mc.id) {
                machineStatus = "A machine \"" + m.name + "\" (" + mc.id + ") already exists";
                return;
            }
        std::string e;
        if (!saveMachine(mc, e)) {
            machineStatus = "New machine: " + e;
            return;
        }
        reloadMachines();
        activeMachineId = mc.id;
        activeModeName.clear();
        machineHint.clear();
        machineStatus = "Created " + mc.name + " in My modes: set the source up, then save it as a mode";
    };
    // Saved into My modes: replaces a built-in mode of the same name (which
    // stays available), or the user's own mode of that name.
    actions.saveCurrentAsMode = [&](int mi, const std::string& modeName, bool conditions, bool sound) {
        if (mi < 0 || mi >= int(machines.size()) || modeName.empty()) return;
        Machine mc = machines[size_t(mi)];
        MachineMode md;
        md.name = modeName;
        if (conditions) {
            md.match.device = detected.device;
            md.match.input = detected.input;
            md.match.signal = detected.signal;
            md.match.width = detected.width, md.match.height = detected.height;
            md.match.lineTotal = detected.lineTotal;
            if (detected.refreshHz > 0) md.match.refreshHz = std::round(detected.refreshHz * 1000) / 1000, md.match.refreshTol = 0.05;
            if (detected.cyclesPerLine > 0) md.match.cyclesPerLine = std::round(detected.cyclesPerLine * 100) / 100, md.match.cyclesTol = 0.1;
        } else {
            md.match.manual = true;
        }
        md.settings = capturePresetState("", modeName);
        if (!sound) md.settings.audioSpecified = false;
        std::vector<std::string> savedChanges;
        bool mergedChanges = false;
        md.origin = ModeOrigin::Mine;
        bool replaced = false;
        for (MachineMode& old : mc.modes)
            if (old.name == modeName) {
                md.hint = old.hint;
                // Saving over an existing mode keeps its conditions (e.g. "colour on
                // blue" and the priority of the C64's S-Video mode); a measured refresh
                // or carrier outside the stored tolerance is taken over (a machine with
                // a slightly different crystal). The device stays as the mode had it.
                if (conditions) {
                    ModeMatch keep = old.match;
                    if (keep.refreshHz > 0 && md.match.refreshHz > 0 && std::abs(md.match.refreshHz - keep.refreshHz) > keep.refreshTol)
                        keep.refreshHz = md.match.refreshHz;
                    if (keep.cyclesPerLine > 0 && md.match.cyclesPerLine > 0 &&
                        std::abs(md.match.cyclesPerLine - keep.cyclesPerLine) > keep.cyclesTol)
                        keep.cyclesPerLine = md.match.cyclesPerLine;
                    md.match = keep;
                }
                md.settings.devicePath = old.settings.devicePath;
                // Only what changed since this mode was applied (else the full state).
                if (haveModeBaseline && baselineMachine == mc.id && baselineMode == modeName) {
                    std::vector<std::string> ch;
                    md.settings = mergeModeChanges(old.settings, modeBaseline, md.settings, ch);
                    if (!sound) md.settings.audioSpecified = old.settings.audioSpecified;
                    savedChanges = ch;
                    mergedChanges = true;
                }
                if (old.origin == ModeOrigin::BuiltIn) md.origin = ModeOrigin::MineReplacing, md.basedOn = old.revision;
                else if (old.origin == ModeOrigin::MineReplacing) md.origin = old.origin, md.basedOn = old.basedOn, md.keptRevision = old.keptRevision;
                md.raw = old.raw;  // keys this version doesn't know are kept
                old = md;
                replaced = true;
            }
        if (!replaced) mc.modes.push_back(md);
        std::string e;
        if (!saveMachine(mc, e)) {
            machineStatus = "Save mode: " + e;
            return;
        }
        reloadMachines();
        activeMachineId = mc.id;
        activeModeName = modeName;
        activeByUser = true;
        decidedSig = detected.signature();
        machineStatus = std::string(md.origin == ModeOrigin::MineReplacing ? "Saved your version of " : replaced ? "Replaced your mode " : "Saved your mode ") +
                        mc.name + " / " + modeName + (conditions ? "" : " (click-only)") + (sound ? ", with the sound sources" : "");
        if (mergedChanges) {
            machineStatus += savedChanges.empty() ? ". Nothing changed since the mode was applied." : ". Changed since the mode was applied: ";
            for (size_t i = 0; i < savedChanges.size(); ++i) machineStatus += (i ? "; " : "") + savedChanges[i];
        }
        // the saved state is the new starting point
        // (a full snapshot of what runs now, like the one taken after applying a mode)
        modeBaseline = capturePresetState("", modeName), haveModeBaseline = true, baselineMachine = mc.id, baselineMode = modeName;
        pendingChanges.clear();
        std::printf("machines: %s\n", machineStatus.c_str());
    };
    // Deletes the user's mode, or (for a replacement) goes back to the built-in one.
    actions.removeMyMode = [&](int mi, const std::string& modeName) {
        if (mi < 0 || mi >= int(machines.size())) return;
        Machine mc = machines[size_t(mi)];
        bool wasReplacement = false;
        for (size_t j = 0; j < mc.modes.size(); ++j)
            if (mc.modes[j].name == modeName && mc.modes[j].origin != ModeOrigin::BuiltIn) {
                wasReplacement = mc.modes[j].origin == ModeOrigin::MineReplacing;
                mc.modes.erase(mc.modes.begin() + long(j));
                break;
            }
        std::string e;
        if (!saveMachine(mc, e)) {
            machineStatus = "Remove mode: " + e;
            return;
        }
        reloadMachines();
        machineStatus = (wasReplacement ? "Back to the built-in " : "Deleted your mode ") + mc.name + " / " + modeName;
        std::printf("machines: %s\n", machineStatus.c_str());
    };
    actions.hideBuiltInMode = [&](int mi, const std::string& modeName, bool hide) {
        if (mi < 0 || mi >= int(machines.size())) return;
        Machine mc = machines[size_t(mi)];
        auto& h = mc.hiddenModes;
        h.erase(std::remove(h.begin(), h.end(), modeName), h.end());
        if (hide) h.push_back(modeName);
        std::string e;
        if (!saveMachine(mc, e)) {
            machineStatus = "Hide mode: " + e;
            return;
        }
        reloadMachines();
        machineStatus = (hide ? "Hid the built-in mode " : "Showing again: ") + mc.name + " / " + modeName;
    };
    actions.keepMyMode = [&](int mi, const std::string& modeName) {
        if (mi < 0 || mi >= int(machines.size())) return;
        Machine mc = machines[size_t(mi)];
        int rev = -1;
        for (const MachineMode& b : mc.replacedBuiltIn)
            if (b.name == modeName) rev = b.revision;
        for (MachineMode& md : mc.modes)
            if (md.name == modeName && md.origin == ModeOrigin::MineReplacing) md.keptRevision = rev;
        std::string e;
        if (!saveMachine(mc, e)) {
            machineStatus = "Keep mine: " + e;
            return;
        }
        reloadMachines();
        machineStatus = "Kept your " + mc.name + " / " + modeName + " (built-in revision " + std::to_string(rev) + " not taken)";
    };
    // The other installation's settings: the installed build's ~/.config
    // and the Flatpak's own folder; files already here are kept.
    actions.importOtherInstallation = [&] {
        const char* home = std::getenv("HOME");
        if (!home) return;
        const bool inFlatpak = std::getenv("FLATPAK_ID") != nullptr;
        const std::filesystem::path other = inFlatpak ? std::filesystem::path(home) / ".config" / "freshvision"
                                                      : std::filesystem::path(home) / ".var/app/io.github.frescho.freshVision/config/freshvision";
        const std::filesystem::path here = configDir();
        std::error_code ec;
        if (!std::filesystem::is_directory(other, ec) || std::filesystem::equivalent(other, here, ec)) {
            machineStatus = "No other installation's settings found (" + other.string() + ")";
            return;
        }
        int copied = 0, kept = 0;
        for (const char* sub : {"machines", "freshcoder", "palettes"}) {
            if (!std::filesystem::is_directory(other / sub, ec)) continue;
            std::filesystem::create_directories(here / sub, ec);
            for (const auto& f : std::filesystem::directory_iterator(other / sub, ec)) {
                if (!f.is_regular_file()) continue;
                const auto dst = here / sub / f.path().filename();
                if (std::filesystem::exists(dst, ec)) ++kept;
                else if (std::filesystem::copy_file(f.path(), dst, ec)) ++copied;
            }
        }
        reloadMachines();
        machineStatus = "Imported " + std::to_string(copied) + " file(s) from " + other.string() +
                        (kept ? "; " + std::to_string(kept) + " you already had were kept" : "");
        std::printf("machines: %s\n", machineStatus.c_str());
    };
    // --load-preset / --save-preset (and their GUI calls) work on modes since
    // 0.1.0: GROUP/NAME = the mode NAME of the machine GROUP ("C64" = the C64
    // machine); without a group, the first mode of that name.
    auto findPresetMode = [&](const std::string& group, const std::string& name, int& mi, int& mo) {
        mi = mo = -1;
        const std::string id = group == "C64" ? "c64" : machineIdFor(group.empty() ? "My setups" : group);
        for (int i = 0; i < int(machines.size()) && mo < 0; ++i) {
            if (!group.empty() && machines[size_t(i)].id != id && machines[size_t(i)].name != group) continue;
            for (int j = 0; j < int(machines[size_t(i)].modes.size()); ++j)
                if (machines[size_t(i)].modes[size_t(j)].name == name) mi = i, mo = j;
        }
        return mo >= 0;
    };
    actions.loadPreset = [&](const std::string& group, const std::string& name) {
        int mi, mo;
        if (!findPresetMode(group, name, mi, mo)) {
            std::printf("preset: no mode \"%s\"%s%s among the machines (presets are modes since 0.1.0)\n", name.c_str(),
                        group.empty() ? "" : " in ", group.c_str());
            return;
        }
        const Machine& mc = machines[size_t(mi)];
        applyPresetState(mc.modes[size_t(mo)].settings);
        activeMachineId = mc.id, activeModeName = name, activeByUser = true;
        lastLoadedGroup = group, lastLoadedName = name;
        std::printf("preset: applied %s / %s\n", mc.name.c_str(), name.c_str());
    };
    actions.savePreset = [&](const std::string& group, const std::string& name) {
        const std::string mname = group.empty() ? "My setups" : group;
        int mi = -1;
        const std::string id = group == "C64" ? "c64" : machineIdFor(mname);
        for (int i = 0; i < int(machines.size()); ++i)
            if (machines[size_t(i)].id == id || machines[size_t(i)].name == mname) mi = i;
        if (mi < 0) {
            actions.newMachine(mname, "");
            for (int i = 0; i < int(machines.size()); ++i)
                if (machines[size_t(i)].id == machineIdFor(mname)) mi = i;
        }
        if (mi >= 0) actions.saveCurrentAsMode(mi, name, false, true);
    };
    actions.deletePreset = [&](const std::string& group, const std::string& name) {
        int mi, mo;
        if (findPresetMode(group, name, mi, mo)) actions.removeMyMode(mi, name);
    };
    // RGB auto-tune.
    RgbAutoTune rgbTune;
    Rect pendingRgbCrop;
    RgbAutoTune::Io rgbIo;
    rgbIo.control = [&](const char* name, int64_t& mn, int64_t& mx, int64_t& v) {
        for (const auto& c : cap.controlsSnapshot())
            if (c.name == name && c.hasValue && controlIsSettable(c)) {
                mn = c.min, mx = c.max, v = c.value;
                return true;
            }
        return false;
    };
    rgbIo.set = [&](const char* name, int64_t v) {
        for (const auto& c : cap.controlsSnapshot())
            if (c.name == name) {
                cap.requestControlChange(c.id, v);
                return;
            }
    };
    rgbIo.crop = [&](const Rect& r) { pendingRgbCrop = r; };  // applied after the frame is handed back
    rgbIo.currentCrop = [&] { return cap.appliedCrop(); };
    actions.runRgbAutoTune = [&] { rgbTunePending = true; };
    actions.useMeasuredScreen = [&] {
        const PictureArea& a = rgbTune.lastArea();
        if (!a.valid) return;
        dispOpts.rgbScreenW = a.x1 - a.x0 + 1;
        dispOpts.rgbScreenH = a.y1 - a.y0 + 1;
        std::printf("rgb auto-tune: screen size %dx%d taken from the measured picture\n", dispOpts.rgbScreenW,
                    dispOpts.rgbScreenH);
    };
    uint64_t rgbTuneGen = 0;
    // --machine NAME[/MODE]: the file name or the machine's name.
    std::string startMachine = opt.machine, startMode;
    if (const size_t sl = startMachine.find('/'); sl != std::string::npos)
        startMode = startMachine.substr(sl + 1), startMachine = startMachine.substr(0, sl);
    int startMachineIdx = -1;
    if (!startMachine.empty()) {
        for (size_t i = 0; i < machines.size(); ++i)
            if (machines[i].id == startMachine || machines[i].name == startMachine) startMachineIdx = int(i);
        if (startMachineIdx < 0) std::printf("--machine: no machine \"%s\" in %s\n", startMachine.c_str(), machinesDir().c_str());
    }
    // Explicit video options on the command line win over the automatic
    // switch for the source as it is at start.
    const bool explicitVideo = opt.format != "auto" || opt.width || opt.crop.set || opt.palTiming >= 0 ||
                               !opt.loadPresetName.empty() || !opt.ctrls.empty() || opt.palDecode;
    bool startDecided = false;
    // --load-preset applies before the main loop starts, same as
    // --record-video's auto-start above; --save-preset is a one-shot CLI
    // tool mode (save whatever --format/--size/--crop/--ctrl/etc. produced,
    // then exit) rather than something a running session does at startup.
    if (!opt.loadPresetName.empty()) actions.loadPreset(opt.presetGroup, opt.loadPresetName);
    // After the preset, so a preset's encoder (and display/audio setup) is
    // what a --record-video recording uses (was started before it).
    // With --pal-timing the recording waits until the new timing is in place
    // (after the device reopen), so the file and its tags describe it.
    // Likewise with --machine (its mode is applied once the source has been
    // measured) and --load-preset (it may change the capture and reopen):
    // the recording starts once the start setup is in place, not in the
    // start format (it used to record 384x298 RGB).
    bool recordAfterPalTiming = !opt.recordVideoPath.empty() &&
                                (opt.palTiming >= 0 || startMachineIdx >= 0 || !opt.loadPresetName.empty());
    if (!opt.recordVideoPath.empty() && !recordAfterPalTiming) actions.toggleVideoRecording(true);
    if (recordAfterPalTiming) std::printf("video: the recording starts once the start setup is in place\n");
    if (opt.palTiming >= 0) actions.setPalTiming(opt.palTiming);
    if (!opt.savePresetName.empty()) {
        actions.savePreset(opt.presetGroup, opt.savePresetName);
        g_quit = 1;
    }

    std::vector<SourceInfo> sources = probeVideoSources();
    int64_t lastSourceScanNs = nowNs();

    // Upload now, or N frames later when the picture waits for the sound.
    // A change of size or kind (raw / decoded) empties the queue.
    auto uploadDelayed = [&](const uint8_t* data, size_t stride, uint32_t w, uint32_t h, int layout) {
        const int n = dispOpts.delayPictureToSound ? pictureDelayFrames : 0;
        if (n <= 0) {
            pictureQueue.clear();
            disp.upload(data, stride);
            return;
        }
        if (!pictureQueue.empty() && (pictureQueue.back().w != w || pictureQueue.back().h != h ||
                                      pictureQueue.back().layout != layout || pictureQueue.back().stride != stride))
            pictureQueue.clear();
        DelayedFrame fr;
        if (pictureQueue.size() > size_t(n)) {  // reuse the oldest buffer
            fr = std::move(pictureQueue.front());
            pictureQueue.pop_front();
        }
        fr.data.assign(data, data + stride * h);
        fr.stride = stride, fr.w = w, fr.h = h, fr.layout = layout;
        pictureQueue.push_back(std::move(fr));
        while (pictureQueue.size() > size_t(n) + 1) pictureQueue.pop_front();
        const DelayedFrame& out = pictureQueue.front();  // n frames old once the queue is full
        disp.upload(out.data.data(), out.stride);
    };
    std::string quitReason;  // printed at exit, so an unexpected end can be explained
    while (!g_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (guiOk) gui.processEvent(ev);
            if (ev.type == SDL_QUIT) g_quit = 1, quitReason = "window closed";
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat && !(guiOk && gui.wantCaptureKeyboard())) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE:
                    case SDLK_q: g_quit = 1, quitReason = "Q/Esc key"; break;
                    case SDLK_f:
                    case SDLK_F11: disp.toggleFullscreen(); break;
                    case SDLK_v: disp.setVsync(!disp.vsync()); break;
                    case SDLK_TAB: if (guiOk) guiVisible = !guiVisible; break;
                    default: break;
                }
            }
        }
        if (g_quit) break;

        if (!cap.streaming()) {  // failed restart: retry every 2 s
            SDL_Delay(20);
            restart("retry after failed start");
        } else {
            WaitResult wr = cap.waitFrame(20);
            Frame f;
            bool gotFrame = false;
            int64_t tTake = 0, cTake = 0, tUp = 0;
            if (wr.frame && cap.takeFrame(f)) {
                tTake = nowNs();
                cTake = cap.clockNowNs();
                lastFrameNs = tTake;
                ++framesSinceStart;
                if (f.tsValid && framesSinceStart > 5) {  // the first frames after a start can be irregular
                    if (!refBase) {
                        refBase = true;
                        refSeq0 = refSeqN = f.sequence;
                        refTs0 = refTsN = f.tsNs;
                    } else if (f.sequence > refSeqN) {
                        refSeqN = f.sequence;
                        refTsN = f.tsNs;
                    }
                }
                const CaptureFormat& cf = cap.format();
                if (!opt.palDumpPath.empty() && palTimingMode < 0 && framesSinceStart == 150 &&
                    shown.layout == Layout::Grey8 && !palDumped) {
                    palDumped = true;  // once per run: a later restart must not overwrite it
                    if (FILE* df = std::fopen(opt.palDumpPath.c_str(), "wb")) {
                        for (uint32_t r = 0; r < cf.height; ++r) std::fwrite(f.data + size_t(r) * cf.stride, 1, cf.width, df);
                        std::fclose(df);
                        std::printf("pal dump: %ux%u samples written to %s (Horizontal Size %lld, Position %lld)\n",
                                    cf.width, cf.height, opt.palDumpPath.c_str(),
                                    (long long)controlValue("Horizontal Size", 0),
                                    (long long)controlValue("Horizontal Position", 0));
                    }
                }
                if (!opt.dumpFramePath.empty() && palTimingMode < 0 && framesSinceStart == 150 && !frameDumped) {
                    frameDumped = true;
                    const size_t rowBytes = size_t(cf.width) * shown.bytesPerPixel;
                    if (FILE* df = std::fopen(opt.dumpFramePath.c_str(), "wb")) {
                        for (uint32_t r = 0; r < cf.height; ++r) std::fwrite(f.data + size_t(r) * cf.stride, 1, rowBytes, df);
                        std::fclose(df);
                        static const char* layoutNames[] = {"B,G,R,x", "R,G,B,x", "x,R,G,B", "x,B,G,R",
                                                            "R,G,B",   "B,G,R",   "U,Y0,V,Y1", "one grey sample"};
                        std::printf("frame dump: %ux%u '%s', %u bytes per pixel (%s), written to %s (Horizontal Size %lld, "
                                    "Position %lld)\n",
                                    cf.width, cf.height, fourccString(cf.fourcc).c_str(), shown.bytesPerPixel,
                                    layoutNames[int(shown.layout)], opt.dumpFramePath.c_str(),
                                    (long long)controlValue("Horizontal Size", 0),
                                    (long long)controlValue("Horizontal Position", 0));
                    }
                }
                // S-Video: an RGB capture with brightness and colour on two inputs.
                // Decode PAL: composite on Green (a g8 capture, or the Green
                // input of an RGB capture); with S-Video ticked, Y on Green
                // and C on the chosen input of an RGB capture.
                const bool rgbIn = channelOffset(shown.layout, 0) >= 0;
                const bool svOn = dispOpts.palSvideo && rgbIn;
                bool palOn = dispOpts.palDecode && (shown.layout == Layout::Grey8 || rgbIn);
                palCompositeNow = palOn && !svOn;
                bool palShowing = false;
                if (palOn) {
                    // Samples per subcarrier cycle from the line length the
                    // card is set to; other lengths are not a PAL sampling rate.
                    // Samples per subcarrier cycle: Horizontal Size over the ~283.6
                    // subcarrier cycles of a PAL line (283.75 standard, 283.5 C64).
                    const int64_t hsize = controlValue("Horizontal Size", 0);
                    const pal::StandardInfo& si = stdInfo();
                    const double q = double(hsize) / si.cyclesPerLine;
                    const int K = (q >= 3.5 && std::abs(q - std::round(q)) < 0.01 * q) ? int(std::lround(q)) : 0;
                    // The Display panel's sampling/line fields follow the card's
                    // actual setting whenever it changes (they are what "Apply
                    // PAL timing" will set; they showed 4x during an 8x capture).
                    if (K && hsize != palSyncedSize) {
                        palSyncedSize = hsize;
                        if (K == 4 || K == 6 || K == 8 || K == 10 || K == 12) {
                            dispOpts.palMultiple = K;
                            dispOpts.palLine = int(hsize * 4 / K);
                        }
                    }
                    if (K) {
                        const double fs = double(hsize) * si.lineRate;
                        pal::DecodeParams dp;
                        dp.standard = videoStd();
                        dp.samplesPerCycle = K;
                        dp.burstLen = 8 * K;
                        // The active picture starts 10.5 us (PAL) / 10.9 us (NTSC) after
                        // the sync edge, the capture window `Horizontal Position` samples after it.
                        dp.activeStart = std::max<int>(0, int(std::lround(si.activeStart * fs)) -
                                                              int(controlValue("Horizontal Position", 90)));
                        dp.activeLen = int(std::lround(si.activeLen * fs));
                        dp.burstSearchEnd = pal::burstSearchEndFor(dp.standard, fs, int(controlValue("Horizontal Position", 90)));
                        {
                            const auto& profs = pal::machineProfiles();
                            dp.burstScale = profs[size_t(std::clamp(dispOpts.palUi.profile, 0, int(profs.size()) - 1))].burstScale;
                        }
                        dp.burstStart = (palBurstStart >= 0 && palFramesSinceSearch < 50) ? palBurstStart : -1;
                        dp.averageLines = dispOpts.palUi.averageLines;
                        dp.chromaCycles = dispOpts.palUi.chromaCycles;
                        dp.edgeDesat = dispOpts.palUi.edgeDesat;
                        dp.brightness = dispOpts.palUi.picture.brightness;
                        dp.contrast = dispOpts.palUi.picture.contrast;
                        dp.saturation = dispOpts.palUi.picture.saturation;
                        dp.hueDeg = dispOpts.palUi.picture.hue;
                        dp.gamma = dispOpts.palUi.picture.gamma;
                        dp.samplesPerLine = int(hsize);
                        dp.forceParity = palParityHeld ? palParity : -1;
                        if (dispOpts.palUi.pll) {
                            palPllLines.assign(cf.height, palPllErr);
                            dp.lineFreqErr = palPllLines.data();
                        }
                        dp.luma = pal::LumaFilter(std::clamp(dispOpts.palUi.luma, 0, 2));
                        dp.threads = palThreads;
                        const uint8_t* rows = f.data;
                        if (svOn) {
                            // Two planes, one byte per sample: brightness and colour.
                            const int lo = channelOffset(shown.layout, std::clamp(dispOpts.svideoLuma, 0, 2));
                            const int co = channelOffset(shown.layout, std::clamp(dispOpts.svideoChroma, 0, 2));
                            const unsigned bpp = shown.bytesPerPixel;
                            palRows.resize(size_t(cf.width) * cf.height);
                            svChroma.resize(size_t(cf.width) * cf.height);
                            for (uint32_t r = 0; r < cf.height; ++r) {
                                const uint8_t* src = f.data + size_t(r) * cf.stride;
                                uint8_t* yd = &palRows[size_t(r) * cf.width];
                                uint8_t* cd = &svChroma[size_t(r) * cf.width];
                                for (uint32_t x = 0; x < cf.width; ++x, src += bpp) yd[x] = src[lo], cd[x] = src[co];
                            }
                            rows = palRows.data();
                            dp.chromaPlane = svChroma.data();
                        } else if (rgbIn) {
                            // Composite on the Green input of an RGB capture: that one plane.
                            const int go = channelOffset(shown.layout, 1);
                            const unsigned bpp = shown.bytesPerPixel;
                            palRows.resize(size_t(cf.width) * cf.height);
                            for (uint32_t r = 0; r < cf.height; ++r) {
                                const uint8_t* src = f.data + size_t(r) * cf.stride + go;
                                uint8_t* yd = &palRows[size_t(r) * cf.width];
                                for (uint32_t x = 0; x < cf.width; ++x, src += bpp) yd[x] = *src;
                            }
                            rows = palRows.data();
                        } else if (cf.stride != cf.width) {  // decoder wants rows without padding
                            palRows.resize(size_t(cf.width) * cf.height);
                            for (uint32_t r = 0; r < cf.height; ++r)
                                std::memcpy(&palRows[size_t(r) * cf.width], f.data + size_t(r) * cf.stride, cf.width);
                            rows = palRows.data();
                        }
                        bool relockMissed = false;
                        if (dispOpts.palUi.relock) {
                            // Writable planes (the captured frame is the driver's buffer).
                            if (rows == f.data) {
                                palRows.resize(size_t(cf.width) * cf.height);
                                for (uint32_t r = 0; r < cf.height; ++r)
                                    std::memcpy(&palRows[size_t(r) * cf.width], f.data + size_t(r) * cf.stride, cf.width);
                                rows = palRows.data();
                            }
                            relockMissed = !palRelock.process(palRows.data(), int(cf.width), int(cf.height), dp.activeStart,
                                                              dp.activeStart + dp.activeLen, svOn ? svChroma.data() : nullptr);
                            dp.lineClamp = true;
                        }
                        if (dispOpts.palUi.removeStripes) {
                            if (palStripesHsize != hsize) palStripes.reset(), palStripesHsize = hsize;
                            palStripes.learn(rows, int(cf.width), int(cf.height), dp.activeStart, dp.activeStart + dp.activeLen);
                            dp.linePattern = palStripes.data();
                        }
                        const int64_t td = nowNs();
                        // A frame the re-alignment couldn't place: the previous picture stays.
                        int outW = relockMissed && palLastOutW > 0 && !palRgb.empty()
                                       ? palLastOutW
                                       : pal::decodeInto(rows, int(cf.width), int(cf.height), dp, palRgb, &palInfo);
                        palLastOutW = outW;
                        if (dispOpts.palUi.softText && !relockMissed) {
                            const auto& profs = pal::machineProfiles();
                            pal::softTwoColour(palRgb.data(), outW, int(cf.height),
                                               profs[size_t(std::clamp(dispOpts.palUi.profile, 0, int(profs.size()) - 1))].palette,
                                               palSoftState);
                        }
                        palMsSum += double(nowNs() - td) / 1e6;
                        ++palMsCount;
                        if (dp.burstStart < 0 || !palInfo.colour) palFramesSinceSearch = 0;
                        else ++palFramesSinceSearch;
                        palBurstStart = palInfo.colour ? palInfo.burstStart : -1;
                        if (palInfo.colour && !palInfo.stepError.empty()) {
                            // The step error is measured on the bursts, which the
                            // correction ramp doesn't touch: it is the raw error, and
                            // the estimate follows it at the "PLL speed".
                            const float a = std::max(0.02f, dispOpts.palUi.pllBandwidth);
                            const float measured = float(palInfo.meanStepErrorDeg * pal::kPi / 180.0);
                            palPllErr = dispOpts.palUi.pll ? palPllErr + a * (measured - palPllErr) : 0.0f;
                        }
                        {
                            // machines: which input carries a burst (S-Video: its colour
                            // input; composite g8: the green one, where the card samples).
                            const int ch = svOn ? std::clamp(dispOpts.svideoChroma, 0, 2) : 1;
                            ++colourFrames[ch];
                            colourSeen[ch] += palInfo.colour ? 1 : 0;
                            // only frames sampled with the mode's own timing in place
                            if (ch == tentativeColourCh && palTimingMode < 0 && framesSinceStart >= 10) {
                                ++tentFrames;
                                tentSeen += palInfo.colour ? 1 : 0;
                            }
                            if (ch == tentativeColourCh && tentFrames >= 25) {
                                tentativeColourCh = -1;
                                colourKnown[ch] = tentSeen * 2 > tentFrames ? 1 : 0;
                                if (!colourKnown[ch] && (tentativeFromStart || (autoSwitch && !activeByUser)) && !recorder.active()) {
                                    DetectedMode d2 = detected;
                                    for (int k = 0; k < 3; ++k) d2.colourOn[k] = colourKnown[k];
                                    const MatchResult m2 = bestMatch(machines, d2);
                                    if (m2.score >= 0) {
                                        const Machine& m2m = machines[size_t(m2.machine)];
                                        const MachineMode& m2mode = m2m.modes[size_t(m2.mode)];
                                        if (m2m.id != activeMachineId || m2mode.name != activeModeName) {
                                            std::printf("machines: no colour on the %s wire -> %s / %s (%s)\n",
                                                        ch == 0 ? "red" : ch == 1 ? "green" : "blue", m2m.name.c_str(),
                                                        m2mode.name.c_str(), m2.why.c_str());
                                            detected = d2;
                                            pendingColourMachine = m2.machine, pendingColourMode = m2.mode;
                                        }
                                    }
                                }
                            }
                        }
                        if (palInfo.colour) {  // machines: carrier cycles per line (as auto-tune reads it)
                            cplVec += std::polar(1.0, palInfo.phaseStepDeg * pal::kPi / 180.0);
                            ++cplN;
                        }
                        if (palInfo.colour && !palInfo.parityAmbiguous) {
                            if (palParity >= 0 && palParity != palInfo.parity)
                                std::printf("pal: V-switch parity %d -> %d\n", palParity, palInfo.parity);
                            palParity = palInfo.parity;
                            palParityHeld = false;
                        } else if (palInfo.colour && palParity >= 0) {
                            palParityHeld = true;  // next frame: keep the last decided parity
                        }
                        // One pixel per machine pixel, from its settled
                        // centre, optionally snapped to the machine's palette.
                        const uint8_t* showRgb = palRgb.data();
                        int showW = outW;
                        palGridW = 0;
                        if (outW > 0 && dispOpts.palUi.pixelGrid) {
                            const auto& profs = pal::machineProfiles();
                            const auto& prof = profs[size_t(std::clamp(dispOpts.palUi.profile, 0, int(profs.size()) - 1))];
                            pal::GridStage::Settings gs;
                            const double kk = K / 4.0;
                            if (prof.samplesPerPixelAt4x > 0)
                                gs.periodLo = prof.samplesPerPixelAt4x * kk / 1.03, gs.periodHi = prof.samplesPerPixelAt4x * kk * 1.03;
                            else
                                gs.periodLo = 1.5 * kk, gs.periodHi = 6.0 * kk;
                            gs.core = dispOpts.palUi.gridCore;
                            gs.snap = dispOpts.palUi.snap;
                            gs.colours = std::max(2, dispOpts.palUi.colours);
                            gs.startPalette = prof.palette;
                            gs.fixedPalette = fv::chosenPalette(dispOpts.palUi);
                            if (!gs.fixedPalette.empty()) gs.colours = int(gs.fixedPalette.size());
                            gs.c64Cells = dispOpts.palUi.c64Cells && pal::isC64Profile(dispOpts.palUi.profile);
                            gs.tmsCells = dispOpts.palUi.tmsCells && pal::isTmsProfile(dispOpts.palUi.profile);
                            // Live: keep learning, slowly (every 5 s), so colours of later
                            // scenes get their own entries.
                            gs.relearnEvery = 250;
                            gs.poolEvery = 10;
                            gs.learnFrames = 1 << 30;
                            palGrid.configure(gs);
                            if (dispOpts.palUi.requestRedetect) palGrid.redetect();
                            if (dispOpts.palUi.requestRelearn) palGrid.relearn();
                            int gw = 0;
                            if (palGrid.process(palRgb.data(), outW, int(cf.height), palGridRgb, gw)) {
                                showRgb = palGridRgb.data();
                                showW = gw;
                                palGridW = gw;
                            }
                        }
                        dispOpts.palUi.requestRedetect = dispOpts.palUi.requestRelearn = false;
                        if (showW > 0) {
                            std::string e;
                            if (!dispIsPal || showW != dispPalW) {
                                if (disp.setFrame(palShown, uint32_t(showW), cf.height, 0, e)) {
                                    // 52 us of active line shown 4:3 against one field's lines.
                                    if (!dispIsPal) aspectBeforePal = dispOpts.pixelAspect;
                                    dispOpts.pixelAspect = (4.0 / 3.0) / (double(showW) / double(cf.height));
                                    dispIsPal = true;
                                    dispPalW = showW;
                                } else {
                                    std::printf("pal display: %s\n", e.c_str());
                                }
                            }
                            if (dispIsPal) {
                                uploadDelayed(showRgb, size_t(showW) * 3, uint32_t(showW), cf.height, 1);
                                palShowing = true;
                            }
                        }
                        std::string relockText;
                        if (dispOpts.palUi.relock) {
                            char b[96];
                            std::snprintf(b, sizeof b, ", re-align %s (%ld placed, %ld kept back)", palRelock.how().c_str(),
                                          palRelock.aligned(), palRelock.missed());
                            relockText = b;
                        }
                        std::string stripesText = "off";
                        if (dispOpts.palUi.removeStripes) {
                            char b[96];
                            if (palStripes.ready())
                                std::snprintf(b, sizeof b, "removed (%.2f codes rms, %.0f%% of the line known)", palStripes.rms(),
                                              palStripes.coverage() * 100);
                            else
                                std::snprintf(b, sizeof b, "learning (%ld flat segments)", palStripes.segments());
                            stripesText = b;
                        }
                        char st[400];
                        std::snprintf(st, sizeof st,
                                      "%s %dxFsc (Horizontal Size %lld): colour %s, burst %.1f codes%s at sample %d, "
                                      "carrier step %.1f deg/line (expected %.1f), PLL %s%.2f deg/line, parity %d%s, "
                                      "line averaging %s, "
                                      "brightness filter %s, clock stripes %s%s, output %dx%u, %d threads",
                                      si.name, K, (long long)hsize, palInfo.colour ? "on" : "off (no burst)", palInfo.burstAmplitude,
                                      palInfo.clipped ? " (clipped at black)" : "", palInfo.burstStart,
                                      palInfo.phaseStepDeg, palInfo.expectedStepDeg, dispOpts.palUi.pll ? "" : "off ",
                                      palPllErr * 180.0 / pal::kPi, palInfo.parity,
                                      palInfo.parityAmbiguous ? " (held)" : "", dp.averageLines ? "on" : "off",
                                      dp.chromaPlane ? "none (S-Video)" : dp.luma == pal::LumaFilter::Box ? "box" : dp.luma == pal::LumaFilter::Notch ? "notch" : "off",
                                      stripesText.c_str(), relockText.c_str(), outW, cf.height, palThreads);
                        palStatus = st;
                        palPanelMsSum += double(nowNs() - td) / 1e6;
                        ++palPanelMsCount;
                        if (nowNs() - palPanelNs >= 1000000000LL || palPanel.empty() || palPanel[0] != 'P') {
                            palPanelNs = nowNs();
                            // The step as 0..360 (-179.8 and 180.2 are the same step).
                            const double step = std::fmod(palInfo.phaseStepDeg + 360.0, 360.0);
                            char pb[640];
                            std::snprintf(pb, sizeof pb,
                                          "%s %dx Fsc, Horizontal Size %lld\n"
                                          "colour    %-3s   burst %5.1f codes at %4d\n"
                                          "carrier   %6.1f deg/line (expected %5.1f)\n"
                                          "PLL       %+6.2f deg/line%s, parity %d%s\n"
                                          "averaging %-3s   brightness filter %s\n"
                                          "stripes   %s\n"
                                          "output    %dx%u, %d threads, %4.1f ms/frame",
                                          si.name, K, (long long)hsize, palInfo.colour ? "on" : "off", palInfo.burstAmplitude,
                                          palInfo.burstStart, step, palInfo.expectedStepDeg,
                                          palPllErr * 180.0 / pal::kPi, dispOpts.palUi.pll ? "" : " (off)", palInfo.parity,
                                          palInfo.parityAmbiguous ? " (held)" : "", dp.averageLines ? "on" : "off",
                                          dp.chromaPlane ? "none (S-Video)" : dp.luma == pal::LumaFilter::Box ? "box" : dp.luma == pal::LumaFilter::Notch ? "notch" : "off",
                                          stripesText.c_str(), outW, cf.height, palThreads,
                                          palPanelMsCount ? palPanelMsSum / palPanelMsCount : 0.0);
                            palPanel = pb;
                            palPanelMsSum = 0;
                            palPanelMsCount = 0;
                        }
                        // Auto-tune measurements from the raw samples of this
                        // frame (every 4th line): the burst's lowest samples,
                        // blanking, and how much of the picture sits at 255.
                        if (dispOpts.palAutoTune && !svOn) {  // composite only: it sets the Green levels
                            const int bs = std::max(0, palInfo.burstStart), bl = dp.burstLen;
                            int bmin = 255, zeros = 0, bcount = 0;
                            size_t at255 = 0, cnt = 0;
                            int peak = 0;
                            for (uint32_t r = 8; r + 8 < cf.height; r += 4) {
                                const uint8_t* row = rows + size_t(r) * cf.width;
                                for (int x = bs; x < std::min<int>(bs + bl, int(cf.width)); ++x) {
                                    bmin = std::min<int>(bmin, row[x]);
                                    zeros += row[x] == 0;
                                    ++bcount;
                                }
                                for (uint32_t x = uint32_t(dp.activeStart); x < cf.width; x += 2) {
                                    at255 += row[x] == 255;
                                    peak = std::max<int>(peak, row[x]);
                                    ++cnt;
                                }
                            }
                            ++autoAcc.frames;
                            autoAcc.blank += palInfo.clipped ? 0.0 : palInfo.blankCode;
                            autoAcc.burstMin += bmin;
                            autoAcc.clippedBurst += bcount ? double(zeros) / bcount : 0.0;
                            autoAcc.white255 += cnt ? double(at255) / double(cnt) : 0.0;
                            autoAcc.peak += peak;
                            if (palInfo.colour) {
                                ++autoAcc.colourFrames;
                                autoAcc.stepVec += std::polar(1.0, palInfo.phaseStepDeg * pal::kPi / 180.0);
                            }
                        }
                    } else {
                        palStatus = "Horizontal Size is " + std::to_string(hsize) + ", not a multiple of the " + si.name +
                                    " subcarrier (" + (si.vSwitch ? "e.g. 1135/1134 = 4x" : "e.g. 910/912 = 4x") +
                                    "): showing the raw samples. \"Apply timing\" sets it.";
                        palPanel = palStatus;
                    }
                } else {
                    palStatus.clear();
                    palPanel.clear();
                }
                if (!palShowing) {
                    if (dispIsPal) {
                        std::string e;
                        if (!disp.setFrame(shown, cf.width, cf.height, cf.colorspace, e))
                            std::printf("display: %s\n", e.c_str());
                        dispIsPal = false;
                        dispOpts.pixelAspect = aspectBeforePal;
                    }
                    uploadDelayed(f.data, cf.stride, cf.width, cf.height, 0);
                }
                // The frame's capture time, moved onto the app's clock (the
                // capture clock and CLOCK_MONOTONIC can differ): display-loop
                // timing then no longer shows up in the recording.
                int64_t capturedNs = f.tsValid ? tTake - (cTake - f.tsNs) : tTake;
                recorder.pushVideoFrame(f.data, cap.format().width, cap.format().height, cap.format().stride,
                                        capturedNs);
                if (rgbTune.running() && shown.layout != Layout::Grey8)
                    rgbTune.onFrame(FrameView{f.data, cf.width, cf.height, cf.stride, shown.layout}, rgbIo);
                cap.release(f);
                if (pendingRgbCrop.set) {  // after the buffer went back: the crop restarts the capture
                    const Rect r = pendingRgbCrop;
                    pendingRgbCrop = Rect{};
                    actions.applyCrop(r);
                }
                tUp = nowNs();
                gotFrame = true;
            }

            if (pendingColourMachine >= 0) {
                const int mi = pendingColourMachine, mo = pendingColourMode;
                pendingColourMachine = pendingColourMode = -1;
                applyMachineMode(mi, mo, false);
            }
            applyPendingPalTiming();
            if (modeBaselineAtNs && nowNs() >= modeBaselineAtNs && palTimingMode < 0 && !windowReopenAtNs &&
                framesSinceStart >= 25) {
                modeBaseline = capturePresetState("", baselineMode);
                haveModeBaseline = true;
                modeBaselineAtNs = 0;
                std::printf("machines: starting point for saving %s taken\n", baselineMode.c_str());
            }
            if (windowReopenAtNs && nowNs() >= windowReopenAtNs && palTimingMode < 0) {
                windowReopenAtNs = 0;
                restart("the new capture window applies at a reopen", 0, 0, true);
            }
            if (dispOpts.palAutoTune && palCompositeNow) runAutoTune();
            // RGB auto-tune: (re)started once the stream has run half a second;
            // a restart in the middle starts it over.
            if (rgbTuneStopRequest) {
                rgbTuneStopRequest = false;
                if (rgbTune.running()) {
                    rgbTune.stop();
                    std::printf("rgb auto-tune: stopped (the new mode doesn't use it)\n");
                }
            }
            if (rgbTune.running() && rgbTuneGen != streamGeneration) {
                rgbTune.stop();
                rgbTunePending = dispOpts.rgbAutoTune && !dispOpts.palDecode;
            }
            // After a mode was applied, wait until the control snapshot (once a
            // second) shows its values: the tune reads the phase to keep from
            // it (it put back a stale 0 over a mode's Phase 21).
            if (rgbTunePending && cap.streaming() && framesSinceStart >= 25 && palTimingMode < 0 &&
                nowNs() - lastMachineApplyNs > 1500000000LL) {
                rgbTunePending = false;
                if (!dispOpts.rgbAutoTune || dispOpts.palDecode) {
                    // Not for PAL decoding (composite or S-Video), nor once switched off.
                } else if (shown.layout == Layout::Grey8) {
                    std::printf("rgb auto-tune: not for a g8 capture (composite has its own auto-tune)\n");
                } else {
                    rgbTune.start(dispOpts.rgbAutoCrop, dispOpts.rgbScreenW, dispOpts.rgbScreenH);
                    rgbTuneGen = streamGeneration;
                }
            }
            if (recordAfterPalTiming && palTimingMode < 0 && framesSinceStart >= 50 && !windowReopenAtNs &&
                (startMachineIdx < 0 || startDecided)) {
                recordAfterPalTiming = false;
                if (!opt.recordEncoder.empty()) videoEncoderKind = cliEncoderKind;
                actions.toggleVideoRecording(true);
            }

            if (wr.restartNeeded) {
                restart(wr.reason);
                if (dispOpts.rgbAutoTune && wr.reason.find("mode change") != std::string::npos) rgbTunePending = true;
            } else {
                const int64_t now = nowNs();
                if (now - lastFrameNs > 5000000000LL && cap.signalPresent()) {
                    restart("no frames for 5 s");
                }
            }

            if (guiOk && guiVisible && nowNs() - lastSourceScanNs >= 2000000000LL) {
                lastSourceScanNs = nowNs();
                sources = probeVideoSources();  // cheap (open+query+close per node); only while a panel could show it
            }
            // The sound-device list takes ~20 ms to read (a whole frame at
            // 50 Hz; every 5 s it cost a captured frame), so it is
            // read on its own thread and taken over once ready.
            if (audioScan.valid() && audioScan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                audioDevices = audioScan.get();
            if (guiOk && guiVisible && !audioScan.valid() && nowNs() - lastAudioScanNs >= 5000000000LL) {
                lastAudioScanNs = nowNs();
                audioScan = std::async(std::launch::async, enumerateAudioDevices);
            }

            // Render every iteration (not only when a frame arrived) so the
            // GUI stays responsive even with no signal / a paused source.
            disp.beginFrame();
            {
                // The picture fills the window beside the control column
                // (or all of it while Tab hides the column).
                int ww = 0, wh = 0;
                SDL_GL_GetDrawableSize(disp.window(), &ww, &wh);
                const int left = (guiOk && guiVisible) ? int(gui.panelWidth()) : 0;
                dispOpts.areaX = left;
                dispOpts.areaY = 0;
                dispOpts.areaW = std::max(1, ww - left);
                dispOpts.areaH = std::max(1, wh);
            }
            disp.drawVideo(dispOpts);
            if (guiOk) {
                gui.newFrame();
                GeometryState geom;
                geom.width = cap.format().width;
                geom.height = cap.format().height;
                geom.crop = cap.appliedCrop();
                geom.maxScreenFactor = disp.maxScreenFactor(dispOpts.pixelAspect);
                geom.grey8 = shown.layout == Layout::Grey8;
                geom.rgbChannels = channelOffset(shown.layout, 0) >= 0;
                geom.palStatus = palPanel;
                geom.autoStatus = dispOpts.palAutoTune ? autoStatus : std::string();
                {
                    const pal::GridEstimate& gg = palGrid.grid();
                    geom.palGrid.valid = gg.valid;
                    geom.palGrid.period = gg.period, geom.palGrid.phase = gg.phase, geom.palGrid.score = gg.score;
                    geom.palGrid.pixelsPerLine = palGridW;
                    geom.palGrid.palette.assign(palGrid.palette().begin(), palGrid.palette().end());
                    const pal::Calibration& cal = palGrid.calibration();
                    geom.palGrid.calSat = cal.satGain, geom.palGrid.calHueDeg = cal.hueRad * 180 / pal::kPi;
                    geom.palGrid.calContrast = cal.contrast, geom.palGrid.calOffset = cal.offset;
                }
                AudioPanelState audioState;
                // Non-blocking: if the audio thread holds a lock right now,
                // the panel shows the last values instead of waiting.
                AudioMixer::DisplayState ads = audioMixer.displayState();
                audioState.devices = audioDevices;
                audioState.activeSources = ads.sources;
                audioState.sourceCounters = ads.sourceCounters;
                audioState.hwAudio = ads.hwInfo;
                audioState.monitoring = audioMixer.monitoring();
                audioState.playbackDevices = playbackDevices;
                audioState.playbackDeviceId = monitorPlaybackId;
                audioState.monitorError = monitorError;
                audioState.monitorHwAvailable = ads.monitorHwAvailable;
                audioState.monitorHwMuted = ads.monitorHwMuted;
                audioState.monitorHwVolume = ads.monitorHwVolume;
                audioState.mixCounters = audioMixer.mixCounters();
                audioState.outputChannels = kAudioOutputChannels;
                audioState.recording = audioMixer.recording();
                audioState.monitorSoundMs = monitorSoundMs;
                audioState.pictureMs = pictureAgeMs;
                audioState.pictureDelayFrames = dispOpts.delayPictureToSound ? pictureDelayFrames : 0;
                audioState.recordingPath = ads.recordingPath;
                audioState.framesRecorded = ads.framesRecorded;
                {
                    // Audio hardware clock vs. system/video clock, from the
                    // first active source's real captured-frame count against wall time
                    // since it was added (see AudioMixer::driftReference in audio.h).
                    auto [audioFrames, audioAddedNs] = ads.driftReference;
                    if (audioAddedNs != 0) {
                        double elapsedSec = double(nowNs() - audioAddedNs) / 1e9;
                        if (elapsedSec > 0.5) {
                            double expected = elapsedSec * double(audioMixer.sampleRate());
                            audioState.driftMs = (double(audioFrames) - expected) / double(audioMixer.sampleRate()) * 1000.0;
                            audioState.driftSeconds = elapsedSec;
                        }
                    }
                }
                RecorderPanelState recState;
                recState.recording = recorder.active();
                recState.path = recorder.path();
                recState.lastRecording = lastRecording;
                recState.encoderName = recorder.activeEncoderName();
                recState.kind = videoEncoderKind;
                recState.counters = recorder.counters();
                recState.availableKinds = availableEncoderKinds;
                recState.canvasWidth = cap.format().width;
                recState.canvasHeight = cap.format().height;
                recState.fps = captureFps >= 1.0 ? double(std::lround(captureFps)) : 50.0;
                recState.elapsedSec = recorder.active() ? double(nowNs() - recStartNs) / 1e9 : 0.0;
                recState.defaultFolder = recDefaultFolder;
                {
                    std::string chosen, e;
                    if (folderPicker.poll(chosen, e)) {
                        if (!e.empty()) recError = e;
                        else if (!chosen.empty()) {
                            pickedFolder = chosen;
                            ++pickedSeq;
                        }
                    }
                }
                recState.error = recError;
                recState.folderDialogOpen = folderPicker.running();
                recState.pickedFolder = pickedFolder;
                recState.pickedSeq = pickedSeq;
                PresetPanelState presetState;
                presetState.lastLoadedGroup = lastLoadedGroup;
                presetState.lastLoadedName = lastLoadedName;
                MachinePanelState machineState;
                for (const Machine& mc : machines) {
                    MachinePanelState::Entry e;
                    e.name = mc.name, e.id = mc.id, e.note = mc.note, e.path = mc.path, e.group = mc.group;
                    e.builtIn = mc.builtIn, e.mine = mc.mine;
                    e.hidden = mc.hiddenModes;
                    const size_t mi = machineState.machines.size();
                    for (size_t j = 0; j < mc.modes.size(); ++j) {
                        const MachineMode& md = mc.modes[j];
                        e.modes.push_back(md.name);
                        e.modeMatches.push_back(scoreMode(mc, md, detected) >= 0);
                        e.modeManual.push_back(md.match.manual);
                        e.modeOrigin.push_back(md.origin == ModeOrigin::BuiltIn ? 0 : md.origin == ModeOrigin::Mine ? 1 : 2);
                        e.modeDiff.push_back(mi < machineDiffs.size() && j < machineDiffs[mi].size() ? machineDiffs[mi][j]
                                                                                                    : std::vector<std::string>{});
                    }
                    machineState.machines.push_back(std::move(e));
                }
                machineState.activeMachine = findMachine(activeMachineId);
                if (machineState.activeMachine >= 0) {
                    const Machine& am = machines[size_t(machineState.activeMachine)];
                    for (size_t j = 0; j < am.modes.size(); ++j)
                        if (am.modes[j].name == activeModeName) machineState.activeMode = int(j);
                }
                machineState.matchMachine = machineMatch.machine;
                machineState.matchMode = machineMatch.mode;
                machineState.matchWhy = machineMatch.why;
                machineState.detected = detected.describeLines();
                machineState.hint = machineHint;
                machineState.status = machineStatus;
                machineState.errors = machineErrors;
                machineState.autoSwitch = autoSwitch;
                machineState.folder = machinesDir();
                machineState.updates = machineUpdates;
                // What a Save into the active mode would take over (refreshed twice a second).
                if (haveModeBaseline && activeModeName == baselineMode) {
                    if (nowNs() - lastChangesNs > 500000000LL) {
                        lastChangesNs = nowNs();
                        pendingChanges.clear();
                        mergeModeChanges(modeBaseline, modeBaseline, capturePresetState("", baselineMode), pendingChanges);
                    }
                    machineState.changesKnown = true;
                    machineState.changes = pendingChanges;
                }
                geom.rgbStatus = rgbTune.status();
                geom.rgbRunning = rgbTune.running();
                if (rgbTune.lastArea().valid) {
                    geom.rgbAreaW = rgbTune.lastArea().x1 - rgbTune.lastArea().x0 + 1;
                    geom.rgbAreaH = rgbTune.lastArea().y1 - rgbTune.lastArea().y0 + 1;
                }
                gui.draw(guiVisible, statsText, cap.controlsSnapshot(), dispOpts, geom, sources, opt.device,
                        audioState, recState, presetState, machineState, actions);
                gui.render();
            }
            disp.endFrame();
            const int64_t tPresent = nowNs();

            // Safety net: if the display loop takes over 1 s per frame for
            // 3 s in a row while recording, stop the recording by itself.
            // A frozen GUI can't be clicked, so Stop would be unreachable and
            // the file left unfinished (NVENC + panels).
            if (recorder.active() && lastLoopEndNs > 0 && tPresent - lastLoopEndNs > 1000000000LL) {
                if (slowSinceNs == 0) slowSinceNs = lastLoopEndNs;
                if (tPresent - slowSinceNs >= 3000000000LL) {
                    recError = "Recording stopped automatically: the app took over 1 s per frame for 3 s. "
                               "(The file is finished and playable up to that point.)";
                    std::printf("video: %s\n", recError.c_str());
                    logRecordingStop();
                    recorder.stop();
                    slowSinceNs = 0;
                }
            } else {
                slowSinceNs = 0;
            }
            lastLoopEndNs = tPresent;

            if (gotFrame) {
                const int64_t cPresent = cap.clockNowNs();
                win.uploadMs.push_back(double(tUp - tTake) / 1e6);
                win.swapMs.push_back(double(tPresent - tUp) / 1e6);
                ++win.displayed;
                if (lastPresentNs) win.presentDt.push_back(double(tPresent - lastPresentNs) / 1e6);
                lastPresentNs = tPresent;
                if (f.tsValid) {
                    win.ageDequeue.push_back(double(f.dequeuedNs - f.tsNs) / 1e6);
                    win.ageTake.push_back(double(cTake - f.tsNs) / 1e6);
                    double a = double(cPresent - f.tsNs) / 1e6;
                    win.agePresent.push_back(a);
                    allAgePresent.push_back(a);
                    if (!clockReported) {
                        std::printf("buffer timestamp clock (detected): %s\n", cap.clockName().c_str());
                        clockReported = true;
                    }
                }
            }

            const int64_t now = nowNs();
            if (now - lastStatNs >= 1000000000LL) {
                double secs = double(now - lastStatNs) / 1e9;
                lastStatNs = now;
                const CaptureFormat& fm = cap.format();
                Counters before = prev;
                Counters cur = accumulate();
                captureFps = double(cur.delivered - before.delivered) / secs;
                {
                    // Sound vs. picture: how late each reaches you.
                    if (!win.agePresent.empty()) pictureAgeMs = median(win.agePresent);
                    const double out = audioMixer.monitorOutputDelayMs();
                    if (audioMixer.monitoring() && out > 0) {
                        int srcDelay = 0;
                        for (const auto& src : audioMixer.displayState().sources) srcDelay = std::max(srcDelay, src.delayMs);
                        // + the capture period (10 ms) + half a mixer tick (10 ms) + any source delay
                        monitorSoundMs = out + 10.0 + 10.0 + srcDelay;
                    } else {
                        monitorSoundMs = 0;
                    }
                    const double frameMs = captureFps > 1 ? 1000.0 / captureFps : 20.0;
                    const int want = monitorSoundMs > 0
                                         ? std::clamp(int(std::lround((monitorSoundMs - pictureAgeMs) / frameMs)), 0, 15)
                                         : 0;
                    if (dispOpts.delayPictureToSound && want != pictureDelayFrames)
                        std::printf("sound sync: picture held back %d frames (monitor sound %.0f ms, picture %.0f ms)\n", want,
                                    monitorSoundMs, pictureAgeMs);
                    pictureDelayFrames = want;
                }
                char line[400];
                std::snprintf(line, sizeof line,
                              "%ux%u %s | fps %.1f (driver %llu) | overwritten %llu | gaps %llu | timeouts %llu | "
                              "age@dq med %.1f | age@take med %.1f | age@present med %.1f max %.1f ms | "
                              "present dt sd %.2f max %.1f | upload %.1f swap %.1f ms | vsync %s | signal %s%s",
                              fm.width, fm.height, chosen->name, win.displayed / secs,
                              (unsigned long long)(cur.delivered - before.delivered),
                              (unsigned long long)(cur.overwritten - before.overwritten),
                              (unsigned long long)(cur.driverGaps - before.driverGaps),
                              (unsigned long long)(cur.timeouts - before.timeouts), median(win.ageDequeue),
                              median(win.ageTake), median(win.agePresent), maxOf(win.agePresent), stddev(win.presentDt),
                              maxOf(win.presentDt), mean(win.uploadMs), mean(win.swapMs), disp.vsync() ? "on" : "off",
                              cap.signalTypeName().empty() ? "?" : cap.signalTypeName().c_str(),
                              cap.signalPresent() ? "" : " | NO SIGNAL");
                std::printf("%s\n", line);
                // The panel's copy: fixed lines and widths, so it doesn't reflow
                // when a number changes length (the log line stays as it is).
                char panel[600];
                std::snprintf(panel, sizeof panel,
                              "%ux%u %s, signal %s%s, vsync %s\n"
                              "fps       %5.1f  (driver %3llu)\n"
                              "lost      overwritten %3llu, gaps %3llu, timeouts %3llu\n"
                              "age ms    dequeue %5.1f, take %5.1f, present %5.1f (max %5.1f)\n"
                              "present   dt sd %5.2f, max %5.1f ms\n"
                              "gpu ms    upload %4.1f, swap %4.1f",
                              fm.width, fm.height, chosen->name, cap.signalTypeName().empty() ? "?" : cap.signalTypeName().c_str(),
                              cap.signalPresent() ? "" : " (NO SIGNAL)", disp.vsync() ? "on" : "off", win.displayed / secs,
                              (unsigned long long)(cur.delivered - before.delivered),
                              (unsigned long long)(cur.overwritten - before.overwritten),
                              (unsigned long long)(cur.driverGaps - before.driverGaps),
                              (unsigned long long)(cur.timeouts - before.timeouts), median(win.ageDequeue), median(win.ageTake),
                              median(win.agePresent), maxOf(win.agePresent), stddev(win.presentDt), maxOf(win.presentDt),
                              mean(win.uploadMs), mean(win.swapMs));
                statsPanel = panel;

                // Print audio/video drift alongside the video stats line
                // (not folded into `line` itself, kept separate so the video-only log
                // scraping used throughout this project keeps working unchanged).
                AudioCounters mixc = audioMixer.mixCounters();
                AudioMixer::DisplayState lds = audioMixer.displayState();  // non-blocking, see the panel fill
                std::string driftText = "no source";
                auto [audioFrames, audioAddedNs] = lds.driftReference;
                if (audioAddedNs != 0) {
                    double elapsedSec = double(now - audioAddedNs) / 1e9;
                    if (elapsedSec > 0.5) {
                        double expected = elapsedSec * double(audioMixer.sampleRate());
                        double driftMs = (double(audioFrames) - expected) / double(audioMixer.sampleRate()) * 1000.0;
                        char d[64];
                        std::snprintf(d, sizeof d, "drift %.1f ms over %.0f s", driftMs, elapsedSec);
                        driftText = d;
                    }
                }
                // Underruns and their growth since the last line, so a
                // listening test can be matched to the log afterward.
                std::printf("audio: mix frames %llu | underruns %llu (+%llu) | schedule resets %llu | monitor %s %s | %s | "
                            "recording %s\n",
                            (unsigned long long)mixc.framesRead, (unsigned long long)mixc.overruns,
                            (unsigned long long)(mixc.overruns - lastLoggedUnderruns),
                            (unsigned long long)audioMixer.scheduleResets(), monitorPlaybackId.c_str(),
                            audioMixer.monitoring() ? "on" : "off", driftText.c_str(),
                            audioMixer.recording() ? lds.recordingPath.c_str() : "off");
                lastLoggedUnderruns = mixc.overruns;
                if (!palStatus.empty()) {
                    std::printf("pal: %s | decode %.2f ms/frame\n", palStatus.c_str(),
                                palMsCount ? palMsSum / palMsCount : 0.0);
                    palMsSum = 0;
                    palMsCount = 0;
                }
                // Machines: what the source looks like, the best mode for it,
                // and the automatic switch.
                {
                    if (machinesDirStamp() != machinesStamp) {
                        reloadMachines();
                        machineStatus = "Machine files changed: reloaded";
                    }
                    if (cplN >= 25) {
                        // Cycles per line from the carrier step, nearest the PAL range
                        // (283.5 C64 .. 283.75 standard; 180-degree ambiguous).
                        // NTSC: around 227.75 (227.5 C64 NTSC, 228 Apple II/MSX1), the step exact.
                        const double sfrac = std::arg(cplVec) / (2 * pal::kPi);
                        const double mid = stdInfo().cyclesPerLine;
                        const bool exact = !stdInfo().vSwitch;
                        double best = 0, bestD = 1e9;
                        for (double base : {std::floor(mid) - 1, std::floor(mid), std::floor(mid) + 1})
                            for (double fr : {sfrac, exact ? sfrac : sfrac + 0.5}) {
                                const double c = base + (fr - std::floor(fr));
                                if (std::abs(c - mid) < bestD) bestD = std::abs(c - mid), best = c;
                            }
                        const double was = cyclesMeasured;
                        cyclesMeasured = std::abs(cplVec) / cplN > 0.5 ? best : 0;  // a steady step only
                        if (cyclesMeasured > 0 && std::abs(cyclesMeasured - was) > 0.05)
                            std::printf("machines: colour carrier %.2f cycles per line\n", cyclesMeasured);
                        cplVec = {};
                        cplN = 0;
                    }
                    DetectedMode dm;
                    dm.device = opt.device;
                    dm.input = cap.inputName();
                    dm.signal = cap.signalTypeName();
                    dm.noSignal = !cap.signalPresent();
                    for (const auto& c : cap.controlsSnapshot()) {
                        if (c.name == "Horizontal Timing") dm.width = uint32_t(c.def);
                        else if (c.name == "Vertical Timing") dm.height = uint32_t(c.def);
                        else if (c.name == "Horizontal Size") dm.lineTotal = int(c.def);
                    }
                    if (refBase && refTsN - refTs0 >= 3000000000LL) {
                        const double hz = double(refSeqN - refSeq0) / (double(refTsN - refTs0) / 1e9);
                        if (hz > 20 && hz < 250) {
                            dm.refreshHz = hz, lastRefreshHz = hz;
                        } else if (hz > 0) {
                            // Far fewer frames than any video rate: the card delivers only some
                            // (an Apple IIc); the driver's reported rate is the source's.
                            const double rep = reportedFrameRate(opt.device);
                            if (rep > 20 && rep < 250) dm.refreshHz = rep;
                        }
                    }
                    dm.cyclesPerLine = cyclesMeasured;
                    for (int ch = 0; ch < 3; ++ch) {
                        if (colourFrames[ch] >= 20) colourKnown[ch] = colourSeen[ch] * 2 > colourFrames[ch] ? 1 : 0;
                        colourFrames[ch] = colourSeen[ch] = 0;
                    }
                    const std::string sig = dm.signature();
                    if (sig != stableSig) {
                        stableSig = sig, stableCount = 0;
                        for (int& k : colourKnown) k = -1;  // a new source: measure again
                    } else {
                        ++stableCount;
                    }
                    for (int ch = 0; ch < 3; ++ch) dm.colourOn[ch] = colourKnown[ch];
                    detected = dm;
                    machineMatch = bestMatch(machines, dm);
                    const bool ready = !dm.noSignal && dm.refreshHz > 0 && stableCount >= 2 && palTimingMode < 0 &&
                                       now - lastMachineApplyNs > 5000000000LL;
                    if (ready && !startDecided) {
                        startDecided = true;
                        if (startMachineIdx >= 0) {
                            int mo = -1;
                            const Machine& mc = machines[size_t(startMachineIdx)];
                            for (size_t j = 0; j < mc.modes.size(); ++j)
                                if (mc.modes[j].name == startMode) mo = int(j);
                            if (!startMode.empty() && mo < 0)
                                std::printf("--machine: %s has no mode \"%s\", using its best mode\n", mc.name.c_str(),
                                            startMode.c_str());
                            applyMachineMode(startMachineIdx, mo, true);
                            // No mode named: the machine's best mode was a guess while a colour
                            // wire is unmeasured; let the colour-wire check correct it.
                            if (mo < 0) {
                                const int ai = findMachine(activeMachineId);
                                if (ai >= 0)
                                    for (const MachineMode& md : machines[size_t(ai)].modes)
                                        if (md.name == activeModeName && md.match.colourOn >= 0 && md.match.colourOn < 3 &&
                                            dm.colourOn[md.match.colourOn] < 0)
                                            tentativeColourCh = md.match.colourOn, tentativeFromStart = true;
                            }
                        } else if (explicitVideo) {
                            decidedSig = sig;  // the command line's settings stay for this source
                            std::printf("machines: command-line settings kept; the automatic switch acts on the next "
                                        "mode change\n");
                        }
                    } else if (ready && autoSwitch && machineMatch.score >= 0 && !recorder.active()) {
                        const Machine& bm = machines[size_t(machineMatch.machine)];
                        const MachineMode& bmode = bm.modes[size_t(machineMatch.mode)];
                        const int ai = findMachine(activeMachineId);
                        // "No longer fits" needs an active mode that stopped matching
                        // (e.g. the carrier measured later); no active mode at all
                        // is not a reason (the command line's setup stays).
                        bool activeFails = false;
                        if (ai >= 0)
                            for (const MachineMode& md : machines[size_t(ai)].modes)
                                if (md.name == activeModeName) activeFails = scoreMode(machines[size_t(ai)], md, dm) < 0;
                        const bool differs = bm.id != activeMachineId || bmode.name != activeModeName;
                        const bool newSource = sig != decidedSig;
                        if (differs && (newSource || (activeFails && !activeByUser))) {
                            std::printf("machines: source %s -> %s / %s (%s)\n", dm.describe().c_str(), bm.name.c_str(),
                                        bmode.name.c_str(), machineMatch.why.c_str());
                            const int needCh = bmode.match.colourOn;
                            const bool unknown = needCh >= 0 && needCh < 3 && dm.colourOn[needCh] < 0;
                            applyMachineMode(machineMatch.machine, machineMatch.mode, false);
                            if (unknown) tentativeColourCh = needCh;
                        }
                        decidedSig = sig;
                    } else if (ready && autoSwitch && machineMatch.score < 0 && sig != decidedSig) {
                        decidedSig = sig;
                        std::printf("machines: no machine mode matches the source (%s)\n", dm.describe().c_str());
                        machineStatus = "No machine mode matches this source; set it up and save it as a mode";
                    }
                }
                std::fflush(stdout);
                disp.setTitle(std::string("freshVision - ") + line);
                statsText = statsPanel;

                totalDisplayed += win.displayed;
                win = Window{};
            }
        }
    }

    if (quitReason.empty())
        quitReason = g_quitSignal == SIGINT ? "SIGINT" : g_quitSignal == SIGTERM ? "SIGTERM" : "quit requested";
    std::printf("exit: %s\n", quitReason.c_str());
    // Remember the window size for the next start (not while fullscreen).
    if (!disp.fullscreen() && disp.window()) {
        int w = 0, h = 0;
        SDL_GetWindowSize(disp.window(), &w, &h);
        if (FILE* wf = std::fopen(windowConf.c_str(), "w")) {
            std::fprintf(wf, "%dx%d\n", w, h);
            std::fclose(wf);
        }
    }
    cap.stop();  // always leave the device stopped
    audioMixer.stop();  // joins its thread first, so no more pushAudioFrame calls can arrive after this
    if (recorder.active()) logRecordingStop();
    recorder.stop();    // flushes and finalizes the file if a recording was active
    if (guiOk) gui.shutdown();
    accumulate();
    totalDisplayed += win.displayed;

    std::printf("\nsummary: %.1f s, delivered %llu, displayed %lu, overwritten %llu, driver gaps %llu, timeouts %llu, "
                "sequence jumps %llu, restarts %lu, bad frames %llu, short frames %llu\n",
                double(nowNs() - t0) / 1e9, (unsigned long long)total.delivered, totalDisplayed,
                (unsigned long long)total.overwritten, (unsigned long long)total.driverGaps,
                (unsigned long long)total.timeouts, (unsigned long long)total.seqJumps, restarts, (unsigned long long)total.errorFrames,
                (unsigned long long)total.shortFrames);
    if (!allAgePresent.empty()) {
        std::printf("frame age at present (timestamp clock %s): median %.1f ms, p95 %.1f ms, max %.1f ms over %zu frames\n",
                    cap.clockName().c_str(), median(allAgePresent), percentile(allAgePresent, 0.95),
                    maxOf(allAgePresent), allAgePresent.size());
    } else {
        std::printf("frame age: unavailable (timestamp clock %s)\n", cap.clockName().c_str());
    }
    return 0;
}

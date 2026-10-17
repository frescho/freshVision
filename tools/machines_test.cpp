// SPDX-License-Identifier: GPL-3.0-or-later
// Offline test of the machine matching (src/machines.*) with the built-in
// machine files: which mode a detected source picks by itself. Runs in a
// temporary HOME, so the user's machine files are not touched.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "machines.h"
#include "presets.h"

using namespace fv;

namespace {

int failures = 0;

void check(const char* name, bool ok, const std::string& detail = "") {
    if (!ok) ++failures;
    std::printf("%-62s %s%s%s\n", name, ok ? "PASS" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream b;
    b << f.rdbuf();
    return b.str();
}

int findMachineIdx(const std::vector<Machine>& ms, const std::string& id) {
    for (size_t i = 0; i < ms.size(); ++i)
        if (ms[i].id == id) return int(i);
    return -1;
}

const MachineMode* findMode(const Machine& m, const std::string& name) {
    for (const MachineMode& md : m.modes)
        if (md.name == name) return &md;
    return nullptr;
}

void expect(const char* name, const std::vector<Machine>& ms, const DetectedMode& d, const std::string& wantMachine,
            const std::string& wantMode) {
    const MatchResult r = bestMatch(ms, d);
    const std::string got = r.machine < 0 ? "(none)" : ms[size_t(r.machine)].name + " / " + ms[size_t(r.machine)].modes[size_t(r.mode)].name;
    const std::string want = wantMachine.empty() ? "(none)" : wantMachine + " / " + wantMode;
    const bool ok = got == want;
    if (!ok) ++failures;
    std::printf("%-62s %s  picked %s%s\n", name, ok ? "PASS" : "FAIL", got.c_str(), ok ? "" : (", wanted " + want).c_str());
}

DetectedMode sog(double hz, double cycles, int blueBurst) {
    DetectedMode d;
    d.device = "/dev/video2";
    d.input = "Analog Sync On Green";
    d.signal = "Video";
    d.width = 384, d.height = 298, d.lineTotal = 524;
    d.refreshHz = hz;
    d.cyclesPerLine = cycles;
    d.colourOn[2] = blueBurst;
    return d;
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/fv-machines-test-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) return 2;
    setenv("HOME", dir, 1);
    unsetenv("XDG_CONFIG_HOME");  // the settings folder follows it (configDir()): keep it in the temporary HOME
    machinesDir();  // first creation writes the built-in files
    std::vector<std::string> errors;
    const std::vector<Machine> ms = loadMachines(errors);
    for (const auto& e : errors) std::printf("error: %s\n", e.c_str()), ++failures;
    bool haveBbc = false;
    for (const Machine& m : ms) haveBbc |= m.id == "bbc-micro-b" && m.modes.size() == 4;
    std::printf("%-62s %s\n", "built-in BBC Micro file with 4 modes", haveBbc ? "PASS" : "FAIL");
    failures += haveBbc ? 0 : 1;

    // The BBC modes are click-only: a BBC-like source goes elsewhere.
    expect("BBC-like composite, *TV ,1 (50.08 Hz, 283.75 cycles)", ms, sog(50.08, 283.75, 0), "ZX Spectrum (PAL)", "Composite 8x");
    expect("BBC-like composite, interlaced sync (50.00 Hz)", ms, sog(50.00, 283.75, 0), "Generic PAL", "Composite 8x");
    expect("BBC-like S-Video (colour on blue, 50.08 Hz)", ms, sog(50.08, 283.75, 1), "ZX Spectrum (PAL)", "Composite 8x");
    DetectedMode rgb;
    rgb.device = "/dev/video3", rgb.input = "Analog 5 Wires (DVI-I SL)", rgb.signal = "Video";
    rgb.width = 640, rgb.height = 256, rgb.lineTotal = 1024, rgb.refreshHz = 50.08;
    expect("BBC-like RGB on the analog input", ms, rgb, "", "");
    // NTSC (all NTSC modes are click-only): NTSC sources never take a PAL
    // machine's mode, PAL sources never an NTSC one.
    bool haveNtsc = false;
    for (const Machine& m : ms) haveNtsc |= m.id == "c64-ntsc" && m.modes.size() == 2;
    for (const Machine& m : ms) haveNtsc &= m.id != "apple2" || m.modes.size() == 1;
    std::printf("%-62s %s\n", "built-in NTSC files (c64-ntsc 2 modes, apple2, msx1-ntsc)", haveNtsc ? "PASS" : "FAIL");
    failures += haveNtsc ? 0 : 1;
    expect("C64 NTSC composite (59.83 Hz, 227.5 cycles)", ms, sog(59.83, 227.5, 0), "", "");
    expect("Apple II / MSX1 composite (59.92 Hz, 228 cycles)", ms, sog(59.92, 228, 0), "", "");
    expect("C64 NTSC S-Video (burst on blue)", ms, sog(59.83, 227.5, 1), "", "");
    expect("PAL source, refresh not measured yet", ms, sog(0, 0, -1), "Commodore 64 (PAL)", "S-Video 8x");
    for (const Machine& m : ms)
        if (m.id == "c64-ntsc" || m.id == "apple2" || m.id == "msx1-ntsc")
            for (const MachineMode& md : m.modes) {
                const bool ok = md.settings.display.videoStandard == 1 && md.match.manual;
                if (!ok) ++failures;
                std::printf("%-62s %s\n", (m.name + " / " + md.name + ": NTSC, click-only").c_str(), ok ? "PASS" : "FAIL");
            }
    // A machine the user names (--machine NAME, its button) may use its
    // click-only modes: the MSX1's composite mode for an MSX-like source
    // whose colour carrier isn't measured yet, never without the name.
    for (const Machine& m : ms)
        if (m.id == "msx1-ntsc") {
            DetectedMode d = sog(59.92, 0, -1);
            const int named = scoreMode(m, m.modes[0], d, nullptr, true);
            const int plain = scoreMode(m, m.modes[0], d, nullptr, false);
            const bool ok = named >= 0 && plain < 0;
            if (!ok) ++failures;
            std::printf("%-62s %s\n", "MSX1 named by the user: its click-only mode fits", ok ? "PASS" : "FAIL");
        }
    // The C64 choices stay as they were.
    expect("C64 composite (no burst on blue)", ms, sog(50.125, 283.5, 0), "Commodore 64 (PAL)", "Composite 8x");
    expect("C64 S-Video (burst on blue)", ms, sog(50.125, 283.5, 1), "Commodore 64 (PAL)", "S-Video 8x");


    // ---- Built-in / My modes (two layers) ----
    {
        std::vector<std::string> e;
        std::vector<Machine> cur = loadMachines(e);
        const int ci = findMachineIdx(cur, "c64");
        check("built-in C64 found, no user file", ci >= 0 && cur[size_t(ci)].builtIn && !cur[size_t(ci)].mine);
        // 1. Save a changed copy of a built-in mode: it replaces it.
        Machine c = cur[size_t(ci)];
        for (MachineMode& md : c.modes)
            if (md.name == "Composite 8x") {
                md.settings.width = 1024;
                md.origin = ModeOrigin::MineReplacing;
                md.basedOn = md.revision;
            }
        std::string err;
        check("save a changed built-in mode into My modes", saveMachine(c, err), err);
        cur = loadMachines(e);
        const Machine& c2 = cur[size_t(findMachineIdx(cur, "c64"))];
        const MachineMode* m1 = findMode(c2, "Composite 8x");
        check("my version replaces the built-in mode", m1 && m1->origin == ModeOrigin::MineReplacing && m1->settings.width == 1024);
        check("the built-in mode stays available", c2.replacedBuiltIn.size() == 1 && c2.replacedBuiltIn[0].settings.width == 2048);
        expect("C64 composite picks my version (same name)", cur, sog(50.125, 283.5, 0), "Commodore 64 (PAL)", "Composite 8x");
        const std::string file = readFile(machinesDir() + "/c64.conf");
        check("user file: only my mode, every key, default markers",
              file.find("[mode Composite 8x]") != std::string::npos && file.find("[mode S-Video 8x]") == std::string::npos &&
                  file.find("width=1024\n") != std::string::npos && file.find("crop_set=0  # default") != std::string::npos);
        // 2. A key from a newer version is kept when the file is written again.
        {
            std::ofstream f(machinesDir() + "/c64.conf", std::ios::app);
            f << "future_key=42\n";
        }
        cur = loadMachines(e);
        Machine c3 = cur[size_t(findMachineIdx(cur, "c64"))];
        check("unknown key read without an error", e.empty() || e.back().find("future_key") == std::string::npos);
        saveMachine(c3, err);
        check("unknown key written back unchanged", readFile(machinesDir() + "/c64.conf").find("future_key=42") != std::string::npos);
        // 3. A user mode with only one key: the rest comes from the built-in mode.
        {
            std::ofstream f(machinesDir() + "/c64.conf", std::ios::app);
            f << "\n[mode S-Video 8x]\nwidth=999\n";
        }
        cur = loadMachines(e);
        const MachineMode* sv = findMode(cur[size_t(findMachineIdx(cur, "c64"))], "S-Video 8x");
        check("missing keys come from the built-in base", sv && sv->settings.width == 999 && sv->settings.display.palDecode &&
                                                               sv->settings.display.palMultiple == 8);
        // 4. Hiding a built-in mode.
        Machine c4 = cur[size_t(findMachineIdx(cur, "c64"))];
        c4.modes.erase(std::remove_if(c4.modes.begin(), c4.modes.end(), [](const MachineMode& m) { return m.name == "S-Video 8x"; }),
                       c4.modes.end());
        c4.hiddenModes.push_back("S-Video 8x");
        saveMachine(c4, err);
        cur = loadMachines(e);
        const Machine& c5 = cur[size_t(findMachineIdx(cur, "c64"))];
        check("a hidden built-in mode is left out", !findMode(c5, "S-Video 8x") && c5.hiddenModes.size() == 1);
        // 5. Upgrade notice: based on revision 0 < built-in 1.
        Machine c6 = c5;
        for (MachineMode& md : c6.modes)
            if (md.name == "Composite 8x") md.basedOn = 0;
        saveMachine(c6, err);
        cur = loadMachines(e);
        const auto ups = pendingModeUpdates(cur);
        check("newer built-in revision is announced", ups.size() == 1 && ups[0].mode == "Composite 8x" && ups[0].builtInRevision == 1);
        Machine c7 = cur[size_t(findMachineIdx(cur, "c64"))];
        for (MachineMode& md : c7.modes)
            if (md.name == "Composite 8x") md.keptRevision = 1;
        saveMachine(c7, err);
        cur = loadMachines(e);
        check("\"keep mine\" silences it for that revision", pendingModeUpdates(cur).empty());
        // 6. Back to the built-in mode: the user file goes away.
        Machine c8 = cur[size_t(findMachineIdx(cur, "c64"))];
        c8.modes.erase(std::remove_if(c8.modes.begin(), c8.modes.end(), [](const MachineMode& m) { return m.origin != ModeOrigin::BuiltIn; }),
                       c8.modes.end());
        c8.hiddenModes.clear();
        saveMachine(c8, err);
        check("nothing of mine left: no user file", !std::filesystem::exists(machinesDir() + "/c64.conf"));
    }
    // ---- Saving over a mode: only what changed since it was applied ----
    {
        Preset old = parsePresetSettings({});
        old.controls = {{"Blue Brightness", 255}, {"Blue Contrast", 48}};
        old.display.palUi.pllBandwidth = 0.2f;
        Preset base = old;  // the state ~4 s after applying: a full snapshot
        base.controls.push_back({"Brightness", 32});
        base.controls.push_back({"Green Brightness", 197});
        base.crop = Rect{0, 0, 2048, 298};
        base.crop.set = false;
        base.display.pixelAspect = 0.2156;
        Preset now = base;
        now.display.palUi.pllBandwidth = 0.25f;  // the user's change
        std::vector<std::string> ch;
        const Preset r = mergeModeChanges(old, base, now, ch);
        bool onlyPll = ch.size() == 1 && ch[0].find("pll") != std::string::npos;
        check("saving: only the changed key is taken over", onlyPll && r.controls.size() == 2 && r.display.palUi.pllBandwidth > 0.24f &&
                                                             r.display.pixelAspect == old.display.pixelAspect,
              ch.empty() ? "" : ch[0]);
        Preset now2 = base;
        now2.controls[1].value = 60;  // Blue Contrast changed
        now2.controls.push_back({"Phase", 3});
        std::vector<std::string> ch2;
        const Preset r2 = mergeModeChanges(old, base, now2, ch2);
        check("saving: changed and newly set controls by name", ch2.size() == 2 && r2.controls.size() == 3);
    }
    // ---- One-time conversion of 0.1.0 settings ----
    {
        namespace fs = std::filesystem;
        const std::string mdir = machinesDir();
        // A 0.1.0 copy of a built-in file (no format_version), with one mode changed and one own mode.
        std::string bi = readFile(builtInMachinesDir() + "/zx-spectrum.conf");
        std::string old;
        std::istringstream in(bi);
        std::string line;
        while (std::getline(in, line))
            if (line.rfind("format_version", 0) != 0 && line.rfind("revision", 0) != 0 && line.rfind("group", 0) != 0) old += line + "\n";
        old += "\n[mode My own]\nmatch_manual=1\nwidth=640\nheight=480\n";
        { std::ofstream f(mdir + "/zx-spectrum.conf"); f << old; }
        std::string c64old = readFile(builtInMachinesDir() + "/c64.conf");
        std::string c64v1;
        std::istringstream in2(c64old);
        while (std::getline(in2, line)) {
            if (line.rfind("format_version", 0) == 0 || line.rfind("revision", 0) == 0 || line.rfind("group", 0) == 0) continue;
            if (line == "pal_line=1134") line = "pal_line=1135";  // the user's change
            c64v1 += line + "\n";
        }
        { std::ofstream f(mdir + "/c64.conf"); f << c64v1; }
        // A preset.
        Preset pr = parsePresetSettings({});
        pr.group = "Primo", pr.name = "CLI", pr.width = 320, pr.height = 240;
        std::string err;
        check("a 0.1.0 preset written", savePreset(pr, err), err);
        std::string report;
        const bool did = migrateSettings(report);
        check("conversion runs once", did && !migrateSettings(report));
        std::vector<std::string> e;
        const std::vector<Machine> cur = loadMachines(e);
        const Machine& zx = cur[size_t(findMachineIdx(cur, "zx-spectrum"))];
        const MachineMode* zxc = findMode(zx, "Composite 8x");
        const MachineMode* own = findMode(zx, "My own");
        check("unchanged copy of a built-in mode: built-in again", zxc && zxc->origin == ModeOrigin::BuiltIn);
        check("own mode kept as mine", own && own->origin == ModeOrigin::Mine && own->settings.width == 640);
        const MachineMode* cc = findMode(cur[size_t(findMachineIdx(cur, "c64"))], "Composite 8x");
        check("changed built-in mode kept as my version (based_on 0)",
              cc && cc->origin == ModeOrigin::MineReplacing && cc->basedOn == 0 && cc->settings.display.palLine == 1135);
        const int pi = findMachineIdx(cur, "primo");
        check("preset -> click-only mode in machine \"Primo\"", pi >= 0 && findMode(cur[size_t(pi)], "CLI") &&
                                                                   findMode(cur[size_t(pi)], "CLI")->match.manual &&
                                                                   findMode(cur[size_t(pi)], "CLI")->settings.width == 320);
        check("backups kept", fs::exists(configDir() + "/machines.backup-0.1.0/c64.conf") && fs::exists(configDir() + "/presets.old"));
    }
    std::string cmd = std::string("rm -rf -- '") + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::printf("note: could not remove %s\n", dir);
    std::printf("%s\n", failures ? "FAILED" : "all cases passed");
    return failures ? 1 : 0;
}

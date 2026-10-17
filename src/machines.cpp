// SPDX-License-Identifier: GPL-3.0-or-later
#include "machines.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "formats.h"
#include "presets.h"

namespace fv {

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseSize(const std::string& s, uint32_t& w, uint32_t& h) {
    unsigned a = 0, b = 0;
    if (std::sscanf(s.c_str(), "%ux%u", &a, &b) != 2) return false;
    w = a, h = b;
    return true;
}

// "50.125 +-0.02" / "50.125" -> value, tolerance (kept when not given).
template <typename T>
bool parseTol(const std::string& s, T& value, T& tol) {
    std::string v = trim(s);
    size_t pm = v.find("+-");
    double a = 0;
    if (std::sscanf(v.c_str(), "%lf", &a) != 1) return false;
    value = T(a);
    if (pm != std::string::npos) {
        double t = 0;
        if (std::sscanf(v.c_str() + pm + 2, "%lf", &t) == 1) tol = T(t);
    }
    return true;
}

std::string fmtNum(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}


const char* kDefaultMarker = "  # default";

// A value written with the default marker: the value alone.
std::string stripDefaultMarker(const std::string& v) {
    const std::string m = "# default";
    if (v.size() >= m.size() && v.compare(v.size() - m.size(), m.size(), m) == 0) return trim(v.substr(0, v.size() - m.size()));
    return v;
}

// The setting keys as the writer produces them for a preset.
std::vector<std::pair<std::string, std::string>> settingLines(const Preset& p) {
    std::ostringstream o;
    writePresetSettings(o, p);
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(o.str());
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) out.push_back({line.substr(0, eq), line.substr(eq + 1)});
    }
    return out;
}

// The program's default value of each setting key (an empty mode).
const std::map<std::string, std::string>& defaultSettings() {
    static const std::map<std::string, std::string> d = [] {
        std::map<std::string, std::string> m;
        for (const auto& kv : settingLines(parsePresetSettings({}))) m[kv.first] = kv.second;
        return m;
    }();
    return d;
}

void writeMatch(std::ostream& f, const MachineMode& md) {
    const ModeMatch& c = md.match;
    if (!c.device.empty()) f << "match_device=" << c.device << "\n";
    if (!c.input.empty()) f << "match_input=" << c.input << "\n";
    if (!c.signal.empty()) f << "match_signal=" << c.signal << "\n";
    if (c.width && c.height) f << "match_size=" << c.width << "x" << c.height << "\n";
    if (c.lineTotal) f << "match_line_total=" << c.lineTotal << " +-" << c.lineTotalTol << "\n";
    if (c.refreshHz > 0) f << "match_refresh=" << fmtNum(c.refreshHz) << " +-" << fmtNum(c.refreshTol) << "\n";
    if (c.cyclesPerLine > 0) f << "match_cycles_per_line=" << fmtNum(c.cyclesPerLine) << " +-" << fmtNum(c.cyclesTol) << "\n";
    if (c.manual) f << "match_manual=1\n";
    static const char* chName[] = {"red", "green", "blue"};
    if (c.colourOn >= 0 && c.colourOn < 3) f << "match_colour_on=" << chName[c.colourOn] << "\n";
    if (c.noColourOn >= 0 && c.noColourOn < 3) f << "match_no_colour_on=" << chName[c.noColourOn] << "\n";
    if (c.priority) f << "match_priority=" << c.priority << "\n";
    if (!md.hint.empty()) f << "hint=" << md.hint << "\n";
}

// Every setting key, marked when it equals the program default; then the
// keys this version doesn't know (from a newer version), unchanged.
void writeSettings(std::ostream& f, const MachineMode& md) {
    const auto& defs = defaultSettings();
    std::map<std::string, bool> written;
    for (const auto& kv : settingLines(md.settings)) {
        f << kv.first << "=" << kv.second;
        auto d = defs.find(kv.first);
        if (d != defs.end() && d->second == kv.second) f << kDefaultMarker;
        f << "\n";
        written[kv.first] = true;
    }
    const Preset known = parsePresetSettings({});
    std::map<std::string, bool> knownKeys;
    for (const auto& kv : settingLines(known)) knownKeys[kv.first] = true;
    for (const auto& kv : md.raw)
        if (!written.count(kv.first) && !knownKeys.count(kv.first) && kv.first.rfind("control.", 0) != 0 &&
            kv.first.rfind("audio_source.", 0) != 0)
            f << kv.first << "=" << kv.second << "\n";
}

// The user's file: machine header, then only the user's modes.
void writeUserMachine(std::ostream& f, const Machine& m) {
    f << "# freshVision machine file: your modes (\"My modes\"). Upgrades never change\n"
         "# this file. Edit it freely or save modes from the Machines section (which\n"
         "# rewrites it, dropping comments). Keys marked \"# default\" had the program's\n"
         "# default value when saved. Format: see src/machines.h.\n";
    f << "[machine]\n";
    f << "format_version=2\n";
    f << "name=" << m.name << "\n";
    if (!m.builtIn) {
        if (!m.group.empty()) f << "group=" << m.group << "\n";
        if (!m.note.empty()) f << "note=" << m.note << "\n";
        if (!m.soundSource.empty()) f << "sound_source=" << m.soundSource << "\n";
        for (const auto& a : m.aliases)
            f << "alias=" << a.first.first << "x" << a.first.second << " -> " << a.second.first << "x" << a.second.second << "\n";
    }
    for (const std::string& h : m.hiddenModes) f << "\n[mode " << h << "]\nhidden=1\n";
    for (const MachineMode& md : m.modes) {
        if (md.origin == ModeOrigin::BuiltIn) continue;
        f << "\n[mode " << md.name << "]\n";
        if (md.basedOn >= 0) f << "based_on=" << md.basedOn << "\n";
        if (md.keptRevision >= 0) f << "kept_revision=" << md.keptRevision << "\n";
        writeMatch(f, md);
        writeSettings(f, md);
    }
}

struct RawMode {
    MachineMode mode;
    std::map<std::string, std::string> kv;  // setting keys (markers stripped)
};

// One file, either layer. Settings are parsed later (a user mode may take
// missing keys from the built-in mode it replaces).
bool parseFile(const fs::path& path, Machine& m, std::vector<RawMode>& modes, int& formatVersion,
               std::vector<std::string>& errors) {
    std::ifstream f(path);
    if (!f) {
        errors.push_back("cannot read " + path.string());
        return false;
    }
    m = Machine{};
    m.id = path.stem().string();
    m.name = m.id;
    formatVersion = 1;
    enum { None, MachineSec, ModeSec } sec = None;
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            std::string h = trim(t.substr(1, t.size() - 2));
            if (h == "machine") {
                sec = MachineSec;
            } else if (h.rfind("mode", 0) == 0) {
                sec = ModeSec;
                modes.push_back(RawMode{});
                modes.back().mode.name = trim(h.substr(4));
                if (modes.back().mode.name.empty()) modes.back().mode.name = "mode " + std::to_string(modes.size());
            } else {
                errors.push_back(path.filename().string() + ":" + std::to_string(lineNo) + ": unknown section [" + h + "]");
                sec = None;
            }
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            errors.push_back(path.filename().string() + ":" + std::to_string(lineNo) + ": not key=value: " + t);
            continue;
        }
        std::string k = trim(t.substr(0, eq)), v = stripDefaultMarker(trim(t.substr(eq + 1)));
        bool ok = true;
        if (sec == MachineSec) {
            if (k == "name") m.name = v;
            else if (k == "note") m.note = v;
            else if (k == "group") m.group = v;
            else if (k == "revision") m.revision = std::atoi(v.c_str());
            else if (k == "format_version") formatVersion = std::atoi(v.c_str());
            else if (k == "sound_source") m.soundSource = v;
            else if (k == "alias") {
                size_t arrow = v.find("->");
                uint32_t a = 0, b = 0, c = 0, d = 0;
                ok = arrow != std::string::npos && parseSize(trim(v.substr(0, arrow)), a, b) &&
                     parseSize(trim(v.substr(arrow + 2)), c, d);
                if (ok) m.aliases.push_back({{a, b}, {c, d}});
            } else ok = false;
        } else if (sec == ModeSec) {
            MachineMode& md = modes.back().mode;
            ModeMatch& c = md.match;
            if (k == "match_device") c.device = v;
            else if (k == "match_input") c.input = v;
            else if (k == "match_signal") c.signal = v;
            else if (k == "match_size") ok = parseSize(v, c.width, c.height);
            else if (k == "match_line_total") ok = parseTol(v, c.lineTotal, c.lineTotalTol);
            else if (k == "match_refresh") ok = parseTol(v, c.refreshHz, c.refreshTol);
            else if (k == "match_cycles_per_line") ok = parseTol(v, c.cyclesPerLine, c.cyclesTol);
            else if (k == "match_manual") c.manual = v == "1" || v == "yes" || v == "true";
            else if (k == "match_colour_on" || k == "match_no_colour_on") {
                const std::string lv = lower(v);
                const int ch = lv == "red" ? 0 : lv == "green" ? 1 : lv == "blue" ? 2 : -1;
                ok = ch >= 0;
                (k == "match_colour_on" ? c.colourOn : c.noColourOn) = ch;
            } else if (k == "match_priority") c.priority = std::atoi(v.c_str());
            else if (k == "hint") md.hint = v;
            else if (k == "revision") md.revision = std::atoi(v.c_str());
            else if (k == "based_on") md.basedOn = std::atoi(v.c_str());
            else if (k == "kept_revision") md.keptRevision = std::atoi(v.c_str());
            else if (k == "hidden") md.hidden = v == "1" || v == "yes" || v == "true";
            else modes.back().kv[k] = v;  // a setting key
        } else {
            ok = false;
        }
        if (!ok) errors.push_back(path.filename().string() + ":" + std::to_string(lineNo) + ": not understood: " + t);
    }
    return true;
}

void finishSettings(MachineMode& md, const std::map<std::string, std::string>& kv, const std::string& where,
                    std::vector<std::string>& errors) {
    md.raw = kv;
    try {
        md.settings = parsePresetSettings(kv);
    } catch (const std::exception& e) {
        errors.push_back(where + ", mode \"" + md.name + "\": a setting is unreadable (" + e.what() + "), its defaults are used");
        md.settings = parsePresetSettings({});
    }
    md.settings.name = md.name;
}

std::vector<fs::path> confFiles(const std::string& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (dir.empty()) return files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".conf") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    return files;
}

// A mode's whole content as text (conditions, hint, settings), for comparisons.
std::string modeText(const MachineMode& md) {
    std::ostringstream o;
    writeMatch(o, md);
    for (const auto& kv : settingLines(md.settings)) o << kv.first << "=" << kv.second << "\n";
    return o.str();
}

std::string exeDir() {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : self.parent_path().string();
}

}  // namespace

std::string DetectedMode::signature() const {
    return device + "|" + input + "|" + signal + "|" + std::to_string(width) + "x" + std::to_string(height) + "|" +
           std::to_string(lineTotal) + (noSignal ? "|nosignal" : "");
}

std::string DetectedMode::describe() const {
    if (noSignal) return "no signal";
    std::string s = signal.empty() ? std::string("?") : signal;
    if (width && height) s += ", " + std::to_string(width) + "x" + std::to_string(height);
    if (lineTotal) s += ", line " + std::to_string(lineTotal);
    char b[64];
    if (refreshHz > 0) {
        std::snprintf(b, sizeof b, ", %.3f Hz", refreshHz);
        s += b;
    }
    if (cyclesPerLine > 0) {
        std::snprintf(b, sizeof b, ", %.2f carrier cycles/line", cyclesPerLine);
        s += b;
    }
    static const char* chName[] = {"red", "green", "blue"};
    for (int ch = 0; ch < 3; ++ch)
        if (colourOn[ch] >= 0) s += std::string(colourOn[ch] ? ", burst on " : ", no burst on ") + chName[ch];
    return s;
}

std::string DetectedMode::describeLines() const {
    if (noSignal) return "no signal";
    char b[320];
    static const char* chName[] = {"red", "green", "blue"};
    std::string bursts;
    for (int ch = 0; ch < 3; ++ch)
        bursts += std::string(bursts.empty() ? "" : ", ") + chName[ch] + (colourOn[ch] < 0 ? " -" : colourOn[ch] ? " yes" : " no");
    char refresh[32] = "-", cycles[32] = "-";
    if (refreshHz > 0) std::snprintf(refresh, sizeof refresh, "%7.3f Hz", refreshHz);
    if (cyclesPerLine > 0) std::snprintf(cycles, sizeof cycles, "%6.2f per line", cyclesPerLine);
    std::snprintf(b, sizeof b, "signal   %s, %ux%u, line %d\nrefresh  %s\ncarrier  %s\nburst    %s",
                  signal.empty() ? "?" : signal.c_str(), width, height, lineTotal, refresh, cycles, bursts.c_str());
    return b;
}

std::string machinesDir() {
    if (configDir().empty()) return {};
    fs::path dir = fs::path(configDir()) / "machines";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {};
    return dir.string();
}

std::string builtInMachinesDir() {
    std::error_code ec;
    if (const char* e = std::getenv("FRESHVISION_BUILTIN_MACHINES"))
        if (*e && fs::is_directory(e, ec)) return e;
    const std::string self = exeDir();
    if (!self.empty()) {
        const fs::path p = fs::path(self) / ".." / "share" / "freshvision" / "machines";
        if (fs::is_directory(p, ec)) return fs::weakly_canonical(p, ec).string();
    }
#ifdef FRESHVISION_SOURCE_DIR
    const fs::path src = fs::path(FRESHVISION_SOURCE_DIR) / "data" / "machines";
    if (fs::is_directory(src, ec)) return src.string();
#endif
    return {};
}

namespace {
// The shipped machines alone.
std::vector<Machine> loadBuiltIns(std::vector<std::string>& errors) {
    std::vector<Machine> out;
    for (const fs::path& p : confFiles(builtInMachinesDir())) {
        Machine m;
        std::vector<RawMode> modes;
        int fv = 0;
        if (!parseFile(p, m, modes, fv, errors)) continue;
        m.builtIn = true;
        for (RawMode& r : modes) {
            r.mode.origin = ModeOrigin::BuiltIn;
            finishSettings(r.mode, r.kv, p.filename().string(), errors);
            m.modes.push_back(std::move(r.mode));
        }
        out.push_back(std::move(m));
    }
    return out;
}
}  // namespace

std::vector<Machine> loadMachines(std::vector<std::string>& errors) {
    // 1. Built-in machines.
    std::vector<Machine> out = loadBuiltIns(errors);
    // 2. The user's machines: their modes replace built-in ones of the same
    // name (missing keys from the built-in mode), or are added.
    const std::string dir = machinesDir();
    for (const fs::path& p : confFiles(dir)) {
        Machine um;
        std::vector<RawMode> modes;
        int fv = 0;
        if (!parseFile(p, um, modes, fv, errors)) continue;
        Machine* m = nullptr;
        for (Machine& b : out)
            if (b.builtIn && b.id == um.id) m = &b;
        if (!m) {
            um.path = p.string();
            um.mine = true;
            out.push_back(std::move(um));
            m = &out.back();
        } else {
            m->path = p.string();
            m->mine = true;
            if (!um.soundSource.empty()) m->soundSource = um.soundSource;
        }
        for (RawMode& r : modes) {
            if (r.mode.hidden) {
                m->hiddenModes.push_back(r.mode.name);
                for (size_t i = 0; i < m->modes.size(); ++i)
                    if (m->modes[i].name == r.mode.name && m->modes[i].origin == ModeOrigin::BuiltIn) {
                        m->modes.erase(m->modes.begin() + long(i));
                        break;
                    }
                continue;
            }
            int idx = -1;
            for (size_t i = 0; i < m->modes.size(); ++i)
                if (m->modes[i].name == r.mode.name && m->modes[i].origin == ModeOrigin::BuiltIn) idx = int(i);
            if (idx >= 0) {
                std::map<std::string, std::string> kv = m->modes[size_t(idx)].raw;  // the built-in base ...
                for (const auto& e : r.kv) kv[e.first] = e.second;                  // ... with the user's values
                r.mode.origin = ModeOrigin::MineReplacing;
                finishSettings(r.mode, kv, p.filename().string(), errors);
                r.mode.raw = r.kv;
                m->replacedBuiltIn.push_back(m->modes[size_t(idx)]);
                m->modes[size_t(idx)] = std::move(r.mode);
            } else {
                r.mode.origin = ModeOrigin::Mine;
                finishSettings(r.mode, r.kv, p.filename().string(), errors);
                m->modes.push_back(std::move(r.mode));
            }
        }
    }
    // default (generic) last: it only takes what nothing else matches better.
    std::stable_sort(out.begin(), out.end(), [](const Machine& a, const Machine& b) {
        const bool da = a.id == "default", db = b.id == "default";
        return da != db && db;
    });
    return out;
}

bool saveMachine(const Machine& m, std::string& err) {
    std::string dir = machinesDir();
    if (dir.empty()) {
        err = "no settings folder (is $HOME set?)";
        return false;
    }
    const fs::path path = fs::path(dir) / (m.id + ".conf");
    bool any = !m.builtIn || !m.hiddenModes.empty();
    for (const MachineMode& md : m.modes) any = any || md.origin != ModeOrigin::BuiltIn;
    std::error_code ec;
    if (!any) {  // nothing of the user's left: no file
        fs::remove(path, ec);
        return true;
    }
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) {
            err = "cannot write " + tmp.string();
            return false;
        }
        writeUserMachine(f, m);
        if (!f) {
            err = "write error on " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        err = "cannot replace " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

Preset mergeModeChanges(const Preset& old, const Preset& baseline, const Preset& now, std::vector<std::string>& changes) {
    auto toMap = [](const Preset& p) {
        std::map<std::string, std::string> m;
        for (const auto& kv : settingLines(p))
            if (kv.first.rfind("control", 0) != 0) m[kv.first] = kv.second;
        return m;
    };
    std::map<std::string, std::string> result = toMap(old);
    const auto base = toMap(baseline), cur = toMap(now);
    for (const auto& kv : cur) {
        auto b = base.find(kv.first);
        if (b != base.end() && b->second == kv.second) continue;  // not changed since the mode was applied
        auto o = result.find(kv.first);
        const std::string was = o == result.end() ? "(not set)" : o->second;
        if (was == kv.second) continue;
        if (o == result.end()) {  // not set before: the default value is no change
            auto d = defaultSettings().find(kv.first);
            if (d != defaultSettings().end() && d->second == kv.second) continue;
        }
        result[kv.first] = kv.second;
        changes.push_back(kv.first + ": " + was + " -> " + kv.second);
    }
    Preset out = parsePresetSettings(result);
    out.audioSpecified = old.audioSpecified || (now.audioSpecified && !baseline.audioSpecified);
    // Controls by name: the old mode's, plus those changed since it was applied.
    std::map<std::string, int64_t> baseCtl;
    for (const auto& c : baseline.controls) baseCtl[c.name] = c.value;
    out.controls = old.controls;
    for (const auto& c : now.controls) {
        auto b = baseCtl.find(c.name);
        if (b != baseCtl.end() && b->second == c.value) continue;
        bool found = false;
        for (auto& oc : out.controls)
            if (oc.name == c.name) {
                if (oc.value != c.value) changes.push_back(c.name + ": " + std::to_string(oc.value) + " -> " + std::to_string(c.value));
                oc.value = c.value;
                found = true;
            }
        if (!found) {
            out.controls.push_back(c);
            changes.push_back(c.name + ": (left as it is) -> " + std::to_string(c.value));
        }
    }
    out.name = old.name;
    return out;
}

std::vector<std::string> modeDifferences(const MachineMode& mine, const MachineMode& builtIn) {
    std::vector<std::string> out;
    // Conditions (when the mode is picked).
    auto cond = [](const MachineMode& md) {
        std::ostringstream o;
        writeMatch(o, md);
        std::map<std::string, std::string> m;
        std::istringstream in(o.str());
        std::string l;
        while (std::getline(in, l)) {
            const size_t eq = l.find('=');
            if (eq != std::string::npos && l.rfind("hint=", 0) != 0) m[l.substr(0, eq)] = l.substr(eq + 1);
        }
        return m;
    };
    const auto ca = cond(mine), cb = cond(builtIn);
    for (const auto& kv : cb)
        if (!ca.count(kv.first)) out.push_back("condition " + kv.first.substr(6) + ": yours none, built-in " + kv.second);
        else if (ca.at(kv.first) != kv.second) out.push_back("condition " + kv.first.substr(6) + ": yours " + ca.at(kv.first) + ", built-in " + kv.second);
    for (const auto& kv : ca)
        if (!cb.count(kv.first)) out.push_back("condition " + kv.first.substr(6) + ": yours " + kv.second + ", built-in none");
    // Card controls, by name.
    std::map<std::string, int64_t> cta, ctb;
    for (const auto& c : mine.settings.controls) cta[c.name] = c.value;
    for (const auto& c : builtIn.settings.controls) ctb[c.name] = c.value;
    for (const auto& kv : ctb) {
        auto it = cta.find(kv.first);
        if (it == cta.end()) out.push_back(kv.first + ": yours leaves it as it is, built-in " + std::to_string(kv.second));
        else if (it->second != kv.second)
            out.push_back(kv.first + ": yours " + std::to_string(it->second) + ", built-in " + std::to_string(kv.second));
    }
    std::string extra;
    for (const auto& kv : cta)
        if (!ctb.count(kv.first)) extra += (extra.empty() ? "" : ", ") + kv.first + " " + std::to_string(kv.second);
    if (!extra.empty()) out.push_back("also sets (the built-in mode leaves them as they are): " + extra);
    // The other settings; a key missing on one side counts as the program default.
    auto settings = [](const MachineMode& md) {
        std::map<std::string, std::string> m;
        for (const auto& kv : settingLines(md.settings))
            if (kv.first.rfind("control", 0) != 0) m[kv.first] = kv.second;
        return m;
    };
    const auto sa = settings(mine), sb = settings(builtIn);
    const auto& defs = defaultSettings();
    auto valueOf = [&](const std::map<std::string, std::string>& m, const std::string& k) {
        auto it = m.find(k);
        if (it != m.end()) return it->second;
        auto d = defs.find(k);
        return d != defs.end() ? d->second : std::string("(not set)");
    };
    std::map<std::string, bool> keys;
    for (const auto& kv : sa) keys[kv.first] = true;
    for (const auto& kv : sb) keys[kv.first] = true;
    for (const auto& k : keys) {
        const std::string va = valueOf(sa, k.first), vb = valueOf(sb, k.first);
        if (va != vb) out.push_back(k.first + ": yours " + (va.empty() ? "(empty)" : va) + ", built-in " + (vb.empty() ? "(empty)" : vb));
    }
    return out;
}

std::vector<ModeUpdate> pendingModeUpdates(const std::vector<Machine>& machines) {
    std::vector<ModeUpdate> out;
    for (size_t i = 0; i < machines.size(); ++i)
        for (const MachineMode& md : machines[i].modes) {
            if (md.origin != ModeOrigin::MineReplacing) continue;
            for (const MachineMode& b : machines[i].replacedBuiltIn)
                if (b.name == md.name && b.revision > std::max(md.basedOn, md.keptRevision))
                    out.push_back({int(i), md.name, b.revision});
        }
    return out;
}

// 0.1.0 kept copies of the built-in machine files in the user's folder and
// presets in presets/GROUP/NAME.txt. Converted once:
// - the folder is copied to machines.backup-0.1.0 first;
// - a copied built-in mode identical to today's built-in one is dropped, a
//   changed one stays as the user's mode replacing it (based_on=0), a mode
//   that isn't built-in stays as the user's own;
// - each preset becomes a click-only mode in My modes: group "C64" in the
//   C64 machine, another group in a machine of that name, no group in "My
//   setups"; presets/ is renamed to presets.old.
bool migrateSettings(std::string& report) {
    const std::string dir = machinesDir();
    if (dir.empty()) return false;
    std::error_code ec;
    std::vector<std::string> errors;
    std::vector<fs::path> old;
    for (const fs::path& p : confFiles(dir)) {
        Machine m;
        std::vector<RawMode> modes;
        int fv = 1;
        if (parseFile(p, m, modes, fv, errors) && fv < 2) old.push_back(p);
    }
    const fs::path presets = fs::path(configDir()) / "presets";
    const bool havePresets = fs::is_directory(presets, ec);
    if (old.empty() && !havePresets) return false;

    std::vector<std::string> e2;
    const std::vector<Machine> builtIns = loadBuiltIns(e2);  // the shipped files only, not combined with the user's
    auto builtInFor = [&](const std::string& id) -> const Machine* {
        for (const Machine& m : builtIns)
            if (m.id == id) return &m;
        return nullptr;
    };
    if (!old.empty()) {
        const fs::path backup = fs::path(configDir()) / "machines.backup-0.1.0";
        if (!fs::exists(backup, ec)) {
            fs::create_directories(backup, ec);
            for (const fs::path& p : old) fs::copy_file(p, backup / p.filename(), fs::copy_options::skip_existing, ec);
            report += "backup of the old machine files: " + backup.string() + "\n";
        }
    }
    std::map<std::string, Machine> result;  // id -> the user's part
    for (const fs::path& p : old) {
        Machine um;
        std::vector<RawMode> modes;
        int fv = 1;
        parseFile(p, um, modes, fv, errors);
        const Machine* b = builtInFor(um.id);
        Machine out = b ? *b : um;
        out.modes.clear();
        out.mine = true;
        int kept = 0, dropped = 0;
        std::string detail;  // printed under the file's line
        for (RawMode& r : modes) {
            finishSettings(r.mode, r.kv, p.filename().string(), errors);
            const MachineMode* bm = nullptr;
            if (b)
                for (const MachineMode& x : b->modes)
                    if (x.name == r.mode.name) bm = &x;
            if (bm) {
                // Compared as the user changed it: keys the old version didn't
                // know take the built-in values, the hint text and an explicit
                // "Auto" encoder are not changes of the user's.
                std::map<std::string, std::string> kv = bm->raw;
                for (const auto& e : r.kv)
                    if (!(e.first == "recorder_encoder" && e.second == "Auto" && !bm->raw.count(e.first))) kv[e.first] = e.second;
                MachineMode cmp = r.mode;
                cmp.hint = bm->hint;
                std::vector<std::string> e3;
                finishSettings(cmp, kv, p.filename().string(), e3);
                if (modeText(*bm) == modeText(cmp)) {
                    ++dropped;
                    continue;
                }
                const std::vector<std::string> diff = modeDifferences(cmp, *bm);
                detail += "  " + r.mode.name + " kept: ";
                for (size_t i = 0; i < diff.size() && i < 4; ++i) detail += (i ? "; " : "") + diff[i];
                if (diff.size() > 4) detail += "; ... (" + std::to_string(diff.size()) + " differences)";
                detail += "\n";
            }
            r.mode.origin = bm ? ModeOrigin::MineReplacing : ModeOrigin::Mine;
            if (bm) r.mode.basedOn = 0;
            out.modes.push_back(r.mode);
            ++kept;
        }
        result[out.id] = out;
        report += p.filename().string() + ": " + std::to_string(dropped) + " mode(s) same as built-in, " +
                  std::to_string(kept) + " kept as yours\n" + detail;
        fs::remove(p, ec);
    }
    if (havePresets) {
        for (const PresetEntry& e : listPresets()) {
            Preset pr;
            std::string err;
            if (!loadPreset(e.group, e.name, pr, err)) {
                report += "preset " + e.group + "/" + e.name + ": " + err + "\n";
                continue;
            }
            const std::string id = e.group == "C64" ? "c64" : machineIdFor(e.group.empty() ? "My setups" : e.group);
            if (!result.count(id)) {
                const Machine* b = builtInFor(id);
                Machine m = b ? *b : Machine{};
                if (!b) m.id = id, m.name = e.group.empty() ? "My setups" : e.group;
                m.modes.clear();
                m.mine = true;
                // keep the user's existing (new-format) file content
                std::vector<std::string> e2;
                for (Machine& cur : loadMachines(e2))
                    if (cur.id == id)
                        for (const MachineMode& md : cur.modes)
                            if (md.origin != ModeOrigin::BuiltIn) m.modes.push_back(md);
                result[id] = m;
            }
            MachineMode md;
            md.name = e.name;
            md.match.manual = true;
            md.settings = pr;
            md.settings.name = e.name;
            md.origin = ModeOrigin::Mine;
            for (const MachineMode& x : result[id].modes)
                if (x.name == md.name) md.name += " (preset)";
            result[id].modes.push_back(md);
            report += "preset " + (e.group.empty() ? std::string() : e.group + "/") + e.name + " -> " + result[id].name +
                      " / " + md.name + " (click-only)\n";
        }
    }
    for (auto& kv : result) {
        std::string err;
        if (!saveMachine(kv.second, err)) report += kv.first + ": " + err + "\n";
    }
    if (havePresets) {
        fs::path dst = fs::path(configDir()) / "presets.old";
        if (fs::exists(dst, ec)) dst = fs::path(configDir()) / "presets.old-2";
        fs::rename(presets, dst, ec);
        report += "presets folder kept as " + dst.string() + "\n";
    }
    for (const std::string& e : errors) report += "note: " + e + "\n";
    return true;
}

std::string machineIdFor(const std::string& name) {
    std::string id;
    for (unsigned char c : name) {
        if (std::isalnum(c)) id += char(std::tolower(c));
        else if (!id.empty() && id.back() != '-') id += '-';
    }
    while (!id.empty() && id.back() == '-') id.pop_back();
    return id.empty() ? "machine" : id;
}

int64_t machinesDirStamp() {
    std::string dir = machinesDir();
    int64_t stamp = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        auto t = fs::last_write_time(e.path(), ec2);
        if (!ec2) stamp = std::max<int64_t>(stamp, int64_t(t.time_since_epoch().count()));
        ++stamp;  // a deleted file changes the count
    }
    return stamp;
}

int scoreMode(const Machine& m, const MachineMode& mode, const DetectedMode& d0, std::string* why, bool allowManual) {
    if (d0.noSignal) return -1;
    DetectedMode d = d0;
    for (const auto& a : m.aliases)
        if (d.width == a.first.first && d.height == a.first.second) {
            d.width = a.second.first;
            d.height = a.second.second;
            break;
        }
    const ModeMatch& c = mode.match;
    if (c.manual && !allowManual) return -1;  // only by a click (or the machine named by the user)
    int score = 0;
    std::string w;
    auto held = [&](const std::string& what) {
        ++score;
        w += (w.empty() ? "" : ", ") + what;
    };
    if (!c.device.empty()) {
        if (d.device != c.device) return -1;
        held("device");
    }
    if (!c.input.empty()) {
        if (lower(d.input).find(lower(c.input)) == std::string::npos) return -1;
        held("input");
    }
    if (!c.signal.empty()) {
        if (lower(d.signal) != lower(c.signal)) return -1;
        held("signal");
    }
    if (c.width && c.height) {
        if (!d.width || !d.height) {
        } else if (d.width != c.width || d.height != c.height) {
            return -1;
        } else {
            held("size");
        }
    }
    if (c.lineTotal) {
        if (!d.lineTotal) {
        } else if (std::abs(d.lineTotal - c.lineTotal) > c.lineTotalTol) {
            return -1;
        } else {
            held("line total");
        }
    }
    if (c.refreshHz > 0 && d.refreshHz > 0) {
        if (std::abs(d.refreshHz - c.refreshHz) > c.refreshTol) return -1;
        held("refresh");
    }
    static const char* chName[] = {"red", "green", "blue"};
    if (c.colourOn >= 0 && c.colourOn < 3 && d.colourOn[c.colourOn] >= 0) {
        if (d.colourOn[c.colourOn] != 1) return -1;
        held(std::string("colour on ") + chName[c.colourOn]);
    }
    if (c.noColourOn >= 0 && c.noColourOn < 3 && d.colourOn[c.noColourOn] >= 0) {
        if (d.colourOn[c.noColourOn] != 0) return -1;
        held(std::string("no colour on ") + chName[c.noColourOn]);
    }
    if (c.cyclesPerLine > 0 && d.cyclesPerLine > 0) {
        if (std::abs(d.cyclesPerLine - c.cyclesPerLine) > c.cyclesTol) return -1;
        held("carrier cycles/line");
    }
    if (why) *why = w;
    return score;
}

MatchResult bestMatch(const std::vector<Machine>& machines, const DetectedMode& d) {
    MatchResult r;
    bool rMine = false;
    for (size_t i = 0; i < machines.size(); ++i)
        for (size_t j = 0; j < machines[i].modes.size(); ++j) {
            std::string why;
            int s = scoreMode(machines[i], machines[i].modes[j], d, &why);
            // Ties: the higher match_priority, then the user's own mode over a
            // built-in one, then the earlier file (default comes last).
            const int pr = machines[i].modes[j].match.priority;
            const bool mine = machines[i].modes[j].origin != ModeOrigin::BuiltIn;
            if (s > r.score || (s >= 0 && s == r.score && (pr > r.priority || (pr == r.priority && mine && !rMine)))) {
                r.machine = int(i), r.mode = int(j), r.score = s, r.why = why, r.priority = pr;
                rMine = mine;
            }
        }
    return r;
}

}  // namespace fv

// SPDX-License-Identifier: GPL-3.0-or-later
#include "autodetect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "formats.h"
#include "grid.hpp"
#include "pal.hpp"

namespace fv::detect {

const char* pathName(Path p) {
    switch (p) {
    case Path::Digital: return "digital";
    case Path::AnalogRgb: return "analog RGB";
    case Path::Composite: return "composite";
    case Path::SVideo: return "S-Video";
    default: return "none";
    }
}

std::string Proposal::summary() const {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s, %s, confidence %d%%", pathName(path), recognised ? "recognised" : "not recognised",
                  int(std::lround(confidence * 100)));
    return buf;
}

static bool contains(const std::string& s, const char* part) {
    std::string a = s, b = part;
    for (char& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

Path pathFor(const std::string& input, const std::string& signal) {
    if (contains(signal, "DVI") || contains(signal, "HDMI") || contains(signal, "Digital") || contains(signal, "DisplayPort"))
        return Path::Digital;
    if (contains(input, "Sync On Green")) return Path::Composite;  // S-Video is decided by the bursts
    return Path::AnalogRgb;
}

double resolveCyclesPerLine(double approx, double stepDeg, bool exact, double* distance) {
    double s = stepDeg / 360.0;
    s -= std::floor(s);
    double best = 0, bestD = 1e9;
    for (double base : {std::floor(approx) - 1, std::floor(approx), std::floor(approx) + 1})
        for (double f : {s, exact ? s : s + 0.5}) {
            const double c = base + (f - std::floor(f));
            if (std::abs(c - approx) < bestD) bestD = std::abs(c - approx), best = c;
        }
    if (distance) *distance = bestD;
    return best;
}

static void addControl(Preset& p, const char* name, int64_t v) {
    for (auto& c : p.controls)
        if (c.name == name) {
            c.value = v;
            return;
        }
    p.controls.push_back({name, v});
}

static std::string fmt(const char* f, double v) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// The shape every proposal starts from: integer scaling, nothing decoded.
static Preset baseSettings() {
    Preset p;
    p.audioSpecified = false;
    p.encoderSpecified = false;
    p.displaySpecified = true;
    p.display.scaleMode = ScaleMode::Integer;
    p.display.integerFactor = 0;
    p.display.palDecode = false;
    p.display.palAutoTune = false;
    p.display.rgbAutoTune = false;
    p.display.palUi.profile = 0;
    return p;
}

// Square-pixel clocks: one line period of 4:3 picture at 576 / 480 lines.
// A 15 kHz source draws each of its lines once per field (no interlace), so a
// machine pixel is two TV lines tall: aspect = square clock / pixel clock / 2.
static double pixelAspectFor(double pixelClockHz, bool ntsc) {
    const double square = ntsc ? 12.272727e6 : 14.75e6;
    return pixelClockHz > 0 ? square / pixelClockHz / 2 : 1;
}

static void composite(const Measurements& m, Proposal& pr) {
    Preset& s = pr.settings;
    double conf = 1;
    // Standard from the frame rate.
    bool ntsc = false;
    if (m.refreshHz >= 47 && m.refreshHz <= 53) {
        pr.reasons.push_back({"Standard", "PAL", fmt("%.3f Hz frames (47-53 Hz)", m.refreshHz)});
    } else if (m.refreshHz >= 57 && m.refreshHz <= 63) {
        ntsc = true;
        pr.reasons.push_back({"Standard", "NTSC", fmt("%.3f Hz frames (57-63 Hz)", m.refreshHz)});
    } else {
        ntsc = m.lineRateHz > 15660;
        conf *= 0.4;
        pr.reasons.push_back({"Standard", ntsc ? "NTSC" : "PAL",
                              m.refreshHz > 0 ? fmt("%.2f Hz frames: neither 50 nor 60 Hz, guessed from the line rate", m.refreshHz)
                                              : "frame rate not measured: guessed from the line rate"});
    }
    const pal::StandardInfo& si = pal::standardInfo(ntsc ? pal::Standard::Ntsc : pal::Standard::Pal);
    s.display.videoStandard = ntsc ? 1 : 0;
    // Line rate: the driver's; twice the 15 kHz rate means it locked onto the
    // half-line pulses of the vertical sync.
    double lineRate = m.lineRateHz;
    if (lineRate > 25000 && lineRate < 34000 && m.refreshHz < 70) {
        pr.warnings.push_back(fmt("The card counts %.0f lines/s, twice a TV line rate: it locked onto the vertical sync's half-line "
                                  "pulses. Re-plug the cable (or switch the machine off and on) and detect again.",
                                  lineRate));
        conf *= 0.3;
        lineRate /= 2;
    }
    if (lineRate < 14000 || lineRate > 17000) {
        // Without the driver's figure: the sampling we measured on.
        if (m.samplesPerCycle > 0 && m.horizontalSize > 0) lineRate = si.fsc * m.samplesPerCycle / m.horizontalSize;
        else lineRate = si.lineRate, conf *= 0.6;
        pr.reasons.push_back({"Line rate", fmt("%.0f Hz", lineRate), "the driver's figure is missing or not a TV line rate: estimated"});
    } else {
        pr.reasons.push_back({"Line rate", fmt("%.1f Hz", lineRate), "the driver's measurement (HorFreq)"});
    }
    if (m.refreshHz > 0) {
        const double lines = lineRate / m.refreshHz;
        pr.reasons.push_back({"Lines per frame", fmt("%.1f", lines), "line rate / frame rate"});
    }
    // Colour: which wires carry a burst.
    const WireBurst& g = m.burst[1];
    int chroma = -1;
    for (int w : {2, 0})
        if (m.burst[w].colour == 1 && (g.colour != 1 || m.burst[w].amplitude > 2 * g.amplitude)) {
            chroma = w;
            break;
        }
    const bool svideo = chroma >= 0;
    const bool colour = svideo || g.colour == 1;
    if (g.colour < 0 && m.burst[0].colour < 0 && m.burst[2].colour < 0) conf *= 0.5;
    pr.path = svideo ? Path::SVideo : Path::Composite;
    const WireBurst& cb = svideo ? m.burst[chroma] : g;
    // Carrier cycles per line: roughly fsc / line rate, exactly from the step.
    const double approx = si.fsc / lineRate;
    double cycles = approx;
    if (colour && cb.colour == 1) {
        double d = 0;
        cycles = resolveCyclesPerLine(approx, cb.phaseStepDeg, !si.vSwitch, &d);
        if (d > 0.3) conf *= 0.6;
        pr.reasons.push_back({"Carrier cycles per line", fmt("%.2f", cycles),
                              fmt("%.2f from the line rate, refined by the measured carrier step ", approx) +
                                  fmt("(%.1f deg per line)", cb.phaseStepDeg)});
    } else {
        pr.reasons.push_back({"Carrier cycles per line", fmt("%.2f", cycles), "from the line rate only (no colour carrier)"});
    }
    const int line4 = int(std::lround(cycles * 4));
    const int K = 8;
    s.display.palDecode = true;
    s.display.palMultiple = K;
    s.display.palLine = line4;
    s.display.palAutoTune = true;
    s.display.palUi.profile = 0;
    pr.reasons.push_back({"Sampling", fmt("8x, Horizontal Size %.0f", double(line4) * K / 4),
                          "8 samples per colour cycle: sharp pixels, well inside the card's tested range (PAL up to 10x)"});
    if (svideo) {
        s.formatFourcc = findFormat("rgb4")->fourcc;
        s.display.palSvideo = true;
        s.display.svideoLuma = 1;
        s.display.svideoChroma = chroma;
        s.display.palUi.averageLines = !ntsc;
        pr.reasons.push_back({"Signal", std::string("S-Video, colour on ") + (chroma == 2 ? "Blue" : "Red"),
                              fmt("burst on that wire (%.0f codes), brightness and sync on Green", cb.amplitude)});
    } else if (colour) {
        s.formatFourcc = findFormat("g8")->fourcc;
        s.display.palUi.averageLines = !ntsc;
        s.display.palUi.luma = 1;
        pr.reasons.push_back({"Signal", "colour composite", fmt("burst on Green (%.0f codes)", g.amplitude)});
    } else {
        s.formatFourcc = findFormat("g8")->fourcc;
        s.display.palUi.mono = true;
        s.display.palUi.luma = 2;
        s.display.palUi.averageLines = false;
        s.display.palUi.pll = false;
        pr.reasons.push_back({"Signal", "black and white composite", "no colour burst on any wire"});
    }
    s.width = ntsc ? 1664 : 2048;
    s.height = ntsc ? 240 : 298;
    pr.reasons.push_back({"Capture", std::to_string(s.width) + "x" + std::to_string(s.height),
                          ntsc ? "the NTSC 8x window (as the Apple II / MSX modes)" : "the PAL 8x window (as the C64 modes)"});
    // Levels: the auto-tune steers them live; say what it starts from.
    if (m.whiteClip > 0.01 || m.blackClip > 0.2)
        pr.reasons.push_back({"Levels", "auto-tune",
                              fmt("%.1f%% of the samples clip at white", m.whiteClip * 100) +
                                  fmt(", %.0f%% of the lines clip the burst at black: auto-tune corrects both", m.blackClip * 100)});
    else
        pr.reasons.push_back({"Levels", "auto-tune", "no clipping measured; auto-tune keeps them there"});
    // No pixel clock for composite or S-Video: one frame's grid depends on the
    // picture (C64: 4.5, 9 or 3.38 samples on different frames).
    pr.confidence = conf;
    pr.recognised = conf >= 0.3;
    if (m.refreshHz <= 0 && (m.lineRateHz < 14000 || m.lineRateHz > 34000)) {
        pr.recognised = false;
        pr.warnings.push_back("Neither a frame rate nor a line rate was measured: nothing to go on.");
    }
}

static void analogRgb(const Measurements& m, Proposal& pr) {
    Preset& s = pr.settings;
    double conf = 1;
    pr.path = Path::AnalogRgb;
    s.formatFourcc = findFormat("rgb4")->fourcc;
    s.display.rgbAutoTune = true;
    pr.reasons.push_back({"Signal", "analog RGB", "not Sync-on-Green, not a digital signal type (" + m.signal + ")"});
    if (m.refreshHz > 0) pr.reasons.push_back({"Frame rate", fmt("%.3f Hz", m.refreshHz), "measured"});
    const int hs = m.horizontalSize > 0 ? m.horizontalSize : m.lineTotal;
    if (hs <= 0 || m.width == 0 || m.height == 0) {
        pr.confidence = 0;
        pr.warnings.push_back("The driver reports no line total or picture size for this signal.");
        return;
    }
    // One sample per machine pixel: the line total divided by the pixel period.
    int newHs = hs;
    double period = 1;
    if (m.pixelPeriod > 1.15 && m.pixelScore >= 0.35) {
        period = m.pixelPeriod;
        newHs = int(std::lround(hs / period));
        pr.reasons.push_back({"Horizontal Size", std::to_string(hs) + " -> " + std::to_string(newHs),
                              fmt("%.3f samples per machine pixel now: one sample per pixel", period)});
    } else if (m.pixelPeriod > 1.15) {
        conf *= 0.8;
        pr.reasons.push_back({"Horizontal Size", std::to_string(hs),
                              fmt("kept: a pixel grid of %.2f samples was found, ", m.pixelPeriod) +
                                  fmt("but too weakly (score %.2f < 0.35) to change the sampling", m.pixelScore)});
    } else if (m.pixelPeriod > 0) {
        pr.reasons.push_back({"Horizontal Size", std::to_string(hs), "already one sample per machine pixel"});
    } else {
        conf *= 0.6;
        pr.reasons.push_back({"Horizontal Size", std::to_string(hs), "no pixel grid found: kept (a picture with more edges helps)"});
    }
    addControl(s, "Horizontal Size", newHs);
    // Width aligned to 16 (the card rejects some widths).
    const uint32_t w = std::max<uint32_t>(64, uint32_t(std::lround(m.width / period)) / 16 * 16);
    s.width = w;
    s.height = m.height;
    addControl(s, "Horizontal Timing", w);
    addControl(s, "Vertical Timing", m.height);
    pr.reasons.push_back({"Capture", std::to_string(w) + "x" + std::to_string(m.height), "the detected active area at the new sampling"});
    if (m.area.valid) {
        const int sw = int(std::lround((m.area.x1 - m.area.x0 + 1) / period)), sh = m.area.y1 - m.area.y0 + 1;
        s.display.rgbScreenW = sw;
        s.display.rgbScreenH = sh;
        pr.reasons.push_back({"Screen size", std::to_string(sw) + "x" + std::to_string(sh),
                              "the drawn picture incl. border; auto-tune keeps it centred"});
    }
    if (m.lineRateHz > 0 && m.lineRateHz < 20000) {
        const double clock = m.lineRateHz * newHs;
        s.display.pixelAspect = std::round(pixelAspectFor(clock, m.refreshHz > 55) * 1000) / 1000;
        pr.reasons.push_back({"Pixel aspect", fmt("%.3f", s.display.pixelAspect),
                              fmt("15 kHz source, %.3f MHz pixels: square-pixel clock / pixel clock / 2", clock / 1e6)});
    } else if (m.lineRateHz > 0) {
        pr.reasons.push_back({"Pixel aspect", "1", fmt("%.1f kHz lines: a monitor-rate source, square pixels assumed", m.lineRateHz / 1000)});
    }
    pr.reasons.push_back({"Phase and position", "RGB auto-tune", "sharpest phase, picture centred (runs after applying)"});
    pr.confidence = conf;
    pr.recognised = true;
}

static void digital(const Measurements& m, Proposal& pr) {
    Preset& s = pr.settings;
    pr.path = Path::Digital;
    s.formatFourcc = findFormat("rgb4")->fourcc;
    s.width = m.width;
    s.height = m.height;
    pr.reasons.push_back({"Signal", "digital (" + m.signal + ")", "pixels arrive as sent: no sampling or levels to tune"});
    pr.reasons.push_back({"Capture", std::to_string(m.width) + "x" + std::to_string(m.height),
                          m.refreshHz > 0 ? fmt("native size, %.3f Hz", m.refreshHz) : "native size"});
    double conf = m.width && m.height ? 1.0 : 0.2;
    if (m.repeatX > 1 || m.repeatY > 1) {
        pr.reasons.push_back({"Machine pixels", std::to_string(m.width / std::max(1, m.repeatX)) + "x" +
                                                    std::to_string(m.height / std::max(1, m.repeatY)),
                              "each machine pixel is sent " + std::to_string(m.repeatX) + "x" + std::to_string(m.repeatY) +
                                  " times (shown as sent: the repetition already gives the shape the machine meant)"});
    } else {
        pr.reasons.push_back({"Machine pixels", "as sent", "no pixel repetition found"});
    }
    // No crop: a digital source is recorded 1:1 at the detected size
    // (crop afterwards if needed; a PC screen can be almost all black).
    pr.reasons.push_back({"Scaling", "integer, auto", "every machine pixel the same size on screen"});
    pr.confidence = conf;
    pr.recognised = conf >= 0.5;
}

Proposal propose(const Measurements& m, const std::vector<Machine>* machines) {
    Proposal pr;
    pr.settings = baseSettings();
    if (m.noSignal) {
        pr.warnings.push_back("No signal on this input.");
        return pr;
    }
    switch (pathFor(m.input, m.signal)) {
    case Path::Digital: digital(m, pr); break;
    case Path::Composite: composite(m, pr); break;
    default: analogRgb(m, pr); break;
    }
    pr.settings.name = "Detected";
    pr.settings.group = "Detected";
    // Known machines with the same timing (a hint only).
    if (machines && m.refreshHz > 0) {
        DetectedMode d;
        d.input = m.input, d.signal = m.signal, d.refreshHz = m.refreshHz;
        if (pr.path == Path::Composite || pr.path == Path::SVideo) {
            for (const auto& r : pr.reasons)
                if (r.what == "Carrier cycles per line") d.cyclesPerLine = std::atof(r.value.c_str());
            // Composite modes match on the input name only.
            d.input = "Analog Sync On Green";
        } else {
            d.width = m.width, d.height = m.height, d.lineTotal = m.lineTotal;
        }
        std::vector<std::string> names;
        for (const Machine& mc : *machines) {
            if (mc.id == "default") continue;
            for (const MachineMode& md : mc.modes) {
                // Only modes that state a timing count (a mode matching on the input alone says nothing).
                if (md.match.refreshHz <= 0) continue;
                if (scoreMode(mc, md, d, nullptr, true) >= 2) {
                    if (std::find(names.begin(), names.end(), mc.name) == names.end()) names.push_back(mc.name);
                    break;
                }
            }
        }
        for (size_t i = 0; i < names.size(); ++i) pr.timingLike += (i ? ", " : "") + names[i];
    }
    return pr;
}

// ---- frames ----

static int wireOffset(Layout l, int wire, int& bpp) {
    // wire 0 red, 1 green, 2 blue
    switch (l) {
    case Layout::Bgrx8: bpp = 4; return 2 - wire;
    case Layout::Rgbx8: bpp = 4; return wire;
    case Layout::Xrgb8: bpp = 4; return 1 + wire;
    case Layout::Xbgr8: bpp = 4; return 3 - wire;
    case Layout::Rgb24: bpp = 3; return wire;
    case Layout::Bgr24: bpp = 3; return 2 - wire;
    case Layout::Grey8: bpp = 1; return 0;
    default: bpp = 0; return -1;
    }
}

std::vector<uint8_t> extractWire(const FrameView& f, int wire) {
    int bpp = 0;
    const int off = wireOffset(f.layout, wire, bpp);
    std::vector<uint8_t> out;
    if (off < 0 || !f.data) return out;
    out.resize(size_t(f.width) * f.height);
    for (uint32_t y = 0; y < f.height; ++y) {
        const uint8_t* row = f.data + size_t(y) * f.stride;
        for (uint32_t x = 0; x < f.width; ++x) out[size_t(y) * f.width + x] = row[size_t(x) * bpp + off];
    }
    return out;
}

std::vector<uint8_t> toRgb(const FrameView& f) {
    std::vector<uint8_t> out(size_t(f.width) * f.height * 3);
    int bpp = 0;
    int off[3];
    for (int c = 0; c < 3; ++c) off[c] = wireOffset(f.layout, c, bpp);
    if (off[0] < 0 || !f.data) return {};
    for (uint32_t y = 0; y < f.height; ++y) {
        const uint8_t* row = f.data + size_t(y) * f.stride;
        uint8_t* o = out.data() + size_t(y) * f.width * 3;
        for (uint32_t x = 0; x < f.width; ++x)
            for (int c = 0; c < 3; ++c) o[x * 3 + c] = row[size_t(x) * bpp + off[c]];
    }
    return out;
}

WireBurst measureBurst(const uint8_t* plane, int w, int h, int K, bool ntsc, int burstAt) {
    WireBurst b;
    if (!plane || w < 64 || h < 16 || K < 3) return b;
    // Two halves of the frame on their own: a real burst gives the same
    // carrier step in both; noise or picture detail taken for one does not.
    auto half = [&](const uint8_t* p, int lines, pal::DecodeInfo& info) {
        pal::DecodeParams dp;
        dp.samplesPerCycle = K;
        dp.burstLen = 4 * K;
        dp.burstStart = burstAt;
        dp.measureOnly = true;
        dp.standard = ntsc ? pal::Standard::Ntsc : pal::Standard::Pal;
        pal::decode(p, w, lines, dp, &info);
    };
    const int h1 = (h / 2) & ~1;
    pal::DecodeInfo a, c;
    half(plane, h1, a);
    half(plane + size_t(h1) * w, h1, c);
    double diff = std::fmod(std::abs(a.phaseStepDeg - c.phaseStepDeg), 360.0);
    diff = std::min(diff, 360 - diff);
    if (!ntsc) diff = std::min(diff, std::abs(180 - diff));  // PAL: the step is known up to 180 deg
    b.colour = a.colour && c.colour && diff < 10 ? 1 : 0;
    b.amplitude = (a.burstAmplitude + c.burstAmplitude) / 2;
    b.phaseStepDeg = a.phaseStepDeg;
    b.blankCode = (a.blankCode + c.blankCode) / 2;
    return b;
}

void pixelRepetition(const FrameView& f, int& rx, int& ry) {
    rx = ry = 1;
    if (!f.data || f.width < 16 || f.height < 16) return;
    const std::vector<uint8_t> rgb = toRgb(f);
    if (rgb.empty()) return;
    const int W = int(f.width), H = int(f.height);
    auto px = [&](int x, int y) { return rgb.data() + (size_t(y) * W + x) * 3; };
    auto differs = [](const uint8_t* a, const uint8_t* b) {
        return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) > 24;
    };
    // Edges between neighbours, per position modulo r: with repetition r all
    // edges fall on one phase; inside a group (the other phases) there are none.
    auto factor = [&](bool horiz) {
        const int n = horiz ? W : H;
        std::vector<long> edgesAt(n, 0);
        long total = 0;
        if (horiz) {
            for (int y = 0; y < H; y += 2)
                for (int x = 1; x < W; ++x)
                    if (differs(px(x, y), px(x - 1, y))) ++edgesAt[size_t(x)], ++total;
        } else {
            for (int y = 1; y < H; ++y)
                for (int x = 0; x < W; x += 2)
                    if (differs(px(x, y), px(x, y - 1))) ++edgesAt[size_t(y)], ++total;
        }
        if (total < 200) return 1;
        for (int r : {4, 3, 2}) {
            for (int ph = 0; ph < r; ++ph) {
                long on = 0;
                for (int i = 1; i < n; ++i)
                    if ((i - ph) % r == 0) on += edgesAt[size_t(i)];
                if (on >= total * 99 / 100) return r;
            }
        }
        return 1;
    };
    rx = factor(true);
    ry = factor(false);
}

void pixelSearchRange(double sampleRateHz, double lineRateHz, double& lo, double& hi) {
    const double k = lineRateHz > 25000 ? 2 : 1;
    lo = std::max(1.2, sampleRateHz / (16e6 * k));
    hi = std::max(lo * 1.5, sampleRateHz / (5e6 * k));
}

double pixelPeriodRgb(const uint8_t* rgb, int w, int h, double lo, double hi, double* score) {
    const pal::GridEstimate g = pal::estimateGrid(rgb, w, h, lo, hi);
    if (score) *score = g.score;
    return g.valid ? g.period : 0;
}

}  // namespace fv::detect

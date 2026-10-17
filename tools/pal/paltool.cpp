// SPDX-License-Identifier: GPL-3.0-or-later
// paltool: exercise the PAL generator/decoder in pal.hpp.
//   paltool test                                   accuracy suite on generated colour bars
//   paltool gen OUT.raw [K] [lift]                 write generated colour bars (rows of 8-bit samples)
//   paltool decode IN.raw WIDTH LINES K OUT.ppm [activeStart] [burstStart] [avg] [luma=box|notch|off] [line=HSIZE]
//   paltool genyc OUT.bgr0 N                        N frames of 8x C64 S-Video bars as bgr0 (Y in G, C in R)
//   paltool bench                                  decode time of a 1024x298 4xFsc field
#include "pal.hpp"
#include "grid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace {

bool writePpm(const char* path, const std::vector<uint8_t>& rgb, int w, int h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

struct Case {
    const char* name;
    pal::GenParams gen;
    bool expectColour;
    bool mustPass;
    bool averageLines = false;
    double maxHanover = 1e9;  // limit on the line-to-line colour difference (Hanover bars)
    double maxErr = 8.0;      // limit on the worst bar error
    bool pll = false;         // decode twice: measure the step error, then decode with it (freshCoder's PLL)
};

// Mean absolute RGB difference between neighbouring lines over the middle of
// the coloured bars: Hanover bars make it large, a clean picture keeps it ~0.
double hanover(const std::vector<uint8_t>& rgb, int w, int h) {
    double sum = 0;
    long n = 0;
    for (int b = 1; b < 7; ++b) {
        int x0 = int((b + 0.25) * w / 8), x1 = int((b + 0.75) * w / 8);
        for (int y = 11; y < h - 10; ++y)
            for (int x = x0; x < x1; ++x)
                for (int c = 0; c < 3; ++c, ++n)
                    sum += std::abs(int(rgb[(size_t(y) * w + x) * 3 + c]) - int(rgb[(size_t(y - 1) * w + x) * 3 + c]));
    }
    return n ? sum / n : 0;
}

// Mean RGB over the middle half of each bar, middle lines; returns the worst
// channel error against the 75% bars (codes, 0..255).
double barError(const std::vector<uint8_t>& rgb, int w, int h, double* chromaLeak, std::string& detail) {
    static const int want[8][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
                                   {191, 0, 191},   {191, 0, 0},   {0, 0, 191},   {0, 0, 0}};
    double worst = 0, leak = 0;
    detail.clear();
    for (int b = 0; b < 8; ++b) {
        double sum[3] = {0, 0, 0};
        int n = 0;
        int x0 = int((b + 0.25) * w / 8), x1 = int((b + 0.75) * w / 8);
        for (int y = 10; y < h - 10; ++y)
            for (int x = x0; x < x1; ++x) {
                const uint8_t* p = &rgb[(size_t(y) * w + x) * 3];
                for (int c = 0; c < 3; ++c) sum[c] += p[c];
                ++n;
            }
        char buf[64];
        std::snprintf(buf, sizeof buf, " (%.0f,%.0f,%.0f)", sum[0] / n, sum[1] / n, sum[2] / n);
        detail += buf;
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(sum[c] / n - want[b][c]));
        leak = std::max({leak, std::abs(sum[0] / n - sum[1] / n), std::abs(sum[2] / n - sum[1] / n)});
    }
    if (chromaLeak) *chromaLeak = leak;
    return worst;
}

// Black left half, white right half: a single sharp brightness step.
pal::Rgb lumaStep(double xFrac) { return xFrac < 0.5 ? pal::Rgb{0, 0, 0} : pal::Rgb{0.75, 0.75, 0.75}; }

// 10-90% rise width (samples) of the decoded brightness across the step, on
// one middle line, and the carrier ripple (max-min of grey) inside the bars.
void filterReport(pal::LumaFilter f, const char* name, int& failures, double boxRise) {
    pal::GenParams p;
    pal::DecodeParams dp;
    dp.burstLen = 32;
    dp.activeStart = int(std::lround(10.5e-6 * p.samplesPerLine * pal::kLineRate)) - p.windowStart;
    dp.activeLen = int(std::lround(52e-6 * p.samplesPerLine * pal::kLineRate));
    dp.luma = f;
    auto raw = pal::generate(p, lumaStep);
    auto rgb = pal::decode(raw.data(), p.width, p.lines, dp, nullptr);
    const int y = p.lines / 2, w = dp.activeLen;
    auto g = [&](int x) { return double(rgb[(size_t(y) * w + x) * 3 + 1]); };
    double lo = g(w / 4), hi = g(3 * w / 4);
    int x10 = 0, x90 = 0;
    for (int x = w / 4; x < 3 * w / 4; ++x) {
        if (!x10 && g(x) >= lo + 0.1 * (hi - lo)) x10 = x;
        if (!x90 && g(x) >= lo + 0.9 * (hi - lo)) x90 = x;
    }
    auto bars = pal::generate(p, pal::colourBars);
    auto brgb = pal::decode(bars.data(), p.width, p.lines, dp, nullptr);
    double ripple = 0;
    for (int b = 0; b < 8; ++b) {
        int x0 = int((b + 0.3) * w / 8), x1 = int((b + 0.7) * w / 8);
        for (int c = 0; c < 3; ++c) {
            double mn = 255, mx = 0;
            for (int x = x0; x < x1; ++x) {
                double v = brgb[(size_t(y) * w + x) * 3 + c];
                mn = std::min(mn, v), mx = std::max(mx, v);
            }
            ripple = std::max(ripple, mx - mn);
        }
    }
    const int rise = x90 - x10;
    // Notch must be sharper than Box and leave little carrier on flat colour.
    bool ok = true;
    if (f == pal::LumaFilter::Notch) ok = rise < boxRise && ripple <= 6.0;
    if (f == pal::LumaFilter::Notch && !ok) ++failures;
    std::printf("luma filter %-6s %s  step rise 10-90%% %d samples, carrier ripple on flat colour %.0f codes\n", name,
                f == pal::LumaFilter::Notch ? (ok ? "PASS" : "FAIL") : "info", rise, ripple);
}

// A C64-style picture of palette pixels at the C64's pixel clock,
// through composite (8x, line 1134) and back: is the grid found, and how many
// pixels come back as the right palette colour after sampling + snapping?
// `run` pixels share a colour (2 = multicolour mode, 1 = hires).
int gridTest(int run, bool withHint, bool learnFromScratch, double minAccuracy, double lumaWeight = 1.0) {
    const auto& c64 = pal::machineProfiles()[1];
    const double dot = 4.0 * pal::kFsc * 4.0 / 9.0;  // the C64 pixel clock, 4/9 of the 17.73 MHz crystal
    // Like C64 character graphics: 8x8 cells, each with its own pair of
    // colours (run 1: hires, a bit per pixel) or four colours on 2-pixel-wide
    // pixels (run 2: multicolour); run 8: a single colour per cell.
    auto mix = [](unsigned v) {
        v ^= v >> 13;
        v *= 0x5bd1e995u;
        v ^= v >> 15;
        return v;
    };
    auto truth = [&](double tActive, int line) {
        long pix = long(std::floor(tActive * dot + 0.37));
        unsigned cell = unsigned(pix / 8) * 2654435761u ^ unsigned(line / 8) * 40503u;
        unsigned bits = mix(unsigned(pix / run) * 97u ^ unsigned(line) * 7919u ^ cell);
        const int colours = run == 1 ? 2 : run == 2 ? 4 : 1;
        return int(mix(cell + unsigned(bits % unsigned(colours)) * 1013u) % 16);
    };
    auto image = [&](double xFrac, int line) {
        const pal::Colour& c = c64.palette[size_t(truth(xFrac * 52e-6, line))];
        return pal::Rgb{c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
    };
    pal::GenParams p;
    p.samplesPerCycle = 8;
    p.samplesPerLine = 2268;
    p.lineRate = 15639.0;
    p.windowStart = 180;
    p.width = 2048;
    p.lines = 298;
    p.liftCode = 60;
    p.codesPerMv = 0.25;
    auto raw = pal::generate(p, image);
    pal::DecodeParams dp;
    dp.samplesPerCycle = 8;
    dp.burstLen = 64;
    dp.activeStart = 192;
    dp.activeLen = 1843;
    dp.samplesPerLine = 2268;
    dp.luma = pal::LumaFilter::Notch;
    dp.averageLines = true;
    pal::DecodeInfo info;
    auto rgb = pal::decode(raw.data(), p.width, p.lines, dp, &info);
    const int w = dp.activeLen, h = p.lines;
    const double hint = c64.samplesPerPixelAt4x * 2;
    pal::GridEstimate g = withHint ? pal::estimateGrid(rgb.data(), w, h, hint / 1.03, hint * 1.03)
                                   : pal::estimateGrid(rgb.data(), w, h, 1.5 * 2, 6.0 * 2);
    std::vector<uint8_t> px;
    int pw = 0;
    pal::sampleGrid(rgb.data(), w, h, g, 0.5, px, pw);
    const size_t n = size_t(pw) * h;
    auto palette = pal::learnPalette(px.data(), n, 16, learnFromScratch ? nullptr : &c64.palette);
    std::vector<uint8_t> idx;
    pal::snapPalette(px.data(), n, palette, &idx, lumaWeight);
    // Map each learned colour to the true colour it most often stands for.
    const double fs = 2268.0 * 15639.0;
    std::vector<std::array<int, 16>> votes(palette.size());
    for (auto& v : votes) v.fill(0);
    std::vector<int> truthOf(n);
    const int k0 = int(std::ceil(-g.phase / g.period));
    for (int y = 0; y < h; ++y)
        for (int k = 0; k < pw; ++k) {
            double c = g.phase + (double(k0 + k) + 0.5) * g.period;  // centre, decoded-sample coordinates
            double t = (180.0 + 192.0 + c) / fs - 10.5e-6;
            int tr = truth(t, y);
            truthOf[size_t(y) * pw + k] = tr;
            votes[idx[size_t(y) * pw + k]][size_t(tr)]++;
        }
    std::vector<int> map(palette.size());
    for (size_t j = 0; j < palette.size(); ++j)
        map[j] = int(std::max_element(votes[j].begin(), votes[j].end()) - votes[j].begin());
    size_t right = 0, inner = 0;
    for (int y = 0; y < h; ++y)
        for (int k = 2; k + 2 < pw; ++k, ++inner)  // skip the picture's edge pixels
            right += map[idx[size_t(y) * pw + k]] == truthOf[size_t(y) * pw + k];
    const double acc = inner ? 100.0 * double(right) / double(inner) : 0;
    const bool ok = g.valid && std::abs(g.period - hint) < 0.02 && acc >= minAccuracy;
    std::printf("grid: cells, %d-pixel runs, luma weight %.1f, %s, palette %s: period %.4f (true %.4f), score %.2f, %d pixels/line, "
                "right colour %.1f%%  %s\n",
                run, lumaWeight, withHint ? "C64 hint" : "no hint", learnFromScratch ? "learned from scratch" : "from the profile + learned",
                g.period, hint, g.score, pw, acc, minAccuracy > 0 ? (ok ? "PASS" : "FAIL") : "info");
    return minAccuracy > 0 && !ok ? 1 : 0;
}

// A machine palette end to end: a picture in that palette (8x8 cells, one
// colour per cell and `run`-pixel detail), decoded with a deliberate colour
// error, then grid + calibration + snapping to the same palette. The pixels
// are compared with the true palette entries directly (no majority mapping).
int machinePaletteTest(int profile, int run, double satErr, double hueErrDeg, double minAccuracy) {
    const auto& prof = pal::machineProfiles()[size_t(profile)];
    const auto& palette = prof.palette;
    const int nCol = int(palette.size());
    const double dot = 4.0 * pal::kFsc * 4.0 / 9.0;  // C64 geometry and pixel clock for all
    auto mix = [](unsigned v) {
        v ^= v >> 13;
        v *= 0x5bd1e995u;
        v ^= v >> 15;
        return v;
    };
    auto truth = [&](double tActive, int line) {
        long pix = long(std::floor(tActive * dot + 0.37));
        unsigned cell = unsigned(pix / 8) * 2654435761u ^ unsigned(line / 8) * 40503u;
        unsigned bits = mix(unsigned(pix / run) * 97u ^ unsigned(line) * 7919u ^ cell);
        const int colours = run >= 8 ? 1 : 2;
        return int(mix(cell + unsigned(bits % unsigned(colours)) * 1013u) % unsigned(nCol));
    };
    auto image = [&](double xFrac, int line) {
        const pal::Colour& c = palette[size_t(truth(xFrac * 52e-6, line))];
        return pal::Rgb{c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
    };
    pal::GenParams p;
    p.samplesPerCycle = 8, p.samplesPerLine = 2268, p.lineRate = 15639.0, p.windowStart = 180, p.width = 2048, p.lines = 298;
    p.liftCode = 60;
    auto raw = pal::generate(p, image);
    pal::DecodeParams dp;
    dp.samplesPerCycle = 8, dp.burstLen = 64, dp.activeStart = 192, dp.activeLen = 1843, dp.samplesPerLine = 2268;
    dp.luma = pal::LumaFilter::Notch;
    dp.averageLines = true;
    dp.saturation = satErr;  // the deliberate error the calibration must take out
    dp.hueDeg = hueErrDeg;
    auto rgb = pal::decode(raw.data(), p.width, p.lines, dp, nullptr);
    pal::GridStage stage;
    pal::GridStage::Settings gs;
    gs.periodLo = 4.5 / 1.03, gs.periodHi = 4.5 * 1.03;
    gs.snap = true;
    gs.colours = nCol;
    gs.fixedPalette = palette;
    gs.poolEvery = 1, gs.relearnEvery = 1, gs.learnFrames = 3;
    stage.configure(gs);
    std::vector<uint8_t> px;
    int pw = 0;
    for (int i = 0; i < 3; ++i) stage.process(rgb.data(), dp.activeLen, p.lines, px, pw);  // pool + calibrate
    const pal::GridEstimate g = stage.grid();
    const pal::Calibration cal = stage.calibration();
    // Output: only palette colours? And how many are the right one?
    size_t right = 0, inner = 0, offPalette = 0;
    const int k0 = int(std::ceil(-g.phase / g.period));
    const double fs = 2268.0 * 15639.0;
    for (int y = 0; y < p.lines; ++y)
        for (int k = 0; k < pw; ++k) {
            const uint8_t* o = &px[(size_t(y) * pw + k) * 3];
            bool onPal = false;
            for (const auto& c : palette) onPal |= c[0] == o[0] && c[1] == o[1] && c[2] == o[2];
            offPalette += !onPal;
            if (k < 2 || k + 2 >= pw) continue;
            double c = g.phase + (double(k0 + k) + 0.5) * g.period;
            const pal::Colour& t = palette[size_t(truth((180.0 + 192.0 + c) / fs - 10.5e-6, y))];
            right += t[0] == o[0] && t[1] == o[1] && t[2] == o[2];
            ++inner;
        }
    const double acc = inner ? 100.0 * double(right) / double(inner) : 0;
    const bool ok = offPalette == 0 && acc >= minAccuracy;
    std::printf("palette %-40s %d-px detail, error sat %.2f hue %+.0f: calibration sat x%.2f hue %+.1f deg, off-palette %zu, "
                "right %.1f%%  %s\n",
                prof.name, run, satErr, hueErrDeg, cal.satGain, cal.hueRad * 180 / pal::kPi, offPalette, acc,
                minAccuracy > 0 ? (ok ? "PASS" : "FAIL") : "info");
    return minAccuracy > 0 && !ok ? 1 : 0;
}

// C64 cell reconstruction against per-pixel snapping, on C64-style cells:
// run 1 = hires (2 colours per cell), run 2 = multicolour (4 colours,
// double-wide pixels), both with the C64 palette and a deliberate colour error.
int cellTest(int run, double minAccuracy, int candidates = 6, double lumaWeight = 2.0) {
    const auto& c64 = pal::machineProfiles()[1];
    const double dot = 4.0 * pal::kFsc * 4.0 / 9.0;
    auto mix = [](unsigned v) {
        v ^= v >> 13;
        v *= 0x5bd1e995u;
        v ^= v >> 15;
        return v;
    };
    auto truth = [&](double tActive, int line) {
        long pix = long(std::floor(tActive * dot + 0.37));
        unsigned cell = unsigned(pix / 8) * 2654435761u ^ unsigned(line / 8) * 40503u;
        unsigned bits = mix(unsigned(pix / run) * 97u ^ unsigned(line) * 7919u ^ cell);
        const int colours = run == 1 ? 2 : 4;
        return int(mix(cell + unsigned(bits % unsigned(colours)) * 1013u) % 16u);
    };
    auto image = [&](double xFrac, int line) {
        const pal::Colour& c = c64.palette[size_t(truth(xFrac * 52e-6, line))];
        return pal::Rgb{c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
    };
    pal::GenParams p;
    p.samplesPerCycle = 8, p.samplesPerLine = 2268, p.lineRate = 15639.0, p.windowStart = 180, p.width = 2048, p.lines = 298;
    p.liftCode = 60;
    auto raw = pal::generate(p, image);
    pal::DecodeParams dp;
    dp.samplesPerCycle = 8, dp.burstLen = 64, dp.activeStart = 192, dp.activeLen = 1843, dp.samplesPerLine = 2268;
    dp.luma = pal::LumaFilter::Notch;
    dp.averageLines = true;
    dp.saturation = 0.8, dp.hueDeg = 8;
    auto rgb = pal::decode(raw.data(), p.width, p.lines, dp, nullptr);
    pal::GridEstimate g = pal::estimateGrid(rgb.data(), dp.activeLen, p.lines, 4.5 / 1.03, 4.5 * 1.03);
    std::vector<uint8_t> sampled;
    int pw = 0;
    pal::sampleGrid(rgb.data(), dp.activeLen, p.lines, g, 0.5, sampled, pw);
    const size_t n = size_t(pw) * p.lines;
    // Calibrate on settled pixels (as GridStage does), snap per pixel, then per cell.
    std::vector<uint8_t> settled;
    for (int y = 0; y < p.lines; ++y)
        for (int x = 1; x + 1 < pw; ++x) {
            const uint8_t* c = &sampled[(size_t(y) * pw + x) * 3];
            bool ok = true;
            for (int ch = 0; ch < 3; ++ch) ok &= std::abs(c[ch] - c[ch - 3]) < 12 && std::abs(c[ch] - c[ch + 3]) < 12;
            if (ok) settled.insert(settled.end(), c, c + 3);
        }
    pal::Calibration cal = pal::fitCalibration(settled.data(), settled.size() / 3, c64.palette);
    std::vector<uint8_t> perPixel = sampled, cells(sampled.size());
    std::vector<uint8_t> idx;
    pal::snapCalibrated(perPixel.data(), n, c64.palette, cal, &idx);
    int ax = 0, ay = 0;
    pal::reconstructC64Cells(sampled.data(), idx, pw, p.lines, c64.palette, cal, cells.data(), &ax, &ay, candidates, lumaWeight);
    const int k0 = int(std::ceil(-g.phase / g.period));
    const double fs = 2268.0 * 15639.0;
    size_t rp = 0, rc = 0, inner = 0;
    for (int y = 0; y < p.lines; ++y)
        for (int k = 2; k + 2 < pw; ++k, ++inner) {
            double c = g.phase + (double(k0 + k) + 0.5) * g.period;
            const pal::Colour& t = c64.palette[size_t(truth((180.0 + 192.0 + c) / fs - 10.5e-6, y))];
            const uint8_t* a = &perPixel[(size_t(y) * pw + k) * 3];
            const uint8_t* b = &cells[(size_t(y) * pw + k) * 3];
            rp += a[0] == t[0] && a[1] == t[1] && a[2] == t[2];
            rc += b[0] == t[0] && b[1] == t[1] && b[2] == t[2];
        }
    const double accP = 100.0 * double(rp) / double(inner), accC = 100.0 * double(rc) / double(inner);
    const bool ok = accC >= minAccuracy;
    std::printf("C64 cells, %s, %d candidates, luma weight %.0f: per pixel %.1f%% -> per cell %.1f%% right (alignment x%d y%d)  %s\n",
                run == 1 ? "hires" : "multicolour", candidates, lumaWeight, accP, accC, ax, ay, minAccuracy > 0 ? (ok ? "PASS" : "FAIL") : "info");
    return minAccuracy > 0 && !ok ? 1 : 0;
}

// S-Video (Y/C): brightness and colour on two planes. Bars must be as right
// as composite, the brightness step as sharp as the raw samples (no filter
// needed) and flat colour free of carrier ripple.
int ycTests() {
    int failures = 0;
    struct YcCase {
        const char* name;
        int K, spl, windowStart, width;
        double lineRate, chromaLift, noise, phaseErr;
        bool average;
        double maxErr, maxHanover;
    };
    const YcCase cases[] = {
        {"Y/C 4xFsc, standard PAL line", 4, 1135, 90, 1024, pal::kLineRate, 128, 0, 0, false, 4, 1},
        {"Y/C 8xFsc, C64 line (2268 at 15,639 Hz)", 8, 2268, 180, 2048, 15639.0, 128, 0, 0, false, 4, 1},
        // The brightness wire is taken unfiltered, so its noise shows in full:
        // sigma 3 codes = ~4.4 RGB codes, i.e. ~3.9 between lines by itself.
        {"Y/C 8xFsc, C64, noise sigma 3, averaging", 8, 2268, 180, 2048, 15639.0, 128, 3, 0, true, 5, 5},
        {"Y/C 8xFsc, C64, colour zero at code 90", 8, 2268, 180, 2048, 15639.0, 90, 0, 0, false, 4, 1},
        {"Y/C 8xFsc, C64, phase error 20 deg, averaging", 8, 2268, 180, 2048, 15639.0, 128, 0, 20, true, 12, 1},
    };
    for (const YcCase& c : cases) {
        pal::GenParams g;
        g.samplesPerCycle = c.K, g.samplesPerLine = c.spl, g.windowStart = c.windowStart, g.width = c.width;
        g.lineRate = c.lineRate, g.noiseCodes = c.noise, g.chromaPhaseErrDeg = c.phaseErr;
        const double fs = g.samplesPerLine * g.lineRate;
        pal::DecodeParams dp;
        dp.samplesPerCycle = c.K;
        dp.samplesPerLine = c.spl;
        dp.burstLen = 8 * c.K;
        dp.averageLines = c.average;
        dp.activeStart = int(std::lround(10.5e-6 * fs)) - g.windowStart;
        dp.activeLen = int(std::lround(52e-6 * fs));
        auto planes = pal::generateYC(g, pal::colourBars, c.chromaLift);
        dp.chromaPlane = planes.c.data();
        pal::DecodeInfo info;
        auto rgb = pal::decode(planes.y.data(), g.width, g.lines, dp, &info);
        double leak = 0;
        std::string detail;
        const double err = barError(rgb, dp.activeLen, g.lines, &leak, detail);
        const double hb = hanover(rgb, dp.activeLen, g.lines);
        // Carrier ripple on flat colour, one middle line.
        double ripple = 0;
        const int y = g.lines / 2, w = dp.activeLen;
        for (int b = 0; b < 8; ++b) {
            int x0 = int((b + 0.3) * w / 8), x1 = int((b + 0.7) * w / 8);
            for (int ch = 0; ch < 3; ++ch) {
                double mn = 255, mx = 0;
                for (int x = x0; x < x1; ++x) {
                    double v = rgb[(size_t(y) * w + x) * 3 + ch];
                    mn = std::min(mn, v), mx = std::max(mx, v);
                }
                ripple = std::max(ripple, mx - mn);
            }
        }
        const double rippleLimit = c.noise > 0 ? 30 : 3;
        const bool ok = info.colour && err <= c.maxErr && hb <= c.maxHanover && ripple <= rippleLimit;
        if (!ok) ++failures;
        std::printf("%-58s %s  worst bar error %5.1f  line-to-line %4.1f  ripple %4.1f  burst@%d amp %.1f step %.0f deg\n",
                    c.name, ok ? "PASS" : "FAIL", err, hb, ripple, info.burstStart, info.burstAmplitude, info.phaseStepDeg);
        std::printf("    bars:%s\n", detail.c_str());
    }
    {
        // Sharpness: a black/white step on the brightness wire, decoded,
        // against composite with the notch filter at the same 8x rate.
        pal::GenParams g;
        g.samplesPerCycle = 8, g.samplesPerLine = 2268, g.windowStart = 180, g.width = 2048, g.lineRate = 15639.0;
        const double fs = g.samplesPerLine * g.lineRate;
        pal::DecodeParams dp;
        dp.samplesPerCycle = 8, dp.samplesPerLine = 2268, dp.burstLen = 64;
        dp.activeStart = int(std::lround(10.5e-6 * fs)) - g.windowStart;
        dp.activeLen = int(std::lround(52e-6 * fs));
        auto rise = [&](const std::vector<uint8_t>& rgb) {
            const int y = g.lines / 2, w = dp.activeLen;
            auto gv = [&](int x) { return double(rgb[(size_t(y) * w + x) * 3 + 1]); };
            const double lo = gv(w / 4), hi = gv(3 * w / 4);
            int x10 = 0, x90 = 0;
            for (int x = w / 4; x < 3 * w / 4; ++x) {
                if (!x10 && gv(x) >= lo + 0.1 * (hi - lo)) x10 = x;
                if (!x90 && gv(x) >= lo + 0.9 * (hi - lo)) x90 = x;
            }
            return x90 - x10;
        };
        auto planes = pal::generateYC(g, lumaStep, 128);
        dp.chromaPlane = planes.c.data();
        const int ycRise = rise(pal::decode(planes.y.data(), g.width, g.lines, dp, nullptr));
        dp.chromaPlane = nullptr;
        dp.luma = pal::LumaFilter::Notch;
        auto comp = pal::generate(g, lumaStep);
        const int compRise = rise(pal::decode(comp.data(), g.width, g.lines, dp, nullptr));
        const bool ok = ycRise <= 1 && ycRise <= compRise;
        if (!ok) ++failures;
        std::printf("Y/C sharpness at 8x %s  step rise 10-90%%: Y/C %d samples, composite (notch) %d samples\n",
                    ok ? "PASS" : "FAIL", ycRise, compRise);
    }
    return failures;
}

// BBC Micro Model B (prepared from documentation, no BBC tested): 64 us
// lines (the standard PAL 283.75 carrier cycles, 2270 samples at 8x), its 8
// digital colours at full level. Composite and S-Video must decode each
// colour to the nearest BBC palette entry; the unmodified composite output
// (no link S39: no burst, no colour) must decode as black and white.
int bbcTests() {
    int failures = 0;
    const auto& prof = pal::machineProfiles();
    const pal::MachineProfile* bbc = nullptr;
    for (const auto& m : prof)
        if (std::string(m.name).find("BBC Micro") != std::string::npos) bbc = &m;
    if (!bbc || bbc->palette.size() != 8) {
        std::printf("BBC profile missing  FAIL\n");
        return 1;
    }
    // Bars in the BBC's colour-number order: black red green yellow blue magenta cyan white.
    auto bars = [&](double xFrac, int) {
        const pal::Colour& c = bbc->palette[size_t(std::min(7, std::max(0, int(xFrac * 8))))];
        return pal::Rgb{c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
    };
    auto check = [&](const char* name, const std::vector<uint8_t>& rgb, int w, int h, const pal::DecodeInfo& info,
                     bool wantColour) {
        int right = 0;
        double worst = 0, spread = 0;
        std::string detail;
        for (int b = 0; b < 8; ++b) {
            double m[3] = {0, 0, 0};
            int n = 0;
            for (int y = h / 4; y < 3 * h / 4; ++y)
                for (int x = int((b + 0.3) * w / 8); x < int((b + 0.7) * w / 8); ++x, ++n)
                    for (int ch = 0; ch < 3; ++ch) m[ch] += rgb[(size_t(y) * w + x) * 3 + ch];
            for (double& v : m) v /= n;
            spread = std::max(spread, std::max({m[0], m[1], m[2]}) - std::min({m[0], m[1], m[2]}));
            int best = 0;
            double bestD = 1e9;
            for (int k = 0; k < 8; ++k) {
                double d = 0;
                for (int ch = 0; ch < 3; ++ch) d += (m[ch] - bbc->palette[size_t(k)][size_t(ch)]) * (m[ch] - bbc->palette[size_t(k)][size_t(ch)]);
                if (d < bestD) bestD = d, best = k;
            }
            right += best == b;
            if (best == b) worst = std::max(worst, std::sqrt(bestD));
            char s[64];
            std::snprintf(s, sizeof s, " %d:(%.0f,%.0f,%.0f)", b, m[0], m[1], m[2]);
            detail += s;
        }
        const bool ok = wantColour ? info.colour && right == 8 : !info.colour && spread < 3;
        if (!ok) ++failures;
        if (wantColour)
            std::printf("%-58s %s  %d/8 bars nearest to their BBC colour, worst distance %.0f, carrier step %.0f deg\n", name,
                        ok ? "PASS" : "FAIL", right, worst, info.phaseStepDeg);
        else
            std::printf("%-58s %s  colour %s, largest R/G/B spread in a bar %.1f\n", name, ok ? "PASS" : "FAIL",
                        info.colour ? "on" : "off", spread);
        std::printf("    bars:%s\n", detail.c_str());
    };
    pal::GenParams g;
    g.samplesPerCycle = 8, g.samplesPerLine = 2270, g.windowStart = 180, g.width = 2048, g.lineRate = pal::kLineRate;
    g.codesPerMv = 0.18, g.liftCode = 60;  // full-level colours peak above white: keep them inside the ADC range
    const double fs = g.samplesPerLine * g.lineRate;
    pal::DecodeParams dp;
    dp.samplesPerCycle = 8, dp.samplesPerLine = 2270, dp.burstLen = 64;
    dp.activeStart = int(std::lround(10.5e-6 * fs)) - g.windowStart;
    dp.activeLen = int(std::lround(52e-6 * fs));
    dp.averageLines = true;
    dp.luma = pal::LumaFilter::Notch;
    {
        pal::DecodeInfo info;
        auto rgb = pal::decode(pal::generate(g, bars).data(), g.width, g.lines, dp, &info);
        check("BBC composite 8x, colour (link S39 fitted)", rgb, dp.activeLen, g.lines, info, true);
    }
    {
        pal::GenParams m = g;
        m.chroma = false;
        pal::DecodeInfo info;
        auto rgb = pal::decode(pal::generate(m, bars).data(), m.width, m.lines, dp, &info);
        check("BBC composite 8x, as delivered (monochrome, no S39)", rgb, dp.activeLen, g.lines, info, false);
    }
    {
        auto planes = pal::generateYC(g, bars, 128);
        pal::DecodeParams d = dp;
        d.chromaPlane = planes.c.data();
        pal::DecodeInfo info;
        auto rgb = pal::decode(planes.y.data(), g.width, g.lines, d, &info);
        check("BBC S-Video 8x (board mod or converter)", rgb, d.activeLen, g.lines, info, true);
    }
    return failures;
}

// Fixed clock stripes: a pattern locked to the line (C64 CPU
// clock: 36 samples at 8x, odd harmonics ~0.8 codes) plus noise, added to
// Y/C and composite frames. LinePattern learns it from flat parts; after
// removal the stripes on flat colour must be mostly gone, while a static
// fine detail with the same period and a sharp edge stay as they were.
int stripeTests() {
    int failures = 0;
    const int period = 36;
    auto stripe = [&](int x) {
        double v = 0;
        const double amp[4] = {0.6, 0.8, 0.8, 0.7}, ph[4] = {-2.2, -1.0, 0.3, 1.5};
        for (int h = 0; h < 4; ++h) v += amp[h] * std::cos(2 * pal::kPi * (2 * h + 1) * x / period + ph[h]);
        return v;
    };
    for (int yc = 1; yc >= 0; --yc) {
        pal::GenParams g;
        g.samplesPerCycle = 8, g.samplesPerLine = 2268, g.windowStart = 180, g.width = 2048, g.lineRate = 15639.0;
        g.noiseCodes = 1.0;
        const double fs = g.samplesPerLine * g.lineRate;
        pal::DecodeParams dp;
        dp.samplesPerCycle = 8, dp.samplesPerLine = 2268, dp.burstLen = 64;
        dp.activeStart = int(std::lround(10.5e-6 * fs)) - g.windowStart;
        dp.activeLen = int(std::lround(52e-6 * fs));
        dp.luma = pal::LumaFilter::Notch;
        const double pFrac = double(period) / dp.activeLen;  // the detail's period as a fraction of the active line
        auto image = [&](double xf, int srcLine) {
            const int line = srcLine % 312;  // the generator counts source lines across frames
            if (line < 200) return pal::colourBars(xf);
            if (line < 240) {  // a static fine detail with exactly the stripes' period
                const bool on = std::fmod(xf / pFrac, 1.0) < 0.5;
                return on ? pal::Rgb{0.7, 0.7, 0.7} : pal::Rgb{0.2, 0.2, 0.2};
            }
            return xf < 0.5 ? pal::Rgb{0.1, 0.1, 0.1} : pal::Rgb{0.8, 0.8, 0.8};  // a sharp edge
        };
        pal::LinePattern lp;
        std::vector<uint8_t> y, c;
        for (int f = 0; f < 30; ++f) {
            g.seed = unsigned(f + 1), g.firstLine = f * 312;
            if (yc) {
                auto planes = pal::generateYC(g, image, 128);
                y = planes.y, c = planes.c;
            } else {
                y = pal::generate(g, image);
            }
            for (int l = 0; l < g.lines; ++l)
                for (int x = 0; x < g.width; ++x) {
                    uint8_t& v = y[size_t(l) * g.width + x];
                    v = uint8_t(std::clamp(std::lround(v + stripe(x)), 0L, 255L));
                }
            lp.learn(y.data(), g.width, g.lines, dp.activeStart, dp.activeStart + dp.activeLen);
        }
        if (yc) dp.chromaPlane = c.data();
        auto rgbOff = pal::decode(y.data(), g.width, g.lines, dp, nullptr);
        dp.linePattern = lp.data();
        auto rgbOn = pal::decode(y.data(), g.width, g.lines, dp, nullptr);
        const int w = dp.activeLen;
        // Coherent amplitude at the stripes' harmonics over the rows [l0, l1), inside [x0, x1) (green).
        auto stripesIn = [&](const std::vector<uint8_t>& rgb, int l0, int l1, int x0, int x1) {
            double total = 0;
            for (int h = 1; h <= 7; h += 2) {
                std::complex<double> acc = 0;
                int n = 0;
                for (int l = l0; l < l1; ++l) {
                    double m = 0;
                    for (int x = x0; x < x1; ++x) m += rgb[(size_t(l) * w + x) * 3 + 1];
                    m /= (x1 - x0);
                    for (int x = x0; x < x1; ++x, ++n)
                        acc += (rgb[(size_t(l) * w + x) * 3 + 1] - m) * std::polar(1.0, -2 * pal::kPi * h * (x + dp.activeStart) / period);
                }
                total += std::norm(acc * (2.0 / n));
            }
            return std::sqrt(total);
        };
        double flatOff = 0, flatOn = 0;
        for (int b : {0, 7}) {  // the grey bars (white and black carry no colour; black sits above blanking here)
            const int x0 = int((b + 0.2) * w / 8), x1 = x0 + 4 * period;
            flatOff += stripesIn(rgbOff, 20, 190, x0, x1), flatOn += stripesIn(rgbOn, 20, 190, x0, x1);
        }
        const double detOff = stripesIn(rgbOff, 205, 235, w / 4, w / 4 + 16 * period);
        const double detOn = stripesIn(rgbOn, 205, 235, w / 4, w / 4 + 16 * period);
        auto rise = [&](const std::vector<uint8_t>& rgb) {
            double sum = 0;
            for (int l = 250; l < 280; ++l) {
                auto gv = [&](int x) { return double(rgb[(size_t(l) * w + x) * 3 + 1]); };
                const double lo = gv(w / 4), hi = gv(3 * w / 4);
                int x10 = 0, x90 = 0;
                for (int x = w / 4; x < 3 * w / 4; ++x) {
                    if (!x10 && gv(x) >= lo + 0.1 * (hi - lo)) x10 = x;
                    if (!x90 && gv(x) >= lo + 0.9 * (hi - lo)) x90 = x;
                }
                sum += x90 - x10;
            }
            return sum / 30;
        };
        const double rOff = rise(rgbOff), rOn = rise(rgbOn);
        const bool ok = lp.ready() && flatOn < 0.3 * flatOff && std::abs(detOn - detOff) < 0.02 * detOff && rOn <= rOff + 0.25;
        // (The edge may measure a little sharper: the stripes rippled across it.)
        if (!ok) ++failures;
        std::printf("%-58s %s  stripes on flat grey %.2f -> %.2f, static detail %.1f -> %.1f, edge rise %.1f -> %.1f samples "
                    "(pattern %.2f codes rms, %.0f%% known)\n",
                    yc ? "Clock stripes, Y/C 8x C64, noise 1" : "Clock stripes, composite 8x C64 (notch), noise 1",
                    ok ? "PASS" : "FAIL", flatOff, flatOn, detOff, detOn, rOff, rOn, lp.rms(), lp.coverage() * 100);
    }
    return failures;
}

// NTSC: the same bars through the NTSC
// generator and decoder, for the standard/C64 NTSC line (227.5 carrier cycles,
// the phase flips every line) and the Apple II / MSX1 line (228 cycles, the
// same phase on every line), composite and Y/C, with noise, a hue error and
// its correction, and 7.5 IRE setup.
int ntscTests() {
    int failures = 0;
    const pal::StandardInfo& si = pal::standardInfo(pal::Standard::Ntsc);
    struct NtscCase {
        const char* name;
        int K;
        double cycles;  // carrier cycles per line: 227.5 or 228
        bool yc;
        double noise, phaseErr, hue, setup;
        bool average;
        double maxErr, wantStep;
    };
    const NtscCase cases[] = {
        {"NTSC 4x, 227.5 cycles/line (standard, C64 NTSC)", 4, 227.5, false, 0, 0, 0, 0, false, 5, 180},
        {"NTSC 8x, 227.5 cycles/line (C64 NTSC 1820)", 8, 227.5, false, 0, 0, 0, 0, false, 5, 180},
        {"NTSC 4x, 228 cycles/line (Apple II, MSX1)", 4, 228, false, 0, 0, 0, 0, false, 5, 0},
        {"NTSC 8x, 228 cycles/line (Apple II, MSX1 1824)", 8, 228, false, 0, 0, 0, 0, false, 5, 0},
        {"NTSC 8x, 227.5, noise sigma 3, averaging", 8, 227.5, false, 3, 0, 0, 0, true, 6, 180},
        {"NTSC 8x, 227.5, hue error +20 deg, corrected by Hue -20", 8, 227.5, false, 0, 20, -20, 0, false, 5, 180},
        {"NTSC 8x, 228, 7.5 IRE setup taken off", 8, 228, false, 0, 0, 0, 7.5, false, 5, 0},
        {"NTSC Y/C 8x, 227.5 (S-Video)", 8, 227.5, true, 0, 0, 0, 0, false, 4, 180},
        {"NTSC Y/C 8x, 228 (S-Video)", 8, 228, true, 0, 0, 0, 0, false, 4, 0},
    };
    for (const NtscCase& c : cases) {
        pal::GenParams g;
        g.standard = pal::Standard::Ntsc;
        g.samplesPerCycle = c.K;
        g.samplesPerLine = int(std::lround(c.cycles * c.K));
        g.lineRate = si.fsc / c.cycles;
        g.windowStart = 10 * c.K;                       // before the burst
        g.width = (g.samplesPerLine - g.windowStart - 2) / 16 * 16;  // inside one line
        g.lines = 240;
        g.noiseCodes = c.noise, g.chromaPhaseErrDeg = c.phaseErr, g.setupIre = c.setup;
        const double fs = g.samplesPerLine * g.lineRate;
        pal::DecodeParams dp;
        dp.standard = pal::Standard::Ntsc;
        dp.samplesPerCycle = c.K, dp.samplesPerLine = g.samplesPerLine, dp.burstLen = 8 * c.K;
        dp.activeStart = int(std::lround(si.activeStart * fs)) - g.windowStart;
        dp.activeLen = std::min(int(std::lround(si.activeLen * fs)), g.width - dp.activeStart);
        dp.averageLines = c.average;
        dp.luma = pal::LumaFilter::Notch;
        dp.hueDeg = c.hue;
        dp.setupIre = c.setup;
        pal::DecodeInfo info;
        std::vector<uint8_t> rgb;
        if (c.yc) {
            auto planes = pal::generateYC(g, pal::colourBars, 128);
            dp.chromaPlane = planes.c.data();
            rgb = pal::decode(planes.y.data(), g.width, g.lines, dp, &info);
        } else {
            rgb = pal::decode(pal::generate(g, pal::colourBars).data(), g.width, g.lines, dp, &info);
        }
        std::string detail;
        const double err = barError(rgb, dp.activeLen, g.lines, nullptr, detail);
        const double stepOff = std::abs(std::remainder(info.phaseStepDeg - c.wantStep, 360.0));
        const bool ok = info.colour && err <= c.maxErr && stepOff < 5;
        if (!ok) ++failures;
        std::printf("%-58s %s  worst bar error %5.1f  carrier step %6.1f deg (expected %.0f)  burst %.1f codes\n", c.name,
                    ok ? "PASS" : "FAIL", err, info.phaseStepDeg, c.wantStep, info.burstAmplitude);
        std::printf("    bars:%s\n", detail.c_str());
    }
    {
        // The hue error uncorrected: it must show (else the case above proves nothing).
        pal::GenParams g;
        g.standard = pal::Standard::Ntsc;
        g.samplesPerCycle = 8, g.samplesPerLine = 1820, g.lineRate = si.fsc / 227.5, g.windowStart = 80;
        g.width = 1728, g.lines = 240, g.chromaPhaseErrDeg = 20;
        const double fs = g.samplesPerLine * g.lineRate;
        pal::DecodeParams dp;
        dp.standard = pal::Standard::Ntsc;
        dp.samplesPerCycle = 8, dp.samplesPerLine = 1820, dp.burstLen = 64;
        dp.activeStart = int(std::lround(si.activeStart * fs)) - g.windowStart;
        dp.activeLen = std::min(int(std::lround(si.activeLen * fs)), g.width - dp.activeStart);
        dp.luma = pal::LumaFilter::Notch;
        std::string detail;
        const double err = barError(pal::decode(pal::generate(g, pal::colourBars).data(), g.width, g.lines, dp, nullptr),
                                    dp.activeLen, g.lines, nullptr, detail);
        const bool ok = err > 15;  // NTSC has no V-switch to cancel it
        if (!ok) ++failures;
        std::printf("%-58s %s  worst bar error %5.1f (a visible hue shift)\n", "NTSC 8x, hue error 20 deg, not corrected",
                    ok ? "PASS" : "FAIL", err);
    }
    {
        // Clock stripes at the C64 NTSC period: 1820 samples / 65 cycles = 28 at 8x.
        const int period = 28;
        pal::GenParams g;
        g.standard = pal::Standard::Ntsc;
        g.samplesPerCycle = 8, g.samplesPerLine = 1820, g.lineRate = si.fsc / 227.5, g.windowStart = 80;
        g.width = 1728, g.lines = 240, g.noiseCodes = 1.0;
        const double fs = g.samplesPerLine * g.lineRate;
        pal::DecodeParams dp;
        dp.standard = pal::Standard::Ntsc;
        dp.samplesPerCycle = 8, dp.samplesPerLine = 1820, dp.burstLen = 64;
        dp.activeStart = int(std::lround(si.activeStart * fs)) - g.windowStart;
        dp.activeLen = std::min(int(std::lround(si.activeLen * fs)), g.width - dp.activeStart);
        pal::LinePattern lp;
        std::vector<uint8_t> y, c;
        for (int f = 0; f < 20; ++f) {
            g.seed = unsigned(f + 1), g.firstLine = f * 263;
            auto planes = pal::generateYC(g, pal::colourBars, 128);
            y = planes.y, c = planes.c;
            for (int l = 0; l < g.lines; ++l)
                for (int x = 0; x < g.width; ++x) {
                    uint8_t& v = y[size_t(l) * g.width + x];
                    const double s = 0.8 * std::cos(2 * pal::kPi * x / period) + 0.7 * std::cos(2 * pal::kPi * 3 * x / period + 1);
                    v = uint8_t(std::clamp(std::lround(v + s), 0L, 255L));
                }
            lp.learn(y.data(), g.width, g.lines, dp.activeStart, dp.activeStart + dp.activeLen);
        }
        dp.chromaPlane = c.data();
        const int w = dp.activeLen;
        auto stripes = [&](const std::vector<uint8_t>& rgb) {
            std::complex<double> a1 = 0, a3 = 0;
            int n = 0;
            const int x0 = w / 80, x1 = x0 + 4 * period;  // inside the first (grey) bar, clear of its edge
            for (int l = 20; l < 220; ++l) {
                double m = 0;
                for (int x = x0; x < x1; ++x) m += rgb[(size_t(l) * w + x) * 3 + 1];
                m /= (x1 - x0);
                for (int x = x0; x < x1; ++x, ++n) {
                    const double d = rgb[(size_t(l) * w + x) * 3 + 1] - m;
                    a1 += d * std::polar(1.0, -2 * pal::kPi * (x + dp.activeStart) / period);
                    a3 += d * std::polar(1.0, -6 * pal::kPi * (x + dp.activeStart) / period);
                }
            }
            return std::sqrt(std::norm(a1 * (2.0 / n)) + std::norm(a3 * (2.0 / n)));
        };
        const double off = stripes(pal::decode(y.data(), g.width, g.lines, dp, nullptr));
        dp.linePattern = lp.data();
        const double on = stripes(pal::decode(y.data(), g.width, g.lines, dp, nullptr));
        const bool ok = lp.ready() && on < 0.3 * off;
        if (!ok) ++failures;
        std::printf("%-58s %s  stripes on flat grey %.2f -> %.2f codes\n", "NTSC clock stripes, C64 NTSC period 28 at 8x (Y/C)",
                    ok ? "PASS" : "FAIL", off, on);
    }
    {
        // Sharpness: a black/white step, Y/C against composite (notch) at 8x NTSC.
        pal::GenParams g;
        g.standard = pal::Standard::Ntsc;
        g.samplesPerCycle = 8, g.samplesPerLine = 1820, g.lineRate = si.fsc / 227.5, g.windowStart = 80;
        g.width = 1728, g.lines = 240;
        const double fs = g.samplesPerLine * g.lineRate;
        pal::DecodeParams dp;
        dp.standard = pal::Standard::Ntsc;
        dp.samplesPerCycle = 8, dp.samplesPerLine = 1820, dp.burstLen = 64;
        dp.activeStart = int(std::lround(si.activeStart * fs)) - g.windowStart;
        dp.activeLen = std::min(int(std::lround(si.activeLen * fs)), g.width - dp.activeStart);
        auto rise = [&](const std::vector<uint8_t>& rgb) {
            const int y = g.lines / 2, w = dp.activeLen;
            auto gv = [&](int x) { return double(rgb[(size_t(y) * w + x) * 3 + 1]); };
            const double lo = gv(w / 4), hi = gv(3 * w / 4);
            int x10 = 0, x90 = 0;
            for (int x = w / 4; x < 3 * w / 4; ++x) {
                if (!x10 && gv(x) >= lo + 0.1 * (hi - lo)) x10 = x;
                if (!x90 && gv(x) >= lo + 0.9 * (hi - lo)) x90 = x;
            }
            return x90 - x10;
        };
        auto planes = pal::generateYC(g, lumaStep, 128);
        dp.chromaPlane = planes.c.data();
        const int ycRise = rise(pal::decode(planes.y.data(), g.width, g.lines, dp, nullptr));
        dp.chromaPlane = nullptr;
        dp.luma = pal::LumaFilter::Notch;
        const int compRise = rise(pal::decode(pal::generate(g, lumaStep).data(), g.width, g.lines, dp, nullptr));
        const bool ok = ycRise <= 1 && ycRise <= compRise;
        if (!ok) ++failures;
        std::printf("%-58s %s  step rise 10-90%%: Y/C %d samples, composite (notch) %d samples\n", "NTSC sharpness at 8x",
                    ok ? "PASS" : "FAIL", ycRise, compRise);
    }
    {
        // Pixel grid on a C64 NTSC picture: random pixels at 8.18 MHz (3.5 samples
        // at 8x) from profile 8's palette, S-Video, found by the grid search.
        const pal::MachineProfile& prof = pal::machineProfiles()[8];
        pal::GenParams g;
        g.standard = pal::Standard::Ntsc;
        g.samplesPerCycle = 8, g.samplesPerLine = 1820, g.lineRate = si.fsc / 227.5, g.windowStart = 80;
        g.width = 1728, g.lines = 240;
        const double fs = g.samplesPerLine * g.lineRate, dot = si.fsc * 16.0 / 7.0;  // 8.1818 MHz
        auto image = [&](double xf, int line) {
            const long pix = long(std::floor(xf * si.activeLen * dot));
            const unsigned h = unsigned(pix) * 2654435761u ^ unsigned(line % 263 / 8) * 40503u;
            const pal::Colour& c = prof.palette[(h >> 7) % 16];
            return pal::Rgb{c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
        };
        auto planes = pal::generateYC(g, image, 128);
        pal::DecodeParams dp;
        dp.standard = pal::Standard::Ntsc;
        dp.samplesPerCycle = 8, dp.samplesPerLine = 1820, dp.burstLen = 64;
        dp.activeStart = int(std::lround(si.activeStart * fs)) - g.windowStart;
        dp.activeLen = std::min(int(std::lround(si.activeLen * fs)), g.width - dp.activeStart);
        dp.chromaPlane = planes.c.data();
        auto rgb = pal::decode(planes.y.data(), g.width, g.lines, dp, nullptr);
        const double want = prof.samplesPerPixelAt4x * 2;  // at 8x
        pal::GridEstimate ge = pal::estimateGrid(rgb.data(), dp.activeLen, g.lines, want / 1.03, want * 1.03);
        const bool ok = ge.valid && std::abs(ge.period - want) < 0.01;
        if (!ok) ++failures;
        std::printf("%-58s %s  period %.4f samples (expected %.4f)\n", "NTSC pixel grid, C64 NTSC profile (S-Video 8x)",
                    ok ? "PASS" : "FAIL", ge.period, want);
    }
    return failures;
}

// TMS9918 two-colour rule (MSX1): synthetic MSX pictures at 12x
// NTSC composite (pixel = 8 samples), decoded, gridded, snapped per pixel and
// then per 8-pixel segment; accuracy against the known picture. (1) the
// 40-column text mode: white one-pixel strokes on dark blue; (2) Graphics II:
// every 8x1 segment its own random colour pair.
int tmsTest(int mode, double minAccuracy) {
    const auto& prof = pal::machineProfiles()[10];
    const pal::StandardInfo& si = pal::standardInfo(pal::Standard::Ntsc);
    const double dot = si.fsc * 1.5;  // 5.37 MHz
    auto mix = [](unsigned v) { v ^= v >> 13; v *= 0x5bd1e995u; v ^= v >> 15; return v; };
    const double t0 = 1.0e-6;  // the picture starts this far into the active line
    auto truth = [&](double tActive, int line) {
        const long pix = long(std::floor((tActive - t0) * dot));
        if (pix < 0 || pix >= 240) return 3;  // border: dark blue (colour index 3 = code 4)
        if (mode == 1) {
            // text: 6-pixel characters, 8 lines; strokes 1 pixel wide, some horizontal runs
            const unsigned cell = unsigned(pix / 6) * 2654435761u ^ unsigned(line / 8) * 40503u;
            const unsigned b = mix(cell ^ unsigned(line % 8) * 7919u);
            const int col = int(pix % 6);
            const bool on = col < 5 && ((b >> col) & 1) && (line % 8) < 7;
            return on ? 14 : 3;  // white (code 15) on dark blue
        }
        const unsigned seg = unsigned(pix / 8) * 2654435761u ^ unsigned(line) * 40503u;
        const int a = int(mix(seg) % 15u), b = int(mix(seg * 7u + 1u) % 15u);
        return ((mix(unsigned(pix / 2) * 97u ^ unsigned(line) * 7919u) >> 3) & 1) ? a : b;  // runs of 2 pixels
    };
    auto image = [&](double xFrac, int line) {
        const pal::Colour& c = prof.palette[size_t(truth(xFrac * si.activeLen, line % 263))];
        return pal::Rgb{c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
    };
    pal::GenParams p;
    p.standard = pal::Standard::Ntsc;
    p.samplesPerCycle = 12, p.samplesPerLine = 2736, p.lineRate = si.fsc / 228, p.windowStart = 192, p.width = 2496, p.lines = 192;
    const double fs = p.samplesPerLine * p.lineRate;
    auto raw = pal::generate(p, image);
    pal::DecodeParams dp;
    dp.standard = pal::Standard::Ntsc;
    dp.samplesPerCycle = 12, dp.samplesPerLine = 2736, dp.burstLen = 96;
    dp.activeStart = int(std::lround(si.activeStart * fs)) - p.windowStart;
    dp.activeLen = std::min(int(std::lround(si.activeLen * fs)), p.width - dp.activeStart);
    dp.luma = pal::LumaFilter::Notch;
    dp.averageLines = true;
    dp.burstSearchEnd = pal::burstSearchEndFor(pal::Standard::Ntsc, fs, p.windowStart);
    auto rgb = pal::decode(raw.data(), p.width, p.lines, dp, nullptr);
    pal::GridEstimate g = pal::estimateGrid(rgb.data(), dp.activeLen, p.lines, 8.0 / 1.03, 8.0 * 1.03);
    std::vector<uint8_t> sampled;
    int pw = 0;
    pal::sampleGrid(rgb.data(), dp.activeLen, p.lines, g, 0.5, sampled, pw);
    const size_t n = size_t(pw) * p.lines;
    std::vector<uint8_t> settled;
    for (int y = 0; y < p.lines; ++y)
        for (int x = 1; x + 1 < pw; ++x) {
            const uint8_t* c = &sampled[(size_t(y) * pw + x) * 3];
            bool ok = true;
            for (int ch = 0; ch < 3; ++ch) ok &= std::abs(c[ch] - c[ch - 3]) < 12 && std::abs(c[ch] - c[ch + 3]) < 12;
            if (ok) settled.insert(settled.end(), c, c + 3);
        }
    pal::Calibration cal = pal::fitCalibration(settled.data(), settled.size() / 3, prof.palette);
    std::vector<uint8_t> perPixel = sampled, cells(sampled.size());
    std::vector<uint8_t> idx;
    pal::snapCalibrated(perPixel.data(), n, prof.palette, cal, &idx);
    const double prior = std::getenv("TMS_PRIOR") ? std::atof(std::getenv("TMS_PRIOR")) : 20000.0;
    pal::reconstructTmsCells(sampled.data(), idx, pw, p.lines, prof.palette, cal, cells.data(), 4.0, prior);
    const int k0 = int(std::ceil(-g.phase / g.period));
    size_t rp = 0, rc = 0, inner = 0;
    for (int y = 0; y < p.lines; ++y)
        for (int k = 2; k + 2 < pw; ++k) {
            const double c = g.phase + (double(k0 + k) + 0.5) * g.period;
            const double tAct = (double(p.windowStart + dp.activeStart) + c) / fs - si.activeStart;
            const pal::Colour& tc = prof.palette[size_t(truth(tAct, y))];
            const uint8_t* a = &perPixel[(size_t(y) * pw + k) * 3];
            const uint8_t* b = &cells[(size_t(y) * pw + k) * 3];
            rp += a[0] == tc[0] && a[1] == tc[1] && a[2] == tc[2];
            rc += b[0] == tc[0] && b[1] == tc[1] && b[2] == tc[2];
            ++inner;
        }
    if (std::getenv("TMS_CONFUSION")) {
        std::map<std::pair<int, int>, int> conf;
        auto find = [&](const uint8_t* c) {
            for (size_t j = 0; j < prof.palette.size(); ++j)
                if (prof.palette[j][0] == c[0] && prof.palette[j][1] == c[1] && prof.palette[j][2] == c[2]) return int(j);
            return -1;
        };
        for (int y = 0; y < p.lines; ++y)
            for (int k = 2; k + 2 < pw; ++k) {
                const double c = g.phase + (double(k0 + k) + 0.5) * g.period;
                const double tAct = (double(p.windowStart + dp.activeStart) + c) / fs - si.activeStart;
                const int want = truth(tAct, y), got = find(&cells[(size_t(y) * pw + k) * 3]);
                if (want != got) conf[{want, got}]++;
            }
        std::vector<std::pair<int, std::pair<int, int>>> v;
        for (auto& e : conf) v.push_back({e.second, e.first});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < std::min<size_t>(6, v.size()); ++i)
            std::printf("   wrong: wanted colour %d, got %d: %d pixels\n", v[i].second.first, v[i].second.second, v[i].first);
        std::printf("   calibration: contrast %.3f offset %.1f sat x%.2f hue %.1f deg\n", cal.contrast, cal.offset, cal.satGain,
                    cal.hueRad * 180 / pal::kPi);
    }
    if (std::getenv("TMS_SHIFTS"))
        for (double sh : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
            size_t r2 = 0, n2 = 0;
            for (int y = 0; y < p.lines; ++y)
                for (int k = 2; k + 2 < pw; ++k, ++n2) {
                    const double c = g.phase + (double(k0 + k) + 0.5) * g.period;
                    const double tAct = (double(p.windowStart + dp.activeStart) + c) / fs - si.activeStart + sh / dot;
                    const pal::Colour& tc = prof.palette[size_t(truth(tAct, y))];
                    const uint8_t* b = &cells[(size_t(y) * pw + k) * 3];
                    r2 += b[0] == tc[0] && b[1] == tc[1] && b[2] == tc[2];
                }
            std::printf("   shift %+.1f px: per segment %.1f%%\n", sh, 100.0 * double(r2) / double(n2));
        }
    const double accP = 100.0 * double(rp) / double(inner), accC = 100.0 * double(rc) / double(inner);
    const bool ok = g.valid && std::abs(g.period - 8.0) < 0.02 && accC >= minAccuracy && accC >= accP;
    std::printf("TMS9918 two colours per 8 pixels, %-28s %s  grid %.4f, per pixel %.1f%% -> per segment %.1f%% right\n",
                mode == 1 ? "40-column text (12x NTSC):" : "Graphics II pairs, 2-px runs:", ok ? "PASS" : "FAIL", g.period, accP,
                accC);
    return ok ? 0 : 1;
}

// Vertical re-alignment: synthetic whole frames (262
// lines, brightness only) with a vertical-sync band of rows. (1) a locked
// source: the band stays at one row, every frame gets the same offset and the
// output is steady; (2) a rolling source: every frame put back with its band
// first; (3) a dark text screen with the black not lifted (no gap between sync
// and black): no band may be found, frames pass through
// unchanged; (4) a lifted dark screen whose captured window has no band: frames
// pass through unchanged (no rotation by a profile match on near-black rows).
int relockTests() {
    const int w = 320, h = 262, band = 6;
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return int(seed >> 24); };
    // The source picture: band rows first, then black with some text rows.
    auto picture = [&](int syncLevel, int black, bool withBand) {
        std::vector<uint8_t> p(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int v = black;
                if (withBand && y < band) v = (x % 40) < 34 ? syncLevel : black;  // serrations
                else if (y > 40 && y < 220 && (y / 8) % 3 == 0 && (x / 3) % 5 < 2 && (x * 7 + y * 3) % 11 < 5) v = 200;
                p[size_t(y) * w + x] = uint8_t(std::clamp(v + (rnd() % 5) - 2, 0, 255));
            }
        return p;
    };
    auto rolled = [&](const std::vector<uint8_t>& p, int shift) {  // the frame starts at source row `shift`
        std::vector<uint8_t> f(p.size());
        for (int y = 0; y < h; ++y) std::memcpy(&f[size_t(y) * w], &p[size_t((y + shift) % h) * w], size_t(w));
        return f;
    };
    auto rowMean = [&](const std::vector<uint8_t>& f, int y) {
        double s = 0;
        for (int x = 0; x < w; ++x) s += f[size_t(y) * w + x];
        return s / w;
    };
    int failures = 0;
    // (1) locked: the band at row 200 of every delivered frame
    {
        const auto src = picture(10, 70, true);
        pal::VerticalRelock vr;
        std::set<int> offsets;
        bool steady = true;
        std::vector<uint8_t> first;
        for (int i = 0; i < 60; ++i) {
            auto f = rolled(src, h - 200);
            vr.process(f.data(), w, h, 0, w);
            offsets.insert(vr.offset());
            if (first.empty()) first = f;
            else for (int y = 0; y < h; ++y) steady = steady && std::abs(rowMean(f, y) - rowMean(first, y)) < 1.0;
        }
        const bool ok = offsets.size() == 1 && steady && vr.aligned() == 60;
        std::printf("re-align, locked source (band at row 200):              %s  %zu offset(s), %ld aligned, steady %s\n", ok ? "PASS" : "FAIL",
                    offsets.size(), vr.aligned(), steady ? "yes" : "no");
        failures += !ok;
    }
    // (2) rolling: a different start row every frame
    {
        const auto src = picture(10, 70, true);
        pal::VerticalRelock vr;
        int right = 0;
        for (int i = 0; i < 60; ++i) {
            auto f = rolled(src, (i * 97 + 13) % h);
            vr.process(f.data(), w, h, 0, w);
            bool r = true;
            for (int y = 0; y < h; ++y) r = r && std::abs(rowMean(f, y) - rowMean(src, y)) < 1.0;
            right += r;
        }
        const bool ok = right == 60;
        std::printf("re-align, rolling source:                               %s  %d of 60 frames put back exactly\n", ok ? "PASS" : "FAIL", right);
        failures += !ok;
    }
    // (3) dark text screen, black not lifted (sync and black both near 0)
    // (4) lifted black but no band inside the captured window
    for (int c = 3; c <= 4; ++c) {
        const auto src = picture(c == 3 ? 2 : 10, c == 3 ? 4 : 70, c == 3);
        pal::VerticalRelock vr;
        int unchanged = 0;
        for (int i = 0; i < 30; ++i) {
            auto f = src;
            vr.process(f.data(), w, h, 0, w);
            unchanged += f == src || vr.offset() == 0;
        }
        const bool ok = unchanged == 30;
        std::printf("re-align, %s %s  %d of 30 frames unchanged (last: %s)\n",
                    c == 3 ? "dark screen, black not lifted:                " : "lifted black, no band in the window:        ", ok ? "PASS" : "FAIL",
                    unchanged, vr.how().c_str());
        failures += !ok;
    }
    return failures;
}

// A picture without a pixel grid must not be taken for one (the weaker-but-
// prominent rule in estimateGrid): random-walk noise, C64 range.
int gridNegativeTest() {
    const int w = 1856, h = 298;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    uint32_t seed = 1;
    for (int y = 0; y < h; ++y) {
        int v = 128;
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            v = std::clamp(v + int(seed >> 26) - 32, 0, 255);
            for (int c = 0; c < 3; ++c) rgb[(size_t(y) * w + x) * 3 + c] = uint8_t(v);
        }
    }
    const pal::GridEstimate g = pal::estimateGrid(rgb.data(), w, h, 4.5 / 1.03, 4.5 * 1.03);
    const bool ok = !g.valid;
    std::printf("grid: random noise, C64 range: no grid                       %s  score %.3f\n", ok ? "PASS" : "FAIL", g.score);
    return ok ? 0 : 1;
}

// Black and white (DecodeParams::mono): a luma-only signal (no burst) at 4x
// and 8x, with noise and with blanking at code 0: every output point must be
// grey (R=G=B) and each bar's brightness the luma the generator put in.
int monoTests() {
    struct M { const char* name; int K; double noise, lift; };
    const M ms[] = {{"B/W 4x", 4, 0, 64}, {"B/W 8x", 8, 0, 64}, {"B/W 8x, noise sigma 3", 8, 3, 64},
                    {"B/W 8x, blanking at code 0", 8, 0, 0}};
    int failures = 0;
    for (const M& m : ms) {
        pal::GenParams g;
        g.chroma = false;
        g.samplesPerCycle = m.K;
        g.samplesPerLine = 1135 * m.K / 4;
        g.windowStart = 90 * m.K / 4;
        g.width = 1024 * m.K / 4;
        g.noiseCodes = m.noise;
        g.liftCode = m.lift;
        const auto raw = pal::generate(g, pal::colourBars);
        const double fs = g.samplesPerLine * g.lineRate;
        pal::DecodeParams dp;
        dp.mono = true;
        dp.samplesPerCycle = m.K;
        dp.burstLen = 8 * m.K;
        dp.activeStart = int(std::lround(10.5e-6 * fs)) - g.windowStart;
        dp.activeLen = int(std::lround(52e-6 * fs));
        pal::DecodeInfo info;
        const auto rgb = pal::decode(raw.data(), g.width, g.lines, dp, &info);
        int maxSpread = 0;
        double worst = 0;
        for (int b = 0; b < 8; ++b) {
            const double x = (b + 0.5) / 8;
            const pal::Rgb c = pal::colourBars(x);
            const double want = 255 * (0.299 * c.r + 0.587 * c.g + 0.114 * c.b);
            const int col = int(x * dp.activeLen);
            double sum = 0;
            int n = 0;
            for (int l = 10; l < g.lines - 10; ++l) {
                const uint8_t* o = &rgb[(size_t(l) * dp.activeLen + col) * 3];
                maxSpread = std::max({maxSpread, std::abs(o[0] - o[1]), std::abs(o[1] - o[2])});
                sum += o[1], ++n;
            }
            worst = std::max(worst, std::abs(sum / n - want));
        }
        const bool ok = !info.colour && maxSpread == 0 && worst <= 4.0;
        failures += !ok;
        std::printf("%-58s %s  worst bar brightness error %4.1f  colour spread %d  blank %.1f\n", m.name, ok ? "PASS" : "FAIL",
                    worst, maxSpread, info.blankCode);
    }
    return failures;
}

int runTests() {
    std::vector<Case> cases;
    pal::GenParams base;  // 4xFsc, 1135/line, window from sample 90, 1024 wide, lift 64
    cases.push_back({"4xFsc ideal (includes the -5.6 ppm line-locked offset)", base, true, true});
    { pal::GenParams p = base; p.extraPpm = 50; cases.push_back({"4xFsc + 50 ppm extra rate error", p, true, true}); }
    { pal::GenParams p = base; p.noiseCodes = 3; cases.push_back({"4xFsc, noise sigma 3 codes", p, true, true}); }
    { pal::GenParams p = base; p.jitterSamples = 0.5; cases.push_back({"4xFsc, line start jitter +/-0.5 sample", p, true, true}); }
    { pal::GenParams p = base; p.samplesPerCycle = 8; p.samplesPerLine = 2270; p.windowStart = 180; p.width = 2048;
      cases.push_back({"8xFsc ideal", p, true, true}); }
    { pal::GenParams p = base; p.chroma = false; cases.push_back({"luma only (no burst, no chroma)", p, false, true}); }
    { pal::GenParams p = base; p.liftCode = 0; cases.push_back({"blanking at code 0 (burst clipped, like the real capture)", p, true, false}); }
    { pal::GenParams p = base; p.chromaPhaseErrDeg = 20; cases.push_back({"chroma phase error 20 deg, no line averaging", p, true, false}); }
    { pal::GenParams p = base; p.chromaPhaseErrDeg = 20;
      // Averaging cancels the hue error but keeps cos(20 deg) = 94% of the
      // saturation, so the bars are allowed that much more error.
      cases.push_back({"chroma phase error 20 deg, line averaging", p, true, true, true, 1.0, 12.0}); }
    { pal::GenParams p = base; p.samplesPerLine = 1134; p.lineRate = 15639.0;
      cases.push_back({"C64 line: 1134 samples at 15,639 Hz (exact 4xFsc)", p, true, true, false, 1.0}); }
    { pal::GenParams p = base; p.samplesPerLine = 1134; p.lineRate = 15639.0; p.firstLine = 1;
      cases.push_back({"C64 line, 1134, frame starting on an odd line", p, true, true, false, 1.0}); }
    { pal::GenParams p = base; p.samplesPerLine = 1134; p.lineRate = 15639.0; p.firstLine = 1; p.noiseCodes = 3;
      cases.push_back({"C64 line, 1134, odd start, noise sigma 3", p, true, true, true, 3.0}); }
    { pal::GenParams p = base; p.samplesPerLine = 1134; p.lineRate = 15639.0; p.carrierPpm = 150;
      cases.push_back({"C64, carrier +150 ppm (unstable machine), no PLL", p, true, false, true}); }
    { pal::GenParams p = base; p.samplesPerLine = 1134; p.lineRate = 15639.0; p.carrierPpm = 150;
      Case c{"C64, carrier +150 ppm, PLL", p, true, true, true, 1.0};
      c.pll = true;
      cases.push_back(c); }
    { pal::GenParams p = base; p.samplesPerLine = 1134; p.lineRate = 15639.0; p.carrierPpm = -300; p.noiseCodes = 3;
      Case c{"C64, carrier -300 ppm, noise 3, PLL", p, true, true, true, 3.0};
      c.pll = true;
      cases.push_back(c); }
    { pal::GenParams p = base; p.samplesPerLine = 1135; p.lineRate = 15639.0;
      cases.push_back({"C64 line sampled with 1135, no line averaging", p, true, false}); }
    { pal::GenParams p = base; p.samplesPerLine = 1135; p.lineRate = 15639.0;
      cases.push_back({"C64 line sampled with 1135, line averaging", p, true, false, true}); }

    int failures = 0;
    for (const Case& c : cases) {
        auto raw = pal::generate(c.gen, pal::colourBars);
        const double fs = c.gen.samplesPerLine * c.gen.lineRate;
        pal::DecodeParams dp;
        dp.averageLines = c.averageLines;
        dp.samplesPerLine = c.gen.samplesPerLine;
        dp.samplesPerCycle = c.gen.samplesPerCycle;
        dp.burstLen = 8 * c.gen.samplesPerCycle;
        dp.activeStart = int(std::lround(10.5e-6 * fs)) - c.gen.windowStart;
        dp.activeLen = int(std::lround(52e-6 * fs));
        pal::DecodeInfo info;
        auto rgb = pal::decode(raw.data(), c.gen.width, c.gen.lines, dp, &info);
        std::vector<float> freqErr;
        if (c.pll && !info.stepError.empty()) {
            // The simplest PLL: every line gets the frame's mean step error.
            freqErr.assign(c.gen.lines, float(info.meanStepErrorDeg * pal::kPi / 180));
            dp.lineFreqErr = freqErr.data();
            rgb = pal::decode(raw.data(), c.gen.width, c.gen.lines, dp, &info);
        }
        double leak = 0;
        std::string detail;
        double err = barError(rgb, dp.activeLen, c.gen.lines, &leak, detail);
        double hb = hanover(rgb, dp.activeLen, c.gen.lines);
        bool ok;
        if (c.expectColour) ok = info.colour && err <= c.maxErr && hb <= c.maxHanover;
        else ok = !info.colour && leak <= 2.0;
        if (c.mustPass && !ok) ++failures;
        std::printf("%-58s %s  worst bar error %5.1f  line-to-line %4.1f  chroma leak %4.1f  burst@%d amp %.1f blank %.1f step %.0f deg colour %d clipped %d\n",
                    c.name, c.mustPass ? (ok ? "PASS" : "FAIL") : "info", err, hb, leak, info.burstStart,
                    info.burstAmplitude, info.blankCode, info.phaseStepDeg, int(info.colour), int(info.clipped));
        std::printf("    bars:%s\n", detail.c_str());
    }
    {
        // Box first: the notch is judged against its rise width.
        int dummy = 0;
        filterReport(pal::LumaFilter::Box, "box", dummy, 0);
        filterReport(pal::LumaFilter::Off, "off", dummy, 0);
        // Box's rise on this step is 3-4 samples at 4x (one carrier cycle); the notch must beat it.
        filterReport(pal::LumaFilter::Notch, "notch", failures, 4);
    }
    // Regression limits a little under the measured 88.7 / 51.0 / 54.4%
    // (the colour signal is smeared over ~3.5 C64 pixels, so fine colour
    // detail is partly lost by the signal itself).
    failures += gridTest(8, true, false, 85, 1.0);
    failures += gridTest(2, true, false, 45, 1.0);
    failures += gridTest(1, true, false, 48, 1.0);
    failures += gridTest(2, false, false, 0, 1.0);
    failures += gridTest(2, true, true, 0, 1.0);
    // Limits a little under the measured values;
    // the C16's 121 close colours and the Spectrum's 85%/100% pairs are
    // harder to tell apart than the C64's 16.
    const double min8[] = {0, 85, 84, 60, 65, 72}, min1[] = {0, 52, 50, 14, 33, 39};
    for (size_t m = 1; m < pal::machineProfiles().size(); ++m) {
        if (pal::machineProfiles()[m].palette.empty()) continue;
        failures += machinePaletteTest(int(m), 8, 0.8, 8.0, m < 6 ? min8[m] : 0);
        failures += machinePaletteTest(int(m), 1, 0.8, 8.0, m < 6 ? min1[m] : 0);
    }
    // Measured: hires 59.9%, multicolour 75.7% (6 candidates, weight 4); per pixel 57.5 / 56.1%.
    failures += cellTest(1, 58, 6, 4.0);
    failures += cellTest(2, 73, 6, 4.0);
    failures += ycTests();
    failures += bbcTests();
    failures += stripeTests();
    failures += ntscTests();
    failures += tmsTest(1, 95.0);
    failures += relockTests();
    failures += monoTests();
    failures += gridNegativeTest();
    failures += tmsTest(2, 40.0);  // fine colour detail: composite can't resolve it (the check: not worse than per pixel)
    std::printf("%s\n", failures ? "SOME REQUIRED CASES FAILED" : "all required cases passed");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    // "avg" anywhere after the command switches on line-pair colour averaging.
    bool avg = false;
    pal::LumaFilter luma = pal::LumaFilter::Box;
    int lineSamples = 0;  // "line=N": the Horizontal Size the samples were taken with (decides the parity)
    pal::Standard standard = pal::Standard::Pal;  // "standard=ntsc"
    for (int i = 2; i < argc;)
        if (std::strcmp(argv[i], "avg") == 0 || std::strncmp(argv[i], "luma=", 5) == 0 ||
            std::strncmp(argv[i], "line=", 5) == 0 || std::strncmp(argv[i], "standard=", 9) == 0) {
            if (argv[i][0] == 'a') avg = true;
            else if (argv[i][0] == 's') standard = std::strcmp(argv[i] + 9, "ntsc") == 0 ? pal::Standard::Ntsc : pal::Standard::Pal;
            else if (argv[i][1] == 'i') lineSamples = std::atoi(argv[i] + 5);
            else if (std::strcmp(argv[i] + 5, "off") == 0) luma = pal::LumaFilter::Off;
            else if (std::strcmp(argv[i] + 5, "notch") == 0) luma = pal::LumaFilter::Notch;
            for (int k = i; k + 1 < argc; ++k) argv[k] = argv[k + 1];
            --argc;
        } else {
            ++i;
        }
    if (argc >= 2 && std::strcmp(argv[1], "test") == 0) return runTests();
    if (argc >= 2 && std::strcmp(argv[1], "bench") == 0) {
        // The app's windows: 1024/2048/4096 samples at 4x/8x/16x, one field.
        const int nThreads[] = {1, 4, 10};
        for (int k : {4, 8, 16}) {
            pal::GenParams p;
            p.lines = 298;
            p.samplesPerCycle = k;
            p.samplesPerLine = 1135 * k / 4;
            p.windowStart = 90 * k / 4;
            p.width = 1024 * k / 4;
            auto raw = pal::generate(p, pal::colourBars);
            for (int nt : nThreads) {
                pal::DecodeParams dp;
                dp.samplesPerCycle = k;
                dp.burstLen = 8 * k;
                dp.activeStart = 96 * k / 4;
                dp.activeLen = int(std::lround(52e-6 * p.samplesPerLine * pal::kLineRate));
                dp.averageLines = true;
                dp.threads = nt;
                std::vector<uint8_t> out;
                pal::DecodeInfo first;
                pal::decodeInto(raw.data(), p.width, p.lines, dp, out, &first);
                dp.burstStart = first.burstStart;  // as the app does between its once-a-second searches
                const int runs = 20;
                auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < runs; ++i) pal::decodeInto(raw.data(), p.width, p.lines, dp, out, nullptr);
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / runs;
                std::printf("%2dxFsc %4dx%d, %2d thread(s): %6.2f ms per field (20 ms per field at 50 Hz)\n", k, p.width,
                            p.lines, nt, ms);
            }
        }
        // Pixel-grid stage on a decoded 8x C64 frame: grid + calibrated snapping.
        {
            const auto& c64 = pal::machineProfiles()[1];
            pal::GenParams p;
            p.samplesPerCycle = 8, p.samplesPerLine = 2268, p.lineRate = 15639.0, p.windowStart = 180, p.width = 2048, p.lines = 298;
            auto raw = pal::generate(p, [&](double x, int line) {
                const int px = int(x * 409.5);
                const pal::Colour& c = c64.palette[size_t(((px / 8) * 7 + (line / 8) * 3 + ((px * 2654435761u >> 7) & 1) * 5) % 16)];
                return pal::Rgb{c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
            });
            pal::DecodeParams dp;
            dp.samplesPerCycle = 8, dp.burstLen = 64, dp.activeStart = 192, dp.activeLen = 1843, dp.samplesPerLine = 2268;
            dp.luma = pal::LumaFilter::Notch;
            dp.averageLines = true;
            auto rgb = pal::decode(raw.data(), p.width, p.lines, dp, nullptr);
            pal::GridStage st;
            pal::GridStage::Settings gs;
            gs.periodLo = 4.5 / 1.03, gs.periodHi = 4.5 * 1.03, gs.snap = true, gs.fixedPalette = c64.palette, gs.colours = 16;
            gs.relearnEvery = 250, gs.poolEvery = 10, gs.learnFrames = 1 << 30;
            st.configure(gs);
            std::vector<uint8_t> out;
            int w = 0;
            st.process(rgb.data(), 1843, 298, out, w);  // first: finds the grid
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 100; ++i) st.process(rgb.data(), 1843, 298, out, w);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 100;
            std::printf("grid stage (sample + calibrated snap, 409x298): %.2f ms per frame on average\n", ms);
            gs.c64Cells = true;
            st.configure(gs);
            st.process(rgb.data(), 1843, 298, out, w);
            auto tc = std::chrono::steady_clock::now();
            for (int i = 0; i < 50; ++i) st.process(rgb.data(), 1843, 298, out, w);
            std::printf("  with C64 cells: %.2f ms per frame\n",
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc).count() / 50);
            auto t1 = std::chrono::steady_clock::now();
            pal::phaseAt(rgb.data(), 1843, 298, 4.5);
            std::printf("  phase update (every 25 frames): %.2f ms; grid %s\n",
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count(),
                        st.grid().valid ? "found" : "NOT found");
        }
        return 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "genc64") == 0) {
        // N frames of 75% colour bars as freshVision captures a PAL C64 at
        // 8x (Horizontal Size 2268, Position 180, 2048x298), the subcarrier
        // running on across frames (312 lines each), rows of samples back to
        // back: for freshCoder's end-to-end test.
        pal::GenParams p;
        p.samplesPerCycle = 8;
        p.samplesPerLine = 2268;
        p.lineRate = 15639.0;
        p.windowStart = 180;
        p.width = 2048;
        p.lines = 298;
        if (standard == pal::Standard::Ntsc) {  // a C64 NTSC: 227.5 cycles, 1820 at 8x, 263 lines
            p.standard = standard;
            p.samplesPerLine = 1820, p.lineRate = pal::standardInfo(standard).fsc / 227.5;
            p.windowStart = 80, p.width = 1728, p.lines = 250;
        }
        const int frames = std::atoi(argv[3]);
        FILE* f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        for (int n = 0; n < frames; ++n) {
            p.firstLine = n * (standard == pal::Standard::Ntsc ? 263 : 312);
            auto raw = pal::generate(p, pal::colourBars);
            std::fwrite(raw.data(), 1, raw.size(), f);
        }
        std::fclose(f);
        std::printf("%d frames of %d x %d samples written\n", frames, p.width, p.lines);
        return 0;
    }
    if (argc >= 4 && std::strcmp(argv[1], "genyc") == 0) {
        // The same as genc64, but S-Video: bgr0 pixels (bytes B,G,R,0) with
        // the brightness wire in green and the colour wire in red (its zero
        // at code 128), as an RGB capture of a C64's S-Video would give.
        pal::GenParams p;
        p.samplesPerCycle = 8;
        p.samplesPerLine = 2268;
        p.lineRate = 15639.0;
        p.windowStart = 180;
        p.width = 2048;
        p.lines = 298;
        if (standard == pal::Standard::Ntsc) {  // a C64 NTSC: 227.5 cycles, 1820 at 8x, 263 lines
            p.standard = standard;
            p.samplesPerLine = 1820, p.lineRate = pal::standardInfo(standard).fsc / 227.5;
            p.windowStart = 80, p.width = 1728, p.lines = 250;
        }
        const int frames = std::atoi(argv[3]);
        FILE* f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        std::vector<uint8_t> px(size_t(p.width) * p.lines * 4);
        for (int n = 0; n < frames; ++n) {
            p.firstLine = n * (standard == pal::Standard::Ntsc ? 263 : 312);
            auto yc = pal::generateYC(p, pal::colourBars, 128);
            for (size_t i = 0; i < yc.y.size(); ++i) px[i * 4] = 0, px[i * 4 + 1] = yc.y[i], px[i * 4 + 2] = yc.c[i], px[i * 4 + 3] = 0;
            std::fwrite(px.data(), 1, px.size(), f);
        }
        std::fclose(f);
        std::printf("%d S-Video frames of %d x %d (bgr0: brightness in G, colour in R) written\n", frames, p.width, p.lines);
        return 0;
    }
    if (argc >= 5 && std::strcmp(argv[1], "relock") == 0) {
        // Vertical re-alignment of a raw stream (frames of W x H samples back
        // to back): per frame how it was aligned, and how steady the result is
        // (mean row-profile difference to the previous aligned frame).
        const int w = std::atoi(argv[3]), h = std::atoi(argv[4]);
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) return 1;
        std::vector<uint8_t> raw(size_t(w) * h);
        std::vector<double> prev;
        pal::VerticalRelock vr;
        int n = 0, bySync = 0, byMatch = 0, missed = 0;
        double diffSum = 0;
        int diffN = 0, jumps = 0;
        while (std::fread(raw.data(), 1, raw.size(), f) == raw.size()) {
            const bool ok = vr.process(raw.data(), w, h, 0, w);
            if (ok) {
                (vr.how() == "sync rows" ? bySync : vr.how() == "matched" ? byMatch : missed)++;
                std::vector<double> prof(static_cast<size_t>(h));
                for (int y = 0; y < h; ++y) {
                    double s = 0;
                    for (int x = 0; x < w; x += 4) s += raw[size_t(y) * w + x];
                    prof[size_t(y)] = s / ((w + 3) / 4);
                }
                if (!prev.empty()) {
                    double d = 0;
                    for (int y = 0; y < h; ++y) d += std::abs(prof[size_t(y)] - prev[size_t(y)]);
                    d /= h;
                    diffSum += d, ++diffN;
                    if (d > 3) ++jumps;
                }
                prev = prof;
            } else {
                ++missed;
            }
            if (n < 12) std::printf("frame %d: %s, offset %d\n", n, vr.how().c_str(), vr.offset());
            ++n;
        }
        std::fclose(f);
        std::printf("%d frames: %d aligned by sync rows, %d matched, %d shown as delivered; frames differ from the "
                    "previous by %.2f codes per row on average, %d by more than 3\n",
                    n, bySync, byMatch, missed, diffN ? diffSum / diffN : 0.0, jumps);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "gen") == 0) {
        pal::GenParams p;
        if (argc > 3) p.samplesPerCycle = std::atoi(argv[3]);
        if (p.samplesPerCycle == 8) p.samplesPerLine = 2270, p.windowStart = 180, p.width = 2048;
        if (standard == pal::Standard::Ntsc) {  // standard NTSC: 227.5 cycles, 910 at 4x
            p.standard = standard;
            p.lineRate = pal::standardInfo(standard).fsc / 227.5;
            p.samplesPerLine = 910 * p.samplesPerCycle / 4, p.windowStart = 10 * p.samplesPerCycle;
            p.width = (p.samplesPerLine - p.windowStart - 2) / 16 * 16, p.lines = 240;
        }
        if (argc > 4) p.liftCode = std::atof(argv[4]);
        auto raw = pal::generate(p, pal::colourBars);
        FILE* f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        std::fwrite(raw.data(), 1, raw.size(), f);
        std::fclose(f);
        std::printf("%d x %d samples written\n", p.width, p.lines);
        return 0;
    }
    if (argc >= 6 && std::strcmp(argv[1], "scan") == 0) {
        // Every frame of a raw stream (frames of W x H samples back to back),
        // decoded on its own with a full burst search: one line per frame.
        // Colour strength = mean |U|+|V| of the decoded picture (RGB spread).
        int w = std::atoi(argv[3]), h = std::atoi(argv[4]);
        pal::DecodeParams dp;
        dp.samplesPerCycle = std::atoi(argv[5]);
        dp.burstLen = 8 * dp.samplesPerCycle;
        dp.standard = standard;
        dp.averageLines = avg;
        dp.luma = luma;
        dp.samplesPerLine = lineSamples;
        dp.threads = 10;
        if (argc > 6) dp.activeStart = std::atoi(argv[6]);
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) return 1;
        std::vector<uint8_t> raw(size_t(w) * h), out;
        for (int n = 0; std::fread(raw.data(), 1, raw.size(), f) == raw.size(); ++n) {
            pal::DecodeInfo info;
            int ow = pal::decodeInto(raw.data(), w, h, dp, out, &info);
            double spread = 0;
            for (size_t i = 0; i + 2 < out.size(); i += 3) {
                int mx = std::max({out[i], out[i + 1], out[i + 2]}), mn = std::min({out[i], out[i + 1], out[i + 2]});
                spread += mx - mn;
            }
            std::printf("%5d burst@%-4d amp %5.1f blank %5.1f parity %d step %7.1f colour %d clipped %d colourStrength %5.2f "
                        "ambiguous %d expected %.1f\n",
                        n, info.burstStart, info.burstAmplitude, info.blankCode, info.parity, info.phaseStepDeg,
                        int(info.colour), int(info.clipped), spread / (double(ow) * h), int(info.parityAmbiguous),
                        info.expectedStepDeg);
        }
        std::fclose(f);
        return 0;
    }
    if (argc >= 7 && std::strcmp(argv[1], "decode") == 0) {
        int w = std::atoi(argv[3]), h = std::atoi(argv[4]);
        pal::DecodeParams dp;
        dp.samplesPerCycle = std::atoi(argv[5]);
        dp.burstLen = 8 * dp.samplesPerCycle;
        dp.standard = standard;
        dp.averageLines = avg;
        dp.luma = luma;
        dp.samplesPerLine = lineSamples;
        if (argc > 7) dp.activeStart = std::atoi(argv[7]);
        if (argc > 8) dp.burstStart = std::atoi(argv[8]);
        std::vector<uint8_t> raw(size_t(w) * h);
        FILE* f = std::fopen(argv[2], "rb");
        if (!f || std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
            std::printf("cannot read %d x %d samples from %s\n", w, h, argv[2]);
            return 1;
        }
        std::fclose(f);
        // S-Video: chroma=FILE is the colour plane (same size); IN is then brightness.
        std::vector<uint8_t> chroma;
        for (int i = 2; i < argc; ++i)
            if (std::strncmp(argv[i], "chroma=", 7) == 0) {
                chroma.resize(raw.size());
                FILE* cf = std::fopen(argv[i] + 7, "rb");
                if (!cf || std::fread(chroma.data(), 1, chroma.size(), cf) != chroma.size()) {
                    std::printf("cannot read the colour plane %s\n", argv[i] + 7);
                    return 1;
                }
                std::fclose(cf);
                dp.chromaPlane = chroma.data();
            }
        pal::DecodeInfo info;
        auto rgb = pal::decode(raw.data(), w, h, dp, &info);
        int ow = w - dp.activeStart;
        writePpm(argv[6], rgb, ow, h);
        std::printf("burst at sample %d, amplitude %.1f codes, blanking %.1f, V-switch parity %d, "
                    "carrier step per line %.1f deg, colour %s, burst clipped %s; wrote %dx%d\n",
                    info.burstStart, info.burstAmplitude, info.blankCode, info.parity, info.phaseStepDeg,
                    info.colour ? "on" : "off (killer)", info.clipped ? "yes" : "no", ow, h);
        return 0;
    }
    std::printf("usage: paltool [standard=ntsc] test | bench | genc64 OUT.raw FRAMES | scan IN.raw W H K [activeStart] [avg] [luma=..] [line=HSIZE] | relock IN.raw W H | gen OUT.raw [K] [lift] | decode IN.raw W H K OUT.ppm [activeStart] [burstStart] [avg] [luma=box|notch|off] [line=HSIZE] [chroma=COLOUR.raw (S-Video)]\n");
    return 2;
}

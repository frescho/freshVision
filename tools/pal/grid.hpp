// Machine pixels from a decoded composite picture: the pixel grid (period and
// phase), sampling each pixel at its settled centre, and snapping to the
// machine's palette (learned from the picture, or given). Phase 3 of the
// composite work.
//
// A home computer draws its picture on a pixel clock; a composite signal
// smears every colour change over a few samples, but the middle of each
// pixel is settled (user's idea, 2026-09-29). Knowing the grid, the middle
// is taken, and with a known palette the colour is snapped to the nearest
// palette entry.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "pal.hpp"

namespace pal {

using Colour = std::array<uint8_t, 3>;

// ------------------------------------------------------------- profiles ---

struct MachineProfile {
    const char* name;
    int lineAt4x;              // samples per line at 4x the subcarrier (0 = leave as it is)
    double samplesPerPixelAt4x;  // pixel period at 4x; 0 = unknown, search the whole range
    int colours;               // palette size for snapping (0 = no fixed palette)
    std::vector<Colour> palette;  // starting values (may be empty: learned from the picture)
    const char* note;
    double burstScale = 1.0;   // the machine's burst relative to the standard's (DecodeParams::burstScale)
};

// Built-in profiles. Clock relations: only the C64's are established here
// (one crystal, 4xFsc = 17.734475 MHz, pixel
// clock = 4/9 of it, line = 1134 crystal cycles); the others are hints from
// published clock rates, *unverified* on real hardware (the grid search
// refines any hint by +/-3%). Palettes (the output colours when snapping to
// the machine's palette) and their sources:
//  - C64: Pepto's measured VIC-II palette (pepto.de, "colorvic").
//  - VIC-20, C16/Plus/4 (TED): Wikipedia's YPbPr tables (luma levels and
//    unit colour directions); the colour strength is not documented and is
//    set to that of Pepto's C64 colours (0.12 on the +-0.5 scale, assumed).
//  - MSX1 (TMS9918/9929): the datasheet's Y / R-Y / B-Y table (Wikipedia),
//    47% = no colour, 0/100% taken as full scale (assumed).
//  - ZX Spectrum: normal colours at 85% signal level (0xD7), bright 100%.
// All *unverified on real hardware*; editable as custom palettes.
// The C64's cell rules apply to both C64 profiles (PAL 1, NTSC 8).
inline bool isC64Profile(int p) { return p == 1 || p == 8; }
// The standard a profile's machine uses (NTSC profiles 8-10).
inline bool isNtscProfile(int p) { return p >= 8 && p <= 10; }
// TMS9918 machines (MSX1 PAL 5, MSX1 NTSC 10): two colours per 8-pixel segment.
inline bool isTmsProfile(int p) { return p == 5 || p == 10; }

inline const std::vector<MachineProfile>& machineProfiles() {
    static const std::vector<MachineProfile> p = {
        {"Generic PAL (detect everything)", 0, 0, 16, {}, "pixel period searched over 1.5-6 samples at 4x; no machine palette"},
        {"Commodore 64 (PAL)", 1134, 2.25, 16,
         {
         {{0x00, 0x00, 0x00}}, {{0xFF, 0xFF, 0xFF}}, {{0x68, 0x37, 0x2B}}, {{0x70, 0xA4, 0xB2}},
         {{0x6F, 0x3D, 0x86}}, {{0x58, 0x8D, 0x43}}, {{0x35, 0x28, 0x79}}, {{0xB8, 0xC7, 0x6F}},
         {{0x6F, 0x4F, 0x25}}, {{0x43, 0x39, 0x00}}, {{0x9A, 0x67, 0x59}}, {{0x44, 0x44, 0x44}},
         {{0x6C, 0x6C, 0x6C}}, {{0x9A, 0xD2, 0x84}}, {{0x6C, 0x5E, 0xB5}}, {{0x95, 0x95, 0x95}}},
         "line 1134, 4.5 samples per pixel at 8x; Pepto palette"},
        {"Commodore VIC-20 (PAL, unverified)", 0, 4.0, 16,
         {
         {{0x00, 0x00, 0x00}}, {{0xFF, 0xFF, 0xFF}}, {{0x67, 0x30, 0x2B}}, {{0x98, 0xCF, 0xD4}},
         {{0x9E, 0x69, 0xA6}}, {{0x61, 0x96, 0x59}}, {{0x40, 0x35, 0x76}}, {{0xBF, 0xCA, 0x89}},
         {{0x9E, 0x77, 0x59}}, {{0xDE, 0xB7, 0x99}}, {{0xA7, 0x6F, 0x6B}}, {{0xD7, 0xFF, 0xFF}},
         {{0xDE, 0xA8, 0xE6}}, {{0xA1, 0xD6, 0x99}}, {{0x80, 0x75, 0xB6}}, {{0xFF, 0xFF, 0xC9}}},
         "pixel clock = the colour carrier (unverified); palette from the documented YPbPr table"},
        {"Commodore C16 / Plus/4 (PAL, unverified)", 0, 2.0, 121,
         {
         {{0x00, 0x00, 0x00}}, {{0x20, 0x20, 0x20}}, {{0x40, 0x40, 0x40}}, {{0x60, 0x60, 0x60}},
         {{0x80, 0x80, 0x80}}, {{0x9F, 0x9F, 0x9F}}, {{0xBF, 0xBF, 0xBF}}, {{0xDF, 0xDF, 0xDF}},
         {{0xFF, 0xFF, 0xFF}}, {{0x48, 0x10, 0x0B}}, {{0x67, 0x30, 0x2B}}, {{0x87, 0x4F, 0x4B}},
         {{0xA7, 0x6F, 0x6B}}, {{0xC7, 0x8F, 0x8B}}, {{0xE7, 0xAF, 0xAA}}, {{0xFF, 0xCF, 0xCA}},
         {{0xFF, 0xEF, 0xEA}}, {{0x00, 0x30, 0x35}}, {{0x18, 0x50, 0x55}}, {{0x38, 0x70, 0x74}},
         {{0x58, 0x90, 0x94}}, {{0x78, 0xB0, 0xB4}}, {{0x98, 0xCF, 0xD4}}, {{0xB7, 0xEF, 0xF4}},
         {{0xD7, 0xFF, 0xFF}}, {{0x3E, 0x09, 0x46}}, {{0x5E, 0x29, 0x66}}, {{0x7E, 0x49, 0x86}},
         {{0x9E, 0x69, 0xA6}}, {{0xBE, 0x88, 0xC6}}, {{0xDE, 0xA8, 0xE6}}, {{0xFD, 0xC8, 0xFF}},
         {{0xFF, 0xE8, 0xFF}}, {{0x02, 0x37, 0x00}}, {{0x21, 0x57, 0x19}}, {{0x41, 0x77, 0x39}},
         {{0x61, 0x96, 0x59}}, {{0x81, 0xB6, 0x79}}, {{0xA1, 0xD6, 0x99}}, {{0xC1, 0xF6, 0xB9}},
         {{0xE1, 0xFF, 0xD9}}, {{0x20, 0x15, 0x56}}, {{0x40, 0x35, 0x76}}, {{0x60, 0x55, 0x96}},
         {{0x80, 0x75, 0xB6}}, {{0x9F, 0x95, 0xD6}}, {{0xBF, 0xB5, 0xF5}}, {{0xDF, 0xD5, 0xFF}},
         {{0xFF, 0xF4, 0xFF}}, {{0x20, 0x2A, 0x00}}, {{0x40, 0x4A, 0x0A}}, {{0x60, 0x6A, 0x29}},
         {{0x80, 0x8A, 0x49}}, {{0x9F, 0xAA, 0x69}}, {{0xBF, 0xCA, 0x89}}, {{0xDF, 0xEA, 0xA9}},
         {{0xFF, 0xFF, 0xC9}}, {{0x3E, 0x18, 0x00}}, {{0x5E, 0x38, 0x19}}, {{0x7E, 0x58, 0x39}},
         {{0x9E, 0x77, 0x59}}, {{0xBE, 0x97, 0x79}}, {{0xDE, 0xB7, 0x99}}, {{0xFD, 0xD7, 0xB9}},
         {{0xFF, 0xF7, 0xD9}}, {{0x30, 0x21, 0x00}}, {{0x50, 0x41, 0x0E}}, {{0x70, 0x61, 0x2E}},
         {{0x90, 0x81, 0x4D}}, {{0xB0, 0xA1, 0x6D}}, {{0xD0, 0xC1, 0x8D}}, {{0xF0, 0xE0, 0xAD}},
         {{0xFF, 0xFF, 0xCD}}, {{0x0F, 0x32, 0x00}}, {{0x2F, 0x52, 0x0E}}, {{0x4F, 0x72, 0x2E}},
         {{0x6F, 0x92, 0x4D}}, {{0x8F, 0xB1, 0x6D}}, {{0xAF, 0xD1, 0x8D}}, {{0xCF, 0xF1, 0xAD}},
         {{0xEF, 0xFF, 0xCD}}, {{0x4B, 0x0A, 0x20}}, {{0x6B, 0x2A, 0x40}}, {{0x8B, 0x4A, 0x60}},
         {{0xAA, 0x6A, 0x80}}, {{0xCA, 0x8A, 0x9F}}, {{0xEA, 0xA9, 0xBF}}, {{0xFF, 0xC9, 0xDF}},
         {{0xFF, 0xE9, 0xFF}}, {{0x00, 0x36, 0x20}}, {{0x15, 0x56, 0x40}}, {{0x35, 0x75, 0x60}},
         {{0x55, 0x95, 0x80}}, {{0x74, 0xB5, 0x9F}}, {{0x94, 0xD5, 0xBF}}, {{0xB4, 0xF5, 0xDF}},
         {{0xD4, 0xFF, 0xFF}}, {{0x02, 0x28, 0x46}}, {{0x21, 0x48, 0x66}}, {{0x41, 0x68, 0x86}},
         {{0x61, 0x88, 0xA6}}, {{0x81, 0xA7, 0xC6}}, {{0xA1, 0xC7, 0xE6}}, {{0xC1, 0xE7, 0xFF}},
         {{0xE1, 0xFF, 0xFF}}, {{0x30, 0x0E, 0x52}}, {{0x50, 0x2E, 0x72}}, {{0x70, 0x4E, 0x92}},
         {{0x90, 0x6D, 0xB2}}, {{0xB0, 0x8D, 0xD1}}, {{0xD0, 0xAD, 0xF1}}, {{0xF0, 0xCD, 0xFF}},
         {{0xFF, 0xED, 0xFF}}, {{0x00, 0x38, 0x0B}}, {{0x18, 0x58, 0x2B}}, {{0x38, 0x78, 0x4B}},
         {{0x58, 0x98, 0x6B}}, {{0x78, 0xB8, 0x8B}}, {{0x98, 0xD7, 0xAA}}, {{0xB7, 0xF7, 0xCA}},
         {{0xD7, 0xFF, 0xEA}}},
         "121 colours (16 hues x 8 luma); pixel clock half the 4xFsc crystal (unverified)"},
        {"Sinclair ZX Spectrum (PAL, unverified)", 1135, 2.5335, 15,
         {
         {{0x00, 0x00, 0x00}}, {{0x00, 0x00, 0xD7}}, {{0xD7, 0x00, 0x00}}, {{0xD7, 0x00, 0xD7}},
         {{0x00, 0xD7, 0x00}}, {{0x00, 0xD7, 0xD7}}, {{0xD7, 0xD7, 0x00}}, {{0xD7, 0xD7, 0xD7}},
         {{0x00, 0x00, 0xFF}}, {{0xFF, 0x00, 0x00}}, {{0xFF, 0x00, 0xFF}}, {{0x00, 0xFF, 0x00}},
         {{0x00, 0xFF, 0xFF}}, {{0xFF, 0xFF, 0x00}}, {{0xFF, 0xFF, 0xFF}}},
         "7 MHz pixels, not locked to the colour carrier (unverified); 85% / 100% levels"},
        {"MSX1, TMS9929 (PAL, unverified)", 0, 0, 15,
         {
         {{0x00, 0x00, 0x00}}, {{0x00, 0xE2, 0x14}}, {{0x46, 0xEF, 0x56}}, {{0x4E, 0x46, 0xFF}},
         {{0x7A, 0x68, 0xFF}}, {{0xF1, 0x48, 0x2F}}, {{0x1C, 0xF8, 0xFF}}, {{0xFF, 0x49, 0x32}},
         {{0xFF, 0x6C, 0x56}}, {{0xDC, 0xCA, 0x10}}, {{0xEE, 0xD4, 0x4C}}, {{0x05, 0xC6, 0x12}},
         {{0xDF, 0x4A, 0xDC}}, {{0xCC, 0xCC, 0xCC}}, {{0xFF, 0xFF, 0xFF}}},
         "palette from the datasheet's colour table (unverified); pixel clock searched"},
        {"Amiga lores (PAL, unverified)", 1135, 2.5, 0, {}, "4096 colours chosen freely: no machine palette (use learned)"},
        // BBC Micro Model B: 64 us lines (1135 samples at 4x), pixels from a
        // 16 MHz clock not locked to the carrier's own 17.734475 MHz crystal:
        // 1.108 (mode 0), 2.217 (1, 4, 6) or 4.434 (2, 5) samples per pixel at
        // 4x, so the period is searched. Eight digital colours (the
        // composite output is monochrome without link S39).
        {"Acorn BBC Micro Model B (PAL, unverified)", 1135, 0, 8,
         {
         {{0x00, 0x00, 0x00}}, {{0xFF, 0x00, 0x00}}, {{0x00, 0xFF, 0x00}}, {{0xFF, 0xFF, 0x00}},
         {{0x00, 0x00, 0xFF}}, {{0xFF, 0x00, 0xFF}}, {{0x00, 0xFF, 0xFF}}, {{0xFF, 0xFF, 0xFF}}},
         "line 1135 (64 us); pixel period searched (modes 1/4/6: 4.43, modes 2/5: 8.87 samples at 8x); 8 colours"},
        // NTSC machines (none tested on hardware yet).
        // C64 NTSC (6567R8): 520 dots per line at 8.18 MHz, 910 samples at 4x
        // (1.75 per pixel); its colours are defined in YUV like the PAL VIC-II,
        // so Pepto's palette is used.
        {"Commodore 64 (NTSC, unverified)", 910, 1.75, 16,
         {
         {{0x00, 0x00, 0x00}}, {{0xFF, 0xFF, 0xFF}}, {{0x68, 0x37, 0x2B}}, {{0x70, 0xA4, 0xB2}},
         {{0x6F, 0x3D, 0x86}}, {{0x58, 0x8D, 0x43}}, {{0x35, 0x28, 0x79}}, {{0xB8, 0xC7, 0x6F}},
         {{0x6F, 0x4F, 0x25}}, {{0x43, 0x39, 0x00}}, {{0x9A, 0x67, 0x59}}, {{0x44, 0x44, 0x44}},
         {{0x6C, 0x6C, 0x6C}}, {{0x9A, 0xD2, 0x84}}, {{0x6C, 0x5E, 0xB5}}, {{0x95, 0x95, 0x95}}},
         "line 910 (227.5 cycles), 3.5 samples per pixel at 8x; Pepto palette (PAL measurements)"},
        // Apple II: 912 14-MHz periods per line = 912 samples at 4x; hi-res
        // pixels 2 samples at 4x; the 16 lo-res colours (Wikipedia "Apple II
        // graphics"); hi-res colours are artifact colours from that set.
        {"Apple II (NTSC, unverified)", 912, 2.0, 16,
         {
         {{0x00, 0x00, 0x00}}, {{0x90, 0x17, 0x40}}, {{0x40, 0x2C, 0xA5}}, {{0xD0, 0x43, 0xE5}},
         {{0x00, 0x69, 0x40}}, {{0x80, 0x80, 0x80}}, {{0x2F, 0x95, 0xE5}}, {{0xBF, 0xAB, 0xFF}},
         {{0x40, 0x54, 0x00}}, {{0xD0, 0x6A, 0x1A}}, {{0x80, 0x80, 0x80}}, {{0xFF, 0x96, 0xBF}},
         {{0x2F, 0xBC, 0x1A}}, {{0xBF, 0xD3, 0x5A}}, {{0x6F, 0xE8, 0xBF}}, {{0xFF, 0xFF, 0xFF}}},
         "line 912 (228 cycles), hi-res pixel 4 samples at 8x; lo-res palette (Wikipedia)"},
        // MSX1 / TMS9918A: 3x the subcarrier, 342 dots per line = 912 samples
        // at 4x (2.667 per pixel); the datasheet colours, as for the TMS9929.
        {"MSX1, TMS9918 (NTSC, unverified)", 912, 8.0 / 3.0, 15,
         {
         {{0x00, 0x00, 0x00}}, {{0x00, 0xE2, 0x14}}, {{0x46, 0xEF, 0x56}}, {{0x4E, 0x46, 0xFF}},
         {{0x7A, 0x68, 0xFF}}, {{0xF1, 0x48, 0x2F}}, {{0x1C, 0xF8, 0xFF}}, {{0xFF, 0x49, 0x32}},
         {{0xFF, 0x6C, 0x56}}, {{0xDC, 0xCA, 0x10}}, {{0xEE, 0xD4, 0x4C}}, {{0x05, 0xC6, 0x12}},
         {{0xDF, 0x4A, 0xDC}}, {{0xCC, 0xCC, 0xCC}}, {{0xFF, 0xFF, 0xFF}}},
         "line 912 (228 cycles), 5.33 samples per pixel at 8x; the datasheet colour table (unverified); burst ~11 IRE",
         0.275},
    };
    return p;
}

// ------------------------------------------------------------ the grid ---

struct GridEstimate {
    double period = 0;  // samples per machine pixel
    double phase = 0;   // position of a pixel boundary, in samples (0 <= phase < period)
    double score = 0;   // 0..1: how well the edges line up on this grid
    bool valid = false;
};

// Brightness edges of packed RGB rows: position n + 0.5 (between samples),
// weight |dY|; only strong ones (the top of the distribution) are kept.
inline void collectEdges(const uint8_t* rgb, int w, int h, std::vector<float>& pos, std::vector<float>& wt) {
    pos.clear();
    wt.clear();
    std::vector<float> all;
    all.reserve(size_t(w) * h / 2);
    auto Y = [&](int y, int x) {
        const uint8_t* p = rgb + (size_t(y) * w + x) * 3;
        return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    };
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x + 1 < w; ++x) all.push_back(std::abs(Y(y, x + 1) - Y(y, x)));
    if (all.empty()) return;
    std::vector<float> sorted = all;
    const size_t k = sorted.size() * 97 / 100;
    std::nth_element(sorted.begin(), sorted.begin() + long(k), sorted.end());
    const float thr = std::max(8.0f, sorted[k]);
    size_t i = 0;
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x + 1 < w; ++x, ++i)
            if (all[i] >= thr) {
                pos.push_back(float(x) + 0.5f);
                wt.push_back(all[i]);
            }
}

// Tries pixel periods in [lo, hi]: for each, how tightly the edge positions
// gather around one phase (circular mean of 2 pi x / period, weighted by
// edge strength). Coarse steps, then a fine search around the best.
inline GridEstimate estimateGrid(const uint8_t* rgb, int w, int h, double lo, double hi) {
    GridEstimate best;
    std::vector<float> pos, wt;
    collectEdges(rgb, w, h, pos, wt);
    if (pos.size() < 50 || lo <= 0 || hi <= lo) return best;
    double wsum = 0;
    for (float v : wt) wsum += v;
    auto score = [&](double period, double* phase) {
        double re = 0, im = 0;
        const double k = 2 * kPi / period;
        for (size_t i = 0; i < pos.size(); ++i) {
            re += wt[i] * std::cos(k * pos[i]);
            im += wt[i] * std::sin(k * pos[i]);
        }
        if (phase) {
            double a = std::atan2(im, re);
            if (a < 0) a += 2 * kPi;
            *phase = a / k;
        }
        return std::sqrt(re * re + im * im) / wsum;
    };
    // Coarse: steps of 0.3% of the period; then 20x finer around the best.
    double bestP = lo, bestS = -1, sumS = 0;
    int nS = 0;
    for (double p = lo; p <= hi; p *= 1.003) {
        double s = score(p, nullptr);
        sumS += s, ++nS;
        if (s > bestS) bestS = s, bestP = p;
    }
    const double a = bestP / 1.003, b = bestP * 1.003;
    for (double p = a; p <= b; p += (b - a) / 60) {
        double s = score(p, nullptr);
        if (s > bestS) bestS = s, bestP = p;
    }
    best.period = bestP;
    best.score = score(bestP, &best.phase);
    // A grid: strong alignment, or a weaker one that stands well above the
    // other periods of the range. Pictures made mostly of double-wide pixels
    // (C64 multicolour games: Commando's title scored 0.14-0.17 at 4.5 samples,
    // 5-7x the range's mean) align only weakly at the single-pixel period;
    // pictures without that grid stay near 2x (measured 2.2-2.5).
    const double mean = nS ? sumS / nS : 0;
    best.valid = best.score > 0.15 || (best.score > 0.08 && best.score > 4 * mean);
    return best;
}

// The grid's phase for a known period (one pass over the edges): the cheap
// follow-up after estimateGrid() has found the period.
inline GridEstimate phaseAt(const uint8_t* rgb, int w, int h, double period) {
    GridEstimate g;
    std::vector<float> pos, wt;
    collectEdges(rgb, w, h, pos, wt);
    if (pos.size() < 50 || period <= 0) return g;
    double re = 0, im = 0, ws = 0;
    const double k = 2 * kPi / period;
    for (size_t i = 0; i < pos.size(); ++i) {
        re += wt[i] * std::cos(k * pos[i]);
        im += wt[i] * std::sin(k * pos[i]);
        ws += wt[i];
    }
    double a = std::atan2(im, re);
    if (a < 0) a += 2 * kPi;
    g.period = period;
    g.phase = a / k;
    g.score = std::sqrt(re * re + im * im) / ws;
    g.valid = g.score > 0.15;
    return g;
}

// One output pixel per machine pixel: the average of the samples within the
// central `core` fraction of the pixel (core 0.5 = the middle half, away from
// the colour transitions on both sides; at least the nearest sample).
// `fixedW` > 0: exactly that many pixels (a stream keeps one width even when
// the phase moves and one more or fewer whole pixels would fit).
inline void sampleGrid(const uint8_t* rgb, int w, int h, const GridEstimate& g, double core, std::vector<uint8_t>& out,
                       int& outW, int fixedW = 0) {
    outW = 0;
    if (!g.valid || g.period <= 0) return;
    const double p = g.period;
    const int k0 = int(std::ceil(-g.phase / p));  // first pixel starting at or after sample 0
    const int k1 = int(std::floor((double(w) - g.phase) / p)) - 1;
    outW = fixedW > 0 ? fixedW : std::max(0, k1 - k0 + 1);
    out.resize(size_t(outW) * h * 3);
    const double half = std::max(0.5, 0.5 * core * p);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgb + size_t(y) * w * 3;
        for (int k = 0; k < outW; ++k) {
            const double c = g.phase + (double(k0 + k) + 0.5) * p;  // centre, in edge coordinates (n + 0.5)
            int n0 = int(std::ceil(c - half - 0.5)), n1 = int(std::floor(c + half - 0.5));
            if (n1 < n0) n0 = n1 = int(std::lround(c - 0.5));
            n0 = std::clamp(n0, 0, w - 1), n1 = std::clamp(n1, 0, w - 1);
            int sum[3] = {0, 0, 0};
            for (int n = n0; n <= n1; ++n)
                for (int ch = 0; ch < 3; ++ch) sum[ch] += row[n * 3 + ch];
            const int cnt = n1 - n0 + 1;
            uint8_t* o = &out[(size_t(y) * outW + k) * 3];
            for (int ch = 0; ch < 3; ++ch) o[ch] = uint8_t((sum[ch] + cnt / 2) / cnt);
        }
    }
}

// ------------------------------------------------------------ palette ---

// Distance used for the palette: brightness counts `lumaWeight` times as much
// as colour (the composite signal's brightness is much sharper and cleaner
// than its colour, which is smeared over ~3.5 C64 pixels).
inline double colourDistance(const float* a, const float* b, double lumaWeight = 2.0) {
    const double dy = lumaWeight * (a[0] - b[0]), du = a[1] - b[1], dv = a[2] - b[2];
    return dy * dy + du * du + dv * dv;
}
inline void toYuv(const uint8_t* c, float* o) {
    const float r = c[0], g = c[1], b = c[2];
    o[0] = 0.299f * r + 0.587f * g + 0.114f * b;
    o[1] = 0.492f * (b - o[0]);
    o[2] = 0.877f * (r - o[0]);
}
inline Colour fromYuv(const float* v) {
    const float y = v[0], b = y + v[1] / 0.492f, r = y + v[2] / 0.877f;
    const float g = (y - 0.299f * r - 0.114f * b) / 0.587f;
    auto c8 = [](float x) { return uint8_t(std::clamp(int(std::lround(x)), 0, 255)); };
    return {c8(r), c8(g), c8(b)};
}

// k-means over the pixels (up to 200,000 of them, evenly picked). `init`
// (e.g. a profile palette) gives the starting colours; without it the start
// is spread out deterministically (k-means++ with a fixed seed).
inline std::vector<Colour> learnPalette(const uint8_t* rgb, size_t count, int k, const std::vector<Colour>* init,
                                        int iterations = 15) {
    std::vector<Colour> result;
    if (count == 0 || k <= 0) return result;
    const size_t step = std::max<size_t>(1, count / 200000);
    std::vector<std::array<float, 3>> pts;
    for (size_t i = 0; i < count; i += step) {
        std::array<float, 3> v;
        toYuv(rgb + i * 3, v.data());
        pts.push_back(v);
    }
    std::vector<std::array<float, 3>> cen;
    if (init && !init->empty()) {
        // Start from the given colours (up to k); if there are fewer than k,
        // add the pixels farthest from all of them, so a colour that was not
        // in the picture before (a later scene) gets an entry of its own.
        for (size_t j = 0; j < init->size() && int(j) < k; ++j) {
            std::array<float, 3> v;
            toYuv((*init)[j].data(), v.data());
            cen.push_back(v);
        }
        while (int(cen.size()) < k) {
            size_t far = 0;
            double farD = -1;
            for (size_t i = 0; i < pts.size(); ++i) {
                double m = 1e30;
                for (const auto& c : cen) m = std::min(m, colourDistance(pts[i].data(), c.data()));
                if (m > farD) farD = m, far = i;
            }
            if (farD <= 1.0) break;  // every pixel is already (almost) on a colour
            cen.push_back(pts[far]);
        }
    } else {
        std::mt19937 rng(12345);
        cen.push_back(pts[rng() % pts.size()]);
        std::vector<double> d(pts.size());
        while (int(cen.size()) < k) {
            double total = 0;
            for (size_t i = 0; i < pts.size(); ++i) {
                double m = 1e30;
                for (const auto& c : cen) m = std::min(m, colourDistance(pts[i].data(), c.data()));
                d[i] = m;
                total += m;
            }
            if (total <= 0) break;
            double r = std::uniform_real_distribution<double>(0, total)(rng);
            size_t pick = 0;
            for (; pick + 1 < pts.size() && r > d[pick]; ++pick) r -= d[pick];
            cen.push_back(pts[pick]);
        }
    }
    std::vector<int> label(pts.size(), 0);
    for (int it = 0; it < iterations; ++it) {
        std::vector<std::array<double, 4>> acc(cen.size(), {0, 0, 0, 0});
        for (size_t i = 0; i < pts.size(); ++i) {
            int bi = 0;
            double bd = 1e30;
            for (size_t j = 0; j < cen.size(); ++j) {
                double dd = colourDistance(pts[i].data(), cen[j].data());
                if (dd < bd) bd = dd, bi = int(j);
            }
            label[i] = bi;
            for (int c = 0; c < 3; ++c) acc[bi][c] += pts[i][c];
            acc[bi][3] += 1;
        }
        for (size_t j = 0; j < cen.size(); ++j)
            if (acc[j][3] > 0)
                for (int c = 0; c < 3; ++c) cen[j][c] = float(acc[j][c] / acc[j][3]);
    }
    // Entries no pixel belongs to are dropped: a start colour the picture
    // never shows must not attract pixels later (a textbook C64 grey took
    // the decoded brown, 2026-09-29).
    std::vector<int> members(cen.size(), 0);
    for (int l : label) members[size_t(l)]++;
    for (size_t j = 0; j < cen.size(); ++j)
        if (members[j] > 0) result.push_back(fromYuv(cen[j].data()));
    return result;
}

// Replaces every pixel by the nearest palette colour. Returns the index per
// pixel if `indices` is given. Brightness weight 1 measured best for snapping
// (paltool grid tests: 88.7% vs 87.5% at 2 and 73% at 4 on C64-style cells);
// the learning uses 2.
inline void snapPalette(uint8_t* rgb, size_t count, const std::vector<Colour>& pal, std::vector<uint8_t>* indices = nullptr,
                        double lumaWeight = 1.0) {
    if (pal.empty()) return;
    std::vector<std::array<float, 3>> py(pal.size());
    for (size_t j = 0; j < pal.size(); ++j) toYuv(pal[j].data(), py[j].data());
    if (indices) indices->resize(count);
    for (size_t i = 0; i < count; ++i) {
        float v[3];
        toYuv(rgb + i * 3, v);
        size_t bi = 0;
        double bd = 1e30;
        for (size_t j = 0; j < pal.size(); ++j) {
            double d = colourDistance(v, py[j].data(), lumaWeight);
            if (d < bd) bd = d, bi = j;
        }
        rgb[i * 3] = pal[bi][0], rgb[i * 3 + 1] = pal[bi][1], rgb[i * 3 + 2] = pal[bi][2];
        if (indices) (*indices)[i] = uint8_t(bi);
    }
}

// ---------------------------------------------------------- calibration ---

// One global colour correction between the decoder's colours and a machine
// palette: brightness y' = contrast * y + offset, colour uv' = gain * e^{j hue} * uv.
struct Calibration {
    double contrast = 1, offset = 0;  // luma (0..255 units)
    double satGain = 1, hueRad = 0;   // colour
    void apply(const float* in, float* out) const {
        out[0] = float(contrast * in[0] + offset);
        const double c = std::cos(hueRad) * satGain, s = std::sin(hueRad) * satGain;
        out[1] = float(c * in[1] - s * in[2]);
        out[2] = float(s * in[1] + c * in[2]);
    }
};

// Fits the correction that brings the pixels onto the palette: pixels are
// matched to their nearest palette colour under the current correction,
// then brightness (least squares line) and colour (one complex gain) are
// refitted; ten rounds from "no correction". Only pixels close to some
// palette colour after matching count (the smeared colour edges don't).
inline Calibration fitCalibration(const uint8_t* rgb, size_t count, const std::vector<Colour>& pal) {
    Calibration cal;
    if (pal.empty() || count == 0) return cal;
    const size_t step = std::max<size_t>(1, count / 60000);
    std::vector<std::array<float, 3>> pts, py(pal.size());
    for (size_t i = 0; i < count; i += step) {
        std::array<float, 3> v;
        toYuv(rgb + i * 3, v.data());
        pts.push_back(v);
    }
    for (size_t j = 0; j < pal.size(); ++j) toYuv(pal[j].data(), py[j].data());
    for (int round = 0; round < 10; ++round) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
        std::complex<double> num = 0;
        double den = 0;
        long coloured = 0;  // pixels that carry colour (observed and matched): only they may set saturation/hue
        std::vector<double> dist;
        std::vector<int> match(pts.size());
        dist.reserve(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            float c[3];
            cal.apply(pts[i].data(), c);
            double bd = 1e30;
            int bj = 0;
            for (size_t j = 0; j < py.size(); ++j) {
                double d = colourDistance(c, py[j].data(), 1.0);
                if (d < bd) bd = d, bj = int(j);
            }
            match[i] = bj;
            dist.push_back(bd);
        }
        std::vector<double> sorted = dist;
        const size_t q = sorted.size() * 7 / 10;
        std::nth_element(sorted.begin(), sorted.begin() + long(q), sorted.end());
        const double keep = sorted[q];  // the best-matching 70%
        for (size_t i = 0; i < pts.size(); ++i) {
            if (dist[i] > keep) continue;
            const auto& t = py[size_t(match[i])];
            sx += pts[i][0], sy += t[0], sxx += double(pts[i][0]) * pts[i][0], sxy += double(pts[i][0]) * t[0], ++n;
            const std::complex<double> zo(pts[i][1], pts[i][2]), zt(t[1], t[2]);
            if (std::abs(zo) > 8 && std::abs(zt) > 8) {
                num += zt * std::conj(zo);
                den += std::norm(zo);
                ++coloured;
            }
        }
        if (n < 10) break;
        const double var = sxx - sx * sx / n;
        if (var > 1e-6) {
            cal.contrast = (sxy - sx * sy / n) / var;
            cal.offset = (sy - cal.contrast * sx) / n;
        }
        // Too few coloured pixels (a black-and-white credits screen): keep
        // the colour neutral rather than fit it to noise (seen on a 3 s clip).
        if (coloured >= 500 && den > 1e-6) {
            const std::complex<double> g = num / den;
            cal.satGain = std::abs(g);
            cal.hueRad = std::arg(g);
        } else {
            cal.satGain = 1, cal.hueRad = 0;
        }
        // A plausible decoder error only: fine colour detail can otherwise
        // drag the fit to "no colour at all" (seen on hires test pictures).
        cal.satGain = std::clamp(cal.satGain, 0.5, 2.0);
        cal.hueRad = std::clamp(cal.hueRad, -kPi / 6, kPi / 6);
        cal.contrast = std::clamp(cal.contrast, 0.6, 1.6);
        cal.offset = std::clamp(cal.offset, -60.0, 60.0);
    }
    return cal;
}

// Snaps with a calibration: each pixel is corrected, then replaced by the
// nearest palette colour; the output holds only palette colours.
inline void snapCalibrated(uint8_t* rgb, size_t count, const std::vector<Colour>& pal, const Calibration& cal,
                           std::vector<uint8_t>* indices = nullptr) {
    if (pal.empty()) return;
    std::vector<std::array<float, 3>> py(pal.size());
    for (size_t j = 0; j < pal.size(); ++j) toYuv(pal[j].data(), py[j].data());
    if (indices) indices->resize(count);
    for (size_t i = 0; i < count; ++i) {
        float v[3], c[3];
        toYuv(rgb + i * 3, v);
        cal.apply(v, c);
        size_t bi = 0;
        double bd = 1e30;
        for (size_t j = 0; j < pal.size(); ++j) {
            double d = colourDistance(c, py[j].data(), 1.0);
            if (d < bd) bd = d, bi = j;
        }
        rgb[i * 3] = pal[bi][0], rgb[i * 3 + 1] = pal[bi][1], rgb[i * 3 + 2] = pal[bi][2];
        if (indices) (*indices)[i] = uint8_t(bi);
    }
}

// ------------------------------------------------- C64 cell reconstruction ---

// The VIC-II draws in 8x8 character cells with 2 colours (hires, a colour
// per pixel from the pair) or 4 colours on double-wide pixels (multicolour).
// The composite colour is smeared over ~3.5 pixels, so pixels decided one
// by one lose fine colour detail (~50% right on hires/multicolour test
// cells). Deciding each cell's colour set first and then each pixel within
// it uses the machine's own rule (backlog 15).
//
// `sampled`: the grid-sampled picture (not snapped), `idx`: the per-pixel
// palette choice already made (for the cell alignment and the candidates).
// Writes the reconstructed picture into `out` (palette colours only).
inline void reconstructC64Cells(const uint8_t* sampled, const std::vector<uint8_t>& idx, int w, int h,
                                const std::vector<Colour>& pal, const Calibration& cal, uint8_t* out, int* alignX = nullptr,
                                int* alignY = nullptr, int candidates = 6, double lumaWeight = 4.0) {
    if (pal.size() < 2 || w < 16 || h < 16) return;
    // Cell alignment: colour changes sit on cell borders most often.
    long hx[8] = {0}, hy[8] = {0};
    for (int y = 0; y < h; ++y)
        for (int x = 1; x < w; ++x)
            if (idx[size_t(y) * w + x] != idx[size_t(y) * w + x - 1]) hx[x % 8]++;
    for (int y = 1; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (idx[size_t(y) * w + x] != idx[size_t(y - 1) * w + x]) hy[y % 8]++;
    const int ax = int(std::max_element(hx, hx + 8) - hx), ay = int(std::max_element(hy, hy + 8) - hy);
    if (alignX) *alignX = ax;
    if (alignY) *alignY = ay;
    std::vector<std::array<float, 3>> py(pal.size());
    for (size_t j = 0; j < pal.size(); ++j) toYuv(pal[j].data(), py[j].data());
    std::vector<std::array<float, 3>> pix(size_t(w) * h);
    {
        const double cr = std::cos(cal.hueRad) * cal.satGain, sr = std::sin(cal.hueRad) * cal.satGain;
        for (size_t i = 0; i < pix.size(); ++i) {
            float v[3];
            toYuv(sampled + i * 3, v);
            pix[i][0] = float(cal.contrast * v[0] + cal.offset);
            pix[i][1] = float(cr * v[1] - sr * v[2]);
            pix[i][2] = float(sr * v[1] + cr * v[2]);
        }
    }
    // Brightness is sharp, colour smeared: within a cell, pixels are told
    // apart mostly by brightness.
    auto d = [&](const std::array<float, 3>& a, size_t j) { return colourDistance(a.data(), py[j].data(), lumaWeight); };
    // Per cell, the distances of its pixels (and of its double-wide pixel
    // pairs) to each candidate colour are computed once; the colour sets
    // are then compared by table lookups only. Rows of cells on 8 threads.
    auto rowsOfCells = [&](int firstRow, int step) {
    for (int cy = ay - 8 + firstRow * 8; cy < h; cy += 8 * step)
        for (int cx = ax - 8; cx < w; cx += 8) {
            const int x0 = std::max(0, cx), x1 = std::min(w, cx + 8), y0 = std::max(0, cy), y1 = std::min(h, cy + 8);
            if (x1 - x0 < 2 || y1 - y0 < 1) continue;
            // Candidates: the most frequent per-pixel choices in the cell.
            std::vector<int> count(pal.size(), 0);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) count[idx[size_t(y) * w + x]]++;
            int cand[16];
            int nc = 0;
            for (int k = 0; k < std::min(candidates, 16); ++k) {
                int best = int(std::max_element(count.begin(), count.end()) - count.begin());
                if (count[size_t(best)] == 0) break;
                cand[nc++] = best;
                count[size_t(best)] = 0;
            }
            if (nc == 0) continue;
            // dPix[pixel][candidate], dPair[pair][candidate]
            float dPix[64][16], dPair[32][16];
            int np = 0, nq = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x, ++np)
                    for (int c = 0; c < nc; ++c) dPix[np][c] = float(d(pix[size_t(y) * w + x], size_t(cand[c])));
                for (int x = cx; x < cx + 8; x += 2, ++nq) {
                    const int xa = std::clamp(x, 0, w - 1), xb = std::clamp(x + 1, 0, w - 1);
                    std::array<float, 3> m;
                    for (int k = 0; k < 3; ++k)
                        m[size_t(k)] = 0.5f * (pix[size_t(y) * w + xa][size_t(k)] + pix[size_t(y) * w + xb][size_t(k)]);
                    for (int c = 0; c < nc; ++c) dPair[nq][c] = float(d(m, size_t(cand[c])));
                }
            }
            // Hires: the best pair.
            double hiCost = 1e30;
            int ha = 0, hb = 0;
            for (int a2 = 0; a2 < nc; ++a2)
                for (int b2 = a2; b2 < nc; ++b2) {
                    double c = 0;
                    for (int q = 0; q < np; ++q) c += std::min(dPix[q][a2], dPix[q][b2]);
                    if (c < hiCost) hiCost = c, ha = a2, hb = b2;
                }
            // Multicolour: the best 4 on double-wide pixels.
            double mcCost = 1e30;
            int mc[4] = {0, 0, 0, 0};
            if (nc >= 3)
                for (int a2 = 0; a2 < nc; ++a2)
                    for (int b2 = a2 + 1; b2 < nc; ++b2)
                        for (int c2 = b2 + 1; c2 < nc; ++c2)
                            for (int e = c2; e < nc; ++e) {
                                double c = 0;
                                for (int q = 0; q < nq; ++q)
                                    c += 2 * std::min(std::min(dPair[q][a2], dPair[q][b2]), std::min(dPair[q][c2], dPair[q][e]));
                                if (c < mcCost) mcCost = c, mc[0] = a2, mc[1] = b2, mc[2] = c2, mc[3] = e;
                            }
            // Multicolour only if clearly better (it has more freedom).
            const bool multi = mcCost < 0.8 * hiCost;
            int q = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x, ++q) {
                    int pick;
                    if (!multi) {
                        pick = cand[dPix[q][ha] <= dPix[q][hb] ? ha : hb];
                    } else {
                        const int pair = (y - y0) * 4 + ((x - cx) >> 1);
                        int bi = mc[0];
                        for (int k = 1; k < 4; ++k)
                            if (dPair[pair][mc[k]] < dPair[pair][bi]) bi = mc[k];
                        pick = cand[bi];
                    }
                    uint8_t* o = out + (size_t(y) * w + x) * 3;
                    o[0] = pal[size_t(pick)][0], o[1] = pal[size_t(pick)][1], o[2] = pal[size_t(pick)][2];
                }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < 8; ++t) pool.emplace_back(rowsOfCells, t, 8);
    for (auto& th : pool) th.join();
}

// ------------------------------------------------------ frame by frame ---

// The pixel grid and palette over a stream of decoded frames: the grid is
// found on the first frame with enough edges (the period searched around
// the profile's hint, or over the generic range), after that only its phase
// is followed (every `phaseEvery` frames); the palette is learned once from
// the first gridded frame (starting from the profile's colours, if any).
// redetect()/relearn() start again; setFixed() pins both (freshCoder's
// encode uses what its preview showed).
// TMS9918 (MSX1, ColecoVision, SG-1000) rule: every 8-pixel row segment of
// a character holds two colours (Graphics I/II; the 40-column text mode two
// for the whole screen). Composite softens one-pixel strokes so much that
// per-pixel snapping turns thin white strokes grey or light blue.
// Per segment the best pair of palette colours is chosen, weighting colours
// that also appear in wide areas of the frame (their full level is seen
// there), and each pixel takes the nearer of the two, mostly by brightness.
// `idx` = the per-pixel snap. Sprites (a third colour) are not modelled.
inline void reconstructTmsCells(const uint8_t* sampled, const std::vector<uint8_t>& idx, int w, int h,
                                const std::vector<Colour>& pal, const Calibration& cal, uint8_t* out,
                                double lumaWeight = 4.0, double widePrior = 20000.0) {
    const size_t np = pal.size();
    if (np < 2 || w < 16 || h < 1) return;
    // Segment alignment: colour changes sit on segment borders most often.
    long hx[8] = {0};
    for (int y = 0; y < h; ++y)
        for (int x = 1; x < w; ++x)
            if (idx[size_t(y) * w + x] != idx[size_t(y) * w + x - 1]) hx[x % 8]++;
    const int ax = int(std::max_element(hx, hx + 8) - hx);
    std::vector<std::array<float, 3>> py(np);
    for (size_t j = 0; j < np; ++j) toYuv(pal[j].data(), py[j].data());
    std::vector<std::array<float, 3>> pix(size_t(w) * h);
    {
        const double cr = std::cos(cal.hueRad) * cal.satGain, sr = std::sin(cal.hueRad) * cal.satGain;
        for (size_t i = 0; i < pix.size(); ++i) {
            float v[3];
            toYuv(sampled + i * 3, v);
            pix[i][0] = float(cal.contrast * v[0] + cal.offset);
            pix[i][1] = float(cr * v[1] - sr * v[2]);
            pix[i][2] = float(sr * v[1] + cr * v[2]);
        }
    }
    // Colours of wide areas: three equal snapped pixels in a row (horizontal
    // strokes and backgrounds show a colour at its full level).
    std::vector<double> wide(np, 0.0);
    double wideTotal = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 1; x + 1 < w; ++x) {
            const size_t i = size_t(y) * w + x;
            if (idx[i] == idx[i - 1] && idx[i] == idx[i + 1]) wide[idx[i]] += 1, wideTotal += 1;
        }
    // A cost for colours rarely seen wide (distances are squared 0-255 YUV
    // units: a one-pixel luma miss of 25 codes x lumaWeight 4 = 10,000).
    // 20,000 measured on synthetic MSX pictures: text 67.5 -> 97.8 %
    // right, 2-pixel graphics 44.7 -> 46.7 % (colour detail finer than the
    // composite colour bandwidth can't be resolved).
    std::vector<double> cost(np);
    for (size_t j = 0; j < np; ++j) cost[j] = widePrior * -std::log((wide[j] + 1.0) / (wideTotal + double(np)));
    auto d = [&](const std::array<float, 3>& a, size_t j) { return colourDistance(a.data(), py[j].data(), lumaWeight); };
    // Text mode (two colours for the whole screen): when two colours make up
    // nearly all wide areas, every segment uses that pair (per-segment
    // choices took the fringes' light blue for the text colour).
    // Counted only in segments with an edge (the picture's detail; a plain
    // margin or the black outside the machine's picture has none).
    std::vector<double> edgeWide(np, 0.0);
    double edgeTotal = 0;
    for (int y = 0; y < h; ++y)
        for (int cx = ax - 8; cx < w; cx += 8) {
            const int x0 = std::max(1, cx), x1 = std::min(w - 1, cx + 8);
            float lo = 1e9f, hi = -1e9f;
            for (int x = x0; x < x1; ++x) lo = std::min(lo, pix[size_t(y) * w + x][0]), hi = std::max(hi, pix[size_t(y) * w + x][0]);
            if (hi - lo < 30.0f) continue;
            for (int x = x0; x < x1; ++x) {
                const size_t i = size_t(y) * w + x;
                if (idx[i] == idx[i - 1] && idx[i] == idx[i + 1]) edgeWide[idx[i]] += 1, edgeTotal += 1;
            }
        }
    // The background: the most common colour there; the other colour of the
    // pair: the one that stands out most from it (count x distance; on a real
    // MSX the fringes' light blue was more common than the white text, but
    // close to the background). Colours nearer to one of the two than half
    // their distance count as its fringes.
    int g1 = -1, g2 = -1;
    for (size_t j = 0; j < np; ++j)
        if (g1 < 0 || edgeWide[j] > edgeWide[size_t(g1)]) g1 = int(j);
    // The other colour: the palette colour nearest to the average of the
    // pixels that stand out most from the background (top 5 % by distance, in
    // segments with an edge): the cores of the strokes. (Live, by count x
    // distance the fringes' light blue won.)
    if (g1 >= 0) {
        std::vector<std::pair<float, size_t>> far;
        for (int y = 0; y < h; ++y)
            for (int cx = ax - 8; cx < w; cx += 8) {
                const int x0 = std::max(0, cx), x1 = std::min(w, cx + 8);
                float lo = 1e9f, hi = -1e9f;
                for (int x = x0; x < x1; ++x) lo = std::min(lo, pix[size_t(y) * w + x][0]), hi = std::max(hi, pix[size_t(y) * w + x][0]);
                if (hi - lo < 30.0f) continue;
                for (int x = x0; x < x1; ++x) {
                    const size_t i = size_t(y) * w + x;
                    far.push_back({float(colourDistance(pix[i].data(), py[size_t(g1)].data(), lumaWeight)), i});
                }
            }
        if (far.size() >= 20) {
            const size_t keep = std::max<size_t>(5, far.size() / 100);  // top 1 %: the strokes' fullest level
            std::nth_element(far.begin(), far.begin() + long(keep), far.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            float m[3] = {0, 0, 0};
            for (size_t k = 0; k < keep; ++k)
                for (int c = 0; c < 3; ++c) m[c] += pix[far[k].second][size_t(c)] / float(keep);
            double bd = 1e30;
            for (size_t j = 0; j < np; ++j) {
                if (int(j) == g1) continue;
                const double dd = colourDistance(m, py[j].data(), lumaWeight);
                if (dd < bd) bd = dd, g2 = int(j);
            }
        }
    }
    bool globalPair = false;
    if (edgeTotal > 100 && g2 >= 0) {
        const double d12 = colourDistance(py[size_t(g1)].data(), py[size_t(g2)].data(), lumaWeight);
        double other = 0;
        for (size_t j = 0; j < np; ++j) {
            if (int(j) == g1 || int(j) == g2 || edgeWide[j] <= 0) continue;
            const double d1 = colourDistance(py[j].data(), py[size_t(g1)].data(), lumaWeight);
            const double d2 = colourDistance(py[j].data(), py[size_t(g2)].data(), lumaWeight);
            if (std::min(d1, d2) > 0.25 * d12) other += edgeWide[j];  // farther than half the distance (squared: a quarter)
        }
        globalPair = other <= 0.03 * edgeTotal && edgeWide[size_t(g1)] + edgeWide[size_t(g2)] >= 0.6 * edgeTotal;
    }
    if (std::getenv("TMS_DEBUG")) {
        static int once = 0;
        if (once++ < 1) {
            std::fprintf(stderr, "tms: edge-wide total %.0f:", edgeTotal);
            for (size_t j = 0; j < np; ++j)
                if (edgeWide[j] > 0) std::fprintf(stderr, " %zu:%.0f", j, edgeWide[j]);
            std::fprintf(stderr, "  global pair %s\n", globalPair ? "yes" : "no");
        }
    }
    for (int y = 0; y < h; ++y)
        for (int cx = ax - 8; cx < w; cx += 8) {
            const int x0 = std::max(0, cx), x1 = std::min(w, cx + 8);
            const int n = x1 - x0;
            if (n < 1) continue;
            // Candidates: the segment's per-pixel choices plus the frame's most common wide colours.
            std::vector<int> cand;
            for (int x = x0; x < x1; ++x) {
                const int k = idx[size_t(y) * w + x];
                if (std::find(cand.begin(), cand.end(), k) == cand.end()) cand.push_back(k);
            }
            {
                std::vector<int> order(np);
                for (size_t j = 0; j < np; ++j) order[j] = int(j);
                std::partial_sort(order.begin(), order.begin() + std::min<size_t>(4, np), order.end(),
                                  [&](int a, int b) { return wide[size_t(a)] > wide[size_t(b)]; });
                for (size_t k = 0; k < std::min<size_t>(4, np); ++k)
                    if (std::find(cand.begin(), cand.end(), order[k]) == cand.end()) cand.push_back(order[k]);
            }
            float dist[8][24];
            const int nc = std::min<int>(int(cand.size()), 24);
            for (int x = x0; x < x1; ++x)
                for (int c = 0; c < nc; ++c) dist[x - x0][c] = float(d(pix[size_t(y) * w + x], size_t(cand[size_t(c)])));
            double best = 1e30;
            int ba = 0, bb = 0;
            if (globalPair) {
                cand.assign({g1, g2});
                for (int x = x0; x < x1; ++x)
                    for (int c = 0; c < 2; ++c) dist[x - x0][c] = float(d(pix[size_t(y) * w + x], size_t(cand[size_t(c)])));
                ba = 0, bb = 1, best = -1;  // no search
            }
            for (int a = 0; a < nc && best >= 0; ++a)
                for (int b = a; b < nc; ++b) {
                    double s = cost[size_t(cand[size_t(a)])] + (b != a ? cost[size_t(cand[size_t(b)])] : 0.0);
                    for (int x = 0; x < n; ++x) s += std::min(dist[x][a], dist[x][b]);
                    if (s < best) best = s, ba = a, bb = b;
                }
            // Each pixel: when the two colours differ in brightness and the
            // segment spans a good part of that difference, by the segment's own
            // brightness range (a softened one-pixel stroke is still the
            // brightest pixel of its segment); otherwise the nearer colour.
            const float ya = py[size_t(cand[size_t(ba)])][0], yb = py[size_t(cand[size_t(bb)])][0];
            float smin = 1e9f, smax = -1e9f;
            for (int x = x0; x < x1; ++x) smin = std::min(smin, pix[size_t(y) * w + x][0]), smax = std::max(smax, pix[size_t(y) * w + x][0]);
            const bool byRange = ba != bb && std::abs(ya - yb) > 20.0f && smax - smin > 0.3f * std::abs(ya - yb);
            const float mid = 0.5f * (smin + smax);
            const int bright = ya > yb ? ba : bb, dark = ya > yb ? bb : ba;
            for (int x = x0; x < x1; ++x) {
                const int c = byRange ? (pix[size_t(y) * w + x][0] > mid ? bright : dark)
                                      : (dist[x - x0][ba] <= dist[x - x0][bb] ? ba : bb);
                const Colour& col = pal[size_t(cand[size_t(c)])];
                uint8_t* o = out + (size_t(y) * w + x) * 3;
                o[0] = col[0], o[1] = col[1], o[2] = col[2];
            }
        }
}

// Pixel phase from where thin bright strokes peak (their centres are pixel
// centres): for TMS9918 text, the edge-based phase sat ~1/3 pixel off on a
// real MSX and split one-pixel strokes between two pixels.
// Samples clearly above their row's mean count, weighted by how much.
inline bool brightPeakPhase(const uint8_t* rgb, int w, int h, double period, double& phase) {
    // The phase whose pixel centres give the sharpest picture: the spread of
    // the sampled brightness (central half of each pixel) around each row's
    // median, over 16 candidate phases (thin strokes sampled on their centre
    // stand out most). Every 2nd row.
    if (period < 2 || w < 4 * period) return false;
    std::vector<float> lum(static_cast<size_t>(w)), vals;
    double best = -1, bestPh = 0, worst = 1e30;
    std::vector<double> score(16, 0.0);
    for (int y = 0; y < h; y += 2) {
        const uint8_t* r = rgb + size_t(y) * w * 3;
        for (int x = 0; x < w; ++x) lum[size_t(x)] = float(0.299 * r[x * 3] + 0.587 * r[x * 3 + 1] + 0.114 * r[x * 3 + 2]);
        for (int k = 0; k < 16; ++k) {
            const double ph = period * k / 16.0;
            vals.clear();
            const int half = std::max(1, int(period / 4));
            for (double c = ph + period / 2; c + half < w; c += period) {
                const int ci = int(c);
                double a = 0;
                for (int x = ci - half; x < ci + half; ++x) a += lum[size_t(std::max(0, x))];
                vals.push_back(float(a / (2 * half)));
            }
            if (vals.size() < 3) continue;
            std::vector<float> sorted = vals;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double m = sorted[sorted.size() / 2];
            double v = 0;
            for (float x : vals) v += (x - m) * (x - m);
            score[size_t(k)] += v / vals.size();
        }
    }
    for (int k = 0; k < 16; ++k) {
        if (score[size_t(k)] > best) best = score[size_t(k)], bestPh = period * k / 16.0;
        worst = std::min(worst, score[size_t(k)]);
    }
    if (best <= 0 || best < 1.02 * worst) return false;  // no phase clearly sharper
    phase = bestPh;
    return true;
}

// Two-colour text, soft: a text screen has a background and one
// text colour. Both are found from the frame (background = the most common
// brightness; text = the brightest (or darkest) 0.5 %), snapped to the
// machine's palette if given, and every point of the full-resolution picture
// becomes a blend of the two by its brightness, stretched so the stroke cores
// reach the full text colour. No colour fringes can remain (only blends of the
// two colours exist), edges stay soft, nothing is decided per pixel (no
// flicker). For screens with more colours it isn't meant. `state` keeps the
// two levels steady over frames (pass the same vector every frame).
inline void softTwoColour(uint8_t* rgb, int w, int h, const std::vector<Colour>& pal, std::vector<double>& state) {
    const size_t n = size_t(w) * h;
    if (n < 100) return;
    // Integer brightness 0-255 (the output depends on it alone: a 256-entry
    // table does the per-pixel work; the double-precision version took ~20 ms
    // per 12x frame).
    auto lumOf = [](const uint8_t* p) { return (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8; };
    // The statistics from every 4th point (plenty for a frame's levels).
    const size_t step = 4, ns = (n + step - 1) / step;
    int hist[256] = {0};
    for (size_t i = 0; i < n; i += step) hist[lumOf(rgb + i * 3)]++;
    // Background: the most common brightness, ignoring near-black (the frame
    // around the machine's picture).
    int mode = 16;
    for (int v = 16; v < 256; ++v)
        if (hist[v] > hist[mode]) mode = v;
    // The text side: the brighter or darker extreme, whichever lies farther.
    long acc = 0;
    int lo = 0, hi = 255;
    for (int v = 0; v < 256; ++v) { acc += hist[v]; if (acc * 200 >= long(ns)) { lo = v; break; } }
    acc = 0;
    for (int v = 255; v >= 0; --v) { acc += hist[v]; if (acc * 200 >= long(ns)) { hi = v; break; } }
    const bool brightText = hi - mode >= mode - lo;
    long bg[3] = {0, 0, 0}, fg[3] = {0, 0, 0}, nb = 0, nf = 0;
    for (size_t i = 0; i < n; i += step) {
        const uint8_t* p = rgb + i * 3;
        const int l = lumOf(p);
        if (std::abs(l - mode) <= 3) bg[0] += p[0], bg[1] += p[1], bg[2] += p[2], ++nb;
        if (brightText ? l >= hi : l <= lo) fg[0] += p[0], fg[1] += p[1], fg[2] += p[2], ++nf;
    }
    if (nb < 1 || nf < 1) return;
    // Is it a two-colour screen? The share of points near the blend line
    // between the two observed colours (a colourful game has many points far
    // from it: then the frame is left as it is). Decided with hysteresis over
    // frames so it doesn't flicker on and off.
    double share = 0;
    {
        const double b0 = double(bg[0]) / nb, b1 = double(bg[1]) / nb, b2 = double(bg[2]) / nb;
        const double d0 = double(fg[0]) / nf - b0, d1 = double(fg[1]) / nf - b1, d2 = double(fg[2]) / nf - b2;
        const double dd = d0 * d0 + d1 * d1 + d2 * d2;
        long near = 0, total = 0;
        for (size_t i = 0; i < n; i += step) {
            const uint8_t* p = rgb + i * 3;
            if (lumOf(p) < 16) continue;  // the black around the picture
            const double x0 = p[0] - b0, x1 = p[1] - b1, x2 = p[2] - b2;
            const double tt = dd > 0 ? std::clamp((x0 * d0 + x1 * d1 + x2 * d2) / dd, 0.0, 1.0) : 0.0;
            const double e0 = x0 - tt * d0, e1 = x1 - tt * d1, e2 = x2 - tt * d2;
            near += e0 * e0 + e1 * e1 + e2 * e2 < 45.0 * 45.0;
            ++total;
        }
        share = total ? double(near) / total : 0;
    }
    double yb = mode, yf = brightText ? hi : lo;
    // Steady over frames: the levels follow slowly; state[2] = on/off.
    if (state.size() != 3) state = {yb, yf, share >= 0.85 ? 1.0 : 0.0};
    state[0] += 0.1 * (yb - state[0]), state[1] += 0.1 * (yf - state[1]);
    if (state[2] > 0.5 && share < 0.80) state[2] = 0;       // a colourful screen: off
    else if (state[2] < 0.5 && share > 0.90) state[2] = 1;  // a two-colour screen: on
    yb = state[0], yf = state[1];
    if (state[2] < 0.5 || std::abs(yf - yb) < 12) return;  // not a text screen, or no clear text
    Colour cb, cf;
    for (int c = 0; c < 3; ++c) cb[size_t(c)] = uint8_t(bg[c] / nb), cf[size_t(c)] = uint8_t(fg[c] / nf);
    auto nearest = [&](const Colour& x, double lumaWeight, bool brighterOnly) {
        if (pal.empty()) return x;
        float a[3];
        toYuv(x.data(), a);
        double bd = 1e30;
        Colour best = x;
        for (const Colour& p : pal) {
            float b[3];
            toYuv(p.data(), b);
            if (brighterOnly && b[0] < a[0]) continue;
            const double d = colourDistance(a, b, lumaWeight);
            if (d < bd) bd = d, best = p;
        }
        return best;
    };
    cb = nearest(cb, 2.0, false);
    // The text colour: snapped as if at full level (stroke cores never reach it on composite).
    cf = brightText ? nearest(cf, 0.3, true) : nearest(cf, 2.0, false);
    // The table: brightness -> blend, stretched (10-60 % of the way = full, smooth).
    uint8_t lut[256][3];
    for (int v = 0; v < 256; ++v) {
        double tt = std::clamp(((v - yb) / (yf - yb) - 0.1) / 0.5, 0.0, 1.0);
        tt = tt * tt * (3 - 2 * tt);
        for (int c = 0; c < 3; ++c) lut[v][c] = uint8_t(cb[size_t(c)] + tt * (double(cf[size_t(c)]) - cb[size_t(c)]) + 0.5);
    }
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = rgb + i * 3;
        const uint8_t* o = lut[lumOf(p)];
        p[0] = o[0], p[1] = o[1], p[2] = o[2];
    }
}

class GridStage {
public:
    struct Settings {
        double periodLo = 0, periodHi = 0;  // search range in samples
        double core = 0.5;                  // central part of each pixel that is averaged
        bool snap = false;
        int colours = 16;
        std::vector<Colour> startPalette;   // profile colours (may be empty)
        int phaseEvery = 25;
        // The palette is learned from a pool of pixels gathered over many
        // frames (one frame may show only a few of the colours): a sample
        // every `poolEvery` frames, re-learned every `relearnEvery` frames,
        // until `learnFrames` frames have been seen; then it stays.
        int poolEvery = 5, relearnEvery = 50, learnFrames = 500;
        // A given palette (the machine's or a custom one): the output uses
        // exactly these colours; the pixel pool then only calibrates the
        // decoder's colours onto them. Empty: learn the palette from the picture.
        std::vector<Colour> fixedPalette;
        bool c64Cells = false;  // decide colours per 8x8 cell (VIC-II rule) after snapping; needs a given 16-colour palette
        int cellCandidates = 6; // colours tried per cell: 6 (live, 9 ms) or 16 (offline: multicolour 78% vs 76%)
        bool tmsCells = false;  // two colours per 8-pixel row segment (TMS9918, MSX1) after snapping; needs a given palette
    };
    void configure(const Settings& s) {
        const bool rangeChanged = s.periodLo != set_.periodLo || s.periodHi != set_.periodHi;
        const bool paletteChanged = s.colours != set_.colours || s.startPalette != set_.startPalette ||
                                    s.fixedPalette != set_.fixedPalette;
        set_ = s;
        if (rangeChanged && !fixed_) grid_ = GridEstimate{}, width_ = 0;
        if (paletteChanged && !fixed_) palette_.clear(), pool_.clear(), seen_ = 0, cal_ = Calibration{};
    }
    void redetect() { grid_ = GridEstimate{}, fixed_ = false, gridPinned_ = false, width_ = 0, retry_ = 0; }
    void relearn() { palette_.clear(), pool_.clear(), seen_ = 0, cal_ = Calibration{}, fixed_ = false; }
    void setFixed(const GridEstimate& g, const std::vector<Colour>& pal, const Calibration& cal = Calibration{}) {
        grid_ = g, palette_ = pal, cal_ = cal, fixed_ = true;
    }
    const Calibration& calibration() const { return cal_; }
    // The grid only: the palette keeps learning.
    void pinGrid(const GridEstimate& g) { grid_ = g, gridPinned_ = true; }
    const GridEstimate& grid() const { return grid_; }
    const std::vector<Colour>& palette() const { return palette_; }

    // `rgb`: decoded picture, w x h packed RGB. Output: one pixel per machine
    // pixel (outW x h). False if no grid could be found (out untouched).
    bool process(const uint8_t* rgb, int w, int h, std::vector<uint8_t>& out, int& outW) {
        if (!grid_.valid) {
            // The full search costs ~20 ms: while a scene has too few edges
            // (a black title), try again only every `phaseEvery` frames.
            if (retry_ > 0) {
                --retry_;
                return false;
            }
            grid_ = estimateGrid(rgb, w, h, set_.periodLo, set_.periodHi);
            if (std::getenv("GRID_DEBUG"))
                std::fprintf(stderr, "grid: %dx%d range %.3f-%.3f -> period %.4f score %.3f valid %d\n", w, h, set_.periodLo,
                             set_.periodHi, grid_.period, grid_.score, int(grid_.valid));
            frames_ = 0;
            if (!grid_.valid) {
                retry_ = std::max(1, set_.phaseEvery);
                return false;
            }
            double ph = 0;
            if (set_.tmsCells && brightPeakPhase(rgb, w, h, grid_.period, ph)) grid_.phase = ph;
        } else if (!fixed_ && !gridPinned_ && ++frames_ % std::max(1, set_.phaseEvery) == 0) {
            // Same period, fresh phase.
            GridEstimate g = phaseAt(rgb, w, h, grid_.period);
            if (g.valid) grid_.phase = g.phase, grid_.score = g.score;
            double ph = 0;
            if (set_.tmsCells && brightPeakPhase(rgb, w, h, grid_.period, ph)) grid_.phase = ph;
        }
        sampleGrid(rgb, w, h, grid_, set_.core, out, outW, width_);
        width_ = outW;
        if (set_.snap && outW > 0) {
            const size_t n = size_t(outW) * h;
            const bool given = !set_.fixedPalette.empty();
            if (given && !fixed_) palette_ = set_.fixedPalette;
            if (!fixed_ && seen_ < set_.learnFrames) {
                if (seen_ % std::max(1, set_.poolEvery) == 0) addToPool(out.data(), outW, h);
                if (given) {
                    if (seen_ == 0 || seen_ % std::max(1, set_.relearnEvery) == 0)
                        cal_ = fitCalibration(pool_.data(), pool_.size() / 3, palette_);
                } else if (palette_.empty() || seen_ % std::max(1, set_.relearnEvery) == 0) {
                    // Start from the previous result (stable colours), or the profile's colours.
                    const std::vector<Colour>* init = !palette_.empty()           ? &palette_
                                                      : !set_.startPalette.empty() ? &set_.startPalette
                                                                                   : nullptr;
                    std::vector<Colour> start = init ? *init : std::vector<Colour>{};
                    palette_ = learnPalette(pool_.data(), pool_.size() / 3, set_.colours, init ? &start : nullptr);
                }
                ++seen_;
            }
            if (set_.c64Cells && given && palette_.size() == 16) {
                pre_.assign(out.begin(), out.begin() + long(n * 3));
                snapWithTable(out.data(), n, true, &idx_);
                reconstructC64Cells(pre_.data(), idx_, outW, h, palette_, cal_, out.data(), nullptr, nullptr,
                                    set_.cellCandidates);
            } else if (set_.tmsCells && given) {
                pre_.assign(out.begin(), out.begin() + long(n * 3));
                snapWithTable(out.data(), n, true, &idx_);
                reconstructTmsCells(pre_.data(), idx_, outW, h, palette_, cal_, out.data());
            } else {
                snapWithTable(out.data(), n, given || isCalibrated());
            }
        }
        return outW > 0;
    }

private:
    Settings set_;
    GridEstimate grid_;
    std::vector<Colour> palette_;
    bool fixed_ = false, gridPinned_ = false;
    int retry_ = 0;  // frames to wait before the next full grid search
    Calibration cal_;
    bool isCalibrated() const { return cal_.contrast != 1 || cal_.offset != 0 || cal_.satGain != 1 || cal_.hueRad != 0; }
    long frames_ = 0;
    int width_ = 0;  // output pixels per line, kept once found
    std::vector<uint8_t> pool_;  // RGB pixels gathered for learning the palette
    // Snapping through a table: each colour (6 bits per channel) is matched
    // once and then looked up; rebuilt when the palette or calibration
    // changes (per-pixel matching took ~20 ms per frame, too slow live).
    std::vector<int16_t> table_;
    std::vector<Colour> tablePalette_;
    Calibration tableCal_;
    bool tableCalibrated_ = false;
    std::vector<uint8_t> pre_, idx_;
    void snapWithTable(uint8_t* rgb, size_t n, bool calibrated, std::vector<uint8_t>* indices = nullptr) {
        if (indices) indices->resize(n);
        if (palette_.empty()) return;
        const bool calChanged = tableCal_.contrast != cal_.contrast || tableCal_.offset != cal_.offset ||
                                tableCal_.satGain != cal_.satGain || tableCal_.hueRad != cal_.hueRad;
        if (table_.empty() || tablePalette_ != palette_ || calChanged || tableCalibrated_ != calibrated) {
            table_.assign(size_t(1) << 18, int16_t(-1));
            tablePalette_ = palette_;
            tableCal_ = cal_;
            tableCalibrated_ = calibrated;
        }
        std::vector<std::array<float, 3>> py(palette_.size());
        bool pyReady = false;
        for (size_t i = 0; i < n; ++i) {
            uint8_t* p = rgb + i * 3;
            const size_t key = (size_t(p[0] >> 2) << 12) | (size_t(p[1] >> 2) << 6) | size_t(p[2] >> 2);
            int16_t idx = table_[key];
            if (idx < 0) {
                if (!pyReady) {
                    for (size_t j = 0; j < palette_.size(); ++j) toYuv(palette_[j].data(), py[j].data());
                    pyReady = true;
                }
                const uint8_t c8[3] = {uint8_t((p[0] & 0xFC) | 2), uint8_t((p[1] & 0xFC) | 2), uint8_t((p[2] & 0xFC) | 2)};
                float v[3], c[3];
                toYuv(c8, v);
                if (calibrated) cal_.apply(v, c);
                else c[0] = v[0], c[1] = v[1], c[2] = v[2];
                double bd = 1e30;
                for (size_t j = 0; j < palette_.size(); ++j) {
                    double d = colourDistance(c, py[j].data(), 1.0);
                    if (d < bd) bd = d, idx = int16_t(j);
                }
                table_[key] = idx;
            }
            const Colour& o = palette_[size_t(idx)];
            p[0] = o[0], p[1] = o[1], p[2] = o[2];
            if (indices) (*indices)[i] = uint8_t(idx);
        }
    }
    long seen_ = 0;              // frames seen while learning
    std::mt19937 rng_{777};
    void addToPool(const uint8_t* px, int w, int h) {
        // Up to 4,000 settled pixels per frame (both horizontal neighbours
        // within a few codes: not on a smeared colour edge), up to 150,000
        // in all (then random replacement).
        const size_t n = size_t(w) * h, cap = 150000;
        if (w < 3 || n == 0) return;
        size_t taken = 0;
        for (int tries = 0; tries < 40000 && taken < 4000; ++tries) {
            const size_t src = std::uniform_int_distribution<size_t>(0, n - 1)(rng_);
            const int x = int(src % size_t(w));
            if (x == 0 || x == w - 1) continue;
            bool settled = true;
            for (int ch = 0; ch < 3; ++ch) {
                const int c = px[src * 3 + ch];
                settled &= std::abs(c - px[(src - 1) * 3 + ch]) < 12 && std::abs(c - px[(src + 1) * 3 + ch]) < 12;
            }
            if (!settled) continue;
            ++taken;
            if (pool_.size() / 3 < cap) {
                pool_.insert(pool_.end(), px + src * 3, px + src * 3 + 3);
            } else {
                const size_t dst = std::uniform_int_distribution<size_t>(0, cap - 1)(rng_);
                std::copy(px + src * 3, px + src * 3 + 3, pool_.begin() + long(dst * 3));
            }
        }
    }
};

}  // namespace pal

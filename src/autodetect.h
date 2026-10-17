// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-detect for an unknown machine:
// explainable rules only, no learning. The app measures
// the source (driver timing, refresh, colour bursts on each wire, levels,
// pixel period, pixel repetition); propose() turns those measurements into a
// mode's settings, a confidence and one reason line per choice. Four paths:
// digital (DVI/HDMI), analog RGB, composite and S-Video (both on the
// Sync-on-Green input). The frame helpers below are the measurements that
// work on captured frames, so the same code runs live and in the tests.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "machines.h"
#include "presets.h"
#include "rgbtune.h"

namespace fv::detect {

enum class Path { None, Digital, AnalogRgb, Composite, SVideo };
const char* pathName(Path p);

// Colour burst on one wire (from pal::decode's measurement pass).
struct WireBurst {
    int colour = -1;            // 1 burst, 0 none, -1 not measured
    double amplitude = 0;       // codes
    double phaseStepDeg = 0;    // carrier phase advance per line (V-switch removed for PAL)
    double blankCode = 0;       // blanking level (codes)
};

struct Measurements {
    // From the driver.
    std::string input, signal;   // ENUMINPUT name, "Signal Type"
    bool noSignal = false;
    uint32_t width = 0, height = 0;  // detected active area (Horizontal/Vertical Timing defaults)
    int lineTotal = 0;           // detected samples per line (Horizontal Size default)
    double lineRateHz = 0;       // the driver's HorFreq (kernel log), 0 = unknown
    double refreshHz = 0;        // measured frame rate, 0 = unknown
    // The capture the measurements below were made on.
    int horizontalSize = 0;      // Horizontal Size in use
    int samplesPerCycle = 0;     // composite/S-Video: K (Horizontal Size / standard cycles per line), 0 = not a carrier-locked sampling
    // Composite / S-Video: bursts on red, green, blue (an rgb4 capture of the
    // Sync-on-Green input: Y + sync on green, colour on blue or red).
    WireBurst burst[3];
    double whiteClip = 0;        // fraction of samples at 255 (green)
    double blackClip = 0;        // fraction of lines whose blanking reads 0 (burst cut off)
    // Pixel period in samples of the measured capture (estimateGrid), 0 = none found.
    double pixelPeriod = 0, pixelScore = 0;
    // Digital / RGB: the picture inside the frame and its pixel repetition.
    PictureArea area;
    int repeatX = 1, repeatY = 1;
};

struct Reason {
    std::string what, value, why;
};

struct Proposal {
    Path path = Path::None;
    bool recognised = false;
    double confidence = 0;      // 0..1
    Preset settings;            // the mode to apply / save (format, size, controls, display)
    std::vector<Reason> reasons;
    std::vector<std::string> warnings;
    std::string timingLike;     // known machines with this timing (hint only)
    std::string summary() const;  // one line
};

// The path for an input: digital by the Signal Type, composite/S-Video on
// the Sync-on-Green input, analog RGB otherwise.
Path pathFor(const std::string& input, const std::string& signal);

// Carrier cycles per line from a rough value (colour subcarrier / line rate)
// and the measured phase step (degrees per line). PAL's step is known only up
// to half a cycle (V-switch), NTSC's exactly. Returns the candidate nearest
// `approx`; `distance` = how far it is from it (cycles).
double resolveCyclesPerLine(double approx, double stepDeg, bool exact, double* distance = nullptr);

// Rules: measurements -> settings. `machines` (may be null) gives the
// "timing like" hint; nothing is picked from it.
Proposal propose(const Measurements& m, const std::vector<Machine>* machines);

// ---- measurements on frames ----

// One wire of an rgb4 (Bgrx8) or g8 frame as a plane of bytes.
std::vector<uint8_t> extractWire(const FrameView& f, int wire /*0 red, 1 green, 2 blue*/);
// Burst on a plane sampled at K samples per carrier cycle; `burstAt` = the
// burst's sample index in the row (the standard position: burst start x
// sample rate - Horizontal Position), -1 = search for it. A burst counts only
// when both halves of the frame show the same carrier step.
WireBurst measureBurst(const uint8_t* plane, int w, int h, int K, bool ntsc, int burstAt);
// Horizontal and vertical pixel repetition (1-4) of a digital frame: the
// largest factor whose groups are uniform across nearly every edge.
void pixelRepetition(const FrameView& f, int& rx, int& ry);
// Pixel period (samples) of packed RGB rows; 0 if no grid was found.
double pixelPeriodRgb(const uint8_t* rgb, int w, int h, double lo, double hi, double* score = nullptr);
// Where to look for the machine pixel (samples), given the sample rate:
// 5-16 MHz pixels on a 15 kHz line (home computers and consoles drawing for a
// TV), twice that on a 31 kHz (line-doubled / monitor) one. A picture of
// double-wide pixels fits half the pixel clock as well as the real one
// (C64 multicolour), so the range is what tells them apart.
void pixelSearchRange(double sampleRateHz, double lineRateHz, double& lo, double& hi);
// Bgrx8 -> packed RGB.
std::vector<uint8_t> toRgb(const FrameView& f);

}  // namespace fv::detect

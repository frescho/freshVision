// SPDX-License-Identifier: GPL-3.0-or-later
// PAL composite: a test-signal generator and a software decoder, for the
// Sync-on-Green capture path;
//
//
// Written from the public PAL standard (ITU-R BT.470 / BT.1700 line timing,
// burst and V-switch), not from any vendor code. Samples are 8-bit codes of
// one channel, one row per video line, starting a fixed number of samples
// after the leading edge of H-sync, which is what the card's G8 capture
// delivers.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <cstring>
#include <random>
#include <thread>
#include <type_traits>
#include <vector>

namespace pal {

constexpr double kFsc = 4433618.75;   // PAL colour subcarrier, Hz
constexpr double kLineRate = 15625.0; // Hz (64 us per line)
constexpr double kPi = 3.14159265358979323846;

// The colour TV standards the decoder knows.
// PAL: V-switch, burst at 180 +/- 45 deg; NTSC-M: no
// V-switch, burst at 180 deg, 3.579545 MHz, 227.5 cycles per standard line
// (computers: C64 NTSC 227.5, Apple II and MSX1 228). Timing from the sync
// edge; levels in mV with blanking at 0.
enum class Standard { Pal, Ntsc };
struct StandardInfo {
    const char* name;
    double fsc;              // colour subcarrier, Hz
    double lineRate;         // nominal line rate, Hz
    double cyclesPerLine;    // nominal carrier cycles per line (the middle of what machines use)
    bool vSwitch;            // PAL's line-by-line V sign
    double burstStart, burstCycles;  // s after the sync edge, carrier cycles
    double activeStart, activeLen;   // s
    double syncMv, whiteMv, burstMv; // sync tip, white (100 %), burst peak
};
inline const StandardInfo& standardInfo(Standard s) {
    static const StandardInfo pal{"PAL", 4433618.75, 15625.0, 283.625, true, 5.6e-6, 10, 10.5e-6, 52e-6, -300, 700, 150};
    // SMPTE 170M: burst 5.3 us after the sync edge, 9 cycles; blanking 10.9 us;
    // 100 IRE = 714.3 mV, sync -40 IRE, burst 40 IRE p-p.
    static const StandardInfo ntsc{"NTSC", 315e6 / 88, 315e6 / 88 * 2 / 455, 227.75, false, 5.3e-6, 9, 10.9e-6, 52.6e-6,
                                   -285.7, 714.3, 142.86};
    return s == Standard::Ntsc ? ntsc : pal;
}
inline const char* standardName(Standard s) { return standardInfo(s).name; }
// Where the burst search should end, in samples of a capture window that starts
// `position` samples after the sync edge at `fs` samples/s: NTSC 0.6 us past
// the burst's standard end; 0 for PAL (the search runs to the picture as before).
inline int burstSearchEndFor(Standard s, double fs, int position) {
    if (s != Standard::Ntsc) return 0;
    const StandardInfo& si = standardInfo(s);
    return std::max(0, int(std::lround((si.burstStart + si.burstCycles / si.fsc + 0.6e-6) * fs)) - position);
}
// Black and white: where blanking is read (DecodeParams::burstStart with
// mono), the standard burst's place on the back porch, in samples of the
// capture window; -1 if the window starts after it.
inline int monoBlankStartFor(Standard s, double fs, int position) {
    const int at = int(std::lround(standardInfo(s).burstStart * fs)) - position;
    return at >= 0 ? at : -1;
}

// --- generator --------------------------------------------------------------

struct GenParams {
    int samplesPerCycle = 4;       // K: 4 = 4xFsc, 8 = 8xFsc
    int samplesPerLine = 1135;     // line-locked: sample rate = samplesPerLine * line rate
    double lineRate = kLineRate;   // the source's line rate (a C64 runs at ~15,639 Hz)
    double extraPpm = 0;           // extra sample-rate error on top of the line-locked rate
    int windowStart = 90;          // first sample, counted from the H-sync leading edge
    int width = 1024;              // samples per output row
    int lines = 288;
    double liftCode = 64;          // code of blanking (0 mV)
    double codesPerMv = 0.25;      // ADC gain
    double noiseCodes = 0;         // Gaussian noise, standard deviation in codes
    double jitterSamples = 0;      // per-line random start offset, +/- this many samples
    double chromaPhaseErrDeg = 0;  // picture chroma phase error against the burst (makes Hanover bars)
    double carrierPpm = 0;         // subcarrier frequency error (an unstable old machine), burst included
    bool chroma = true;            // false: luma only, no burst (like a Y-only signal)
    int firstLine = 0;             // source line number of the first row (V-switch and carrier phase)
    unsigned seed = 1;
    Standard standard = Standard::Pal;
    double setupIre = 0;           // NTSC black lift (7.5 in North America, 0 in Japan)
};

struct Rgb {
    double r, g, b;  // 0..1
};

// 75% colour bars, left to right: white yellow cyan green magenta red blue black.
inline Rgb colourBars(double xFrac) {
    static const Rgb bars[8] = {{.75, .75, .75}, {.75, .75, 0}, {0, .75, .75}, {0, .75, 0},
                                {.75, 0, .75},   {.75, 0, 0},   {0, 0, .75},   {0, 0, 0}};
    int i = std::min(7, std::max(0, int(xFrac * 8)));
    return bars[i];
}

// The two parts of the video signal in mV at time t (seconds since the
// H-sync leading edge of `line`), for an image given as a function of the
// fraction across the active line: `yMv` brightness with sync, `cMv` the
// modulated colour with its burst. Composite = their sum; S-Video carries
// them on two wires. `absTime` drives the continuously running subcarrier.
template <typename Image>
void componentsMv(const GenParams& p, const Image& image, int line, double t, double absTime, double& yMv, double& cMv) {
    const StandardInfo& si = standardInfo(p.standard);
    const double sync = 4.7e-6, burstStart = si.burstStart, burstLen = si.burstCycles / si.fsc;
    const double activeStart = si.activeStart, activeLen = si.activeLen;
    const double w = 2 * kPi * si.fsc * (1.0 + p.carrierPpm * 1e-6) * absTime;
    const int sw = si.vSwitch ? ((line % 2 == 0) ? 1 : -1) : 1;  // PAL V-switch
    yMv = cMv = 0;
    if (t < sync) {
        yMv = si.syncMv;
        return;
    }
    if (p.chroma && t >= burstStart && t < burstStart + burstLen) {
        if (si.vSwitch) {
            // Burst: U = -a, V = +a before switching (180 deg +/- 45 deg), 300 mV p-p.
            const double a = si.burstMv / std::sqrt(2.0);
            cMv = -a * std::sin(w) + sw * a * std::cos(w);
        } else {
            cMv = -si.burstMv * std::sin(w);  // NTSC: on -U (180 deg), 40 IRE p-p
        }
        return;
    }
    if (t < activeStart || t >= activeStart + activeLen) return;
    // An image is a function of the fraction across the active line, or of
    // that and the line number (for pictures that change down the frame).
    Rgb c;
    if constexpr (std::is_invocable_v<Image, double, int>) c = image((t - activeStart) / activeLen, line);
    else c = image((t - activeStart) / activeLen);
    double y = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    double u = 0.493 * (c.b - y), v = 0.877 * (c.r - y);
    const double setup = p.setupIre / 100.0;
    yMv = si.whiteMv * (setup + (1.0 - setup) * y);
    if (p.chroma) {
        const double e = p.chromaPhaseErrDeg * kPi / 180.0;
        cMv = si.whiteMv * (1.0 - setup) * (u * std::sin(w + e) + sw * v * std::cos(w + e));
    }
}

template <typename Image>
double compositeMv(const GenParams& p, const Image& image, int line, double t, double absTime) {
    double y, c;
    componentsMv(p, image, line, t, absTime, y, c);
    return y + c;
}

// Rows of 8-bit samples, as the card's G8 capture would deliver them.
template <typename Image>
std::vector<uint8_t> generate(const GenParams& p, const Image& image) {
    std::vector<uint8_t> out(size_t(p.width) * p.lines);
    std::mt19937 rng(p.seed);
    std::normal_distribution<double> noise(0.0, p.noiseCodes > 0 ? p.noiseCodes : 1.0);
    std::uniform_real_distribution<double> jit(-p.jitterSamples, p.jitterSamples);
    const double fs = p.samplesPerLine * p.lineRate * (1.0 + p.extraPpm * 1e-6);
    for (int l = 0; l < p.lines; ++l) {
        double shift = p.jitterSamples > 0 ? jit(rng) : 0.0;
        const int srcLine = l + p.firstLine;
        for (int x = 0; x < p.width; ++x) {
            double t = (p.windowStart + x + shift) / fs;       // since this line's sync edge
            double absTime = srcLine / p.lineRate + t;          // subcarrier keeps running across lines
            double code = p.liftCode + p.codesPerMv * compositeMv(p, image, srcLine, t, absTime);
            if (p.noiseCodes > 0) code += noise(rng);
            out[size_t(l) * p.width + x] = uint8_t(std::lround(std::min(255.0, std::max(0.0, code))));
        }
    }
    return out;
}

// S-Video: two planes of rows as a 3-channel capture would deliver them with
// the brightness wire on one input and the colour wire on another (sampled
// on one clock). The colour wire has no black level: its zero sits at
// `chromaLiftCode` (what the input's offset makes of it).
struct YcPlanes {
    std::vector<uint8_t> y, c;
};
template <typename Image>
YcPlanes generateYC(const GenParams& p, const Image& image, double chromaLiftCode = 128) {
    YcPlanes out;
    out.y.resize(size_t(p.width) * p.lines);
    out.c.resize(size_t(p.width) * p.lines);
    std::mt19937 rng(p.seed);
    std::normal_distribution<double> noise(0.0, p.noiseCodes > 0 ? p.noiseCodes : 1.0);
    const double fs = p.samplesPerLine * p.lineRate * (1.0 + p.extraPpm * 1e-6);
    auto code = [](double c) { return uint8_t(std::lround(std::min(255.0, std::max(0.0, c)))); };
    for (int l = 0; l < p.lines; ++l) {
        const int srcLine = l + p.firstLine;
        for (int x = 0; x < p.width; ++x) {
            const double t = (p.windowStart + x) / fs;
            const double absTime = srcLine / p.lineRate + t;
            double ymv, cmv;
            componentsMv(p, image, srcLine, t, absTime, ymv, cmv);
            double yc = p.liftCode + p.codesPerMv * ymv, cc = chromaLiftCode + p.codesPerMv * cmv;
            if (p.noiseCodes > 0) yc += noise(rng), cc += noise(rng);
            out.y[size_t(l) * p.width + x] = code(yc);
            out.c[size_t(l) * p.width + x] = code(cc);
        }
    }
    return out;
}

// --- decoder ----------------------------------------------------------------

// How the brightness (luma) is taken from the composite samples:
//  Box   - average over one subcarrier cycle: removes the carrier completely,
//          but is a strong low-pass (soft picture).
//  Notch - the samples minus the colour signal's repeating waveform,
//          averaged at the same carrier phase over ~4 neighbouring cycles:
//          removes narrow bands at the subcarrier and its harmonics (a C64's
//          colour signal isn't a pure sine), keeps the rest of the detail
//          (sharp; faint echoes around sharp colour changes).
//  Off   - the samples themselves: sharpest, the carrier shows as dots.
enum class LumaFilter { Box, Notch, Off };

struct DecodeParams {
    int samplesPerCycle = 4;     // K
    int burstStart = -1;         // sample index of the burst window; -1 = find it automatically
    int burstLen = 32;           // samples used to measure the burst (a multiple of K)
    int activeStart = 0;         // first output sample
    int activeLen = -1;          // -1 = to the end of the row
    double killerCodes = 3.0;    // burst amplitude (codes) below this: no colour
    double fallbackMvPerCode = 4.0;  // gain when there is no burst to calibrate against
    bool averageLines = false;   // PAL delay-line method: colour = mean of this and the previous line
    LumaFilter luma = LumaFilter::Box;
    int forceParity = -1;        // 0/1: use this V-switch parity (a caller holding it steady); -1: decide
    int samplesPerLine = 0;      // the card's Horizontal Size; decides the parity (see step 3); 0 = unknown
    // Software PLL input (freshCoder): the carrier-frequency error of each
    // line, in radians of carrier phase per line (the measured step minus
    // the expected one, smoothed by the caller). Applied as a phase ramp
    // along the line, from the burst on. Null = none. Needs samplesPerLine.
    const float* lineFreqErr = nullptr;
    // Picture controls (applied before the conversion to RGB):
    double brightness = 0;       // added to luma (0..1 = black..white)
    double contrast = 1;         // luma and chroma gain
    double saturation = 1;       // chroma gain
    double hueDeg = 0;           // chroma rotation
    double gamma = 1;            // RGB gamma (1 = none; >1 brightens mid-tones)
    bool measureOnly = false;    // only fill DecodeInfo (burst, parity, step errors): the PLL's first pass
    // S-Video (Y/C): the colour wire's samples, same layout as the main
    // plane, which then holds the brightness wire. Burst, parity and colour
    // come from this plane; brightness is taken as it is (no carrier in it,
    // so no filter: the full detail). Null = composite.
    const uint8_t* chromaPlane = nullptr;
    // Y/C: mV per code of the brightness plane; 0 = the same as the colour
    // plane's (calibrated on its burst; right when both inputs have the
    // same gain).
    double lumaMvPerCode = 0;
    // A fixed pattern locked to the line, in codes per sample of the main
    // plane (`width` values), subtracted from it before decoding (Y/C: from
    // the brightness wire only). The C64's CPU clock leaks into its video as
    // such a pattern: stripes one character column wide. Learned by
    // LinePattern; null = none. Nothing is filtered, so sharpness stays.
    const float* linePattern = nullptr;
    int threads = 1;             // lines are split across this many threads
    Standard standard = Standard::Pal;  // NTSC: no V-switch, burst at 180 deg
    double setupIre = 0;         // NTSC black lift taken off (7.5: black reads as black)
    // Black from each line's own blanking (the burst window's mean) instead of
    // the frame's average: for a card whose black level jumps within a frame
    // (an Apple IIc whose frames the card re-acquires). Not used when
    // the burst is clipped at black (no blanking level to read).
    bool lineClamp = false;
    // The burst window search ends here (first sample past it); 0 = at the
    // active picture. NTSC sets it from the standard's burst timing: an MSX1
    // has a strongly coloured border right after a weak burst, and the search
    // took the border.
    int burstSearchEnd = 0;
    // The source's burst relative to the standard's (1 = 40 IRE NTSC / 300 mV
    // PAL p-p). The gain comes from the burst, so a weak burst made the
    // picture far too bright: a Casio MX-10's TMS9918 sends ~11 IRE.
    double burstScale = 1.0;
    // Colour smoothing: the demodulated colour is averaged over this many
    // carrier cycles (whole cycles cancel the carrier). 2 = the sharpest;
    // more fades the cross-colour fringes of fine brightness detail (an MSX1's
    // text) at the cost of colour resolution, like a TV's narrow
    // colour filter.
    int chromaCycles = 2;
    // Edge colour reduction: colour is reduced where the brightness has fine
    // detail (the cross-colour fringes sit exactly there; white text on a
    // coloured background has no colour of its own); flat areas keep theirs.
    // 0 = off; 1 = strong.
    double edgeDesat = 0;
    // Black and white source (no colour burst, e.g. a Microkey Primo): no
    // burst search (blanking is read at burstStart, or just before the
    // active picture), no colour, brightness taken unfiltered (there is no
    // carrier to remove), black per line. Gain = fallbackMvPerCode.
    bool mono = false;
};

struct DecodeInfo {
    int burstStart = 0;
    double burstAmplitude = 0;   // burst peak amplitude in codes, mean over lines (150 mV nominal)
    double blankCode = 0;        // mean of the burst window = blanking level
    int parity = 0;              // which lines carry V-switch +1
    double phaseStepDeg = 0;     // carrier phase advance per line, measured
    bool colour = false;
    bool clipped = false;        // burst cut off at blanking (blanking taken as code 0, amplitude doubled)
    // The parity could not be decided from the expected step (no
    // samplesPerLine given, or both candidates equally far from it).
    bool parityAmbiguous = false;
    double expectedStepDeg = 0;  // from samplesPerLine, when given
    // Per line: measured step minus expected (radians; line 0 = 0), when
    // samplesPerLine is given and there is colour. The software PLL's input.
    std::vector<float> stepError;
    double meanStepErrorDeg = 0;
};

// Decodes `lines` rows of `width` samples into packed RGB (3 bytes per output
// pixel, one output pixel per input sample of the active range), into `out`
// (resized as needed, so a caller decoding every frame reuses one buffer).
// Returns the output width. The carrier is a table of K complex values, and
// luma a running sum, so a 1024x298 field decodes in a few milliseconds.
// Learns a fixed pattern locked to the line (DecodeParams::linePattern) from
// flat parts of the picture: per sample position, the mean deviation from
// the local level over many lines and frames. Only parts that are flat
// (spread below `flatCodes`) and not clipped count, so picture detail stays
// out of it; a part at the same place on every line (a static screen) is
// only used where it is flat. The result is high-passed (the pattern's own
// level is not a picture change) and used once every position has enough
// samples. It follows slow changes: old samples are halved as new ones come.
class LinePattern {
public:
    static constexpr int kSeg = 48;          // flatness is judged per segment of this many samples
    static constexpr int kMinCount = 150;    // samples per position before it is used
    static constexpr int kMaxCount = 6000;   // then older samples count half
    double flatCodes = 3.0;

    void reset() { width_ = 0, sum_.clear(), cnt_.clear(), pat_.clear(), segments_ = 0, ready_ = false; }
    // One frame of the plane (`lines` rows of `width`); only [x0, x1) is looked at.
    void learn(const uint8_t* plane, int width, int lines, int x0, int x1) {
        if (width != width_) {
            reset();
            width_ = width;
            sum_.assign(size_t(width), 0.0);
            cnt_.assign(size_t(width), 0.0);
        }
        x0 = std::max(0, x0), x1 = std::min(width, x1);
        for (int l = 0; l < lines; ++l) {
            const uint8_t* r = plane + size_t(l) * width;
            // Segment starts move from line to line, so their edges don't leave a mark.
            for (int s = x0 + (l * 17) % kSeg; s + kSeg <= x1; s += kSeg) {
                double m = 0, q = 0;
                int lo = 255, hi = 0;
                for (int x = s; x < s + kSeg; ++x) m += r[x], q += double(r[x]) * r[x], lo = std::min<int>(lo, r[x]), hi = std::max<int>(hi, r[x]);
                m /= kSeg;
                if (lo <= 1 || hi >= 254 || q / kSeg - m * m > flatCodes * flatCodes) continue;
                for (int x = s; x < s + kSeg; ++x) sum_[size_t(x)] += r[x] - m, cnt_[size_t(x)] += 1;
                ++segments_;
            }
        }
        double mx = 0;
        for (double c : cnt_) mx = std::max(mx, c);
        if (mx > kMaxCount)
            for (size_t i = 0; i < sum_.size(); ++i) sum_[i] *= 0.5, cnt_[i] *= 0.5;
        update();
    }
    bool ready() const { return ready_; }
    const float* data() const { return ready_ ? pat_.data() : nullptr; }
    long segments() const { return segments_; }
    double rms() const { return rms_; }  // of the pattern, codes
    double coverage() const { return coverage_; }  // share of positions with enough samples

private:
    void update() {
        const size_t n = sum_.size();
        std::vector<double> raw(n, 0.0);
        std::vector<char> ok(n, 0);
        size_t good = 0;
        for (size_t i = 0; i < n; ++i)
            if (cnt_[i] >= kMinCount) raw[i] = sum_[i] / cnt_[i], ok[i] = 1, ++good;
        coverage_ = n ? double(good) / double(n) : 0;
        // High-pass: minus the mean over +-kSeg known positions.
        pat_.assign(n, 0.0f);
        double sq = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!ok[i]) continue;
            double a = 0;
            int c = 0;
            for (size_t j = i >= size_t(kSeg) ? i - size_t(kSeg) : 0; j < std::min(n, i + size_t(kSeg) + 1); ++j)
                if (ok[j]) a += raw[j], ++c;
            pat_[i] = float(raw[i] - a / c);
            sq += double(pat_[i]) * pat_[i];
        }
        rms_ = good ? std::sqrt(sq / double(good)) : 0;
        ready_ = good > 0;
    }
    int width_ = 0;
    std::vector<double> sum_, cnt_;
    std::vector<float> pat_;
    long segments_ = 0;
    bool ready_ = false;
    double rms_ = 0, coverage_ = 0;
};

// Re-aligns frames whose first line wanders: a source whose vertical sync
// the card doesn't hold (an Apple IIc on Sync-on-Green: every delivered frame
// starts at another line). Capture the whole frame (Vertical
// Timing = the source's line count) with the black lifted above sync level
// (as the composite auto-tune does); then each frame's vertical sync shows
// as a band of rows at sync level. The rows are rotated so that band comes
// first. A frame without such a band is matched against the last aligned
// frame's row profile; if that doesn't fit either, it is shown as delivered
// (a steady source passes through untouched).
class VerticalRelock {
public:
    // `plane` (and `chroma`, if given: S-Video's colour plane) are rotated in
    // place; only columns [x0, x1) are looked at. True if the frame is aligned.
    bool process(uint8_t* plane, int width, int lines, int x0, int x1, uint8_t* chroma = nullptr) {
        x0 = std::max(0, x0), x1 = std::min(width, std::max(x0 + 1, x1));
        const int n = (x1 - x0 + 1) / 2;
        // Levels: sync = the frame's lowest codes, black = the median of each
        // row's 10th percentile; a sample counts as sync below the midpoint.
        std::vector<int> hist(256, 0);
        std::vector<int> rowP10(static_cast<size_t>(lines));
        std::vector<uint8_t> tmp(static_cast<size_t>(n));
        for (int y = 0; y < lines; ++y) {
            const uint8_t* r = plane + size_t(y) * width;
            for (int i = 0; i < n; ++i) tmp[size_t(i)] = r[x0 + 2 * i], ++hist[r[x0 + 2 * i]];
            std::nth_element(tmp.begin(), tmp.begin() + n / 10, tmp.end());
            rowP10[size_t(y)] = tmp[size_t(n / 10)];
        }
        long acc = 0, total = long(lines) * n;
        int low = 0;
        for (int v = 0; v < 256; ++v) {
            acc += hist[size_t(v)];
            if (acc * 200 >= total) { low = v; break; }  // 0.5th percentile: the sync band is only ~1.5 % of a frame
        }
        std::vector<int> sortedP10 = rowP10;
        std::nth_element(sortedP10.begin(), sortedP10.begin() + lines / 2, sortedP10.end());
        const int black = sortedP10[size_t(lines / 2)];
        const int threshold = low + (black - low) / 2;
        std::vector<double> prof(static_cast<size_t>(lines));
        std::vector<char> solid(static_cast<size_t>(lines), 0);
        for (int y = 0; y < lines; ++y) {
            const uint8_t* r = plane + size_t(y) * width;
            int below = 0;
            double sum = 0;
            for (int i = 0; i < n; ++i) below += r[x0 + 2 * i] < threshold, sum += r[x0 + 2 * i];
            solid[size_t(y)] = black - low >= 16 && below * 2 > n;  // needs a clear gap between sync and black
            prof[size_t(y)] = sum / n;
        }
        // The longest circular run of sync rows.
        int bestStart = -1, bestLen = 0;
        for (int y = 0; y < lines; ++y) {
            if (!solid[size_t(y)] || solid[size_t((y + lines - 1) % lines)]) continue;  // run starts here
            int len = 0;
            while (len < lines && solid[size_t((y + len) % lines)]) ++len;
            if (len > bestLen) bestLen = len, bestStart = y;
        }
        int offset = -1;
        if (bestLen >= 3 && bestLen < lines / 4) {
            offset = bestStart;
            how_ = "sync rows";
        } else if (!ref_.empty() && int(ref_.size()) == lines) {
            // No sync band in this frame: the shift that best matches the last aligned frame.
            double best = 1e18;
            int bs = 0;
            for (int s = 0; s < lines; ++s) {
                double d = 0;
                for (int y = 0; y < lines; y += 2) d += std::abs(prof[size_t((y + s) % lines)] - ref_[size_t(y)]);
                if (d < best) best = d, bs = s;
            }
            if (best / (lines / 2) < 2.0) offset = bs, how_ = "matched";  // within 2 codes per row on average
        }
        if (offset < 0) {
            // No sync band and no match: shown as delivered (a steady source,
            // e.g. a card that holds the sync, passes through untouched; it
            // used to be kept back, which froze a locked text screen).
            how_ = "shown as is";
            ++missed_;
            return true;
        }
        rotate(plane, width, lines, offset);
        if (chroma) rotate(chroma, width, lines, offset);
        ref_.assign(size_t(lines), 0.0);
        for (int y = 0; y < lines; ++y) ref_[size_t(y)] = prof[size_t((y + offset) % lines)];
        offset_ = offset;
        ++aligned_;
        return true;
    }
    void reset() { ref_.clear(), aligned_ = missed_ = 0, offset_ = 0, how_.clear(); }
    int offset() const { return offset_; }
    const std::string& how() const { return how_; }
    long aligned() const { return aligned_; }
    long missed() const { return missed_; }

private:
    void rotate(uint8_t* p, int width, int lines, int offset) {
        if (offset == 0) return;
        buf_.resize(size_t(width) * lines);
        for (int y = 0; y < lines; ++y)
            std::memcpy(&buf_[size_t(y) * width], p + size_t((y + offset) % lines) * width, size_t(width));
        std::memcpy(p, buf_.data(), buf_.size());
    }
    std::vector<double> ref_;
    std::vector<uint8_t> buf_;
    int offset_ = 0;
    long aligned_ = 0, missed_ = 0;
    std::string how_;
};

inline int decodeInto(const uint8_t* s, int width, int lines, const DecodeParams& dp, std::vector<uint8_t>& out,
                      DecodeInfo* info) {
    const int K = dp.samplesPerCycle;
    const bool yc = dp.chromaPlane != nullptr;
    const uint8_t* cs = yc ? dp.chromaPlane : s;  // where the colour (and its burst) is
    std::vector<std::complex<double>> carrier(K);  // e^{-j w m} depends only on m mod K
    for (int m = 0; m < K; ++m) carrier[m] = std::polar(1.0, -2 * kPi * m / K);
    const std::complex<double> j(0, 1);
    auto demodAt = [&](const uint8_t* row, int start, int len, double dc) {
        double zr = 0, zi = 0;
        for (int m = start; m < start + len; ++m) {
            double d = double(row[m]) - dc;
            zr += d * carrier[m % K].real();
            zi += d * carrier[m % K].imag();
        }
        return std::complex<double>(zr, zi) * (2.0 / double(len));
    };
    auto mean = [&](const uint8_t* row, int start, int len) {
        double a = 0;
        for (int m = start; m < start + len; ++m) a += row[m];
        return a / len;
    };

    // 1. Burst window: where the carrier-frequency energy is strongest,
    //    averaged over all lines, searched only *before* the active picture
    //    (picture edges have carrier-frequency energy too, and a luma-only
    //    signal must end up with no burst at all).
    int bStart = dp.burstStart;
    if (bStart < 0 && dp.mono) bStart = std::max(0, std::min(dp.activeStart, width) - dp.burstLen - K);
    if (bStart < 0) {
        double best = -1;
        int limit = dp.activeStart > dp.burstLen ? dp.activeStart : std::min(width, 40 * K);
        if (dp.burstSearchEnd > dp.burstLen) limit = std::min(limit, dp.burstSearchEnd);
        bStart = 0;
        for (int st = 0; st + dp.burstLen <= limit; ++st) {
            double e = 0;
            for (int l = 0; l < lines; l += std::max(4, K)) {  // fewer lines at high rates: the cost grows with K^2
                const uint8_t* row = cs + size_t(l) * width;
                e += std::abs(demodAt(row, st, dp.burstLen, mean(row, st, dp.burstLen)));
            }
            if (e > best) best = e, bStart = st;
        }
    }

    // 2. Per-line burst phase and amplitude; blanking = the burst's mean.
    std::vector<double> alpha(lines), amp(lines);
    double blank = 0, ampSum = 0, lumaBlank = 0;
    std::vector<double> lineBlank(static_cast<size_t>(lines));  // per line, on the brightness plane
    for (int l = 0; l < lines; ++l) {
        if (yc) {
            lineBlank[size_t(l)] = mean(s + size_t(l) * width, bStart, dp.burstLen);  // no burst on the brightness wire
            lumaBlank += lineBlank[size_t(l)];
        }
        const uint8_t* row = cs + size_t(l) * width;
        double dc = mean(row, bStart, dp.burstLen);
        if (!yc) lineBlank[size_t(l)] = dc;
        std::complex<double> zb = demodAt(row, bStart, dp.burstLen, dc);
        alpha[l] = std::arg(zb);
        amp[l] = std::abs(zb);  // burst peak amplitude in codes (the burst is 150 mV peak)
        blank += dc;
        ampSum += amp[l];
    }
    blank /= lines;
    lumaBlank /= lines;
    double burstAmp = ampSum / lines;
    // Clipped burst: with blanking clamped to code 0, the burst's lower half
    // is cut off (seen on the real Sync-on-Green capture). A sine cut
    // at its midpoint keeps exactly half its fundamental, and blanking is 0.
    size_t zeros = 0, total = 0;
    for (int l = 0; l < lines; ++l)
        for (int m = bStart; m < bStart + dp.burstLen; ++m) zeros += (cs[size_t(l) * width + m] == 0), ++total;
    bool clipped = total && zeros * 3 > total;  // a cut sine is 0 for about half its samples
    if (clipped) {
        burstAmp *= 2.0;
        blank = 0.0;
    }
    bool colour = !dp.mono && burstAmp >= dp.killerCodes;

    // 3. V-switch parity: the burst angle is psi+45 deg on +1 lines and
    //    psi+135 deg on -1 lines. Both assignments give a perfectly steady
    //    line-to-line step, 180 deg apart (the wrong one adds 180 deg), so the
    //    steadiness can't choose (it used to be a tie broken by rounding
    //    noise: 1.7% of C64 frames came out with swapped colour axes).
    //    What decides it: with sampling locked to
    //    K x the subcarrier, the true step is the fractional part of the
    //    carrier cycles per line, samplesPerLine / K (C64 1134/4 = 283.5 ->
    //    180 deg; standard PAL 1135/4 = 283.75 -> -90 deg). The parity whose
    //    measured step is nearer to that wins.
    auto wrap = [](double a) { while (a > kPi) a -= 2 * kPi; while (a <= -kPi) a += 2 * kPi; return a; };
    const StandardInfo& si = standardInfo(dp.standard);
    // The burst's angle against the U axis: PAL 135/225 deg by line, NTSC 180.
    auto psiFor = [&](int par, int l) {
        if (!si.vSwitch) return alpha[l] - kPi / 2;
        return alpha[l] - (((l + par) % 2 == 0) ? kPi / 4 : 3 * kPi / 4);
    };
    int parity = 0;
    double stepDeg = 0, expectedDeg = 0;
    bool ambiguous = false;
    std::vector<double> psi(lines);
    if (colour) {
        double var[2], stepOf[2];
        for (int par = 0; par < 2; ++par) {
            std::complex<double> accum = 0;
            for (int l = 1; l < lines; ++l) accum += std::exp(j * wrap(psiFor(par, l) - psiFor(par, l - 1)));
            var[par] = 1.0 - std::abs(accum) / (lines - 1);  // circular variance of the step
            stepOf[par] = std::arg(accum);
        }
        if (!si.vSwitch) {
            // NTSC: no V-switch, so no parity to decide.
            parity = 0;
            if (dp.samplesPerLine > 0) {
                double cycles = double(dp.samplesPerLine) / K;
                expectedDeg = wrap(2 * kPi * (cycles - std::floor(cycles))) * 180 / kPi;
            }
        } else if (dp.samplesPerLine > 0) {
            double cycles = double(dp.samplesPerLine) / K;
            double expected = wrap(2 * kPi * (cycles - std::floor(cycles)));
            expectedDeg = expected * 180 / kPi;
            double d0 = std::abs(wrap(stepOf[0] - expected)), d1 = std::abs(wrap(stepOf[1] - expected));
            parity = d1 < d0 ? 1 : 0;
            ambiguous = std::abs(d0 - d1) < kPi / 4;  // neither is clearly nearer (e.g. a mismatched line length)
        } else {
            parity = var[1] < var[0] ? 1 : 0;
            ambiguous = true;
        }
        if (dp.forceParity == 0 || dp.forceParity == 1) parity = dp.forceParity;
        for (int l = 0; l < lines; ++l) psi[l] = psiFor(parity, l);
        stepDeg = stepOf[parity] * 180 / kPi;
    }
    std::vector<float> stepErr;
    double meanStepErr = 0;
    if (colour && dp.samplesPerLine > 0) {
        stepErr.assign(lines, 0.0f);
        const double expected = expectedDeg * kPi / 180;
        std::complex<double> acc = 0;
        for (int l = 1; l < lines; ++l) {
            double e = wrap(psi[l] - psi[l - 1] - expected);
            stepErr[l] = float(e);
            acc += std::polar(1.0, e);
        }
        meanStepErr = std::arg(acc) * 180 / kPi;
    }
    auto fillInfo = [&] {
        if (!info) return;
        info->burstStart = bStart;
        info->burstAmplitude = burstAmp;
        info->blankCode = blank;
        info->parity = parity;
        info->phaseStepDeg = stepDeg;
        info->colour = colour;
        info->clipped = clipped;
        info->parityAmbiguous = ambiguous;
        info->expectedStepDeg = expectedDeg;
        info->stepError = stepErr;
        info->meanStepErrorDeg = meanStepErr;
    };
    if (dp.measureOnly) {
        fillInfo();
        return 0;
    }

    // 4. Luma = K-sample average (cancels the carrier); chroma = the rest,
    //    demodulated against each line's own burst phase. Two passes over
    //    line bands on `threads` threads: (a) luma and U/V per line, (b) RGB,
    //    with U/V optionally averaged with the previous line (PAL delay-line
    //    method: a phase error turns the hue one way on one line and the
    //    other way on the next, so the mean cancels it, at a small loss of
    //    saturation and of vertical colour resolution).
    const int aStart = std::max(0, std::min(dp.activeStart, width));
    const int aLen = dp.activeLen < 0 ? width - aStart : std::min(dp.activeLen, width - aStart);
    // Colour gain from the burst as it is (a TV's automatic colour control);
    // brightness gain from the burst corrected by the machine's burst level
    // (burstScale: an MSX1's colour and burst are both ~1/4 of standard
    // against its brightness).
    const double mvPerCode = colour ? si.burstMv * dp.burstScale / burstAmp : dp.fallbackMvPerCode;
    const double scale = (colour ? si.burstMv / burstAmp : dp.fallbackMvPerCode) / si.whiteMv;  // colour
    const double lumaScale = mvPerCode / si.whiteMv;
    // Brightness: its own blanking and gain in Y/C (the colour plane's blank
    // is the colour wire's zero, not black).
    const double yBlank = yc ? lumaBlank : blank;
    const bool perLine = (dp.lineClamp || dp.mono) && !clipped;
    // NTSC setup: black sits that far above blanking; taken off so black is black.
    const double setup = dp.setupIre / 100.0, setupGain = 1.0 / std::max(0.5, 1.0 - setup);
    const double yScale = yc && dp.lumaMvPerCode > 0 ? dp.lumaMvPerCode / si.whiteMv : lumaScale;
    out.resize(size_t(aLen) * lines * 3);
    std::vector<float> ys(size_t(aLen) * lines), us(size_t(aLen) * lines), vs(size_t(aLen) * lines);
    std::vector<double> cr(K), ci(K);
    for (int m = 0; m < K; ++m) cr[m] = carrier[m].real(), ci[m] = carrier[m].imag();

    auto demodLines = [&](int l0, int l1) {
        std::vector<double> y(width), di(width + 1), dq(width + 1);  // di/dq: prefix sums of the demodulated chroma
        const float* rpat = dp.linePattern;       // on the main plane
        const float* cpat = yc ? nullptr : rpat;  // the colour input is that plane only for composite
        auto rv = [&](const uint8_t* r, int m) { return double(r[m]) - (rpat ? double(rpat[m]) : 0.0); };
        auto cv = [&](const uint8_t* r, int m) { return double(r[m]) - (cpat ? double(cpat[m]) : 0.0); };
        for (int l = l0; l < l1; ++l) {
            const uint8_t* row = s + size_t(l) * width;
            const uint8_t* crow = cs + size_t(l) * width;  // == row for composite
            // Running K-sample window [n - K/2, n - K/2 + K), clipped at the
            // row ends: composite: the box luma; Y/C: the colour wire's zero.
            double a = 0;
            int lo = 0, hi = 0;  // current window [lo, hi)
            for (int n = 0; n < width; ++n) {
                int wlo = std::max(0, n - K / 2), whi = std::min(width, n - K / 2 + K);
                while (hi < whi) a += cv(crow, hi++);
                while (lo < wlo) a -= cv(crow, lo++);
                y[n] = a / (hi - lo);
            }
            // Chroma = sample - luma, multiplied by the carrier; a 2-cycle box
            // over that (from prefix sums) is the demodulated colour at n.
            di[0] = dq[0] = 0;
            for (int m = 0, k = 0; m < width; ++m, k = (k + 1 == K ? 0 : k + 1)) {
                double cm = cv(crow, m) - y[m];
                di[m + 1] = di[m] + cm * cr[k];
                dq[m + 1] = dq[m] + cm * ci[k];
            }
            const int sw = !si.vSwitch || ((l + parity) % 2 == 0) ? 1 : -1;
            // Reference rotation e^{-j psi}; with the PLL input it turns a
            // little further every sample (e^{-j eps/S}), zero at the burst's
            // centre, so a carrier running off the sampling rate is followed
            // along the whole line instead of only matching at the burst.
            const bool ramp = colour && dp.lineFreqErr && dp.samplesPerLine > 0;
            const double eps = ramp ? double(dp.lineFreqErr[l]) : 0.0;
            const double perSample = eps / double(std::max(1, dp.samplesPerLine));
            const double a0 = -psi[l] - perSample * (double(aStart) - (bStart + dp.burstLen / 2.0));
            double rotRe = colour ? std::cos(a0) : 0, rotIm = colour ? std::sin(a0) : 0;
            const double stepRe = std::cos(-perSample), stepIm = std::sin(-perSample);
            float* yo = &ys[size_t(l) * aLen];
            float* uo = &us[size_t(l) * aLen];
            float* vo = &vs[size_t(l) * aLen];
            for (int i = 0; i < aLen; ++i) {
                int n = aStart + i;
                double u = 0, v = 0;
                if (colour) {
                    const int halfWin = K * std::max(1, dp.chromaCycles) / 2;
                    int m0 = std::max(0, n - halfWin), m1 = std::min(width, n - halfWin + K * std::max(1, dp.chromaCycles));
                    double k2 = 2.0 / double(m1 - m0);
                    double zr = (di[m1] - di[m0]) * k2, zi = (dq[m1] - dq[m0]) * k2;
                    double rr = zr * rotRe - zi * rotIm, ri = zr * rotIm + zi * rotRe;  // z * e^{-j psi}
                    u = -ri * scale;
                    v = sw * rr * scale;
                    if (ramp) {
                        double nr = rotRe * stepRe - rotIm * stepIm;
                        rotIm = rotRe * stepIm + rotIm * stepRe;
                        rotRe = nr;
                    }
                }
                double lum = y[n];
                if (yc) {
                    lum = rv(row, n);  // the brightness wire as it is
                } else if (dp.luma == LumaFilter::Off || dp.mono) {
                    lum = rv(row, n);
                } else if (dp.luma == LumaFilter::Notch) {
                    // The carrier waveform at this phase: the chroma part
                    // (sample - box luma) at n and at the same phase one and
                    // two cycles either side, weights 1/2,1,1,1,1/2 (4 cycles).
                    double est = 0, wsum = 0;
                    for (int j = -2; j <= 2; ++j) {
                        int m = n + j * K;
                        if (m < 0 || m >= width) continue;
                        double wj = (j == -2 || j == 2) ? 0.5 : 1.0;
                        est += wj * (rv(row, m) - y[m]);
                        wsum += wj;
                    }
                    lum = rv(row, n) - est / wsum;
                }
                yo[i] = float(((lum - (perLine ? lineBlank[size_t(l)] : yBlank)) * yScale - setup) * setupGain);
                uo[i] = float(u * setupGain);
                vo[i] = float(v * setupGain);
            }
        }
    };
    const double hue = dp.hueDeg * kPi / 180, cg = dp.contrast * dp.saturation;
    const double hc = std::cos(hue) * cg, hs = std::sin(hue) * cg;
    const bool picture = dp.brightness != 0 || dp.contrast != 1 || dp.saturation != 1 || dp.hueDeg != 0;
    std::vector<uint8_t> gammaLut;
    if (dp.gamma != 1 && dp.gamma > 0) {
        gammaLut.resize(4096);
        for (int i = 0; i < 4096; ++i)
            gammaLut[i] = uint8_t(std::lround(255.0 * std::pow(i / 4095.0, 1.0 / dp.gamma)));
    }
    auto rgbLines = [&](int l0, int l1) {
        auto clamp8 = [&](double x) {
            if (!gammaLut.empty()) return gammaLut[x <= 0 ? 0 : x >= 1 ? 4095 : int(x * 4095 + 0.5)];
            return uint8_t(x <= 0 ? 0 : x >= 1 ? 255 : int(x * 255 + 0.5));
        };
        std::vector<float> detail, spread;
        for (int l = l0; l < l1; ++l) {
            const float* yi = &ys[size_t(l) * aLen];
            const float* ui = &us[size_t(l) * aLen];
            const float* vi = &vs[size_t(l) * aLen];
            if (dp.edgeDesat > 0) {
                // Brightness detail: |y - its mean over 2 carrier cycles|, spread
                // over +-1 cycle (as far as the colour filter reaches).
                detail.assign(size_t(aLen), 0.0f), spread.assign(size_t(aLen), 0.0f);
                double acc = 0;
                int lo = 0, hi = 0;
                for (int i = 0; i < aLen; ++i) {
                    const int wlo = std::max(0, i - K), whi = std::min(aLen, i + K);
                    while (hi < whi) acc += yi[hi++];
                    while (lo < wlo) acc -= yi[lo++];
                    detail[size_t(i)] = float(std::abs(yi[i] - acc / (hi - lo)));
                }
                for (int i = 0; i < aLen; ++i) {
                    float m = 0;
                    for (int j = std::max(0, i - K); j < std::min(aLen, i + K + 1); j += std::max(1, K / 4)) m = std::max(m, detail[size_t(j)]);
                    spread[size_t(i)] = m;
                }
            }
            const bool avg = dp.averageLines && l > 0;
            const float* up = avg ? &us[size_t(l - 1) * aLen] : ui;
            const float* vp = avg ? &vs[size_t(l - 1) * aLen] : vi;
            uint8_t* o = &out[size_t(l) * aLen * 3];
            for (int i = 0; i < aLen; ++i, o += 3) {
                double u = 0.5 * (double(ui[i]) + up[i]), v = 0.5 * (double(vi[i]) + vp[i]);
                if (dp.edgeDesat > 0) {
                    const double f = 1.0 / (1.0 + dp.edgeDesat * 12.0 * spread[size_t(i)]);
                    u *= f, v *= f;
                }
                double yy = yi[i];
                if (picture) {
                    yy = yy * dp.contrast + dp.brightness;
                    const double u2 = u * hc - v * hs, v2 = u * hs + v * hc;
                    u = u2, v = v2;
                }
                double b = yy + u / 0.493, r = yy + v / 0.877;
                double g = (yy - 0.299 * r - 0.114 * b) / 0.587;
                o[0] = clamp8(r);
                o[1] = clamp8(g);
                o[2] = clamp8(b);
            }
        }
    };
    auto parallel = [&](auto&& fn) {
        const int nt = std::max(1, std::min(dp.threads, lines));
        if (nt == 1) {
            fn(0, lines);
            return;
        }
        std::vector<std::thread> pool;
        for (int t = 0; t < nt; ++t) pool.emplace_back(fn, lines * t / nt, lines * (t + 1) / nt);
        for (auto& th : pool) th.join();
    };
    parallel(demodLines);
    parallel(rgbLines);
    fillInfo();
    return aLen;
}

inline std::vector<uint8_t> decode(const uint8_t* s, int width, int lines, const DecodeParams& dp, DecodeInfo* info) {
    std::vector<uint8_t> out;
    decodeInto(s, width, lines, dp, out, info);
    return out;
}

}  // namespace pal

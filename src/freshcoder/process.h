// SPDX-License-Identifier: GPL-3.0-or-later
// freshCoder: one frame of raw samples -> the output picture (decode, crop,
// scale, pad), and the software PLL's per-frame carrier track.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "grid.hpp"
#include "palui.h"
#include "recording.h"

namespace fc {

enum class SizeMode {
    Aspect43,  // 4:3 picture, `factor` x the number of lines high
    Native,    // one output pixel per decoded sample and line (not square)
    Custom,    // fitted into customW x customH, padded
    Target,    // fitted into a 16:9 frame targetH lines high (720/1080/1440/2160), padded
    Integer,   // pixel-exact: every pixel repeated intX x intY times, no filtering (optionally padded into a frame)
};

enum class Scaler { Nearest, Bilinear, Bicubic, Lanczos };

struct OutputGeometry {
    // Crop on the decoded picture: samples/lines; w/h 0 = to the edge.
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    SizeMode mode = SizeMode::Aspect43;
    int factor = 2;
    int customW = 1920, customH = 1080;
    int targetH = 1080;
    bool integerScale = false;  // Target/Custom: whole multiples of the line count only (crisp lines)
    int intX = 3, intY = 3;     // Integer: repetitions across / down
    bool intSame = true;        // Integer: one factor for both directions (square blocks)
    int intFrameH = 0;          // Integer: 0 = the picture's own size; 720/1080/1440/2160 = the largest factor that fits, padded
    Scaler scaler = Scaler::Bicubic;
    uint8_t pad[3] = {0, 0, 0};
};

struct OutputSize {
    int scaledW = 0, scaledH = 0;  // the picture after scaling
    int canvasW = 0, canvasH = 0;  // the output frame (picture + padding)
    int offX = 0, offY = 0;        // where the picture sits in the canvas
};

// Horizontal size of one decoded sample relative to a line's height, for a
// 4:3 picture: the 52 us active line is 4/3 of the 288 visible lines of one
// PAL field.
double sampleAspect(int activeLen);
// `pixelAspect`: width of one input pixel relative to a line's height
// (sampleAspect() for decoded samples; times the grid period for machine pixels).
OutputSize computeOutputSize(int croppedW, int croppedH, double pixelAspect, const OutputGeometry& g, bool evenSizes);

// Per-frame carrier-frequency error (radians of carrier phase per line),
// measured by pass 1 and smoothed; the software PLL's input to the decoder.
struct PllTrack {
    std::vector<float> raw;       // per frame, as measured (NaN: no colour in that frame)
    std::vector<float> smoothed;  // per frame, after smooth()
    void smooth(float bandwidth);  // 0 = one value (the mean) for all frames, 1 = as measured
};

// Pass 1: measure every frame's step error. `progress` gets frames done;
// stops early when `cancel` becomes true.
bool measurePll(const std::string& path, const fv::PalDecodeUi& ui, PllTrack& track, std::atomic<int64_t>& progress,
                std::atomic<bool>& cancel, std::string& err);

// The pixel grid (and, with snapping, the palette) from the first frame of
// the recording where they can be found (a black title frame has no edges);
// false if none of the first `maxFrames` frames gives one.
bool findGrid(const std::string& path, const fv::PalDecodeUi& ui, pal::GridEstimate& grid,
              std::vector<pal::Colour>& palette, pal::Calibration& cal, int maxFrames = 500);

// The user's choices for how a recording is read (inputs, standard).
inline void applyUiChoices(Recording& r, const fv::PalDecodeUi& u) {
    r.chooseInputs(u.svideoMode, u.svideoLuma, u.svideoChroma);
    r.chooseStandard(u.standardMode);
}

class Processor {
public:
    Processor() = default;
    ~Processor();
    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;
    void configure(const RecordingInfo& info, const fv::PalDecodeUi& ui, const OutputGeometry& g, int threads,
                   bool evenSizes);
    // Decodes `samples` (frame `index`) and produces the output picture as
    // packed RGB24, size() wide/high. `pll` may be null (per-frame PLL if
    // ui.pll is on). Returns false if the recording's sampling isn't PAL.
    bool process(const uint8_t* samples, int64_t index, const PllTrack* pll, std::vector<uint8_t>& rgb,
                 std::string* status = nullptr);
    const OutputSize& size() const { return size_; }
    int decodedW() const { return decW_; }
    int decodedH() const { return info_.height; }
    int pictureW() const { return picW_; }  // what crop/scale work on: decoded samples, or machine pixels
    double pictureAspect() const { return picAspect_; }  // width of one picture pixel relative to a line
    // Digital black/white levels from the last decoded frame: the picture
    // controls that put its darkest 0.5% at black and brightest 0.1% at 95%.
    bool autoLevels(fv::PalPicture& pic) const;
    // Pixel grid: current state, restart, or pin (the encode uses the preview's).
    fv::PalGridStatus gridStatus() const;
    void redetectGrid() { stage_.redetect(); }
    void relearnPalette() { stage_.relearn(); }
    void fixGrid(const pal::GridEstimate& g, const std::vector<pal::Colour>& palette,
                 const pal::Calibration& cal = pal::Calibration{}) {
        stage_.setFixed(g, palette, cal);
    }
    const pal::Calibration& calibration() const { return stage_.calibration(); }
    void pinGrid(const pal::GridEstimate& g) { stage_.pinGrid(g); }
    // Colours tried per C64 cell: 6 for the preview, all 16 for an encode
    // (slower, a little better: multicolour 78% vs 76% on test pictures).
    void setCellCandidates(int n) { cellCandidates_ = n; }
    // Fixed clock stripes (ui.removeStripes): learned from every processed
    // frame; learnStripes() feeds frames without decoding them (the encode
    // learns from the first seconds before it starts).
    void learnStripes(const uint8_t* samples);
    const pal::LinePattern& stripes() const { return stripes_; }

private:
    RecordingInfo info_;
    fv::PalDecodeUi ui_;
    OutputGeometry g_;
    int threads_ = 1;
    bool even_ = false;
    int decW_ = 0, picW_ = 0;
    double picAspect_ = 1;
    int cellCandidates_ = 6;
    float previewErr_ = NAN;         // preview PLL: the last per-frame carrier measurement ...
    int framesSinceMeasure_ = 1000;  // ... reused for a second (the decode ran twice per frame, ~30 fps)
    OutputSize size_;
    pal::GridStage stage_;
    pal::LinePattern stripes_;
    pal::VerticalRelock relock_;        // ui.relock: frames re-aligned at their vertical sync
    std::vector<uint8_t> relockBuf_;    // a writable copy of the frame's plane(s)
    bool haveDecoded_ = false;
    std::vector<double> softState_;     // ui.softText: its two levels, kept steady over frames
    std::vector<uint8_t> grid_;
    std::vector<uint8_t> decoded_, scaled_;
    std::vector<float> freqErr_;
    void* sws_ = nullptr;
};

}  // namespace fc

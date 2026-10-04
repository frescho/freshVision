// Auto-tune for analog RGB sources (M4 step 4): Phase (the sharpest
// picture), Horizontal/Vertical Position (the picture centred in the
// capture) and a crop to the picture's area. Measured on the captured
// frames only; the controls are found by the names the driver reports.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "capture.h"
#include "formats.h"

namespace fv {

struct FrameView {
    const uint8_t* data = nullptr;
    uint32_t width = 0, height = 0, stride = 0;
    Layout layout = Layout::Bgrx8;
};

// Brightest channel of pixel (x, y) (Y for YUV/grey).
int frameLevel(const FrameView& f, uint32_t x, uint32_t y);
// Sharpness: mean squared difference of horizontally adjacent pixels, over
// every 4th line. A sampling phase between two pixels blurs every edge and
// lowers it.
double edgeEnergy(const FrameView& f);

// Rows/columns whose brightest pixel is above `threshold`: the picture
// (border included) against the black of blanking.
struct PictureArea {
    bool valid = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
};
PictureArea findPictureArea(const FrameView& f, int threshold = 24);

class RgbAutoTune {
public:
    // Access to the device, supplied by main.cpp.
    struct Io {
        // false if the driver has no settable control of that name
        std::function<bool(const char* name, int64_t& min, int64_t& max, int64_t& value)> control;
        std::function<void(const char* name, int64_t value)> set;
        std::function<void(const Rect& crop)> crop;  // capture this region (restarts the capture)
        std::function<Rect()> currentCrop;
    };
    // screenW/H: the machine's whole screen in capture pixels (0 = unknown).
    void start(bool withCrop, int screenW = 0, int screenH = 0);
    void stop() { stage_ = Stage::Idle; }
    bool running() const { return stage_ != Stage::Idle; }
    // Feed every captured frame while running.
    void onFrame(const FrameView& f, const Io& io);
    const std::string& status() const { return status_; }
    // The picture area measured last (capture pixels), invalid if none yet.
    const PictureArea& lastArea() const { return lastArea_; }

private:
    enum class Stage { Idle, PhaseSweep, HPosProbe, HPosCorrect, VPosProbe, VPosCorrect, Crop };
    void log(const std::string& s);
    bool settle();  // true once enough frames passed after the last change
    void measureArea(const FrameView& f);
    void nextStage(const FrameView& f, const Io& io);

    Stage stage_ = Stage::Idle;
    bool withCrop_ = true;
    int screenW_ = 0, screenH_ = 0;
    PictureArea lastArea_;
    std::string status_;
    int wait_ = 0;       // frames to skip after a change
    int got_ = 0;        // frames measured at the current setting
    double acc_ = 0;
    // Phase sweep
    int64_t phaseMin_ = 0, phaseMax_ = 0, phaseNow_ = 0, phaseStart_ = 0;
    std::vector<double> energy_;
    // Position
    int64_t posBase_ = 0, posMin_ = 0, posMax_ = 0, probeStep_ = 0;
    double marginBase_ = 0;  // (left - right) / 2 or (top - bottom) / 2 before the probe
    int areaFrames_ = 0;
    double ax0_ = 0, ay0_ = 0, ax1_ = 0, ay1_ = 0;
    int corrections_ = 0;
};

}  // namespace fv

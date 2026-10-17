// SPDX-License-Identifier: GPL-3.0-or-later
#include "rgbtune.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fv {

int frameLevel(const FrameView& f, uint32_t x, uint32_t y) {
    const uint8_t* row = f.data + size_t(y) * f.stride;
    switch (f.layout) {
        case Layout::Bgrx8:
        case Layout::Rgbx8: {
            const uint8_t* p = row + size_t(x) * 4;
            return std::max({p[0], p[1], p[2]});
        }
        case Layout::Xrgb8:
        case Layout::Xbgr8: {
            const uint8_t* p = row + size_t(x) * 4;
            return std::max({p[1], p[2], p[3]});
        }
        case Layout::Rgb24:
        case Layout::Bgr24: {
            const uint8_t* p = row + size_t(x) * 3;
            return std::max({p[0], p[1], p[2]});
        }
        case Layout::Uyvy: return row[size_t(x) * 2 + 1];
        case Layout::Grey8: return row[x];
    }
    return 0;
}

double edgeEnergy(const FrameView& f) {
    if (!f.data || f.width < 2 || f.height < 1) return 0;
    double sum = 0;
    size_t n = 0;
    for (uint32_t y = 0; y < f.height; y += 4) {
        int prev = frameLevel(f, 0, y);
        for (uint32_t x = 1; x < f.width; ++x) {
            const int v = frameLevel(f, x, y);
            const double d = v - prev;
            sum += d * d;
            prev = v;
            ++n;
        }
    }
    return n ? sum / double(n) : 0;
}

PictureArea findPictureArea(const FrameView& f, int threshold) {
    PictureArea a;
    if (!f.data || !f.width || !f.height) return a;
    std::vector<int> colMax(f.width, 0), rowMax(f.height, 0);
    for (uint32_t y = 0; y < f.height; y += 2)
        for (uint32_t x = 0; x < f.width; ++x) {
            const int v = frameLevel(f, x, y);
            colMax[x] = std::max(colMax[x], v);
            rowMax[y] = std::max(rowMax[y], v);
        }
    for (uint32_t y = 1; y < f.height; y += 2) rowMax[y] = rowMax[y - 1];
    int x0 = -1, x1 = -1, y0 = -1, y1 = -1;
    for (uint32_t x = 0; x < f.width; ++x)
        if (colMax[x] > threshold) {
            if (x0 < 0) x0 = int(x);
            x1 = int(x);
        }
    for (uint32_t y = 0; y < f.height; ++y)
        if (rowMax[y] > threshold) {
            if (y0 < 0) y0 = int(y);
            y1 = int(y);
        }
    if (x0 < 0 || y0 < 0 || x1 - x0 < 16 || y1 - y0 < 16) return a;
    a.valid = true;
    a.x0 = x0, a.x1 = x1, a.y0 = y0, a.y1 = y1;
    return a;
}

void RgbAutoTune::log(const std::string& s) {
    status_ = s;
    std::printf("rgb auto-tune: %s\n", s.c_str());
}

void RgbAutoTune::start(bool withCrop, int screenW, int screenH) {
    withCrop_ = withCrop;
    screenW_ = screenW;
    screenH_ = screenH;
    lastArea_ = PictureArea{};
    stage_ = Stage::PhaseSweep;
    energy_.clear();
    phaseNow_ = -1;
    wait_ = 0;
    got_ = 0;
    acc_ = 0;
    corrections_ = 0;
    areaFrames_ = 0;
    log("started");
}

bool RgbAutoTune::settle() {
    if (wait_ > 0) {
        --wait_;
        return false;
    }
    return true;
}

void RgbAutoTune::measureArea(const FrameView& f) {
    PictureArea a = findPictureArea(f);
    if (!a.valid) return;
    ax0_ += a.x0, ax1_ += a.x1, ay0_ += a.y0, ay1_ += a.y1;
    ++areaFrames_;
}

void RgbAutoTune::onFrame(const FrameView& f, const Io& io) {
    if (stage_ == Stage::Idle || !f.data) return;
    if (!settle()) return;
    constexpr int kSettleFrames = 8, kMeasureFrames = 4, kAreaFrames = 6;

    if (stage_ == Stage::PhaseSweep) {
        if (phaseNow_ < 0) {
            int64_t v = 0;
            if (!io.control("Phase", phaseMin_, phaseMax_, v) || phaseMax_ <= phaseMin_) {
                log("no Phase control on this source: phase skipped");
                nextStage(f, io);
                return;
            }
            phaseStart_ = v;
            phaseNow_ = phaseMin_;
            io.set("Phase", phaseNow_);
            wait_ = kSettleFrames;
            got_ = 0;
            acc_ = 0;
            return;
        }
        acc_ += edgeEnergy(f);
        if (++got_ < kMeasureFrames) return;
        energy_.push_back(acc_ / got_);
        got_ = 0;
        acc_ = 0;
        if (phaseNow_ < phaseMax_) {
            io.set("Phase", ++phaseNow_);
            wait_ = kSettleFrames;
            return;
        }
        // Phase is circular: smooth over the neighbours, take the maximum.
        const int n = int(energy_.size());
        int best = 0;
        double bestE = -1, lo = 1e300, hi = 0;
        for (int i = 0; i < n; ++i) {
            const double e = energy_[size_t((i + n - 1) % n)] + 2 * energy_[size_t(i)] + energy_[size_t((i + 1) % n)];
            if (e > bestE) bestE = e, best = i;
            lo = std::min(lo, energy_[size_t(i)]);
            hi = std::max(hi, energy_[size_t(i)]);
        }
        const int64_t chosen = phaseMin_ + best;
        char b[200];
        // Too little detail to judge (e.g. a plain start-up screen: 10 vs 10)
        // or hardly any change: the phase stays as it was.
        if (hi < 30 || (hi - lo) / hi < 0.10) {
            std::snprintf(b, sizeof b, "phase: too little detail or change over %lld-%lld (sharpness %.0f, %.1f%%): Phase left at %lld",
                          (long long)phaseMin_, (long long)phaseMax_, hi, hi > 0 ? (hi - lo) / hi * 100 : 0.0,
                          (long long)phaseStart_);
            io.set("Phase", phaseStart_);
        } else {
            std::snprintf(b, sizeof b, "phase: sharpest at %lld (sharpness %.0f, lowest %.0f) -> Phase %lld",
                          (long long)chosen, hi, lo, (long long)chosen);
            io.set("Phase", chosen);
        }
        log(b);
        wait_ = kSettleFrames;
        stage_ = Stage::HPosProbe;
        posBase_ = -1;
        areaFrames_ = 0;
        ax0_ = ax1_ = ay0_ = ay1_ = 0;
        corrections_ = 0;
        return;
    }

    // Position: measure the area, probe one step to learn which way and how
    // far the picture moves per control unit, then correct.
    const bool horiz = stage_ == Stage::HPosProbe || stage_ == Stage::HPosCorrect;
    const char* ctrl = horiz ? "Horizontal Position" : "Vertical Position";
    if (stage_ == Stage::HPosProbe || stage_ == Stage::VPosProbe || stage_ == Stage::HPosCorrect ||
        stage_ == Stage::VPosCorrect) {
        measureArea(f);
        if (areaFrames_ < kAreaFrames) {
            if (areaFrames_ == 0 && got_++ > 30) {  // 30 frames without a picture area
                log(std::string(horiz ? "horizontal" : "vertical") + " position: no picture area found (all black?): skipped");
                got_ = 0;
                nextStage(f, io);
            }
            return;
        }
        got_ = 0;
        const double x0 = ax0_ / areaFrames_, x1 = ax1_ / areaFrames_, y0 = ay0_ / areaFrames_, y1 = ay1_ / areaFrames_;
        lastArea_.valid = true;
        lastArea_.x0 = int(std::lround(x0)), lastArea_.x1 = int(std::lround(x1));
        lastArea_.y0 = int(std::lround(y0)), lastArea_.y1 = int(std::lround(y1));
        areaFrames_ = 0;
        ax0_ = ax1_ = ay0_ = ay1_ = 0;
        const double first = horiz ? x0 : y0;
        const double last = horiz ? x1 : y1;
        const double size = horiz ? f.width : f.height;
        const double off = (first - (size - 1 - last)) / 2;  // > 0: the picture sits too far right/down
        char b[240];
        if (stage_ == Stage::HPosProbe || stage_ == Stage::VPosProbe) {
            int64_t v = 0;
            if (!io.control(ctrl, posMin_, posMax_, v)) {
                log(std::string("no ") + ctrl + " control: skipped");
                nextStage(f, io);
                return;
            }
            // A known screen size and a smaller picture: the border is black
            // (it can't be told from blanking), so the picture's edges don't
            // show where the screen is. The position stays as it is.
            const int screen = horiz ? screenW_ : screenH_;
            if (screen > 0 && last - first + 1 < screen - 8) {
                std::snprintf(b, sizeof b,
                              "%s: the picture (%.0f px) is smaller than the screen size (%d): black border? position kept",
                              ctrl, last - first + 1, screen);
                log(b);
                nextStage(f, io);
                return;
            }
            if (first <= 1 && last >= size - 2) {
                std::snprintf(b, sizeof b, "%s: the picture fills the capture (%.0f-%.0f of %.0f): nothing to centre", ctrl,
                              first, last, size);
                log(b);
                nextStage(f, io);
                return;
            }
            if (std::abs(off) < 1.5) {
                std::snprintf(b, sizeof b, "%s: centred (margins %.0f / %.0f)", ctrl, first, size - 1 - last);
                log(b);
                nextStage(f, io);
                return;
            }
            posBase_ = v;
            marginBase_ = first;
            probeStep_ = horiz ? 8 : 2;
            if (v + probeStep_ > posMax_) probeStep_ = -probeStep_;
            io.set(ctrl, v + probeStep_);
            wait_ = kSettleFrames;
            stage_ = horiz ? Stage::HPosCorrect : Stage::VPosCorrect;
            corrections_ = 0;
            return;
        }
        // Correct: the probe moved the first edge by (first - marginBase_).
        if (corrections_ == 0) {
            const double moved = first - marginBase_;
            if (std::abs(moved) < 0.5) {
                std::snprintf(b, sizeof b, "%s: a step of %lld did not move the picture: put back to %lld", ctrl,
                              (long long)probeStep_, (long long)posBase_);
                io.set(ctrl, posBase_);
                log(b);
                wait_ = kSettleFrames;
                nextStage(f, io);
                return;
            }
            const double gain = moved / double(probeStep_);  // pixels per control unit
            // Centre from the probe position: move by -off pixels.
            int64_t target = posBase_ + probeStep_ + int64_t(std::lround(-off / gain));
            target = std::clamp(target, posMin_, posMax_);
            std::snprintf(b, sizeof b, "%s: picture %.1f px off centre, %.2f px per step -> %lld", ctrl, off, gain,
                          (long long)target);
            log(b);
            io.set(ctrl, target);
            posBase_ = target;
            marginBase_ = gain;  // reused: pixels per step for the fine correction
            wait_ = kSettleFrames;
            corrections_ = 1;
            return;
        }
        if (std::abs(off) >= 1.5 && corrections_ < 4) {
            const double gain = marginBase_;
            int64_t target = std::clamp<int64_t>(posBase_ + int64_t(std::lround(-off / gain)), posMin_, posMax_);
            if (target != posBase_) {
                std::snprintf(b, sizeof b, "%s: still %.1f px off -> %lld", ctrl, off, (long long)target);
                log(b);
                io.set(ctrl, target);
                posBase_ = target;
                wait_ = kSettleFrames;
                ++corrections_;
                return;
            }
        }
        std::snprintf(b, sizeof b, "%s: %lld, margins %.0f / %.0f", ctrl, (long long)posBase_, first, size - 1 - last);
        log(b);
        nextStage(f, io);
        return;
    }

    if (stage_ == Stage::Crop) {
        measureArea(f);
        if (areaFrames_ < kAreaFrames) {
            if (areaFrames_ == 0 && got_++ > 30) {
                log("crop: no picture area found: skipped");
                stage_ = Stage::Idle;
            }
            return;
        }
        int x0 = int(std::lround(ax0_ / areaFrames_)), x1 = int(std::lround(ax1_ / areaFrames_));
        int y0 = int(std::lround(ay0_ / areaFrames_)), y1 = int(std::lround(ay1_ / areaFrames_));
        lastArea_.valid = true;
        lastArea_.x0 = x0, lastArea_.x1 = x1, lastArea_.y0 = y0, lastArea_.y1 = y1;
        // With a known screen size the crop is the screen, centred in the
        // capture (the position was centred on a visible border when the
        // size was taken), as long as the drawn picture lies inside it.
        bool toScreen = false;
        if (screenW_ > 0 && screenH_ > 0 && screenW_ <= int(f.width) && screenH_ <= int(f.height)) {
            const int sx = (int(f.width) - screenW_) / 2, sy = (int(f.height) - screenH_) / 2;
            if (x0 >= sx - 2 && y0 >= sy - 2 && x1 <= sx + screenW_ + 1 && y1 <= sy + screenH_ + 1) {
                x0 = sx, y0 = sy, x1 = sx + screenW_ - 1, y1 = sy + screenH_ - 1;
                toScreen = true;
            } else {
                char w[200];
                std::snprintf(w, sizeof w, "crop: the picture (%d,%d-%d,%d) reaches outside the screen size %dx%d: cropping to the picture",
                              x0, y0, x1, y1, screenW_, screenH_);
                log(w);
            }
        }
        const int w = x1 - x0 + 1, h = y1 - y0 + 1;
        char b[200];
        if (int(f.width) - w < 8 && int(f.height) - h < 8) {
            std::snprintf(b, sizeof b, "done; the picture fills the capture (%dx%d), no crop needed", w, h);
            log(b);
        } else {
            const Rect cur = io.currentCrop();
            if (cur.set && (cur.w != f.width || cur.h != f.height)) {
                std::snprintf(b, sizeof b, "done; picture area %d,%d %dx%d, not cropped (the capture is scaled)", x0, y0, w, h);
                log(b);
            } else {
                Rect r;
                r.set = true;
                r.x = (cur.set ? cur.x : 0) + x0;
                r.y = (cur.set ? cur.y : 0) + y0;
                // Even sizes: a driver may refuse odd ones.
                r.w = uint32_t(w + (w & 1));
                r.h = uint32_t(h + (h & 1));
                std::snprintf(b, sizeof b, "done; cropping to the %s: %d,%d %ux%u", toScreen ? "screen size" : "picture", r.x, r.y, r.w, r.h);
                log(b);
                io.crop(r);
            }
        }
        stage_ = Stage::Idle;
        return;
    }
}

void RgbAutoTune::nextStage(const FrameView&, const Io&) {
    areaFrames_ = 0;
    ax0_ = ax1_ = ay0_ = ay1_ = 0;
    got_ = 0;
    corrections_ = 0;
    switch (stage_) {
        case Stage::PhaseSweep: stage_ = Stage::HPosProbe; break;
        case Stage::HPosProbe:
        case Stage::HPosCorrect: stage_ = Stage::VPosProbe; break;
        case Stage::VPosProbe:
        case Stage::VPosCorrect:
            if (withCrop_) {
                stage_ = Stage::Crop;
            } else {
                stage_ = Stage::Idle;
                log("done");
            }
            break;
        default: stage_ = Stage::Idle; break;
    }
    wait_ = 8;
}

}  // namespace fv

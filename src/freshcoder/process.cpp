#include "process.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "pal.hpp"

extern "C" {
#include <libswscale/swscale.h>
}

namespace fc {

double sampleAspect(int activeLen) { return activeLen > 0 ? (4.0 / 3.0 * 288.0) / double(activeLen) : 1.0; }

OutputSize computeOutputSize(int cw, int ch, double sa, const OutputGeometry& g, bool evenSizes) {
    OutputSize o;
    const double dispW = double(cw) * sa;  // picture width in line units
    auto fit = [&](int boxW, int boxH) {
        if (g.integerScale) {
            int n = std::max(1, boxH / std::max(1, ch));
            while (n > 1 && dispW * n > boxW) --n;
            o.scaledW = int(std::lround(dispW * n));
            o.scaledH = ch * n;
        } else {
            double f = std::min(double(boxW) / dispW, double(boxH) / ch);
            o.scaledW = int(std::lround(dispW * f));
            o.scaledH = int(std::lround(ch * f));
        }
        o.canvasW = boxW;
        o.canvasH = boxH;
    };
    switch (g.mode) {
        case SizeMode::Aspect43:
            o.scaledW = o.canvasW = int(std::lround(dispW * std::max(1, g.factor)));
            o.scaledH = o.canvasH = ch * std::max(1, g.factor);
            break;
        case SizeMode::Native:
            o.scaledW = o.canvasW = cw;
            o.scaledH = o.canvasH = ch;
            break;
        case SizeMode::Custom:
            fit(std::max(16, g.customW), std::max(16, g.customH));
            break;
        case SizeMode::Target: {
            int h = std::max(16, g.targetH);
            fit(int(std::lround(h * 16.0 / 9.0)), h);
            break;
        }
        case SizeMode::Integer: {
            int nx = std::max(1, g.intX), ny = std::max(1, g.intSame ? g.intX : g.intY);
            if (g.intFrameH > 0) {
                // The largest factor (same both ways) that fits the 16:9 frame; padded.
                const int fh = g.intFrameH, fw = int(std::lround(fh * 16.0 / 9.0));
                int n = std::max(1, std::min(fw / std::max(1, cw), fh / std::max(1, ch)));
                nx = ny = n;
                o.canvasW = fw, o.canvasH = fh;
            }
            o.scaledW = cw * nx;
            o.scaledH = ch * ny;
            if (g.intFrameH <= 0) o.canvasW = o.scaledW, o.canvasH = o.scaledH;
            break;
        }
    }
    if (evenSizes) {  // 4:2:0 encoders need even sizes: pad (never shrink the exact blocks)
        if (g.mode == SizeMode::Integer) o.canvasW += o.canvasW & 1, o.canvasH += o.canvasH & 1;
        else o.scaledW &= ~1, o.scaledH &= ~1, o.canvasW &= ~1, o.canvasH &= ~1;
    }
    o.scaledW = std::max(2, std::min(o.scaledW, o.canvasW));
    o.scaledH = std::max(2, std::min(o.scaledH, o.canvasH));
    o.offX = ((o.canvasW - o.scaledW) / 2) & ~1;
    o.offY = ((o.canvasH - o.scaledH) / 2) & ~1;
    return o;
}

void PllTrack::smooth(float bandwidth) {
    const size_t n = raw.size();
    smoothed.assign(n, 0.0f);
    double sum = 0;
    size_t cnt = 0;
    for (float v : raw)
        if (!std::isnan(v)) sum += v, ++cnt;
    const double mean = cnt ? sum / double(cnt) : 0.0;
    const double b = std::max(0.0f, std::min(1.0f, bandwidth));
    // Forward and backward first-order filters, averaged: no lag either way.
    std::vector<double> fwd(n), bwd(n);
    double y = mean;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isnan(raw[i])) y += b * (raw[i] - y);
        fwd[i] = y;
    }
    y = mean;
    for (size_t i = n; i-- > 0;) {
        if (!std::isnan(raw[i])) y += b * (raw[i] - y);
        bwd[i] = y;
    }
    for (size_t i = 0; i < n; ++i) smoothed[i] = float(0.5 * (fwd[i] + bwd[i]));
}

bool measurePll(const std::string& path, const fv::PalDecodeUi& ui, PllTrack& track, std::atomic<int64_t>& progress,
                std::atomic<bool>& cancel, std::string& err) {
    Recording rec;
    if (!rec.open(path, err)) return false;
    applyUiChoices(rec, ui);
    const RecordingInfo& info = rec.info();
    if (!info.samplesPerCycle) {
        err = std::string("the recording's Horizontal Size is not a ") + pal::standardName(info.standard) + " sampling rate";
        return false;
    }
    pal::DecodeParams dp;
    dp.samplesPerCycle = info.samplesPerCycle;
    dp.burstLen = 8 * info.samplesPerCycle;
    dp.activeStart = info.activeStart;
    dp.activeLen = info.activeLen;
    dp.samplesPerLine = info.horizontalSize;
    dp.standard = info.standard;
    dp.burstSearchEnd = pal::burstSearchEndFor(info.standard, double(info.horizontalSize) * pal::standardInfo(info.standard).lineRate,
                                               info.horizontalPosition);
    dp.measureOnly = true;
    track.raw.assign(size_t(std::max<int64_t>(info.frames, 0)), NAN);
    std::vector<uint8_t> samples, dummy;
    int64_t index = 0;
    int burst = -1;
    while (!cancel && rec.next(samples, index)) {
        dp.burstStart = burst;
        dp.chromaPlane = chromaPlaneOf(info, samples.data());
        pal::DecodeInfo di;
        pal::decodeInto(samples.data(), info.width, info.height, dp, dummy, &di);
        if (di.colour) burst = di.burstStart;
        if (index >= 0) {
            if (size_t(index) >= track.raw.size()) track.raw.resize(size_t(index) + 1, NAN);
            if (di.colour && !di.stepError.empty()) track.raw[size_t(index)] = float(di.meanStepErrorDeg * pal::kPi / 180);
        }
        progress = index + 1;
    }
    return !cancel;
}

bool findGrid(const std::string& path, const fv::PalDecodeUi& ui, pal::GridEstimate& grid,
              std::vector<pal::Colour>& palette, pal::Calibration& cal, int maxFrames) {
    Recording rec;
    std::string err;
    if (!rec.open(path, err)) return false;
    applyUiChoices(rec, ui);  // before the check: the standard decides the sampling rate
    if (!rec.info().samplesPerCycle) return false;
    Processor p;
    fv::PalDecodeUi u = ui;
    u.pixelGrid = true;
    u.pll = false;
    u.c64Cells = false;  // only grid, palette and calibration are wanted here (TMS cells stay: they set the grid phase)
    p.configure(rec.info(), u, OutputGeometry{}, std::max(1, int(std::thread::hardware_concurrency() / 2)), false);
    std::vector<uint8_t> samples, rgb;
    int64_t index = 0;
    // The grid from the first frame that has one; with snapping, the palette
    // from pixels gathered over up to 500 frames spread over the recording
    // (every 5th frame is pooled; a title sequence shows other colours than
    // the first scene).
    bool found = false;
    for (int n = 0; n < maxFrames && rec.next(samples, index); ++n) {
        p.process(samples.data(), index, nullptr, rgb);
        const fv::PalGridStatus st = p.gridStatus();
        if (st.valid) {
            grid.valid = true, grid.period = st.period, grid.phase = st.phase, grid.score = st.score;
            found = true;
            break;
        }
    }
    if (!found || !ui.snap) return found;
    // The palette from frames spread over the whole recording: 100 places,
    // 10 frames each (each 10th is pooled; relearned as the pool grows).
    p.relearnPalette();
    p.pinGrid(grid);
    const int64_t total = std::max<int64_t>(1, rec.info().frames);
    const int places = int(std::min<int64_t>(100, total / 10 + 1));  // a short clip has fewer places than 100
    for (int place = 0; place < places; ++place) {
        rec.seek(total * place / places);
        for (int f = 0; f < 10 && rec.next(samples, index); ++f) p.process(samples.data(), index, nullptr, rgb);
    }
    const fv::PalGridStatus st = p.gridStatus();
    palette.assign(st.palette.begin(), st.palette.end());
    cal = p.calibration();
    return true;
}

Processor::~Processor() { sws_freeContext(static_cast<SwsContext*>(sws_)); }

void Processor::configure(const RecordingInfo& info, const fv::PalDecodeUi& ui, const OutputGeometry& g, int threads,
                          bool evenSizes) {
    // Another recording, input or window: the stripes are learned afresh.
    if (info.path != info_.path || info.lumaChannel != info_.lumaChannel || info.horizontalSize != info_.horizontalSize ||
        info.standard != info_.standard ||
        info.horizontalPosition != info_.horizontalPosition)
        stripes_.reset(), relock_.reset(), haveDecoded_ = false;
    info_ = info;
    ui_ = ui;
    g_ = g;
    threads_ = std::max(1, threads);
    even_ = evenSizes;
    decW_ = std::min(info.activeLen, std::max(0, info.width - info.activeStart));
    // Pixel grid: the profile's pixel period (scaled to this sampling rate)
    // searched +/-3%, or 1.5-6 samples at 4x without a hint.
    const auto& profiles = pal::machineProfiles();
    const auto& prof = profiles[size_t(std::clamp(ui.profile, 0, int(profiles.size()) - 1))];
    const double k = info.samplesPerCycle / 4.0;
    pal::GridStage::Settings gs;
    if (prof.samplesPerPixelAt4x > 0) gs.periodLo = prof.samplesPerPixelAt4x * k / 1.03, gs.periodHi = prof.samplesPerPixelAt4x * k * 1.03;
    else gs.periodLo = 1.5 * k, gs.periodHi = 6.0 * k;
    gs.core = ui.gridCore;
    gs.snap = ui.snap;
    gs.colours = std::max(2, ui.colours);
    gs.startPalette = prof.palette;
    gs.fixedPalette = fv::chosenPalette(ui);
    if (!gs.fixedPalette.empty()) gs.colours = int(gs.fixedPalette.size());
    gs.c64Cells = ui.c64Cells && pal::isC64Profile(ui.profile);
    gs.tmsCells = ui.tmsCells && pal::isTmsProfile(ui.profile);
    gs.cellCandidates = cellCandidates_;
    gs.relearnEvery = 250;  // the preview keeps learning slowly; the encode pins a palette (findGrid)
    gs.poolEvery = 10;
    gs.learnFrames = 1 << 30;
    stage_.configure(gs);
}

bool Processor::autoLevels(fv::PalPicture& pic) const {
    if (decoded_.empty()) return false;
    std::vector<int> hist(256, 0);
    size_t n = 0;
    for (size_t i = 0; i + 2 < decoded_.size(); i += 3 * 3) {  // every 3rd pixel
        const int y = int(0.299 * decoded_[i] + 0.587 * decoded_[i + 1] + 0.114 * decoded_[i + 2] + 0.5);
        hist[size_t(std::clamp(y, 0, 255))]++;
        ++n;
    }
    if (n < 100) return false;
    auto pct = [&](double q) {
        size_t acc = 0, target = size_t(double(n) * q);
        for (int v = 0; v < 256; ++v)
            if ((acc += size_t(hist[size_t(v)])) > target) return v;
        return 255;
    };
    const double lo = pct(0.005) / 255.0, hi = pct(0.999) / 255.0;
    if (hi - lo < 0.1) return false;
    // The decoded values already include the current controls: y' = c y + b.
    // New: y'' = (y' - lo) * f = y (c f) + (b - lo) f.
    const double f = 0.95 / (hi - lo);
    pic.contrast = float(pic.contrast * f);
    pic.brightness = float((pic.brightness - lo) * f);
    return true;
}

fv::PalGridStatus Processor::gridStatus() const {
    fv::PalGridStatus st;
    const pal::GridEstimate& g = stage_.grid();
    st.valid = g.valid;
    st.period = g.period, st.phase = g.phase, st.score = g.score;
    st.pixelsPerLine = ui_.pixelGrid ? picW_ : 0;
    for (const auto& c : stage_.palette()) st.palette.push_back(c);
    const pal::Calibration& cal = stage_.calibration();
    st.calSat = cal.satGain, st.calHueDeg = cal.hueRad * 180 / pal::kPi, st.calContrast = cal.contrast, st.calOffset = cal.offset;
    return st;
}

void Processor::learnStripes(const uint8_t* samples) {
    // The brightness plane (S-Video) or the samples (composite) come first.
    stripes_.learn(samples, info_.width, info_.height, info_.activeStart, info_.activeStart + info_.activeLen);
}

bool Processor::process(const uint8_t* samples, int64_t index, const PllTrack* pll, std::vector<uint8_t>& rgb,
                        std::string* status) {
    if (!info_.samplesPerCycle) return false;
    pal::DecodeParams dp;
    dp.samplesPerCycle = info_.samplesPerCycle;
    dp.burstLen = 8 * info_.samplesPerCycle;
    dp.activeStart = info_.activeStart;
    dp.activeLen = info_.activeLen;
    dp.samplesPerLine = info_.horizontalSize;
    dp.standard = info_.standard;
    dp.chromaCycles = ui_.chromaCycles;
    dp.edgeDesat = ui_.edgeDesat;
    dp.burstSearchEnd = pal::burstSearchEndFor(info_.standard, double(info_.horizontalSize) * pal::standardInfo(info_.standard).lineRate,
                                               info_.horizontalPosition);
    {
        const auto& profs = pal::machineProfiles();
        dp.burstScale = profs[size_t(std::clamp(ui_.profile, 0, int(profs.size()) - 1))].burstScale;
    }
    dp.averageLines = ui_.averageLines;
    dp.luma = pal::LumaFilter(std::clamp(ui_.luma, 0, 2));
    dp.threads = threads_;
    dp.brightness = ui_.picture.brightness;
    dp.contrast = ui_.picture.contrast;
    dp.saturation = ui_.picture.saturation;
    dp.hueDeg = ui_.picture.hue;
    dp.gamma = ui_.picture.gamma;
    dp.chromaPlane = chromaPlaneOf(info_, samples);  // S-Video recordings
    const int lines = info_.height;
    float err = NAN;
    if (ui_.pll) {
        if (pll && index >= 0 && size_t(index) < pll->smoothed.size()) {
            err = pll->smoothed[size_t(index)];
        } else {
            // No pass-1 track (the preview): measure the carrier once a
            // second and reuse it in between.
            if (++framesSinceMeasure_ >= 25 || std::isnan(previewErr_)) {
                framesSinceMeasure_ = 0;
                pal::DecodeParams mp = dp;
                mp.measureOnly = true;
                pal::DecodeInfo mi;
                std::vector<uint8_t> dummy;
                pal::decodeInto(samples, info_.width, lines, mp, dummy, &mi);
                if (mi.colour) previewErr_ = float(mi.meanStepErrorDeg * pal::kPi / 180);
            }
            err = previewErr_;
        }
        if (!std::isnan(err)) {
            freqErr_.assign(size_t(lines), err);
            dp.lineFreqErr = freqErr_.data();
        }
    }
    bool relockMissed = false;
    if (ui_.relock) {
        // Re-align the frame at its vertical sync (a copy: the samples are the reader's).
        const size_t plane = size_t(info_.width) * lines;
        relockBuf_.assign(samples, samples + plane * (dp.chromaPlane ? 2 : 1));
        relockMissed = !relock_.process(relockBuf_.data(), info_.width, lines, info_.activeStart,
                                        info_.activeStart + info_.activeLen, dp.chromaPlane ? relockBuf_.data() + plane : nullptr);
        samples = relockBuf_.data();
        if (dp.chromaPlane) dp.chromaPlane = relockBuf_.data() + plane;
        dp.lineClamp = true;
    }
    if (ui_.removeStripes) {
        learnStripes(samples);
        dp.linePattern = stripes_.data();
    }
    pal::DecodeInfo info;
    // A frame the re-alignment couldn't place: the previous picture is used again.
    int w = relockMissed && haveDecoded_ ? decW_ : pal::decodeInto(samples, info_.width, lines, dp, decoded_, &info);
    if (ui_.softText && !(relockMissed && haveDecoded_)) {
        const auto& profs = pal::machineProfiles();
        pal::softTwoColour(decoded_.data(), w, lines, profs[size_t(std::clamp(ui_.profile, 0, int(profs.size()) - 1))].palette,
                           softState_);
    }
    haveDecoded_ = true;
    decW_ = w;
    double pixelAspect = sampleAspect(info_.activeLen);
    const std::vector<uint8_t>* pic = &decoded_;
    if (ui_.pixelGrid) {
        int gw = 0;
        if (stage_.process(decoded_.data(), w, lines, grid_, gw)) {
            pic = &grid_;
            pixelAspect *= stage_.grid().period;
            w = gw;
        }
    }
    picW_ = w;

    // Crop.
    const int cx = std::clamp(g_.cropX, 0, std::max(0, w - 2)), cy = std::clamp(g_.cropY, 0, std::max(0, lines - 2));
    const int cw = std::clamp(g_.cropW > 0 ? g_.cropW : w - cx, 2, w - cx);
    const int ch = std::clamp(g_.cropH > 0 ? g_.cropH : lines - cy, 2, lines - cy);
    size_ = computeOutputSize(cw, ch, pixelAspect, g_, even_);

    // Scale the cropped picture, then place it on the padded canvas.
    picAspect_ = pixelAspect;
    // Integer mode repeats pixels exactly: always "nearest".
    int flags = (g_.scaler == Scaler::Nearest || g_.mode == SizeMode::Integer) ? SWS_POINT
                : g_.scaler == Scaler::Bilinear ? SWS_BILINEAR
                : g_.scaler == Scaler::Lanczos  ? SWS_LANCZOS
                                                : SWS_BICUBIC;
    flags |= SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND;
    SwsContext* sws = sws_getCachedContext(static_cast<SwsContext*>(sws_), cw, ch, AV_PIX_FMT_RGB24, size_.scaledW,
                                           size_.scaledH, AV_PIX_FMT_RGB24, flags, nullptr, nullptr, nullptr);
    sws_ = sws;
    if (!sws) return false;
    scaled_.resize(size_t(size_.scaledW) * size_.scaledH * 3);
    const uint8_t* src[1] = {&(*pic)[(size_t(cy) * w + cx) * 3]};
    int srcStride[1] = {w * 3};
    uint8_t* dst[1] = {scaled_.data()};
    int dstStride[1] = {size_.scaledW * 3};
    sws_scale(sws, src, srcStride, 0, ch, dst, dstStride);

    rgb.resize(size_t(size_.canvasW) * size_.canvasH * 3);
    for (size_t i = 0; i < rgb.size(); i += 3) rgb[i] = g_.pad[0], rgb[i + 1] = g_.pad[1], rgb[i + 2] = g_.pad[2];
    for (int y = 0; y < size_.scaledH; ++y)
        std::memcpy(&rgb[(size_t(size_.offY + y) * size_.canvasW + size_.offX) * 3], &scaled_[size_t(y) * size_.scaledW * 3],
                    size_t(size_.scaledW) * 3);

    if (status) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "burst %.1f codes, step %.1f deg (expected %.1f), parity %d, PLL %s%s",
                      info.burstAmplitude, info.phaseStepDeg, info.expectedStepDeg, info.parity,
                      std::isnan(err) ? "off" : "on", !std::isnan(err) ? (pll ? " (pass 1 track)" : " (this frame)") : "");
        *status = buf;
        if (ui_.removeStripes) {
            if (stripes_.ready())
                std::snprintf(buf, sizeof buf, "; clock stripes removed (%.2f codes rms, %.0f%% of the line known)", stripes_.rms(),
                              stripes_.coverage() * 100);
            else
                std::snprintf(buf, sizeof buf, "; clock stripes: learning (%ld flat segments)", stripes_.segments());
            *status += buf;
        }
    }
    return true;
}

}  // namespace fc

// SPDX-License-Identifier: GPL-3.0-or-later
// freshCoder: encoding the processed pictures plus the recording's sound.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "palui.h"
#include "process.h"

namespace fc {

// One selectable video encoder, with how its quality is set.
struct VideoCodec {
    std::string id;           // FFmpeg encoder name
    std::string label;        // for the GUI and --help
    std::string qualityName;  // "CRF", "CQ", "q" (empty: lossless, no quality setting)
    int qualityMin = 0, qualityMax = 0, qualityDefault = 0;  // lower = better
    std::vector<std::string> presets;  // empty: none
    std::string presetDefault;
    bool can444 = false, can420 = false;  // chroma choices the encoder opened with
    bool lossless = false;
};

// The encoders that really open on this machine (probed once).
const std::vector<VideoCodec>& availableCodecs();
const VideoCodec* findCodec(const std::string& id);

struct EncodeSettings {
    std::string outPath;
    std::string codec = "libx264";
    int quality = 14;           // meaning per codec (VideoCodec::qualityName)
    std::string preset = "slow";
    bool chroma444 = true;      // 4:4:4 (no colour subsampling) where the codec can
    std::string audio = "auto"; // auto (PCM for FFV1/ProRes, FLAC otherwise), pcm, flac, aac, opus, none
    int audioKbps = 256;        // aac/opus
};

struct EncodeProgress {
    std::atomic<int64_t> framesDone{0};
    std::atomic<int64_t> framesTotal{0};
    std::atomic<int> pass{0};    // 1 = measuring (PLL), 2 = encoding
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> ok{false};
};

// Pass 1 (if ui.pll) and pass 2: `in` -> settings.outPath. Blocking; run on
// a thread for the GUI. `err` gets the reason on failure.
// `grid`/`palette`: the pixel grid and palette to use (the GUI passes what
// its preview shows); null = found on the recording's first frame.
bool transcode(const std::string& in, const EncodeSettings& settings, const fv::PalDecodeUi& ui,
               const OutputGeometry& geometry, EncodeProgress& progress, std::string& err,
               const pal::GridEstimate* grid = nullptr, const std::vector<pal::Colour>* palette = nullptr,
               const pal::Calibration* calibration = nullptr);

}  // namespace fc

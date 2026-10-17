// SPDX-License-Identifier: GPL-3.0-or-later
// SDL2 + OpenGL (core profile) display: shader-based rendering, double-
// buffered PBO uploads, GPU-side UYVY decode (replaces the first
// fixed-function path).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "formats.h"
#include "palui.h"

struct SDL_Window;
typedef void* SDL_GLContext;

namespace fv {

enum class ScaleMode { Integer, Fit, Stretch };

struct DisplayOptions {
    ScaleMode scaleMode = ScaleMode::Integer;
    bool linearFilter = false;
    double pixelAspect = 1.0;  // display width multiplier; 1.0 = square pixels
    // Integer mode only. 0 = Auto: the largest whole multiple that fits the
    // window. 1-4 = that multiple, whatever the window size (centered, cut
    // at the window edge if the window is smaller).
    int integerFactor = 0;
    // Decode a Sync-on-Green composite (`g8`) capture as PAL colour instead
    // of showing the raw samples (tools/pal/pal.hpp).
    //
    bool palDecode = false;
    int palMultiple = 4;         // samples per colour-subcarrier cycle: 4, 8 or 10 (12x+ left out)
    int palLine = 1135;          // samples per line at 4x (1135 standard PAL; a C64 line is 1134; NTSC 910, Apple II/MSX1 912)
    int videoStandard = 0;       // 0 PAL, 1 NTSC (pal::Standard): the decoder, timing and window
    PalDecodeUi palUi;           // brightness filter, line averaging, picture controls (shared with freshCoder)
    // S-Video on an RGB capture: brightness (with sync) on one input, colour
    // on another, sampled together (0 = red, 1 = green, 2 = blue).
    bool palSvideo = false;
    int svideoLuma = 1, svideoChroma = 2;  // Y (with sync) on Green, C on Blue by default (Red the alternative)
    bool palAutoTune = false;    // auto-tune (freshVision): line length, black level, swing, pixel grid
    bool rgbAutoTune = false;    // analog RGB: phase, position (and crop) tuned after every mode change
    // The machine's whole screen (border included) in capture pixels, 0 = not
    // known. With it the RGB auto-tune copes with a black border: it keeps
    // the position when the picture is smaller than the screen and crops to
    // the screen size instead of the drawn content.
    int rgbScreenW = 0, rgbScreenH = 0;
    bool rgbAutoCrop = false;    // ... and crop to the picture (off by default: with a black border it follows the drawn content)
    bool delayPictureToSound = false;  // hold the picture back to line up with the (later) monitor sound
    // Where in the window the picture goes (window pixels from the top-left);
    // areaW/areaH 0 = the whole window. freshCoder keeps its controls beside it.
    int areaX = 0, areaY = 0, areaW = 0, areaH = 0;
};
constexpr int kMaxIntegerFactor = 4;

class Display {
public:
    ~Display();

    // Window sized to the largest integer multiple of the frame that fits the
    // desktop, or `windowW` x `windowH` when given (clamped to the screen).
    bool create(uint32_t frameW, uint32_t frameH, bool fullscreen, bool vsync, std::string& err, int windowW = 0,
                int windowH = 0);
    void destroy();

    SDL_Window* window() const { return window_; }
    SDL_GLContext glContext() const { return glContext_; }

    // (Re)creates the texture when size or format changed.
    bool setFrame(const PixelFormat& pf, uint32_t w, uint32_t h, uint32_t colorspace, std::string& err);
    // Uploads via a double-buffered PBO (DMA transfer overlaps the GPU's use
    // of the previous frame's texture). UYVY is uploaded as raw packed bytes
    // and decoded to RGB in the fragment shader (no CPU colour conversion).
    void upload(const uint8_t* data, size_t stride);
    // Draws the texture per `opts` (integer / fit / stretch, filter, pixel
    // aspect), then the caller draws any ImGui overlay, then this swaps and
    // glFinish()es so the driver does not queue frames ahead (extra latency).
    void beginFrame();
    // The window resizes freely; only the picture follows the scale mode, so
    // the space around it stays free for panels.
    void drawVideo(const DisplayOptions& opts);
    // Largest integer factor (up to kMaxIntegerFactor) at which the current
    // frame, with `pixelAspect`, fits the usable area of the screen the
    // window is on; 0 if not even 1x fits.
    int maxScreenFactor(double pixelAspect) const;
    void endFrame();

    void setTitle(const std::string& title);
    void toggleFullscreen();
    bool fullscreen() const { return fullscreen_; }
    bool vsync() const { return vsync_; }
    void setVsync(bool on);

private:
    bool compileShaders(std::string& err);
    unsigned uploadPbo(const uint8_t* data, size_t bytes);

    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    unsigned texture_ = 0;
    unsigned vao_ = 0, vbo_ = 0;
    unsigned progDirect_ = 0, progUyvy_ = 0;  // fragment shader variants
    unsigned pbo_[2] = {0, 0};
    int pboIndex_ = 0;
    size_t pboBytes_ = 0;
    PixelFormat format_{};
    uint32_t w_ = 0, h_ = 0, colorspace_ = 0;
    bool vsync_ = true;
    bool fullscreen_ = false;
};

}  // namespace fv

// SPDX-License-Identifier: GPL-3.0-or-later
// Offline test of the RGB auto-tune (src/rgbtune.*): a simulated analog
// source whose picture sharpness depends on Phase and whose position moves
// with Horizontal/Vertical Position. Checks that the tuner finds the
// sharpest phase, centres the picture both ways and crops to it.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rgbtune.h"

using namespace fv;

namespace {

struct Sim {
    int64_t phase = 0, hpos = 40, vpos = 13;
    int bestPhase = 11;
    int hGain = -1;  // picture pixels per Horizontal Position step (the start moves later: picture left)
    uint32_t W = 480, H = 298, pw = 384, ph = 270;
    // Black border: only this part of the screen is drawn (offsets inside it).
    bool blackBorder = false;
    int cx = 20, cy = 10, cw = 300, ch = 200;
    std::vector<uint8_t> buf;
    int x0() const { return 60 + int(hGain * (hpos - 40)); }
    int y0() const { return int(vpos) + 5; }
    void render(unsigned seed) {
        buf.assign(size_t(W) * H * 4, 0);
        const int d = std::min<int>(std::abs(int(phase) - bestPhase), 32 - std::abs(int(phase) - bestPhase));
        const double sharp = 1.0 - 0.6 * d / 16.0;
        std::srand(seed);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                uint8_t* p = &buf[(size_t(y) * W + x) * 4];
                const int px = int(x) - x0(), py = int(y) - y0();
                int v = std::rand() % 6;
                const bool drawn = !blackBorder || (px >= cx && py >= cy && px < cx + cw && py < cy + ch);
                if (px >= 0 && py >= 0 && px < int(pw) && py < int(ph) && drawn) {
                    const bool on = (px / 4) % 2;
                    v = int(60 + (on ? 150 : 0) * sharp + (on ? 0 : 150 * (1 - sharp))) + std::rand() % 6;
                }
                p[0] = p[1] = p[2] = uint8_t(std::min(v, 255));
            }
    }
};

}  // namespace

// One run of the tuner on a simulated source; true if it ends as expected.
bool runCase(const char* name, bool blackBorder, int screenW, int screenH, int64_t hpos, int64_t vpos, bool expectCentred,
             const Sim& base = Sim{}) {
    Sim sim = base;
    sim.blackBorder = blackBorder;
    sim.hpos = hpos, sim.vpos = vpos;
    const int64_t hpos0 = hpos, vpos0 = vpos;
    Rect cropped;
    RgbAutoTune t;
    RgbAutoTune::Io io;
    io.control = [&](const char* n0, int64_t& mn, int64_t& mx, int64_t& v) {
        const std::string n = n0;
        if (n == "Phase") return mn = 0, mx = 31, v = sim.phase, true;
        if (n == "Horizontal Position") return mn = 10, mx = 524, v = sim.hpos, true;
        if (n == "Vertical Position") return mn = 3, mx = 14, v = sim.vpos, true;
        return false;
    };
    io.set = [&](const char* n0, int64_t v) {
        const std::string n = n0;
        if (n == "Phase") sim.phase = v;
        if (n == "Horizontal Position") sim.hpos = v;
        if (n == "Vertical Position") sim.vpos = v;
    };
    io.crop = [&](const Rect& r) { cropped = r; };
    io.currentCrop = [] { return Rect{}; };
    std::printf("== %s\n", name);
    t.start(true, screenW, screenH);
    int frames = 0;
    for (; frames < 5000 && t.running(); ++frames) {
        sim.render(unsigned(frames));
        t.onFrame(FrameView{sim.buf.data(), sim.W, sim.H, sim.W * 4, Layout::Bgrx8}, io);
    }
    const int left = sim.x0(), right = int(sim.W) - int(sim.pw) - sim.x0();
    const int top = sim.y0(), bottom = int(sim.H) - int(sim.ph) - sim.y0();
    std::printf("after %d frames: Phase %lld (best %d), screen margins H %d/%d (Position %lld), V %d/%d (Position %lld), "
                "crop %s %d,%d %ux%u\n",
                frames, (long long)sim.phase, sim.bestPhase, left, right, (long long)sim.hpos, top, bottom,
                (long long)sim.vpos, cropped.set ? "set" : "none", cropped.x, cropped.y, cropped.w, cropped.h);
    bool ok = sim.phase == sim.bestPhase && cropped.set && std::abs(int(cropped.w) - int(sim.pw)) <= 2 &&
              std::abs(int(cropped.h) - int(sim.ph)) <= 2 && std::abs(cropped.x - left) <= 1 && std::abs(cropped.y - top) <= 1;
    if (expectCentred) ok = ok && std::abs(left - right) <= 2 && std::abs(top - bottom) <= 2;
    else ok = ok && sim.hpos == hpos0 && sim.vpos == vpos0;  // position kept
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main() {
    bool ok = true;
    // 1. Visible border, no screen size: centre and crop to the picture.
    ok &= runCase("visible border, screen size unknown", false, 0, 0, 40, 13, true);
    // 2. Visible border, screen size known: the same result.
    ok &= runCase("visible border, screen size known", false, 384, 270, 40, 13, true);
    // 3. Black border, screen size known, position already right (as saved
    //    in the machine mode): the position stays, the crop is the screen.
    ok &= runCase("black border, screen size known", true, 384, 270, 52, 9, false);
    // 4. BBC Micro RGB mode (bbc-micro-b.conf): 672x288 capture at 1024
    //    samples per line, a 640x256 screen whose border is always black,
    //    only some text drawn; screen size from the mode: position kept,
    //    crop = the screen.
    {
        Sim bbc;
        bbc.W = 672, bbc.H = 288, bbc.pw = 640, bbc.ph = 256;
        bbc.cx = 0, bbc.cy = 0, bbc.cw = 320, bbc.ch = 80;
        ok &= runCase("BBC Micro RGB: black border, text only, screen 640x256", true, 640, 256, 84, 11, false, bbc);
    }
    std::printf("%s\n", ok ? "all cases passed" : "FAILED");
    return ok ? 0 : 1;
}

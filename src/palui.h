// SPDX-License-Identifier: GPL-3.0-or-later
// PAL composite decode settings and their ImGui controls, shared by
// freshVision's Display panel and freshCoder, so both show the same controls
// for the same decoder (tools/pal/pal.hpp).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fv {

struct PalPicture {
    float brightness = 0;  // added to luma, -0.5..0.5 of full scale
    float contrast = 1;    // luma and colour gain
    float saturation = 1;  // colour gain
    float hue = 0;         // colour rotation, degrees
    float gamma = 1;       // 1 = none
};

struct PalDecodeUi {
    int luma = 1;              // pal::LumaFilter order: 0 box (soft), 1 notch (sharp), 2 off (raw)
    bool averageLines = true;  // PAL delay-line method against Hanover bars
    bool removeStripes = false;
    bool relock = false;
    int chromaCycles = 2;        // colour smoothing (DecodeParams::chromaCycles)
    float edgeDesat = 0;         // edge colour reduction (DecodeParams::edgeDesat), 0-1
    bool mono = false;           // black and white source, no colour burst (DecodeParams::mono)
    bool softText = false;       // two-colour text, soft (pal::softTwoColour)         // re-align rolling frames + black per line (pal::VerticalRelock, DecodeParams::lineClamp)  // subtract the fixed pattern locked to the line (C64 clock stripes, pal::LinePattern)
    PalPicture picture;
    bool pll = true;           // freshCoder: follow the carrier over the recording (software PLL)
    float pllBandwidth = 0.2f; // freshCoder: 0 = one frequency for the whole recording, 1 = every frame on its own
    // freshCoder, RGB recordings: how the inputs are read (fc::SvideoMode:
    // 0 as the recording says, 1 S-Video with these inputs, 2 composite from
    // svideoLuma); inputs 0 red, 1 green, 2 blue.
    int svideoMode = 0, svideoLuma = 1, svideoChroma = 2;
    int standardMode = 0;  // freshCoder: 0 = as the recording says (tag FRESHVISION_STANDARD, else PAL), 1 = PAL, 2 = NTSC
    bool lumaFilterUnused = false;  // set by the owner while S-Video is decoded (brightness is taken unfiltered)
    // The machine's own pixels (tools/pal/grid.hpp).
    int profile = 1;           // index into pal::machineProfiles() (1 = C64 PAL)
    bool pixelGrid = false;    // one output pixel per machine pixel, taken from its settled centre
    float gridCore = 0.5f;     // central part of each pixel that is averaged
    bool snap = false;         // snap to the machine's palette
    int colours = 16;          // palette size for a learned palette
    // Where the snapping colours come from: 0 = the machine's palette (the
    // profile's documented one), 1 = custom (edited / loaded here),
    // 2 = learned from the picture. With 0 and 1 the output holds exactly
    // those colours; the picture only calibrates the decoder onto them.
    int paletteSource = 0;
    bool c64Cells = false;
    bool tmsCells = false;     // TMS9918 (MSX1): two colours per 8-pixel row segment, machine or custom palette     // C64: colours decided per 8x8 cell (2 hires / 4 multicolour), machine or custom 16-colour palette
    std::vector<std::array<uint8_t, 3>> customPalette;
    char paletteFile[512] = "";
    bool requestRedetect = false, requestRelearn = false;  // set by buttons, cleared by the owner
};

// What the grid stage currently uses, for the controls to show.
struct PalGridStatus {
    bool valid = false;
    double period = 0, phase = 0, score = 0;
    int pixelsPerLine = 0;
    std::vector<std::array<uint8_t, 3>> palette;
    double calSat = 1, calHueDeg = 0, calContrast = 1, calOffset = 0;  // the calibration in use
};

// The palette the settings ask for (machine / custom); empty = learn it.
std::vector<std::array<uint8_t, 3>> chosenPalette(const PalDecodeUi& s);
// Palette files: GIMP .gpl, or one "RRGGBB" / "#RRGGBB" per line.
bool loadPaletteFile(const std::string& path, std::vector<std::array<uint8_t, 3>>& out, std::string& err);
bool savePaletteFile(const std::string& path, const std::vector<std::array<uint8_t, 3>>& pal, const std::string& name,
                     std::string& err);
std::string paletteFolder();  // ~/.config/freshvision/palettes (created on demand)

// Draws the brightness filter, line averaging, the PLL controls (when
// `withPll`), the picture controls and the pixel grid section (`grid`: the
// current state, may be null). Returns true if anything changed.
bool drawPalDecodeControls(PalDecodeUi& s, bool withPll, const PalGridStatus* grid = nullptr);

}  // namespace fv

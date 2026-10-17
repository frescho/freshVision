// SPDX-License-Identifier: GPL-3.0-or-later
#include "palui.h"
#include "platform.h"

#include <algorithm>

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "grid.hpp"

namespace fv {

std::vector<std::array<uint8_t, 3>> chosenPalette(const PalDecodeUi& s) {
    const auto& profiles = pal::machineProfiles();
    const auto& prof = profiles[size_t(std::max(0, std::min(s.profile, int(profiles.size()) - 1)))];
    if (s.paletteSource == 0) return prof.palette;
    if (s.paletteSource == 1) return s.customPalette;
    return {};
}

std::string paletteFolder() {
    std::string dir = (std::filesystem::u8path(settingsBaseDir()) / "palettes").u8string();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool loadPaletteFile(const std::string& path, std::vector<std::array<uint8_t, 3>>& out, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "cannot read " + path;
        return false;
    }
    out.clear();
    auto isHex6 = [](const std::string& t) {
        return t.size() == 6 && t.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    };
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string first;
        if (!(ls >> first)) continue;                        // empty line
        if (first == "GIMP" || first.rfind("Name:", 0) == 0 || first.rfind("Columns:", 0) == 0) continue;
        std::string hex = first[0] == '#' ? first.substr(1) : first;
        unsigned r, g, b;
        if (isHex6(hex) && std::sscanf(hex.c_str(), "%02x%02x%02x", &r, &g, &b) == 3) {  // RRGGBB / #RRGGBB
            out.push_back({uint8_t(r), uint8_t(g), uint8_t(b)});
            continue;
        }
        if (first[0] == '#') continue;                      // a comment
        std::istringstream nums(line);                      // GIMP .gpl: "R G B name"
        if (nums >> r >> g >> b && r < 256 && g < 256 && b < 256) out.push_back({uint8_t(r), uint8_t(g), uint8_t(b)});
    }
    if (out.empty()) {
        err = "no colours found in " + path;
        return false;
    }
    return true;
}

bool savePaletteFile(const std::string& path, const std::vector<std::array<uint8_t, 3>>& pal, const std::string& name,
                     std::string& err) {
    std::ofstream f(path);
    if (!f) {
        err = "cannot write " + path;
        return false;
    }
    f << "GIMP Palette\nName: " << name << "\nColumns: 16\n#\n";
    for (const auto& c : pal) f << int(c[0]) << " " << int(c[1]) << " " << int(c[2]) << "\tcolour\n";
    return true;
}

bool drawPalDecodeControls(PalDecodeUi& s, bool withPll, const PalGridStatus* grid) {
    bool changed = false;
    changed |= ImGui::Checkbox("Black and white source (no colour burst)", &s.mono);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For a machine without colour (e.g. a Microkey Primo): no burst is looked for, black is\n"
                          "read on each line's back porch and the brightness is taken unfiltered (full detail).\n"
                          "The picture is grey instead of green.");
    changed |= ImGui::Checkbox("Average colour of line pairs (against Hanover bars)", &s.averageLines);
    ImGui::SetNextItemWidth(120);
    changed |= ImGui::SliderInt("colour smoothing", &s.chromaCycles, 1, 12, "%d cycles");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How many colour-carrier cycles the colour is averaged over. 2 = sharpest colour;\n"
                          "more fades the colour fringes around fine brightness detail (MSX1 text) at the\n"
                          "cost of colour resolution, like a TV's narrow colour filter.");
    ImGui::SetNextItemWidth(120);
    changed |= ImGui::SliderFloat("edge colour reduction", &s.edgeDesat, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reduces colour where the brightness has fine detail: the colour fringes of\n"
                          "composite sit exactly there (white text on a coloured background); flat areas\n"
                          "keep their colour. 0 = off.");
    changed |= ImGui::Checkbox("Two-colour text (soft)", &s.softText);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For text screens (one background, one text colour, e.g. MSX BASIC): the two colours\n"
                          "are found and snapped to the machine's palette, every point is a blend of them by\n"
                          "its brightness. No colour fringes, soft edges, no flicker. Not for colourful screens.");
    changed |= ImGui::Checkbox("Re-align rolling frames", &s.relock);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For a source whose vertical sync the card doesn't hold (an Apple IIc): each frame is\n"
                          "turned so its vertical sync comes first, and black is read from every line's own\n"
                          "blanking. Needs the whole frame captured (Vertical Timing = the source's lines) and\n"
                          "the black lifted above sync (auto-tune does that). Frames that can't be placed\n"
                          "keep the previous picture.");
    changed |= ImGui::Checkbox("Remove fixed clock stripes", &s.removeStripes);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Learns the faint vertical stripes the computer's own clock leaves in its video\n"
                          "(C64: one character column wide, locked to the line) from flat parts of the\n"
                          "picture over a few seconds and subtracts them. Nothing is filtered: sharpness stays.");
    const char* lumas[] = {"Box (soft)", "Notch (sharp)", "Off (raw, carrier dots)"};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Off (raw, carrier dots)").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                            ImGui::GetFrameHeight());
    ImGui::BeginDisabled(s.lumaFilterUnused);
    changed |= ImGui::Combo("brightness filter", &s.luma, lumas, 3);
    ImGui::EndDisabled();
    if (s.lumaFilterUnused) {
        ImGui::SameLine();
        ImGui::TextDisabled("(not used for S-Video)");
    }
    if (withPll) {
        changed |= ImGui::Checkbox("Follow the colour carrier (software PLL)", &s.pll);
        ImGui::BeginDisabled(!s.pll);
        ImGui::SetNextItemWidth(160);
        changed |= ImGui::SliderFloat("PLL speed", &s.pllBandwidth, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How fast the carrier estimate follows: low = steady, high = follows a drifting\n"
                              "machine quickly (freshCoder: 0 = one frequency for the whole recording).");
        ImGui::EndDisabled();
    }
    if (ImGui::TreeNode("Picture")) {
        const float w = 200;
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::SliderFloat("brightness", &s.picture.brightness, -0.5f, 0.5f, "%.2f");
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::SliderFloat("contrast", &s.picture.contrast, 0.25f, 2.0f, "%.2f");
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::SliderFloat("saturation", &s.picture.saturation, 0.0f, 2.5f, "%.2f");
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::SliderFloat("hue", &s.picture.hue, -45.0f, 45.0f, "%.1f deg");
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::SliderFloat("gamma", &s.picture.gamma, 0.5f, 2.0f, "%.2f");
        if (ImGui::Button("Reset picture")) {
            s.picture = PalPicture{};
            changed = true;
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Pixel grid (machine pixels)")) {
        const auto& profiles = pal::machineProfiles();
        s.profile = std::max(0, std::min(s.profile, int(profiles.size()) - 1));
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("machine", profiles[size_t(s.profile)].name)) {
            for (size_t i = 0; i < profiles.size(); ++i)
                if (ImGui::Selectable(profiles[i].name, int(i) == s.profile)) {
                    s.profile = int(i);
                    if (profiles[i].colours > 0) s.colours = profiles[i].colours;
                    if (profiles[i].palette.empty() && s.paletteSource == 0) s.paletteSource = 2;
                    s.requestRedetect = s.requestRelearn = true;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("%s", profiles[size_t(s.profile)].note);
        changed |= ImGui::Checkbox("Sample the machine's pixels (from their settled centres)", &s.pixelGrid);
        ImGui::BeginDisabled(!s.pixelGrid);
        ImGui::SetNextItemWidth(160);
        changed |= ImGui::SliderFloat("pixel core", &s.gridCore, 0.1f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How much of each pixel is averaged: small = only the settled middle, 1 = the whole pixel.");
        changed |= ImGui::Checkbox("Snap to the palette", &s.snap);
        const auto& prof = profiles[size_t(s.profile)];
        const char* sources[] = {"the machine's palette", "custom palette", "learned from the picture"};
        ImGui::SetNextItemWidth(220);
        if (ImGui::Combo("palette", &s.paletteSource, sources, 3)) s.requestRelearn = true, changed = true;
        if (s.paletteSource == 0 && prof.palette.empty())
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "This machine has no fixed palette: choose custom or learned.");
        if (s.paletteSource == 2) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("colours", &s.colours, 1, 4)) {
                s.colours = std::max(2, std::min(s.colours, 128));
                s.requestRelearn = true;
                changed = true;
            }
        }
        if (s.paletteSource == 1 && ImGui::TreeNode("Custom palette")) {
            if (ImGui::Button("Start from the machine's")) s.customPalette = prof.palette, s.requestRelearn = true, changed = true;
            ImGui::SameLine();
            if (ImGui::Button("Start from the learned") && grid) {
                s.customPalette = grid->palette;
                s.requestRelearn = true;
                changed = true;
            }
            for (size_t i = 0; i < s.customPalette.size(); ++i) {
                auto& c = s.customPalette[i];
                float f[3] = {c[0] / 255.f, c[1] / 255.f, c[2] / 255.f};
                char id[32];
                std::snprintf(id, sizeof id, "##cp%zu", i);
                if (ImGui::ColorEdit3(id, f, ImGuiColorEditFlags_NoInputs)) {
                    for (int k = 0; k < 3; ++k) c[size_t(k)] = uint8_t(f[k] * 255.f + 0.5f);
                    s.requestRelearn = true;
                    changed = true;
                }
                if ((i + 1) % 16 != 0 && i + 1 < s.customPalette.size()) ImGui::SameLine(0, 2);
            }
            if (ImGui::Button("Add colour")) s.customPalette.push_back({128, 128, 128}), s.requestRelearn = true, changed = true;
            ImGui::SameLine();
            if (ImGui::Button("Remove last") && !s.customPalette.empty())
                s.customPalette.pop_back(), s.requestRelearn = true, changed = true;
            if (!s.paletteFile[0]) std::snprintf(s.paletteFile, sizeof s.paletteFile, "%s/my-palette.gpl", paletteFolder().c_str());
            ImGui::SetNextItemWidth(-110);
            ImGui::InputText("##palfile", s.paletteFile, sizeof s.paletteFile);
            ImGui::SameLine();
            static std::string palMsg;
            if (ImGui::Button("Load")) {
                std::string e;
                std::vector<std::array<uint8_t, 3>> loaded;
                palMsg = loadPaletteFile(s.paletteFile, loaded, e) ? "loaded " + std::to_string(loaded.size()) + " colours" : e;
                if (!loaded.empty()) s.customPalette = loaded, s.requestRelearn = true, changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                std::string e;
                palMsg = savePaletteFile(s.paletteFile, s.customPalette, prof.name, e) ? "saved" : e;
            }
            if (!palMsg.empty()) ImGui::TextDisabled("%s", palMsg.c_str());
            ImGui::TreePop();
        }
        if (pal::isC64Profile(s.profile)) {
            ImGui::BeginDisabled(!s.snap || s.paletteSource == 2);
            changed |= ImGui::Checkbox("C64 cells: 2 colours per 8x8 (4 in multicolour)", &s.c64Cells);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Decides each character cell's colours first, then each pixel within them\n"
                                  "(the VIC-II's own rule). Test pictures: multicolour 56%% -> 76%% of pixels right.");
            ImGui::EndDisabled();
        }
        if (pal::isTmsProfile(s.profile)) {
            ImGui::BeginDisabled(!s.snap || s.paletteSource == 2);
            changed |= ImGui::Checkbox("TMS9918: 2 colours per 8 pixels", &s.tmsCells);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Decides the two colours of every 8-pixel row segment first (the MSX1 video\n"
                                  "chip's own rule), preferring colours seen in wide areas, then each pixel by\n"
                                  "brightness: thin text strokes keep their colour. Test pictures: text 67%% -> 98%%.");
            ImGui::EndDisabled();
        }
        if (ImGui::Button("Re-detect grid")) s.requestRedetect = true, changed = true;
        ImGui::SameLine();
        if (ImGui::Button(s.paletteSource == 2 ? "Re-learn palette" : "Re-calibrate")) s.requestRelearn = true, changed = true;
        if (grid) {
            if (grid->valid)
                ImGui::TextDisabled("grid: %.4f samples per pixel, phase %.2f, fit %.2f, %d pixels per line", grid->period,
                                    grid->phase, grid->score, grid->pixelsPerLine);
            else
                ImGui::TextDisabled("grid: not found yet");
            if (s.snap && s.paletteSource != 2)
                ImGui::TextDisabled("calibration: saturation x%.2f, hue %+.1f deg, contrast x%.2f, offset %+.0f", grid->calSat,
                                    grid->calHueDeg, grid->calContrast, grid->calOffset);
            for (size_t i = 0; i < grid->palette.size(); ++i) {
                const auto& c = grid->palette[i];
                char id[32];
                std::snprintf(id, sizeof id, "##pal%zu", i);
                ImGui::ColorButton(id, ImVec4(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1), ImGuiColorEditFlags_NoTooltip,
                                   ImVec2(16, 16));
                if ((i + 1) % 16 != 0 && i + 1 < grid->palette.size()) ImGui::SameLine(0, 2);
            }
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    return changed;
}

}  // namespace fv

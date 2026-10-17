// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui.h"

#include <imgui.h>

#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>

#include <algorithm>
#include <cstdio>
#include <linux/videodev2.h>

namespace fv {

namespace {

const char* typeLabel(uint32_t t) {
    switch (t) {
        case V4L2_CTRL_TYPE_INTEGER: return "int";
        case V4L2_CTRL_TYPE_BOOLEAN: return "bool";
        case V4L2_CTRL_TYPE_MENU: return "menu";
        case V4L2_CTRL_TYPE_BUTTON: return "button";
        case V4L2_CTRL_TYPE_INTEGER64: return "int64";
        case V4L2_CTRL_TYPE_STRING: return "string";
        case V4L2_CTRL_TYPE_BITMASK: return "bitmask";
        case V4L2_CTRL_TYPE_INTEGER_MENU: return "intmenu";
        default: return "other";
    }
}

bool nonEditable(const ControlInfo& c) {
    return (c.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_GRABBED |
                       V4L2_CTRL_FLAG_INACTIVE)) ||
           controlIsDetectedOnly(c);
}

// "def N" + a reset button (or, when the control can't be edited, the reason
// why), drawn on the same line as whatever came just before it.
void drawControlHint(const ControlInfo& c, bool disabled, const GuiActions& actions) {
    ImGui::SameLine();
    if (!disabled) {
        ImGui::TextDisabled("def %lld", (long long)c.def);
        if (c.value != c.def) {
            ImGui::SameLine();
            if (ImGui::SmallButton("reset") && actions.resetControl) actions.resetControl(c.id);
        }
        return;
    }
    const char* why = "grabbed by another handle";
    if (controlIsDetectedOnly(c)) why = "detected by the driver";
    else if (c.flags & V4L2_CTRL_FLAG_INACTIVE) why = "inactive for the current signal";
    else if (c.flags & V4L2_CTRL_FLAG_DISABLED) why = "disabled";
    else if (c.flags & V4L2_CTRL_FLAG_READ_ONLY) why = "read-only";
    ImGui::TextDisabled("(%s)", why);
}

void drawOneControl(const ControlInfo& c, const GuiActions& actions) {
    if (c.type == V4L2_CTRL_TYPE_CTRL_CLASS) {
        ImGui::SeparatorText(c.name.c_str());
        return;
    }
    ImGui::PushID(int(c.id));
    bool disabled = nonEditable(c);
    if (disabled) ImGui::BeginDisabled();
    // Sliders and combos fill the row, so their hint goes on a separate name
    // line above them; anything placed after them on the same row got
    // clipped by the panel edge (a value like "def 32" showing as "d").
    bool hintDrawn = false;

    switch (c.type) {
        case V4L2_CTRL_TYPE_BOOLEAN: {
            bool v = c.value != 0;
            if (ImGui::Checkbox(c.name.c_str(), &v) && actions.setControl) actions.setControl(c.id, v ? 1 : 0);
            break;
        }
        case V4L2_CTRL_TYPE_BUTTON: {
            if (ImGui::Button(c.name.c_str()) && actions.setControl) actions.setControl(c.id, 1);
            break;
        }
        case V4L2_CTRL_TYPE_MENU:
        case V4L2_CTRL_TYPE_INTEGER_MENU: {
            int curIdx = -1;
            for (size_t i = 0; i < c.menu.size(); ++i)
                if (c.menu[i].first == c.value) curIdx = int(i);
            const char* preview = (curIdx >= 0) ? c.menu[curIdx].second.c_str() : "?";
            ImGui::TextUnformatted(c.name.c_str());
            drawControlHint(c, disabled, actions);
            hintDrawn = true;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##combo", preview)) {
                for (size_t i = 0; i < c.menu.size(); ++i) {
                    bool sel = int(i) == curIdx;
                    if (ImGui::Selectable(c.menu[i].second.c_str(), sel) && actions.setControl)
                        actions.setControl(c.id, c.menu[i].first);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case V4L2_CTRL_TYPE_INTEGER: {
            int v = int(c.value);
            // The slider's own value text is hidden (format " " has no '%',
            // so nothing is printed inside the bar, where it used to clip
            // "194" to "19"); the number box is the one place the value shows.
            ImGui::TextUnformatted(c.name.c_str());
            drawControlHint(c, disabled, actions);
            hintDrawn = true;
            const float boxW = ImGui::CalcTextSize("-00000000").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float sliderW = ImGui::GetContentRegionAvail().x - boxW - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(std::max(40.0f, sliderW));
            bool changed = ImGui::SliderInt("##slider", &v, int(c.min), int(c.max), " ");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(boxW);
            if (ImGui::InputInt("##typed", &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) changed = true;
            if (changed && actions.setControl) actions.setControl(c.id, std::clamp(v, int(c.min), int(c.max)));
            break;
        }
        default: {
            ImGui::Text("%s: %lld (%s, no editor)", c.name.c_str(), (long long)c.value, typeLabel(c.type));
            break;
        }
    }
    if (!hintDrawn) drawControlHint(c, disabled, actions);
    if (disabled) ImGui::EndDisabled();
    ImGui::PopID();
}

const char* encoderLabel(VideoEncoderKind k) {
    switch (k) {
        case VideoEncoderKind::Auto: return "Auto";
        case VideoEncoderKind::Nvenc: return "NVENC (H.264)";
        case VideoEncoderKind::Vaapi: return "VAAPI (H.264)";
        case VideoEncoderKind::X264: return "x264 (H.264)";
        case VideoEncoderKind::Ffv1: return "FFV1 (lossless)";
        case VideoEncoderKind::Raw: return "Uncompressed (lossless)";
    }
    return "?";
}

// The settings the recorder really uses (FFmpeg defaults; nothing is tuned
// yet), in plain words. FFV1 is fed RGB, so it is pixel-exact (verified).
// The H.264 encoders convert to 4:2:0 colour.
const char* encoderDescription(VideoEncoderKind k) {
    switch (k) {
        case VideoEncoderKind::Nvenc: return "H.264 on the graphics card, preset p4, automatic quality, 4:2:0 colour.";
        case VideoEncoderKind::Vaapi: return "H.264 via VAAPI, 4:2:0 colour.";
        case VideoEncoderKind::X264: return "H.264 on the CPU, constant quality CRF 23, preset medium, 4:2:0 colour.";
        case VideoEncoderKind::Ffv1:
            return "FFV1 lossless, pixel-exact: RGB, or one channel of raw samples for a g8 (composite) capture. "
                   "Very large files.";
        case VideoEncoderKind::Raw:
            return "No compression at all, pixel-exact: RGB, or one channel of raw samples for a g8 (composite) "
                   "capture. Almost no CPU, huge files.";
        case VideoEncoderKind::Auto: return "No working encoder found.";
    }
    return "";
}

// Measured on the C64 (672x504, 50 fps). FFV1 is
// scaled by pixel rate (~1.06 bytes per pixel there); the H.264 figures
// depend mostly on how much moves, so they're given as measured, not scaled.
std::string sizeEstimate(VideoEncoderKind k, uint32_t w, uint32_t h, double fps) {
    char buf[200];
    if (k == VideoEncoderKind::Ffv1) {
        double mbMin = 1.06 * double(w) * double(h) * (fps > 0 ? fps : 50.0) * 60.0 / (1024.0 * 1024.0);
        std::snprintf(buf, sizeof buf, "Expected video size: about %.0f MB/min (scaled from a C64 measurement).", mbMin);
    } else if (k == VideoEncoderKind::Raw) {
        double mbMin = double(w) * double(h) * (fps > 0 ? fps : 50.0) * 60.0 / (1024.0 * 1024.0);
        std::snprintf(buf, sizeof buf, "Video size: %.0f MB/min for a g8 capture, %.0f MB/min for RGB.", mbMin,
                      4.0 * mbMin);
    } else if (k == VideoEncoderKind::Nvenc) {
        std::snprintf(buf, sizeof buf, "Video size measured on the C64 title screen: ~14 MB/min; more with motion.");
    } else if (k == VideoEncoderKind::X264) {
        std::snprintf(buf, sizeof buf, "Video size measured on the C64 title screen: ~1.3 MB/min; much more with motion.");
    } else {
        buf[0] = 0;
    }
    return buf;
}

}  // namespace

bool Gui::init(SDL_Window* window, SDL_GLContext ctx, std::string& err) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini left behind in the working directory
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL2_InitForOpenGL(window, ctx)) {
        err = "ImGui_ImplSDL2_InitForOpenGL failed";
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 150")) {
        err = "ImGui_ImplOpenGL3_Init failed";
        ImGui_ImplSDL2_Shutdown();
        return false;
    }
    initialized_ = true;
    return true;
}

void Gui::shutdown() {
    if (!initialized_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void Gui::processEvent(const SDL_Event& e) { ImGui_ImplSDL2_ProcessEvent(&e); }

bool Gui::wantCaptureKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
bool Gui::wantCaptureMouse() const { return ImGui::GetIO().WantCaptureMouse; }

void Gui::newFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void Gui::draw(bool visible, const std::string& statsText, const std::vector<ControlInfo>& controls,
               DisplayOptions& dispOpts, const GeometryState& geom, const std::vector<SourceInfo>& sources,
               const std::string& activePath, const AudioPanelState& audio, const RecorderPanelState& recorder,
               const PresetPanelState& presets, const MachinePanelState& machines, const GuiActions& actions) {
    if (!visible) {
        ImGui::Render();  // still end the frame ImGui::NewFrame() started
        return;
    }

    // One control column on the left (freshCoder's layout
    // over the floating panels), full height, width adjustable by dragging
    // its right edge; the picture fills the rest (main.cpp reads
    // panelWidth()). Sections collapse; Tab hides the whole column.
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelW_, vp->WorkSize.y), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(320, vp->WorkSize.y), ImVec2(std::max(320.0f, vp->WorkSize.x * 0.7f), vp->WorkSize.y));
    }
    ImGui::Begin("freshVision", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    panelW_ = ImGui::GetWindowWidth();
    ImGui::TextDisabled("freshVision   (Tab hides this column)");
    drawMachines(machines, actions);
    if (ImGui::CollapsingHeader("Stats and source", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted(statsText.c_str());  // fixed lines (main.cpp): nothing reflows
        ImGui::Separator();
        ImGui::Text("Source:");
        for (const auto& s : sources) {
            bool active = s.path == activePath;
            std::string label = s.path + "  " + s.name;
            if (s.noPower) label += " [no power]";
            else if (!s.hasSignal) label += " [no signal]";
            if (active) {
                ImGui::BeginDisabled();
                ImGui::Button((label + "  (active)").c_str());
                ImGui::EndDisabled();
            } else if (ImGui::Button(label.c_str()) && actions.selectSource) {
                actions.selectSource(s.path);
            }
        }
        if (sources.empty()) ImGui::TextDisabled("(no capture devices found)");
        ImGui::Separator();
        ImGui::TextDisabled("Tab hides the column; the picture then uses the whole window.");
        }
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        int mode = int(dispOpts.scaleMode);
        const char* modes[] = {"Integer", "Fit", "Stretch"};
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::Combo("##scalemode", &mode, modes, 3)) dispOpts.scaleMode = ScaleMode(mode);
        if (dispOpts.scaleMode == ScaleMode::Integer) {
            // Auto plus only the factors whose picture fits this screen. A
            // fixed factor that no longer fits (source got bigger) is still
            // shown as the current choice so it's never silently changed.
            ImGui::SameLine();
            char cur[48];
            if (dispOpts.integerFactor <= 0) std::snprintf(cur, sizeof cur, "Auto");
            else if (dispOpts.integerFactor > geom.maxScreenFactor)
                std::snprintf(cur, sizeof cur, "%dx (too big)", dispOpts.integerFactor);
            else std::snprintf(cur, sizeof cur, "%dx", dispOpts.integerFactor);
            // Sized to its own text so the preview is never clipped.
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("4x (too big)").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                                    ImGui::GetFrameHeight());
            if (ImGui::BeginCombo("##factor", cur)) {
                if (ImGui::Selectable("Auto", dispOpts.integerFactor <= 0)) dispOpts.integerFactor = 0;
                for (int f = 1; f <= std::min(geom.maxScreenFactor, kMaxIntegerFactor); ++f) {
                    char label[16];
                    std::snprintf(label, sizeof label, "%dx", f);
                    if (ImGui::Selectable(label, dispOpts.integerFactor == f)) dispOpts.integerFactor = f;
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("scale");
        ImGui::Checkbox("linear filter", &dispOpts.linearFilter);
        float par = float(dispOpts.pixelAspect);
        if (ImGui::DragFloat("pixel aspect", &par, 0.005f, 0.25f, 2.0f, "%.4f")) dispOpts.pixelAspect = par;
        if (ImGui::Button("1:1")) dispOpts.pixelAspect = 1.0;
        ImGui::SameLine();
        if (ImGui::Button("4:3 display") && geom.height) {
            dispOpts.pixelAspect = (4.0 / 3.0) / (double(geom.width) / double(geom.height));
        }
        ImGui::Separator();
        // Decode PAL = composite on Green; S-Video under it = the two-wire
        // connection, Y (with sync) on Green and C on Blue or Red.
        const bool canDecode = geom.grey8 || geom.rgbChannels;
        ImGui::BeginDisabled(!canDecode);
        if (ImGui::Checkbox("Decode", &dispOpts.palDecode) && !dispOpts.palDecode) dispOpts.palSvideo = false;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A composite colour signal on the Green input, decoded to colour.");
        ImGui::SameLine();
        {
            // The colour standard: PAL (default) or NTSC; SECAM later.
            static const char* stds[] = {"PAL", "NTSC", "SECAM (later)"};
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("NTSC").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight());
            if (ImGui::BeginCombo("##standard", stds[std::clamp(dispOpts.videoStandard, 0, 1)])) {
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::Selectable(stds[i], dispOpts.videoStandard == i, i == 2 ? ImGuiSelectableFlags_Disabled : 0) &&
                        i != dispOpts.videoStandard) {
                        dispOpts.videoStandard = i;
                        // The line field to the standard's usual value when it doesn't fit.
                        if (i == 1 && dispOpts.palLine > 1000) dispOpts.palLine = 910;
                        if (i == 0 && dispOpts.palLine < 1100) dispOpts.palLine = 1135;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!dispOpts.palDecode || dispOpts.palSvideo);  // composite only: it sets the Green levels
        ImGui::Checkbox("Auto-tune", &dispOpts.palAutoTune);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Composite: sets the line length, black level (Green Brightness) and swing (Green Contrast)\n"
                              "by itself, and re-finds the pixel grid; every change is logged. The sampling multiple\n"
                              "stays yours. Not used with S-Video.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (!canDecode) {
            ImGui::SameLine();
            ImGui::TextDisabled("(needs g8 or an RGB format)");
        }
        ImGui::BeginDisabled(!dispOpts.palDecode);
        bool sv = dispOpts.palSvideo;
        if (ImGui::Checkbox("S-Video (two wires)", &sv)) {
            dispOpts.palSvideo = sv;
            if (sv) {
                dispOpts.svideoLuma = 1;  // Y is on Green: the card syncs on it
                if (dispOpts.svideoChroma != 0) dispOpts.svideoChroma = 2;
                if (geom.grey8 && actions.switchToRgbForSvideo) actions.switchToRgbForSvideo();
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Brightness (Y, with sync) on Green and colour (C) on its own input: decoded like\n"
                              "composite, but brightness keeps its full detail. Needs an RGB capture (switched to\n"
                              "by itself). Tick Decode first.");
        if (dispOpts.palSvideo) {
            ImGui::SameLine();
            ImGui::TextUnformatted("Y + sync on Green, C on");
            ImGui::SameLine();
            const char* cs[] = {"Blue", "Red"};
            int ci = dispOpts.svideoChroma == 0 ? 1 : 0;
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("Blue").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight());
            if (ImGui::Combo("##svc", &ci, cs, 2)) dispOpts.svideoChroma = ci == 1 ? 0 : 2;
            dispOpts.svideoLuma = 1;
        }
        ImGui::EndDisabled();
        dispOpts.palUi.lumaFilterUnused = dispOpts.palDecode && dispOpts.palSvideo;
        drawPalDecodeControls(dispOpts.palUi, true, &geom.palGrid);
        {
            // 4x, 8x and 10x: from 12x on the card doesn't
            // deliver a fully correct frame on this input, and
            // 16x gives unusable samples.
            // NTSC also 6x and 12x (an MSX pixel = 4 / 8 samples).
            const bool ntsc = dispOpts.videoStandard == 1;
            const char* mults[] = {"4x", "6x", "8x", "10x", "12x"};
            const int multVals[] = {4, 6, 8, 10, 12};
            const bool offered[] = {true, ntsc, true, true, ntsc};
            int mi = 0;
            for (int i = 0; i < 5; ++i)
                if (multVals[i] == dispOpts.palMultiple) mi = i;
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("10x").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                                    ImGui::GetFrameHeight());
            if (ImGui::BeginCombo("sampling", mults[mi])) {
                for (int i = 0; i < 5; ++i)
                    if (offered[i] && ImGui::Selectable(mults[i], i == mi)) dispOpts.palMultiple = multVals[i];
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
            ImGui::InputInt("samples/line at 4x", &dispOpts.palLine, 0, 0);
            dispOpts.palLine = std::max(850, std::min(1300, dispOpts.palLine));
        }
        ImGui::TextDisabled(dispOpts.videoStandard == 1 ? "910 = standard NTSC and C64 NTSC, 912 = Apple II, MSX1"
                                                        : "1135 = standard PAL, 1134 = C64");
        if (ImGui::Button("Apply timing") && actions.setPalTiming) actions.setPalTiming(dispOpts.palMultiple);
        ImGui::SameLine();
        if (ImGui::Button("Restore SoG baseline") && actions.setPalTiming) actions.setPalTiming(0);
        if (!geom.autoStatus.empty()) ImGui::TextWrapped("auto-tune: %s", geom.autoStatus.c_str());
        ImGui::Separator();
        // Analog RGB sources (not the composite path).
        ImGui::BeginDisabled(geom.grey8);
        if (ImGui::Checkbox("Auto-tune RGB", &dispOpts.rgbAutoTune) && dispOpts.rgbAutoTune && actions.runRgbAutoTune)
            actions.runRgbAutoTune();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Analog RGB: finds the sharpest Phase, centres the picture with Horizontal/Vertical\n"
                              "Position and (if ticked) crops to the picture. Runs now and after every mode change.");
        ImGui::SameLine();
        ImGui::Checkbox("crop to the picture", &dispOpts.rgbAutoCrop);
        ImGui::SameLine();
        ImGui::BeginDisabled(geom.rgbRunning);
        if (ImGui::Button("Run now") && actions.runRgbAutoTune) actions.runRgbAutoTune();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (geom.grey8) {
            ImGui::SameLine();
            ImGui::TextDisabled("(not for g8)");
        }
        {
            ImGui::BeginDisabled(geom.grey8);
            int sz[2] = {dispOpts.rgbScreenW, dispOpts.rgbScreenH};
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000 00000").x + ImGui::GetStyle().FramePadding.x * 4.0f);
            if (ImGui::InputInt2("screen size", sz)) {
                dispOpts.rgbScreenW = std::max(0, sz[0]);
                dispOpts.rgbScreenH = std::max(0, sz[1]);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The machine's whole screen, border included, in capture pixels (0 0 = not known).\n"
                                  "With it a black border no longer misleads the auto-tune: the position is kept\n"
                                  "when the picture is smaller, and the crop is the screen, not the drawn content.\n"
                                  "Take it once while the border is visible (e.g. the start-up screen), then save the mode.");
            ImGui::SameLine();
            ImGui::BeginDisabled(geom.rgbAreaW <= 0);
            char lbl[64];
            std::snprintf(lbl, sizeof lbl, "use measured %dx%d", geom.rgbAreaW, geom.rgbAreaH);
            if (ImGui::SmallButton(geom.rgbAreaW > 0 ? lbl : "use measured") && actions.useMeasuredScreen)
                actions.useMeasuredScreen();
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }
        if (!geom.rgbStatus.empty())
            ImGui::TextWrapped("RGB auto-tune%s: %s", geom.rgbRunning ? " (running)" : "", geom.rgbStatus.c_str());
        if (!geom.palStatus.empty()) {
            // Several fixed lines: shown as they are, so nothing reflows when a number changes.
            if (geom.palStatus.find('\n') != std::string::npos) ImGui::TextUnformatted(geom.palStatus.c_str());
            else ImGui::TextWrapped("%s", geom.palStatus.c_str());
        }
        }
    if (ImGui::CollapsingHeader("Controls", 0)) {
        // User feedback: "it was not that obvious how to get
        // back to defaults" after changing several controls via a preset.
        if (ImGui::Button("Reset all to defaults") && actions.resetAllControls) actions.resetAllControls();
        ImGui::Separator();
        if (controls.empty()) {
            ImGui::TextDisabled("(none enumerated)");
        } else {
            for (const auto& c : controls) drawOneControl(c, actions);
        }
        }
    if (ImGui::CollapsingHeader("Geometry", 0)) {
        ImGui::Text("Capture size: %u x %u", geom.width, geom.height);
        static int w = 0, h = 0;
        static uint32_t shownForW = 0, shownForH = 0;
        if (shownForW != geom.width || shownForH != geom.height) {
            w = int(geom.width);
            h = int(geom.height);
            shownForW = geom.width;
            shownForH = geom.height;
        }
        ImGui::InputInt("width", &w);
        ImGui::InputInt("height", &h);
        if (ImGui::Button("Apply size") && actions.applySize) {
            w = std::clamp(w, 1, 4096);
            h = std::clamp(h, 1, 4096);
            actions.applySize(uint32_t(w), uint32_t(h));
        }

        ImGui::Separator();
        ImGui::Text("Crop: %d,%d %ux%u%s", geom.crop.x, geom.crop.y, geom.crop.w, geom.crop.h,
                    geom.crop.set ? "" : " (default)");
        static int cx = 0, cy = 0, cw = 0, ch = 0;
        static bool cropInit = false;
        if (!cropInit) {
            cx = geom.crop.x;
            cy = geom.crop.y;
            cw = int(geom.crop.w);
            ch = int(geom.crop.h);
            cropInit = true;
        }
        ImGui::InputInt("crop x", &cx);
        ImGui::InputInt("crop y", &cy);
        ImGui::InputInt("crop w", &cw);
        ImGui::InputInt("crop h", &ch);
        if (ImGui::Button("Apply crop") && actions.applyCrop) {
            actions.applyCrop(Rect{cx, cy, uint32_t(std::max(1, cw)), uint32_t(std::max(1, ch)), true});
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset crop") && actions.applyCrop) {
            actions.applyCrop(Rect{});  // set=false: back to the driver's default
        }
        bool canFit = geom.width == 720 && geom.height == 576;
        ImGui::BeginDisabled(!canFit);
        if (ImGui::Button("Fit active area (720x576 DVI)") && actions.applyCrop)
            actions.applyCrop(Rect{0, 0, 696, geom.height, true});
        ImGui::EndDisabled();
        if (!canFit) ImGui::TextDisabled("only known for the 720x576 digital source");
        }
    if (ImGui::CollapsingHeader("Recorder", 0)) {
        if (recorder.recording) {
            int el = int(recorder.elapsedSec);
            ImGui::Text("Recording %02d:%02d:%02d  (%s)", el / 3600, (el / 60) % 60, el % 60, recorder.encoderName.c_str());
            ImGui::TextWrapped("%s", recorder.path.c_str());
            double mb = double(recorder.counters.bytesWritten) / (1024.0 * 1024.0);
            double mbPerMin = recorder.elapsedSec > 1.0 ? mb / recorder.elapsedSec * 60.0 : 0.0;
            ImGui::TextWrapped("video %llu frames, %llu dropped | %.1f MB | %.1f Mbit/s (%.0f MB/min)",
                               (unsigned long long)recorder.counters.videoFrames,
                               (unsigned long long)recorder.counters.droppedVideoFrames, mb,
                               recorder.elapsedSec > 1.0 ? mb * 8.0 * 1.048576 / recorder.elapsedSec : 0.0, mbPerMin);
        } else {
            ImGui::TextDisabled("Not recording.");
        }
        if (!recorder.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", recorder.error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(recorder.recording);
        // "Auto" (try Nvenc/Vaapi/X264 in order) is always offered; the
        // rest are filtered to what availableVideoEncoders() actually
        // found working on this machine.
        // Wide enough for the longest label, so the preview is never cut off;
        // its caption goes on the line above, like "folder" and "file name".
        ImGui::TextUnformatted("encoder");
        const float encW = ImGui::CalcTextSize(encoderLabel(VideoEncoderKind::Nvenc)).x +
                           ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(encW);
        if (ImGui::BeginCombo("##encoder", encoderLabel(recorder.kind))) {
            bool selAuto = recorder.kind == VideoEncoderKind::Auto;
            if (ImGui::Selectable("Auto", selAuto) && actions.setVideoEncoderKind)
                actions.setVideoEncoderKind(VideoEncoderKind::Auto);
            if (selAuto) ImGui::SetItemDefaultFocus();
            for (VideoEncoderKind k : recorder.availableKinds) {
                bool sel = recorder.kind == k;
                if (ImGui::Selectable(encoderLabel(k), sel) && actions.setVideoEncoderKind)
                    actions.setVideoEncoderKind(k);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (recorder.availableKinds.empty())
            ImGui::TextDisabled("(no hardware/software encoder probed working -- Auto may still fail)");

        // What a recording started now would do, in plain words.
        VideoEncoderKind eff = recorder.kind;
        if (eff == VideoEncoderKind::Auto) {
            // Same order as Recorder::openVideoEncoder's Auto list: x264, NVENC, VAAPI.
            for (VideoEncoderKind want : {VideoEncoderKind::X264, VideoEncoderKind::Nvenc, VideoEncoderKind::Vaapi})
                if (std::find(recorder.availableKinds.begin(), recorder.availableKinds.end(), want) !=
                    recorder.availableKinds.end()) {
                    eff = want;
                    break;
                }
            if (eff != VideoEncoderKind::Auto) ImGui::TextWrapped("Auto uses: %s", encoderLabel(eff));
        }
        if (!recorder.recording) {
            ImGui::TextWrapped("%s", encoderDescription(eff));
            ImGui::TextWrapped("Size %ux%u, %.0f fps; audio: PCM stereo 48 kHz (~11.5 MB/min)", recorder.canvasWidth,
                               recorder.canvasHeight, recorder.fps);
            ImGui::TextWrapped("%s", sizeEstimate(eff, recorder.canvasWidth, recorder.canvasHeight, recorder.fps).c_str());
        }

        // Output folder and file name.
        if (!recFolderInit_ && !recorder.defaultFolder.empty()) {
            std::snprintf(recFolderBuf_, sizeof recFolderBuf_, "%s", recorder.defaultFolder.c_str());
            recFolderInit_ = true;
        }
        if (recorder.pickedSeq != recPickedSeqSeen_) {  // a folder was chosen in the dialog: copy it in once
            recPickedSeqSeen_ = recorder.pickedSeq;
            std::snprintf(recFolderBuf_, sizeof recFolderBuf_, "%s", recorder.pickedFolder.c_str());
        }
        ImGui::TextUnformatted("folder");
        const float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseW - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##recfolder", recFolderBuf_, sizeof recFolderBuf_);
        ImGui::SameLine();
        ImGui::BeginDisabled(recorder.folderDialogOpen);
        if (ImGui::Button("Browse...") && actions.browseRecordFolder) actions.browseRecordFolder(recFolderBuf_);
        ImGui::EndDisabled();
        ImGui::TextUnformatted("file name");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##recname", "empty = freshvision-<date>_<time>.mkv", recNameBuf_, sizeof recNameBuf_);
        ImGui::EndDisabled();
        if (recorder.recording) {
            if (ImGui::Button("Stop recording") && actions.toggleVideoRecording) actions.toggleVideoRecording(false);
        } else {
            if (ImGui::Button("Start recording") && actions.startVideoRecordingTo)
                actions.startVideoRecordingTo(recFolderBuf_, recNameBuf_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open folder") && actions.openRecordFolder) actions.openRecordFolder(recFolderBuf_);
        ImGui::SameLine();
        if (ImGui::Button(recorder.lastRecording.empty() ? "Open freshCoder" : "Open in freshCoder") &&
            actions.openInFreshCoder)
            actions.openInFreshCoder(recorder.lastRecording);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", recorder.lastRecording.empty()
                                        ? "freshCoder turns a raw composite (g8) recording into a colour video."
                                        : ("Decode and encode " + recorder.lastRecording + " in freshCoder.").c_str());
        if (recorder.folderDialogOpen) ImGui::TextDisabled("Folder dialog is open...");
        ImGui::TextWrapped("The canvas is fixed at the size when recording starts; a later mode change is "
                           "scaled/letterboxed into it, not resized.");
        }
    if (ImGui::CollapsingHeader("Audio", 0)) {
        ImGui::Text("Mix: %d ch, frames %llu, underruns %llu", audio.outputChannels,
                    (unsigned long long)audio.mixCounters.framesRead, (unsigned long long)audio.mixCounters.overruns);

        // Monitor output device: PipeWire/PulseAudio's "default" is not
        // necessarily what's actually connected to speakers/headphones (a
        // real case on this machine: "default" is the HDMI-monitor output,
        // not the onboard analog jack).
        // Picking a device is therefore front and center, not buried.
        ImGui::Text("Monitor output:");
        ImGui::SameLine();
        std::string preview = audio.playbackDeviceId.empty() ? "(choose a device)" : audio.playbackDeviceId;
        if (ImGui::BeginCombo("##playbackDevice", preview.c_str())) {
            for (const auto& d : audio.playbackDevices) {
                bool selected = d.id == audio.playbackDeviceId;
                std::string label = d.id + "  (" + d.name + ")";
                if (ImGui::Selectable(label.c_str(), selected) && actions.setAudioPlaybackDevice)
                    actions.setAudioPlaybackDevice(d.id);
            }
            if (audio.playbackDevices.empty()) ImGui::TextDisabled("(none found)");
            ImGui::EndCombo();
        }
        if (!audio.monitorError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", audio.monitorError.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Text("Monitor: %s", audio.monitoring ? "on" : "off");
        ImGui::SameLine();
        if (ImGui::Button(audio.monitoring ? "Stop monitor" : "Start monitor") && actions.toggleAudioMonitor)
            actions.toggleAudioMonitor(!audio.monitoring);
        // Sound vs. picture: the monitor sound reaches you later
        // than the picture; the picture can wait for it (adds latency, so
        // off by default; recordings don't need it, they carry timestamps).
        ImGui::Checkbox("Line the picture up with the monitor sound", &dispOpts.delayPictureToSound);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Holds the picture back by whole frames so it matches the monitor sound.\n"
                              "Adds that much picture latency (not for fast games); recordings are not affected.");
        if (audio.monitoring && audio.monitorSoundMs > 0)
            ImGui::TextDisabled("sound ~%.0f ms, picture ~%.0f ms after the source%s", audio.monitorSoundMs, audio.pictureMs,
                                audio.pictureDelayFrames > 0
                                    ? (": picture held back " + std::to_string(audio.pictureDelayFrames) + " frames").c_str()
                                    : "");
        else
            ImGui::TextDisabled("(start the monitor to measure the sound delay)");
        // Hardware output volume, only offered
        // for a real hw: device override -- "default" has no single
        // hardware level here and stays software-only, same split as a
        // source's own hwAudio entry above.
        if (audio.monitorHwAvailable) {
            bool muted = audio.monitorHwMuted;
            if (ImGui::Checkbox("Mute (hw)##monitor", &muted) && actions.setMonitorHwMuted)
                actions.setMonitorHwMuted(muted);
            ImGui::SameLine();
            float vol = audio.monitorHwVolume;
            ImGui::SetNextItemWidth(150);
            if (ImGui::SliderFloat("Output volume (hw)", &vol, 0.0f, 1.0f) && actions.setMonitorHwVolume)
                actions.setMonitorHwVolume(vol);
        }

        ImGui::Text("Record: %s", audio.recording ? audio.recordingPath.c_str() : "(stopped)");
        if (audio.recording) ImGui::TextDisabled("%llu frames written", (unsigned long long)audio.framesRecorded);
        if (ImGui::Button(audio.recording ? "Stop recording" : "Start recording") && actions.toggleAudioRecording)
            actions.toggleAudioRecording(!audio.recording);

        if (audio.driftSeconds >= 5.0)
            ImGui::Text("Audio/system clock drift: %.1f ms over %.0f s", audio.driftMs, audio.driftSeconds);
        else
            ImGui::TextDisabled("Audio/system clock drift: measuring (needs ~5 s of a source running)...");
        ImGui::Separator();

        ImGui::Text("Devices (toggle to add/remove as a source):");
        const char* modes[] = {"Left", "Right", "Stereo", "Separate L/R"};
        const char* routes[] = {"Left", "Right", "Stereo"};
        for (const auto& d : audio.devices) {
            const RoutedSource* active = nullptr;
            for (const auto& r : audio.activeSources)
                if (r.deviceId == d.id) active = &r;

            ImGui::PushID(d.id.c_str());
            bool on = active != nullptr;
            if (ImGui::Checkbox("##enabled", &on) && actions.setAudioSourceEnabled) actions.setAudioSourceEnabled(d.id, on);
            ImGui::SameLine();
            ImGui::TextWrapped("%s  (%s)", d.id.c_str(), d.name.c_str());

            if (active) {
                RoutedSource r = *active;  // local copy; widgets below edit this and push changes back on change
                ImGui::Indent();
                int mode = int(r.mode);
                if (ImGui::Combo("mode", &mode, modes, 4) && actions.setAudioChannelMode)
                    actions.setAudioChannelMode(r.deviceId, ChannelMode(mode));

                int route0 = int(r.route[0]);
                if (ImGui::Combo("output (logical 0)", &route0, routes, 3) && actions.setAudioOutputRoute)
                    actions.setAudioOutputRoute(r.deviceId, 0, OutputRoute(route0));
                if (ChannelMode(mode) == ChannelMode::SeparateLR) {
                    int route1 = int(r.route[1]);
                    if (ImGui::Combo("output (logical 1, right)", &route1, routes, 3) && actions.setAudioOutputRoute)
                        actions.setAudioOutputRoute(r.deviceId, 1, OutputRoute(route1));
                }

                // Prefer the hardware mixer over a software multiply when
                // this device has one (preferred) — the
                // capture level is then set at the ALSA/codec level, same
                // as alsamixer/Cinnamon's sound settings would, instead of
                // scaling samples after the fact.
                const HwAudioInfo* hw = nullptr;
                for (const auto& kv : audio.hwAudio)
                    if (kv.first == r.deviceId) hw = &kv.second;
                if (hw && hw->available) {
                    bool muted = hw->muted;
                    if (ImGui::Checkbox("mute (hw)", &muted) && actions.setAudioHwMuted)
                        actions.setAudioHwMuted(r.deviceId, muted);
                    float vol = hw->volume;
                    if (ImGui::SliderFloat("volume (hw)", &vol, 0.0f, 1.0f, "%.2f") && actions.setAudioHwVolume)
                        actions.setAudioHwVolume(r.deviceId, vol);
                    float bal = hw->balance;
                    if (ImGui::SliderFloat("balance (hw)", &bal, -1.0f, 1.0f, "%.2f (L..R)") && actions.setAudioHwBalance)
                        actions.setAudioHwBalance(r.deviceId, bal);
                } else {
                    ImGui::TextDisabled("no hardware mixer control found for this device; using software volume");
                    float gain = r.gain;
                    if (ImGui::SliderFloat("volume", &gain, 0.0f, 2.0f, "%.2fx") && actions.setAudioGain)
                        actions.setAudioGain(r.deviceId, gain);
                    float balance = r.balance;
                    if (ImGui::SliderFloat("balance", &balance, -1.0f, 1.0f, "%.2f (L..R)") && actions.setAudioBalance)
                        actions.setAudioBalance(r.deviceId, balance);
                }
                // Physical input jack (Rear Mic / Front Mic / Line, ...) —
                // only shown if this device actually has one to pick from
                // (user feedback: "make it possible to switch
                // between front and back microphone and line inputs").
                if (hw && !hw->inputSources.empty()) {
                    std::string preview = hw->currentInputSource.empty() ? "(unknown)" : hw->currentInputSource;
                    if (ImGui::BeginCombo("input jack", preview.c_str())) {
                        for (const auto& opt : hw->inputSources) {
                            bool selected = opt == hw->currentInputSource;
                            if (ImGui::Selectable(opt.c_str(), selected) && actions.setAudioInputSource)
                                actions.setAudioInputSource(r.deviceId, opt);
                        }
                        ImGui::EndCombo();
                    }
                }

                int delay = r.delayMs;
                if (ImGui::SliderInt("delay (ms)", &delay, 0, 500) && actions.setAudioDelay)
                    actions.setAudioDelay(r.deviceId, delay);
                for (const auto& kv : audio.sourceCounters) {
                    if (kv.first != r.deviceId) continue;
                    ImGui::TextDisabled("frames %llu, overruns %llu, errors %llu", (unsigned long long)kv.second.framesRead,
                                        (unsigned long long)kv.second.overruns, (unsigned long long)kv.second.readErrors);
                }
                ImGui::Unindent();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        if (audio.devices.empty()) ImGui::TextDisabled("(none found)");
        }
    (void)presets;  // presets are machine modes since 0.1.0 (Machines section)
    ImGui::End();

    ImGui::Render();
}

void Gui::drawMachines(const MachinePanelState& m, const GuiActions& actions) {
    if (!ImGui::CollapsingHeader("Machines", ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto label = [&](int mi, int mo) {
        if (mi < 0 || mi >= int(m.machines.size())) return std::string("-");
        const auto& e = m.machines[size_t(mi)];
        std::string s = e.name;
        if (mo >= 0 && mo < int(e.modes.size())) s += " / " + e.modes[size_t(mo)];
        return s;
    };
    ImGui::TextUnformatted("Source:");
    ImGui::Indent();
    ImGui::TextUnformatted(m.detected.empty() ? "(measuring...)" : m.detected.c_str());
    ImGui::Unindent();
    if (m.matchMachine >= 0) {
        ImGui::TextWrapped("Matches: %s  (%s)", label(m.matchMachine, m.matchMode).c_str(), m.matchWhy.c_str());
        if (m.matchMachine != m.activeMachine || m.matchMode != m.activeMode) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply##match") && actions.applyMachineMode)
                actions.applyMachineMode(m.matchMachine, m.matchMode);
        }
    } else {
        ImGui::TextDisabled("Matches: no machine mode");
    }
    ImGui::Text("Active: %s", label(m.activeMachine, m.activeMode).c_str());
    if (!m.hint.empty()) ImGui::TextDisabled("%s", m.hint.c_str());
    bool autoSwitch = m.autoSwitch;
    if (ImGui::Checkbox("Switch automatically when the source changes mode", &autoSwitch) && actions.setAutoSwitch)
        actions.setAutoSwitch(autoSwitch);

    // Newer built-in versions of modes the user replaced: the user decides.
    if (!m.updates.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "New built-in versions of modes you changed:");
        for (size_t u = 0; u < m.updates.size(); ++u) {
            const auto& up = m.updates[u];
            ImGui::PushID(int(u) + 10000);
            ImGui::TextWrapped("%s / %s (built-in revision %d)", label(up.machine, -1).c_str(), up.mode.c_str(), up.revision);
            if (ImGui::SmallButton("Keep mine") && actions.keepMyMode) actions.keepMyMode(up.machine, up.mode);
            ImGui::SameLine();
            if (ImGui::SmallButton("Take the built-in one") && actions.removeMyMode) actions.removeMyMode(up.machine, up.mode);
            ImGui::SameLine();
            if (ImGui::TreeNode("Differences")) {
                for (const auto& d : up.diff) ImGui::BulletText("%s", d.c_str());
                if (up.diff.empty()) ImGui::TextDisabled("(no differences in the settings)");
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::Separator();

    // Machines grouped by maker/family; a search narrows the list.
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##machinesearch", "search machines and modes", machineSearchBuf_, sizeof machineSearchBuf_);
    auto lowerS = [](std::string t) {
        for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    const std::string query = lowerS(machineSearchBuf_);
    auto matchesQuery = [&](const MachinePanelState::Entry& e) {
        if (query.empty()) return true;
        if (lowerS(e.name + " " + e.group).find(query) != std::string::npos) return true;
        for (const auto& md : e.modes)
            if (lowerS(md).find(query) != std::string::npos) return true;
        return false;
    };
    std::vector<std::string> groups;
    for (const auto& e : m.machines) {
        const std::string g = e.group.empty() ? "Other" : e.group;
        if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    std::sort(groups.begin(), groups.end(), [](const std::string& x, const std::string& y) {
        if ((x == "Other") != (y == "Other")) return y == "Other";
        return x < y;
    });
    auto drawMachine = [&](int i) {
        const auto& e = m.machines[size_t(i)];
        ImGui::PushID(i);
        const bool active = i == m.activeMachine;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(e.name.c_str())) {
            saveTarget_ = i;  // also the machine a "Save current settings as a mode" goes into
            if (actions.applyMachineMode) actions.applyMachineMode(i, -1);
        }
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !e.note.empty()) ImGui::SetTooltip("%s", e.note.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", e.builtIn ? (e.mine ? "built-in + mine" : "built-in") : "mine");
        if (e.mine) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit") && actions.editMachine) actions.editMachine(i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens your file for this machine: %s", e.path.c_str());
        }
        if (!e.modes.empty()) {
            ImGui::Indent();
            for (int j = 0; j < int(e.modes.size()); ++j) {
                ImGui::PushID(j);
                const bool on = active && j == m.activeMode;
                const bool fits = j < int(e.modeMatches.size()) && e.modeMatches[size_t(j)];
                const bool manual = j < int(e.modeManual.size()) && e.modeManual[size_t(j)];
                const int origin = j < int(e.modeOrigin.size()) ? e.modeOrigin[size_t(j)] : 0;
                std::string t = e.modes[size_t(j)];
                if (origin == 1) t += "  [mine]";
                else if (origin == 2) t += "  [replaced by yours]";
                t += on ? "  (active)" : manual ? "  (manual)" : fits ? "" : "  (doesn't match)";
                if (!fits && !on) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                if (ImGui::SmallButton(t.c_str()) && actions.applyMachineMode) actions.applyMachineMode(i, j);
                if (!fits && !on) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s%sRight-click for more.", fits ? "" : "This mode's conditions don't fit the source now; a click applies it anyway.\n",
                                      origin == 0 ? "Built-in mode (ships with freshVision). " : origin == 1 ? "Your mode. " : "Your version of a built-in mode. ");
                if (ImGui::BeginPopupContextItem("modemenu")) {
                    if (origin == 0) {
                        if (ImGui::MenuItem("Hide this built-in mode") && actions.hideBuiltInMode)
                            actions.hideBuiltInMode(i, e.modes[size_t(j)], true);
                    } else if (origin == 1) {
                        if (ImGui::MenuItem("Delete this mode") && actions.removeMyMode) actions.removeMyMode(i, e.modes[size_t(j)]);
                    } else {
                        if (ImGui::MenuItem("Back to the built-in mode") && actions.removeMyMode)
                            actions.removeMyMode(i, e.modes[size_t(j)]);
                        if (j < int(e.modeDiff.size())) {
                            ImGui::Separator();
                            ImGui::TextDisabled("Differences from the built-in mode:");
                            for (const auto& d : e.modeDiff[size_t(j)]) ImGui::BulletText("%s", d.c_str());
                            if (e.modeDiff[size_t(j)].empty()) ImGui::TextDisabled("(none in the settings)");
                        }
                    }
                    ImGui::EndPopup();
                }
                if (origin == 2) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("back to built-in") && actions.removeMyMode) actions.removeMyMode(i, e.modes[size_t(j)]);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Deletes your version; the built-in mode is used again.\nRight-click the mode for the differences.");
                }
                ImGui::PopID();  // one mode per line: its buttons are never cut off at the column's edge
            }
            for (const auto& h : e.hidden) {
                ImGui::TextDisabled("hidden: %s", h.c_str());
                ImGui::SameLine();
                ImGui::PushID(h.c_str());
                if (ImGui::SmallButton("show") && actions.hideBuiltInMode) actions.hideBuiltInMode(i, h, false);
                ImGui::PopID();
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    };
    for (const std::string& g : groups) {
        std::vector<int> members;
        bool holdsCurrent = false;
        for (int i = 0; i < int(m.machines.size()); ++i) {
            const auto& e = m.machines[size_t(i)];
            if ((e.group.empty() ? "Other" : e.group) != g || !matchesQuery(e)) continue;
            members.push_back(i);
            holdsCurrent = holdsCurrent || i == m.activeMachine || i == m.matchMachine;
        }
        if (members.empty()) continue;
        if (!query.empty() || holdsCurrent) ImGui::SetNextItemOpen(true, query.empty() ? ImGuiCond_Once : ImGuiCond_Always);
        if (ImGui::TreeNode(g.c_str(), "%s (%d)", g.c_str(), int(members.size()))) {
            for (int i : members) drawMachine(i);
            ImGui::TreePop();
        }
    }
    if (m.machines.empty()) ImGui::TextDisabled("(no machines found)");
    ImGui::Separator();

    // Saving: always into My modes. The row follows the active mode, so "Save"
    // alone stores your changes back to the mode you are on.
    if (m.activeMachine != prefillMachine_ || m.activeMode != prefillMode_) {
        prefillMachine_ = m.activeMachine, prefillMode_ = m.activeMode;
        if (m.activeMachine >= 0 && m.activeMachine < int(m.machines.size())) {
            saveTarget_ = m.activeMachine;
            const auto& am = m.machines[size_t(m.activeMachine)];
            if (m.activeMode >= 0 && m.activeMode < int(am.modes.size()))
                std::snprintf(modeNameBuf_, sizeof modeNameBuf_, "%s", am.modes[size_t(m.activeMode)].c_str());
        }
    }
    {
        int target = saveTarget_ >= 0 && saveTarget_ < int(m.machines.size()) ? saveTarget_ : m.activeMachine;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        if (ImGui::BeginCombo("##savetarget", target >= 0 ? m.machines[size_t(target)].name.c_str() : "(machine)")) {
            for (int i = 0; i < int(m.machines.size()); ++i)
                if (ImGui::Selectable(m.machines[size_t(i)].name.c_str(), i == target)) saveTarget_ = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##modename", "mode name", modeNameBuf_, sizeof modeNameBuf_);
        ImGui::Checkbox("with the source's conditions", &saveConditions_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: picked by itself when the source looks like this again.\nOff: a click-only mode.");
        ImGui::SameLine();
        ImGui::Checkbox("with the sound sources", &saveSound_);
        if (target == m.activeMachine && m.activeMode >= 0) {
            if (!m.changesKnown) ImGui::TextDisabled("Changes since the mode was applied: measuring...");
            else if (m.changes.empty()) ImGui::TextDisabled("Changes since the mode was applied: none");
            else {
                ImGui::TextUnformatted("Changes since the mode was applied (Save stores these):");
                for (const auto& c : m.changes) ImGui::BulletText("%s", c.c_str());
            }
        }
        ImGui::BeginDisabled(target < 0 || modeNameBuf_[0] == '\0');
        if (ImGui::Button("Save current settings as a mode") && actions.saveCurrentAsMode)
            actions.saveCurrentAsMode(target, modeNameBuf_, saveConditions_, saveSound_);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Saved to My modes (your settings folder; upgrades never change it). A built-in mode\n"
                              "of the same name is replaced by yours; the built-in one stays available.");
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::InputTextWithHint("##machinename", "new machine name", machineNameBuf_, sizeof machineNameBuf_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::InputTextWithHint("##machinegroup", "maker (group)", machineGroupBuf_, sizeof machineGroupBuf_);
    ImGui::SameLine();
    ImGui::BeginDisabled(machineNameBuf_[0] == '\0');
    if (ImGui::Button("New machine") && actions.newMachine) {
        actions.newMachine(machineNameBuf_, machineGroupBuf_);
        machineNameBuf_[0] = '\0';
    }
    ImGui::EndDisabled();
    if (ImGui::SmallButton("Open my folder") && actions.openMachinesFolder) actions.openMachinesFolder();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload") && actions.reloadMachines) actions.reloadMachines();
    ImGui::SameLine();
    if (ImGui::SmallButton("Import from the other installation") && actions.importOtherInstallation)
        actions.importOtherInstallation();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copies your modes (and freshCoder settings, palettes) from the other freshVision\n"
                          "installation on this computer (installed build <-> Flatpak); files you already have are kept.");
    if (!m.status.empty()) ImGui::TextWrapped("%s", m.status.c_str());
    for (const auto& e : m.errors) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", e.c_str());
}

void Gui::render() { ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); }

}  // namespace fv

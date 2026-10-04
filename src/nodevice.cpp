#include "nodevice.h"

#include <SDL.h>
#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include "capture.h"
#include "display.h"
#include "gui.h"

namespace fv {

namespace {

// The driver of the supported cards reports this name (VIDIOC_QUERYCAP).
bool isVision(const SourceInfo& s) { return s.isCaptureDevice && s.driver == "Vision"; }

std::string lastDeviceFile() {
    const char* cfg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    return (cfg && *cfg ? std::string(cfg) : std::string(home ? home : ".") + "/.config") + "/freshvision/last-device";
}

}  // namespace

std::string chooseStartDevice() {
    const std::vector<SourceInfo> all = probeVideoSources(63);
    std::string last;
    if (std::ifstream f(lastDeviceFile()); f) std::getline(f, last);
    for (const SourceInfo& s : all)
        if (isVision(s) && s.path == last) return last;
    for (const SourceInfo& s : all)
        if (isVision(s) && s.hasSignal) return s.path;
    for (const SourceInfo& s : all)
        if (isVision(s)) return s.path;
    return {};
}

void rememberDevice(const std::string& path) {
    const std::string file = lastDeviceFile();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
    if (std::ofstream f(file); f) f << path << "\n";
}

void printDevices() {
    const std::vector<SourceInfo> all = probeVideoSources(63);
    if (all.empty()) std::printf("no video capture devices found (/dev/video*)\n");
    for (const SourceInfo& s : all)
        std::printf("%-13s driver %-10s %-40s %s%s\n", s.path.c_str(), s.driver.c_str(), s.name.c_str(),
                    s.hasSignal ? "signal" : "no signal", isVision(s) ? "" : "  (not a Vision card: not supported)");
}

bool runNoDeviceScreen(const std::string& tried, const std::string& error, std::string& chosen,
                       const std::function<void()>& openFreshCoder) {
    Display disp;
    std::string err;
    if (!disp.create(640, 360, false, true, err, 900, 600)) {
        std::fprintf(stderr, "display: %s\n", err.c_str());
        return false;
    }
    disp.setTitle("freshVision - no capture device");
    Gui gui;
    if (!gui.init(disp.window(), disp.glContext(), err)) {
        std::fprintf(stderr, "gui: %s\n", err.c_str());
        return false;
    }
    std::vector<SourceInfo> devices = probeVideoSources(63);
    Uint32 lastProbe = SDL_GetTicks();
    bool run = true, picked = false;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            gui.processEvent(e);
            if (e.type == SDL_QUIT) run = false;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q)) run = false;
        }
        if (SDL_GetTicks() - lastProbe > 2000) devices = probeVideoSources(63), lastProbe = SDL_GetTicks();
        disp.beginFrame();
        gui.newFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("no device", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("No capture card could be opened");
        ImGui::Separator();
        if (!tried.empty()) ImGui::TextWrapped("Tried %s: %s", tried.c_str(), error.empty() ? "not found" : error.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(
            "freshVision needs a Datapath Vision capture card and the card's own Linux driver, which is installed "
            "separately (freshVision is an independent program and doesn't include it). See the user guide, "
            "\"Getting started\": installing the driver, checking that /dev/video* devices appear, and the IOMMU "
            "setting some Intel computers need for RGB capture.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Capture devices found (refreshed every 2 seconds):");
        if (devices.empty()) ImGui::TextDisabled("  none");
        for (const SourceInfo& s : devices) {
            ImGui::PushID(s.path.c_str());
            const bool ok = isVision(s);
            if (!ok) ImGui::BeginDisabled();
            if (ImGui::Button("Use")) chosen = s.path, picked = true, run = false;
            if (!ok) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text("%s  %s  %s%s", s.path.c_str(), s.name.c_str(), s.hasSignal ? "signal" : "no signal",
                        ok ? "" : "  (not a Vision card)");
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Open freshCoder")) openFreshCoder();
        ImGui::SameLine();
        ImGui::TextDisabled("(plays and converts recordings; needs no card)");
        if (ImGui::Button("Quit")) run = false;
        ImGui::End();
        ImGui::Render();
        gui.render();
        disp.endFrame();
    }
    gui.shutdown();
    disp.destroy();
    return picked;
}

}  // namespace fv

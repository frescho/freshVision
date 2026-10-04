// freshCoder: saving and loading its settings (decoding, picture, pixel grid
// and palette, output picture, encoding, preview) as named files, plus the
// last-used settings restored at the next start (user request 2026-09-29:
// "if I set something I should be able to save it").
#pragma once

#include <string>
#include <vector>

#include "display.h"
#include "encode.h"
#include "palui.h"
#include "process.h"

namespace fc {

struct CoderSettings {
    fv::PalDecodeUi ui;
    OutputGeometry geom;
    EncodeSettings enc;              // outPath is not stored (it follows the recording)
    fv::DisplayOptions preview;      // scale mode, factor, filter, pixel aspect
    bool previewAspectAuto = true;
};

std::string settingsFolder();                        // ~/.config/freshvision/freshcoder (created on demand)
std::vector<std::string> listSettings();             // saved names, sorted; "last" is not listed
bool saveSettings(const std::string& name, const CoderSettings& s, std::string& err);
bool loadSettings(const std::string& name, CoderSettings& s, std::string& err);
bool deleteSettings(const std::string& name, std::string& err);
constexpr const char* kLastSettings = "last";        // written on exit and at every Encode

}  // namespace fc

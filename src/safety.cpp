// SPDX-License-Identifier: GPL-3.0-or-later
#include "safety.h"

#include <fstream>
#include <sstream>

namespace fv {

bool intelIommuOff(std::string* cmdline) {
    std::ifstream f("/proc/cmdline");
    std::string line;
    std::getline(f, line);
    if (cmdline) *cmdline = line;

    std::istringstream words(line);
    std::string w;
    std::string last;
    const std::string key = "intel_iommu=";
    while (words >> w)
        if (w.compare(0, key.size(), key) == 0) last = w.substr(key.size());
    return last == "off";
}

bool rgbCaptureAllowed(bool overrideGuard, std::string& why) {
    std::string cmdline;
    if (intelIommuOff(&cmdline)) return true;
    if (overrideGuard) {
        why = "WARNING: /proc/cmdline has no intel_iommu=off; RGB capture may freeze this machine "
              "(guard overridden by --i-know-it-may-freeze)";
        return true;
    }
    why = "refusing RGB capture: /proc/cmdline has no intel_iommu=off (RGB capture froze this machine "
          "5 of 5 times with the IOMMU on). Use a YUV format (--format uyvy), fix the kernel command "
          "line, or pass --i-know-it-may-freeze";
    return false;
}

}  // namespace fv

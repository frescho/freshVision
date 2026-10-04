// RGB capture safety guard.
//
// With the Intel IOMMU on, any RGB-format capture froze the development
// machine within ~5 s (5 of 5). With `intel_iommu=off` it is stable. RGB
// capture is refused unless the kernel command line contains that parameter,
// or the user explicitly overrides the guard.
#pragma once

#include <string>

namespace fv {

// True if the last `intel_iommu=` parameter on /proc/cmdline is exactly "off".
bool intelIommuOff(std::string* cmdline = nullptr);

// True if RGB capture may start. `why` explains a refusal or a warning.
bool rgbCaptureAllowed(bool overrideGuard, std::string& why);

}  // namespace fv

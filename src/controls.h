// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime enumeration and by-name access of V4L2 controls (clean-room: no
// control IDs or names are hard-coded; whatever the driver reports is used).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fv {

struct ControlInfo {
    uint32_t id = 0;
    std::string name;
    uint32_t type = 0;
    int64_t min = 0, max = 0, step = 0, def = 0;
    uint32_t flags = 0;
    std::vector<std::pair<int64_t, std::string>> menu;  // menu index/value -> label
    bool hasValue = false;
    int64_t value = 0;
};

// VIDIOC_QUERYCTRL with V4L2_CTRL_FLAG_NEXT_CTRL, plus menus and current values.
std::vector<ControlInfo> enumerateControls(int fd);

void printControls(const std::vector<ControlInfo>& controls);

// True unless the driver has flagged this control disabled, read-only or
// grabbed (V4L2_CTRL_FLAG_DISABLED/READ_ONLY/GRABBED) — i.e. whether a write
// to it could ever succeed right now. Shared by setControlByName() and by
// the Presets feature (main.cpp), so both agree on "is this settable"
// without duplicating the flag check or the raw <linux/videodev2.h> flag
// values outside this file.
bool controlIsSettable(const ControlInfo& c);

// A control the driver sets from what it detects and doesn't accept writes
// to, although it isn't flagged read-only ("Signal Type": every write is
// rejected). Treated as not settable everywhere (GUI, presets, reset).
bool controlIsDetectedOnly(const ControlInfo& c);

// Sets the control whose name matches `name` (case-insensitive; exact match,
// else unique substring). `msg` receives the result or the error.
bool setControlByName(int fd, const std::vector<ControlInfo>& controls, const std::string& name,
                      const std::string& value, std::string& msg);

}  // namespace fv

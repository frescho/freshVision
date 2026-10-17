// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats.h"

#include "v4l2compat.h"

#include <algorithm>
#include <cctype>

namespace fv {

// Byte orders follow the kernel's packed-RGB documentation; the legacy
// BGR32/RGB32 names are treated like ABGR32/ARGB32. Unverified on the real
// card: a red/blue swap on the first run would show it immediately.
static const PixelFormat kFormats[] = {
    {"xbgr32", V4L2_PIX_FMT_XBGR32, Layout::Bgrx8, 4, true},
    {"abgr32", V4L2_PIX_FMT_ABGR32, Layout::Bgrx8, 4, true},
    {"bgr4", V4L2_PIX_FMT_BGR32, Layout::Bgrx8, 4, true},
    {"xrgb32", V4L2_PIX_FMT_XRGB32, Layout::Xrgb8, 4, true},
    {"argb32", V4L2_PIX_FMT_ARGB32, Layout::Xrgb8, 4, true},
    {"rgb4", V4L2_PIX_FMT_RGB32, Layout::Xrgb8, 4, true},
    {"bgr24", V4L2_PIX_FMT_BGR24, Layout::Bgr24, 3, true},
    {"rgb24", V4L2_PIX_FMT_RGB24, Layout::Rgb24, 3, true},
    {"uyvy", V4L2_PIX_FMT_UYVY, Layout::Uyvy, 2, false},
    // Composite-via-Sync-on-Green investigation:
    // the driver's raw single-channel format (V4L2 GREY), used for a
    // signal fed into Green.
    // Not RGB and not
    // YUV -- one 8-bit sample per pixel, no colour space to get wrong.
    {"g8", V4L2_PIX_FMT_GREY, Layout::Grey8, 1, false},
};

const PixelFormat* findFormat(const std::string& name) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const auto& f : kFormats)
        if (n == f.name) return &f;
    return nullptr;
}

const PixelFormat* findFormat(uint32_t fourcc) {
    for (const auto& f : kFormats)
        if (f.fourcc == fourcc) return &f;
    return nullptr;
}

std::string fourccString(uint32_t fourcc) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) {
        char c = char((fourcc >> (8 * i)) & 0xff);
        s[i] = std::isprint(static_cast<unsigned char>(c)) ? c : '?';
    }
    return s;
}

// Measured: on the driver reporting "Vision",
// all four RGB formats (RGB4, BGR4, RGB3, BGR3) deliver bytes B,G,R(,x) in memory.
Layout effectiveLayout(const PixelFormat& pf, const std::string& driverName, bool swapRb) {
    Layout l = pf.layout;
    if (driverName == "Vision") {
        if (pf.bytesPerPixel == 4 && pf.rgb) l = Layout::Bgrx8;
        if (pf.bytesPerPixel == 3 && pf.rgb) l = Layout::Bgr24;
    }
    if (swapRb) {
        switch (l) {
            case Layout::Bgrx8: l = Layout::Rgbx8; break;
            case Layout::Rgbx8: l = Layout::Bgrx8; break;
            case Layout::Xrgb8: l = Layout::Xbgr8; break;
            case Layout::Xbgr8: l = Layout::Xrgb8; break;
            case Layout::Rgb24: l = Layout::Bgr24; break;
            case Layout::Bgr24: l = Layout::Rgb24; break;
            case Layout::Uyvy: break;
            case Layout::Grey8: break;  // single channel, nothing to swap
        }
    }
    return l;
}

bool needsIommuGuard(uint32_t fourcc) {
    switch (fourcc) {
        case V4L2_PIX_FMT_UYVY:
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_YVYU:
        case V4L2_PIX_FMT_VYUY:
        case V4L2_PIX_FMT_NV12:
        case V4L2_PIX_FMT_NV21:
        case V4L2_PIX_FMT_NV16:
        case V4L2_PIX_FMT_NV61:
        case V4L2_PIX_FMT_YUV420:
        case V4L2_PIX_FMT_YVU420:
            return false;
        default:
            return true;
    }
}

int channelOffset(Layout l, int ch) {
    if (ch < 0 || ch > 2) return -1;
    switch (l) {
        case Layout::Bgrx8: return 2 - ch;      // B,G,R,x
        case Layout::Rgbx8: return ch;          // R,G,B,x
        case Layout::Xrgb8: return 1 + ch;      // x,R,G,B
        case Layout::Xbgr8: return 3 - ch;      // x,B,G,R
        case Layout::Rgb24: return ch;          // R,G,B
        case Layout::Bgr24: return 2 - ch;      // B,G,R
        default: return -1;
    }
}

}  // namespace fv

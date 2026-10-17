// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel formats freshVision can display, and the safety classification of formats.
#pragma once

#include <cstdint>
#include <string>

namespace fv {

// Memory layout of one pixel as delivered by the driver (see the V4L2 packed
// RGB documentation) and hence how the display path uploads it.
enum class Layout {
    Bgrx8,  // bytes B,G,R,x
    Rgbx8,  // bytes R,G,B,x
    Xrgb8,  // bytes x,R,G,B
    Xbgr8,  // bytes x,B,G,R
    Rgb24,  // bytes R,G,B
    Bgr24,  // bytes B,G,R
    Uyvy,   // bytes U,Y0,V,Y1 (converted to BGRA on the CPU: fallback path)
    Grey8,  // one byte per sample, no colour -- V4L2_PIX_FMT_GREY ("G8"); see kFormats
};

struct PixelFormat {
    const char* name;  // command-line name
    uint32_t fourcc;
    Layout layout;
    unsigned bytesPerPixel;
    bool rgb;
};

const PixelFormat* findFormat(const std::string& name);
const PixelFormat* findFormat(uint32_t fourcc);

std::string fourccString(uint32_t fourcc);

// Byte order actually delivered for `pf`. The table follows the kernel's
// documentation; a driver whose behaviour was measured to differ gets an
// override keyed by the driver name it reports at runtime (see formats.cpp).
// `swapRb` flips red and blue on top (command-line override).
Layout effectiveLayout(const PixelFormat& pf, const std::string& driverName, bool swapRb);

// True unless the format is a known YUV format. RGB capture froze the
// development machine with the IOMMU on, YUV did
// not; everything that is not known YUV is treated as unsafe.
bool needsIommuGuard(uint32_t fourcc);

// Byte offset of red (ch 0), green (1) or blue (2) within one pixel of an
// RGB layout; -1 for layouts without separate channels (UYVY, grey).
int channelOffset(Layout l, int ch);

}  // namespace fv

// SPDX-License-Identifier: GPL-3.0-or-later
// The V4L2 pixel-format and colour-space constants freshVision's format and
// display code uses. Linux: the kernel's own header. Elsewhere (freshCoder on
// Windows): the same values, defined here (the standard V4L2 four-character codes).
#pragma once

#if defined(__linux__)
#include <linux/videodev2.h>
#else
#include <cstdint>
#define v4l2_fourcc(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define V4L2_PIX_FMT_RGB24 v4l2_fourcc('R', 'G', 'B', '3')
#define V4L2_PIX_FMT_BGR24 v4l2_fourcc('B', 'G', 'R', '3')
#define V4L2_PIX_FMT_RGB32 v4l2_fourcc('R', 'G', 'B', '4')
#define V4L2_PIX_FMT_BGR32 v4l2_fourcc('B', 'G', 'R', '4')
#define V4L2_PIX_FMT_ABGR32 v4l2_fourcc('A', 'R', '2', '4')
#define V4L2_PIX_FMT_ARGB32 v4l2_fourcc('B', 'A', '2', '4')
#define V4L2_PIX_FMT_XBGR32 v4l2_fourcc('X', 'R', '2', '4')
#define V4L2_PIX_FMT_XRGB32 v4l2_fourcc('B', 'X', '2', '4')
#define V4L2_PIX_FMT_GREY v4l2_fourcc('G', 'R', 'E', 'Y')
#define V4L2_PIX_FMT_UYVY v4l2_fourcc('U', 'Y', 'V', 'Y')
#define V4L2_PIX_FMT_VYUY v4l2_fourcc('V', 'Y', 'U', 'Y')
#define V4L2_PIX_FMT_YUYV v4l2_fourcc('Y', 'U', 'Y', 'V')
#define V4L2_PIX_FMT_YVYU v4l2_fourcc('Y', 'V', 'Y', 'U')
#define V4L2_PIX_FMT_NV12 v4l2_fourcc('N', 'V', '1', '2')
#define V4L2_PIX_FMT_NV21 v4l2_fourcc('N', 'V', '2', '1')
#define V4L2_PIX_FMT_NV16 v4l2_fourcc('N', 'V', '1', '6')
#define V4L2_PIX_FMT_NV61 v4l2_fourcc('N', 'V', '6', '1')
#define V4L2_PIX_FMT_YUV420 v4l2_fourcc('Y', 'U', '1', '2')
#define V4L2_PIX_FMT_YVU420 v4l2_fourcc('Y', 'V', '1', '2')
#define V4L2_COLORSPACE_REC709 3
#endif

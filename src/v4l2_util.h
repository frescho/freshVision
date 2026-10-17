// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers shared by the V4L2 modules.
#pragma once

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <sys/ioctl.h>

namespace fv {

// ioctl() that retries on EINTR.
inline int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

// CLOCK_MONOTONIC in nanoseconds (the clock V4L2 buffer timestamps use when
// V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC is set).
inline int64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace fv

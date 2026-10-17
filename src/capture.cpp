// SPDX-License-Identifier: GPL-3.0-or-later
#include "capture.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>

#include "controls.h"
#include "formats.h"
#include "safety.h"
#include "v4l2_util.h"

namespace fv {

namespace {

std::string sysError(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

const char* fieldName(uint32_t field) {
    switch (field) {
        case V4L2_FIELD_ANY: return "any";
        case V4L2_FIELD_NONE: return "progressive";
        case V4L2_FIELD_TOP: return "top";
        case V4L2_FIELD_BOTTOM: return "bottom";
        case V4L2_FIELD_INTERLACED: return "interlaced";
        case V4L2_FIELD_SEQ_TB: return "seq-tb";
        case V4L2_FIELD_SEQ_BT: return "seq-bt";
        case V4L2_FIELD_ALTERNATE: return "alternate";
        default: return "other";
    }
}

int64_t clockNs(clockid_t id) {
    timespec ts;
    clock_gettime(id, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace

Capture::~Capture() { close(); }

bool Capture::open(const std::string& path, std::string& err) {
    close();
    // Blocking fd on purpose (see capture.h).
    fd_ = ::open(path.c_str(), O_RDWR);
    if (fd_ < 0) {
        err = sysError(("open " + path).c_str());
        return false;
    }
    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        err = sysError("VIDIOC_QUERYCAP");
        close();
        return false;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        err = path + " is not a single-planar streaming capture device";
        close();
        return false;
    }
    path_ = path;
    driver_ = reinterpret_cast<const char*>(cap.driver);
    // The current input's name as the driver reports it (e.g. "Analog Sync
    // On Green (DVI-I SL)"); the machine files match on it.
    input_.clear();
    int cur = 0;
    if (xioctl(fd_, VIDIOC_G_INPUT, &cur) == 0) {
        v4l2_input in{};
        in.index = uint32_t(cur);
        if (xioctl(fd_, VIDIOC_ENUMINPUT, &in) == 0) input_ = reinterpret_cast<const char*>(in.name);
    }
    return true;
}

void Capture::close() {
    stop();
    if (fd_ >= 0 && !broken_) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Capture::printInfo() const {
    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) == 0) {
        std::printf("driver   : %s\ncard     : %s\nbus      : %s\nversion  : %u.%u.%u\ncaps     : 0x%08x (device 0x%08x)\n",
                    cap.driver, cap.card, cap.bus_info, (cap.version >> 16) & 0xff,
                    (cap.version >> 8) & 0xff, cap.version & 0xff, cap.capabilities, cap.device_caps);
    }

    std::printf("\ninputs:\n");
    int cur = -1;
    if (xioctl(fd_, VIDIOC_G_INPUT, &cur) < 0) cur = -1;
    for (uint32_t i = 0;; ++i) {
        v4l2_input in{};
        in.index = i;
        if (xioctl(fd_, VIDIOC_ENUMINPUT, &in) < 0) break;
        std::printf("  %s%u: %s (type %u, status 0x%x%s%s)\n", int(i) == cur ? "*" : " ", i, in.name, in.type,
                    in.status, (in.status & V4L2_IN_ST_NO_SIGNAL) ? " NO_SIGNAL" : "",
                    (in.status & V4L2_IN_ST_NO_POWER) ? " NO_POWER" : "");
    }

    std::printf("\ncapture formats (* = displayable by freshVision):\n");
    for (uint32_t i = 0;; ++i) {
        v4l2_fmtdesc d{};
        d.index = i;
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_ENUM_FMT, &d) < 0) break;
        const PixelFormat* pf = findFormat(d.pixelformat);
        std::printf("  %c '%s' %s%s%s\n", pf ? '*' : ' ', fourccString(d.pixelformat).c_str(), d.description,
                    (d.flags & V4L2_FMT_FLAG_COMPRESSED) ? " [compressed]" : "",
                    pf ? (std::string(" (--format ") + pf->name + ")").c_str() : "");
    }

    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_FMT, &f) == 0) {
        std::printf("\ncurrent format: '%s' %ux%u stride %u sizeimage %u field %s colorspace %u\n",
                    fourccString(f.fmt.pix.pixelformat).c_str(), f.fmt.pix.width, f.fmt.pix.height,
                    f.fmt.pix.bytesperline, f.fmt.pix.sizeimage, fieldName(f.fmt.pix.field),
                    f.fmt.pix.colorspace);
    }

    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_CROP;
    if (xioctl(fd_, VIDIOC_G_SELECTION, &sel) == 0)
        std::printf("crop          : %d,%d %ux%u\n", sel.r.left, sel.r.top, sel.r.width, sel.r.height);
    sel.target = V4L2_SEL_TGT_CROP_BOUNDS;
    if (xioctl(fd_, VIDIOC_G_SELECTION, &sel) == 0)
        std::printf("crop bounds   : %d,%d %ux%u\n", sel.r.left, sel.r.top, sel.r.width, sel.r.height);

    v4l2_dv_timings t{};
    if (xioctl(fd_, VIDIOC_QUERY_DV_TIMINGS, &t) == 0) {
        std::printf("DV timings    : %ux%u%s pixelclock %llu Hz\n", t.bt.width, t.bt.height,
                    t.bt.interlaced ? "i" : "p", static_cast<unsigned long long>(t.bt.pixelclock));
    } else {
        std::printf("DV timings    : query not available (%s)\n", std::strerror(errno));
    }
    std::printf("\n");
}

std::vector<uint32_t> Capture::enumFormats() const {
    std::vector<uint32_t> out;
    for (uint32_t i = 0;; ++i) {
        v4l2_fmtdesc d{};
        d.index = i;
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_ENUM_FMT, &d) < 0) break;
        out.push_back(d.pixelformat);
    }
    return out;
}

bool Capture::readFormat(std::string& err) {
    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_FMT, &f) < 0) {
        err = sysError("VIDIOC_G_FMT");
        return false;
    }
    fmt_.width = f.fmt.pix.width;
    fmt_.height = f.fmt.pix.height;
    fmt_.stride = f.fmt.pix.bytesperline;
    fmt_.sizeImage = f.fmt.pix.sizeimage;
    fmt_.colorspace = f.fmt.pix.colorspace;
    fmt_.field = f.fmt.pix.field;
    if (const PixelFormat* pf = findFormat(fmt_.fourcc))
        if (fmt_.stride == 0) fmt_.stride = fmt_.width * pf->bytesPerPixel;
    return true;
}

bool Capture::applyCrop(std::string& err) {
    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_CROP;
    if (!crop_.set) {
        // The driver keeps the crop of the previous run: without --crop go
        // back to the default so that runs do not depend on each other.
        v4l2_selection def{};
        def.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        def.target = V4L2_SEL_TGT_CROP_DEFAULT;
        if (xioctl(fd_, VIDIOC_G_SELECTION, &def) < 0) return true;  // no selection API: nothing to reset
        sel.r = def.r;
        xioctl(fd_, VIDIOC_S_SELECTION, &sel);
        {
            std::lock_guard<std::mutex> lk(ctrlMu_);
            appliedCrop_ = Rect{sel.r.left, sel.r.top, uint32_t(sel.r.width), uint32_t(sel.r.height), false};
        }
        return readFormat(err);
    }
    sel.r.left = crop_.x;
    sel.r.top = crop_.y;
    sel.r.width = crop_.w;
    sel.r.height = crop_.h;
    if (xioctl(fd_, VIDIOC_S_SELECTION, &sel) < 0) {
        err = sysError("VIDIOC_S_SELECTION");
        return false;
    }
    std::printf("crop set: %d,%d %ux%u (driver returned %d,%d %ux%u)\n", crop_.x, crop_.y, crop_.w, crop_.h,
                sel.r.left, sel.r.top, sel.r.width, sel.r.height);
    {
        std::lock_guard<std::mutex> lk(ctrlMu_);
        appliedCrop_ = Rect{sel.r.left, sel.r.top, uint32_t(sel.r.width), uint32_t(sel.r.height), true};
    }
    return readFormat(err);  // the frame size may follow the crop
}

Rect Capture::appliedCrop() const {
    std::lock_guard<std::mutex> lk(ctrlMu_);
    return appliedCrop_;
}

std::vector<ControlInfo> Capture::controlsSnapshot() const {
    std::lock_guard<std::mutex> lk(ctrlMu_);
    return controlsSnapshot_;
}

void Capture::requestControlChange(uint32_t id, int64_t value) {
    if (!id) return;
    std::lock_guard<std::mutex> lk(ctrlMu_);
    pendingWrites_.emplace_back(id, value);
}

bool Capture::start(uint32_t fourcc, uint32_t width, uint32_t height, std::string& err) {
    if (fd_ < 0 || broken_) {
        err = broken_ ? "device is stuck (worker thread could not be stopped)" : "device not open";
        return false;
    }
    stop();

    // Safety guard first, before any ioctl that changes driver state.
    if (needsIommuGuard(fourcc)) {
        std::string why;
        if (!rgbCaptureAllowed(overrideGuard_, why)) {
            err = why;
            return false;
        }
        if (!why.empty()) std::fprintf(stderr, "%s\n", why.c_str());
    }

    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_FMT, &f) < 0) {
        err = sysError("VIDIOC_G_FMT");
        return false;
    }
    if (width && height) {
        f.fmt.pix.width = width;
        f.fmt.pix.height = height;
    }
    f.fmt.pix.pixelformat = fourcc;
    f.fmt.pix.bytesperline = 0;
    f.fmt.pix.sizeimage = 0;
    if (xioctl(fd_, VIDIOC_S_FMT, &f) < 0) {
        err = sysError(("VIDIOC_S_FMT '" + fourccString(fourcc) + "'").c_str());
        return false;
    }
    if (f.fmt.pix.pixelformat != fourcc) {
        err = "driver changed the pixel format '" + fourccString(fourcc) + "' -> '" +
              fourccString(f.fmt.pix.pixelformat) + "'";
        return false;
    }
    fmt_.fourcc = fourcc;
    if (!readFormat(err) || !applyCrop(err)) return false;
    findSignalControl();
    {
        std::vector<ControlInfo> snap = enumerateControls(fd_);  // single-threaded here, worker not started yet
        std::lock_guard<std::mutex> lk(ctrlMu_);
        controlsSnapshot_ = std::move(snap);
        pendingWrites_.clear();
    }
    reqFourcc_ = fourcc;
    reqW_ = width;
    reqH_ = height;

    if (frameRate_ > 0) {
        v4l2_streamparm sp{};
        sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        sp.parm.capture.timeperframe.numerator = 1000;
        sp.parm.capture.timeperframe.denominator = uint32_t(std::lround(frameRate_ * 1000));
        if (xioctl(fd_, VIDIOC_S_PARM, &sp) == 0)
            std::printf("capture: frame rate set: %u/%u s per frame\n", sp.parm.capture.timeperframe.numerator,
                        sp.parm.capture.timeperframe.denominator);
        else
            std::printf("capture: VIDIOC_S_PARM refused (%s)\n", std::strerror(errno));
    }
    v4l2_requestbuffers req{};
    req.count = bufferCount_;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        err = sysError("VIDIOC_REQBUFS");
        return false;
    }
    if (req.count != bufferCount_) std::printf("capture: %u buffers asked, %u granted\n", bufferCount_, req.count);
    if (req.count < 2) {
        err = "driver granted fewer than 2 buffers";
        req.count = 0;
        xioctl(fd_, VIDIOC_REQBUFS, &req);
        return false;
    }
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) {
            err = sysError("VIDIOC_QUERYBUF");
            stop();
            return false;
        }
        void* p = mmap(nullptr, b.length, PROT_READ, MAP_SHARED, fd_, b.m.offset);
        if (p == MAP_FAILED) {
            err = sysError("mmap");
            stop();
            return false;
        }
        buffers_.push_back({p, b.length});
    }
    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        if (!queue(i)) {
            err = sysError("VIDIOC_QBUF");
            stop();
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        mailboxFull_ = false;
        released_.clear();
        restartNeeded_ = false;
        restartReason_.clear();
    }
    signal_ = true;
    clock_ = Clock::Unknown;
    delivered_ = overwritten_ = driverGaps_ = errorFrames_ = shortFrames_ = timeouts_ = seqJumps_ = 0;

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        err = sysError("VIDIOC_STREAMON");
        stop();
        return false;
    }
    streaming_ = true;
    stopRequested_ = false;
    workerDone_ = false;
    worker_ = std::thread(&Capture::workerMain, this);
    return true;
}

void Capture::stop() {
    if (fd_ < 0) return;
    if (worker_.joinable()) {
        stopRequested_ = true;
        // DQBUF returns at the latest when the driver's ~1 s frame timeout expires.
        auto waitDone = [&](int ms) {
            std::unique_lock<std::mutex> lk(mu_);
            return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return workerDone_.load(); });
        };
        if (!waitDone(1500)) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(fd_, VIDIOC_STREAMOFF, &type);  // wake a DQBUF that is stuck
            waitDone(3000);
        }
        if (workerDone_) {
            worker_.join();
        } else {
            std::fprintf(stderr, "capture worker did not stop; leaving the device as it is\n");
            worker_.detach();
            broken_ = true;
            streaming_ = false;
            return;
        }
    }
    if (streaming_) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    if (!buffers_.empty()) {
        for (auto& b : buffers_) munmap(b.ptr, b.len);
        buffers_.clear();
        v4l2_requestbuffers req{};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(fd_, VIDIOC_REQBUFS, &req);
    }
}

bool Capture::restart(std::string& err, uint32_t width, uint32_t height) {
    if (path_.empty()) {
        err = "device was never opened";
        return false;
    }
    std::string path = path_;
    uint32_t fourcc = reqFourcc_, w = width ? width : reqW_, h = height ? height : reqH_;
    stop();
    if (broken_) {
        err = "device is stuck (worker thread could not be stopped)";
        return false;
    }
    if (!open(path, err)) return false;  // closes the old fd first
    return start(fourcc, w, h, err);
}

bool Capture::queue(uint32_t index) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = index;
    return xioctl(fd_, VIDIOC_QBUF, &b) == 0;
}

int64_t Capture::readClock(Clock c) const {
    switch (c) {
        case Clock::Monotonic: return clockNs(CLOCK_MONOTONIC);
        case Clock::Realtime: return clockNs(CLOCK_REALTIME);
        case Clock::Boottime: return clockNs(CLOCK_BOOTTIME);
        default: return 0;
    }
}

std::string Capture::clockName() const {
    switch (clock_.load()) {
        case Clock::Monotonic: return "monotonic";
        case Clock::Realtime: return "realtime";
        case Clock::Boottime: return "boottime";
        default: return "unknown";
    }
}

int64_t Capture::clockNowNs() const {
    return readClock(clock_.load());
}

// The driver flags its timestamps "unknown", so the clock is found by
// comparison: a frame's end-of-frame timestamp is a little in the past on the
// clock it was taken from.
void Capture::detectClock(int64_t tsNs) {
    auto plausible = [&](Clock c) {
        int64_t d = readClock(c) - tsNs;
        return d >= -5000000LL && d <= 1000000000LL;
    };
    if (plausible(Clock::Monotonic)) clock_ = Clock::Monotonic;
    else if (plausible(Clock::Boottime)) clock_ = Clock::Boottime;
    else if (plausible(Clock::Realtime)) clock_ = Clock::Realtime;
}

void Capture::fail(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        restartNeeded_ = true;
        restartReason_ = msg;
    }
    cv_.notify_all();
}

// The driver's "Signal Type" control (name match, no ID assumed) tells what is
// connected: DVI, analog variants, "No Signal", ...
void Capture::findSignalControl() {
    signalCtrlId_ = 0;
    signalLabels_.clear();
    noSignalValue_ = -1;
    signalValue_ = -1;
    for (const ControlInfo& c : enumerateControls(fd_)) {
        std::string n;
        for (unsigned char ch : c.name) n += char(std::tolower(ch));
        if (n != "signal type" || c.menu.empty()) continue;
        signalCtrlId_ = c.id;
        signalLabels_ = c.menu;
        for (const auto& m : c.menu) {
            std::string l;
            for (unsigned char ch : m.second) l += char(std::tolower(ch));
            if (l == "no signal") noSignalValue_ = m.first;
        }
        if (c.hasValue) signalValue_ = int(c.value);
        break;
    }
}

std::string Capture::signalLabelFor(int v) const {
    for (const auto& m : signalLabels_)
        if (m.first == v) return m.second;
    return std::to_string(v);
}

std::string Capture::signalTypeName() const {
    int v = signalValue_.load();
    if (!signalCtrlId_ || v < 0) return {};
    for (const auto& m : signalLabels_)
        if (m.first == v) return m.second;
    return std::to_string(v);
}

// Runs in the worker thread (about once per second): input status and frame size.
void Capture::pollSourceState(int64_t& lastW, int64_t& lastH) {
    int cur = 0;
    bool haveStatus = false, ok = true;
    if (xioctl(fd_, VIDIOC_G_INPUT, &cur) == 0) {
        v4l2_input in{};
        in.index = uint32_t(cur);
        if (xioctl(fd_, VIDIOC_ENUMINPUT, &in) == 0) {
            haveStatus = true;
            ok = !(in.status & (V4L2_IN_ST_NO_SIGNAL | V4L2_IN_ST_NO_POWER));
        }
    }
    int typeNow = -1;
    const int typeBefore = signalValue_.load();
    if (signalCtrlId_) {
        v4l2_control v{};
        v.id = signalCtrlId_;
        if (xioctl(fd_, VIDIOC_G_CTRL, &v) == 0) {
            typeNow = v.value;
            signalValue_ = typeNow;
            haveStatus = true;
            if (typeNow == noSignalValue_) ok = false;
        }
    }
    if (haveStatus) {
        bool was = signal_.exchange(ok);
        if (ok && !was) {
            fail("signal regained");
            return;
        }
        if (!ok && was) std::printf("signal lost\n");
        if (!ok) return;  // frame size of a missing signal is not a mode
        if (typeNow >= 0 && typeBefore >= 0 && typeNow != typeBefore && typeBefore != noSignalValue_) {
            fail("signal type changed: " + signalLabelFor(typeBefore) + " -> " + signalLabelFor(typeNow));
            return;
        }
    }

    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // Only the size counts as a mode change: after S_FMT the driver reports a
    // different pixel format for some RGB formats than the one just set.
    if (xioctl(fd_, VIDIOC_G_FMT, &f) == 0 &&
        (int64_t(f.fmt.pix.width) != lastW || int64_t(f.fmt.pix.height) != lastH)) {
        fail("mode change: " + std::to_string(lastW) + "x" + std::to_string(lastH) + " -> " +
             std::to_string(f.fmt.pix.width) + "x" + std::to_string(f.fmt.pix.height));
        lastW = f.fmt.pix.width;
        lastH = f.fmt.pix.height;
    }
}

void Capture::workerMain() {
    int64_t lastW = fmt_.width, lastH = fmt_.height;
    int64_t lastPollNs = nowNs();
    int64_t lastCtrlRefreshNs = lastPollNs;
    bool haveSeq = false;
    uint32_t lastSeq = 0;
    size_t bpp = 1;
    if (const PixelFormat* pf = findFormat(fmt_.fourcc)) bpp = pf->bytesPerPixel;
    const size_t needed = size_t(fmt_.stride) * (fmt_.height ? fmt_.height - 1 : 0) + size_t(fmt_.width) * bpp;

    while (!stopRequested_) {
        std::vector<uint32_t> back;
        {
            std::lock_guard<std::mutex> lk(mu_);
            back.swap(released_);
        }
        for (uint32_t idx : back) queue(idx);

        std::vector<std::pair<uint32_t, int64_t>> writes;
        {
            std::lock_guard<std::mutex> lk(ctrlMu_);
            writes.swap(pendingWrites_);
        }
        for (const auto& w : writes) {
            v4l2_control ctl{};
            ctl.id = w.first;
            ctl.value = int32_t(w.second);
            if (xioctl(fd_, VIDIOC_S_CTRL, &ctl) < 0)
                std::fprintf(stderr, "control 0x%08x: set failed: %s\n", w.first, std::strerror(errno));
        }

        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) {
                ++timeouts_;  // no frame within the driver's timeout
                // During signal loss the driver can also return EAGAIN at once,
                // hundreds of times per second (and logs every one): do not spin.
                usleep(5000);
            } else {
                fail(sysError("VIDIOC_DQBUF"));
                break;
            }
        } else {
            Frame f;
            f.index = b.index;
            f.sequence = b.sequence;
            int64_t tsNs = int64_t(b.timestamp.tv_sec) * 1000000000LL + int64_t(b.timestamp.tv_usec) * 1000LL;
            if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
                clock_ = Clock::Monotonic;
            else if (clock_.load() == Clock::Unknown)
                detectClock(tsNs);
            Clock c = clock_.load();
            if (c != Clock::Unknown) {
                f.tsValid = true;
                f.tsNs = tsNs;
                f.dequeuedNs = readClock(c);
            }

            // After a signal loss the driver's counter can jump by tens of thousands:
            // that is a discontinuity, not that many lost frames.
            if (haveSeq && b.sequence > lastSeq + 1) {
                uint32_t gap = b.sequence - lastSeq - 1;
                if (gap <= 100) driverGaps_ += gap;
                else ++seqJumps_;
            }
            haveSeq = true;
            lastSeq = b.sequence;

            if (b.flags & V4L2_BUF_FLAG_ERROR) {
                ++errorFrames_;
                queue(b.index);
            } else if (b.bytesused < needed) {
                ++shortFrames_;
                queue(b.index);
            } else {
                ++delivered_;
                f.data = static_cast<const uint8_t*>(buffers_[b.index].ptr);
                bool replaced = false;
                uint32_t oldIndex = 0;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    replaced = mailboxFull_;
                    oldIndex = mailbox_.index;
                    mailbox_ = f;
                    mailboxFull_ = true;
                }
                cv_.notify_all();
                if (replaced) {  // the main thread was too slow for the previous frame: newest wins
                    queue(oldIndex);
                    ++overwritten_;
                }
            }
        }

        if (pollSource_ && nowNs() - lastPollNs >= 1000000000LL) {
            lastPollNs = nowNs();
            pollSourceState(lastW, lastH);
        }
        if (nowNs() - lastCtrlRefreshNs >= 1000000000LL) {
            lastCtrlRefreshNs = nowNs();
            std::vector<ControlInfo> snap = enumerateControls(fd_);
            std::lock_guard<std::mutex> lk(ctrlMu_);
            controlsSnapshot_ = std::move(snap);
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        workerDone_ = true;
    }
    cv_.notify_all();
}

WaitResult Capture::waitFrame(int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                 [&] { return mailboxFull_ || restartNeeded_ || workerDone_.load(); });
    WaitResult r;
    r.frame = mailboxFull_;
    if (restartNeeded_) {
        r.restartNeeded = true;
        r.reason = restartReason_;
    } else if (workerDone_ && streaming_) {
        r.restartNeeded = true;
        r.reason = "capture worker stopped";
    }
    return r;
}

bool Capture::takeFrame(Frame& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!mailboxFull_) return false;
    out = mailbox_;
    mailboxFull_ = false;
    return true;
}

void Capture::release(const Frame& f) {
    std::lock_guard<std::mutex> lk(mu_);
    released_.push_back(f.index);
}

Counters Capture::counters() const {
    Counters c;
    c.delivered = delivered_;
    c.overwritten = overwritten_;
    c.driverGaps = driverGaps_;
    c.errorFrames = errorFrames_;
    c.shortFrames = shortFrames_;
    c.timeouts = timeouts_;
    c.seqJumps = seqJumps_;
    return c;
}

std::vector<SourceInfo> probeVideoSources(int maxIndex) {
    std::vector<SourceInfo> out;
    for (int i = 0; i <= maxIndex; ++i) {
        std::string path = "/dev/video" + std::to_string(i);
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0) continue;  // node doesn't exist (or busy in a way open() itself refuses)

        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            ::close(fd);
            continue;
        }
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
            ::close(fd);
            continue;  // e.g. this driver's separate control channel
        }

        SourceInfo s;
        s.path = path;
        s.isCaptureDevice = true;
        s.name = reinterpret_cast<const char*>(cap.card);
        s.driver = reinterpret_cast<const char*>(cap.driver);
        v4l2_input in{};
        in.index = 0;
        if (xioctl(fd, VIDIOC_ENUMINPUT, &in) == 0) {
            s.name = reinterpret_cast<const char*>(in.name);
            s.hasSignal = !(in.status & V4L2_IN_ST_NO_SIGNAL);
            s.noPower = in.status & V4L2_IN_ST_NO_POWER;
        }
        ::close(fd);
        out.push_back(std::move(s));
    }
    return out;
}

double reportedFrameRate(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return 0;
    v4l2_streamparm p{};
    p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    double hz = 0;
    if (xioctl(fd, VIDIOC_G_PARM, &p) == 0 && p.parm.capture.timeperframe.numerator > 0)
        hz = double(p.parm.capture.timeperframe.denominator) / double(p.parm.capture.timeperframe.numerator);
    ::close(fd);
    return hz;
}

}  // namespace fv

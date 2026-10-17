// SPDX-License-Identifier: GPL-3.0-or-later
// V4L2 capture (mmap streaming). A worker thread does the blocking DQBUF and
// keeps only the newest frame in a one-slot mailbox; the main thread takes it.
//
// Why a thread: on the development card the driver's poll() ignores its
// timeout (poll(fd, POLLIN, 0) blocks for about a frame time) and a blocking
// DQBUF returns EAGAIN after ~1 s without a frame. poll() therefore cannot be
// used to ask "is a newer frame ready?". All buffer ioctls happen in the
// worker thread, so no two threads ever call into the driver at once while
// streaming.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "controls.h"

namespace fv {

struct CaptureFormat {
    uint32_t fourcc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;  // bytes per line
    uint32_t sizeImage = 0;
    uint32_t colorspace = 0;
    uint32_t field = 0;
};

struct Frame {
    const uint8_t* data = nullptr;
    uint32_t index = 0;
    uint32_t sequence = 0;
    bool tsValid = false;      // buffer timestamp clock known (see Capture::clockName)
    int64_t tsNs = 0;          // buffer timestamp on that clock
    int64_t dequeuedNs = 0;    // time of DQBUF on that clock (valid if tsValid)
};

struct Rect {
    int x = 0, y = 0;
    uint32_t w = 0, h = 0;
    bool set = false;
};

// Snapshot of the worker's counters (monotonic since start()).
struct Counters {
    uint64_t delivered = 0;    // good frames dequeued
    uint64_t overwritten = 0;  // frames replaced in the mailbox before the main thread took them
    uint64_t driverGaps = 0;   // frames missing in the driver's sequence numbers
    uint64_t errorFrames = 0;
    uint64_t shortFrames = 0;
    uint64_t timeouts = 0;     // DQBUF returned EAGAIN (no frame within the driver's timeout)
    uint64_t seqJumps = 0;     // sequence discontinuities larger than 100 (e.g. after signal loss)
};

struct WaitResult {
    bool frame = false;          // a frame is waiting in the mailbox
    bool restartNeeded = false;  // mode change, signal regained or worker failure
    std::string reason;
};

// One /dev/videoN probed briefly (open, query, close; no streaming).
struct SourceInfo {
    std::string path;
    bool isCaptureDevice = false;  // opened and reports V4L2_CAP_VIDEO_CAPTURE
    std::string name;              // ENUMINPUT input 0's name, else the QUERYCAP card name
    std::string driver;            // QUERYCAP driver name (the Vision cards' driver reports "Vision")
    bool hasSignal = false;
    bool noPower = false;
};

// Probes /dev/video0 .. /dev/video<maxIndex> with a short-lived open of each
// (safe to call while this process streams a different one of them: V4L2
// allows multiple independent opens of the same device node). Nodes that
// don't exist or aren't video capture devices (e.g. a driver's separate
// control channel) are omitted.
std::vector<SourceInfo> probeVideoSources(int maxIndex = 15);
// The frame rate the driver reports for the current signal (VIDIOC_G_PARM's
// time per frame), read through a short-lived second open of `path` (like
// probeVideoSources); 0 if not available. For a source whose frames the card
// delivers only in part (an Apple IIc: ~8-13 of 60 per second)
// it is the source's real rate when counting delivered frames isn't.
double reportedFrameRate(const std::string& path);

class Capture {
public:
    Capture() = default;
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    bool open(const std::string& path, std::string& err);
    void close();
    int fd() const { return fd_; }
    const std::string& driverName() const { return driver_; }
    // Name of the current input (ENUMINPUT), read at open(); empty if none.
    const std::string& inputName() const { return input_; }

    // Prints capabilities, inputs, formats, current format, crop and DV timings.
    void printInfo() const;
    // FourCCs the driver enumerates for capture, in driver order.
    std::vector<uint32_t> enumFormats() const;

    // Region of the source to capture (standard selection API); applied by
    // every start() after the format is negotiated.
    void setCrop(const Rect& r) { crop_ = r; }
    // Crop rectangle the driver actually reported after the last start()
    // (may differ from what was requested).
    Rect appliedCrop() const;

    // Negotiates `fourcc` (width/height 0 = keep the driver's current size),
    // maps the buffers, starts streaming and the worker thread. RGB formats
    // are refused unless the safety guard allows them (checked here, before
    // any ioctl that changes state).
    bool start(uint32_t fourcc, uint32_t width, uint32_t height, std::string& err);
    // Before start(): the number of V4L2 buffers asked for (default 4), and a
    // frame rate set with VIDIOC_S_PARM (0 = leave the driver's; experiments
    // for the Apple IIc).
    void setBufferCount(uint32_t n) { bufferCount_ = std::max<uint32_t>(2, std::min<uint32_t>(32, n)); }
    void setFrameRate(double hz) { frameRate_ = hz; }
    // Stops the worker, the stream and unmaps the buffers.
    void stop();
    // stop(), close and re-open the device, start() again with the same
    // request, or with `width`/`height` instead when they are non-zero.
    bool restart(std::string& err, uint32_t width = 0, uint32_t height = 0);
    bool streaming() const { return streaming_; }
    const CaptureFormat& format() const { return fmt_; }
    void setOverrideGuard(bool v) { overrideGuard_ = v; }
    void setSourcePolling(bool v) { pollSource_ = v; }

    // Name of the detected buffer-timestamp clock ("monotonic", "realtime",
    // "boottime") or "unknown"; frame age needs a known clock.
    std::string clockName() const;
    // Now on the detected timestamp clock (0 if unknown).
    int64_t clockNowNs() const;

    // Waits until a frame is in the mailbox, a restart is needed, or the timeout expires.
    WaitResult waitFrame(int timeoutMs);
    // Moves the mailbox frame out; the caller must release() it.
    bool takeFrame(Frame& out);
    // Hands the buffer back to the driver (done by the worker thread).
    void release(const Frame& f);

    Counters counters() const;
    // False after the driver reported NO_SIGNAL for the current input (or its
    // "Signal Type" control reads "No Signal").
    bool signalPresent() const { return signal_.load(); }
    // Label of the driver's "Signal Type" control (e.g. "DVI"), empty if the
    // driver has no such control.
    std::string signalTypeName() const;

    // Thread-safe snapshot of the controls (id, name, range, menu, current
    // value), refreshed by the worker thread about once per second. The GUI
    // reads this instead of calling into the driver itself: while streaming,
    // only the worker thread may issue ioctls on this fd.
    std::vector<ControlInfo> controlsSnapshot() const;
    // Queues a VIDIOC_S_CTRL by control id; applied by the worker thread on
    // its next iteration (near-immediately). Silently ignored if `id` is 0.
    void requestControlChange(uint32_t id, int64_t value);

private:
    struct Buffer {
        void* ptr = nullptr;
        size_t len = 0;
    };
    enum class Clock { Unknown, Monotonic, Realtime, Boottime };

    void workerMain();
    bool queue(uint32_t index);
    void pollSourceState(int64_t& lastW, int64_t& lastH);
    void detectClock(int64_t tsNs);
    int64_t readClock(Clock c) const;
    void fail(const std::string& msg);
    void findSignalControl();
    std::string signalLabelFor(int v) const;
    bool applyCrop(std::string& err);
    bool readFormat(std::string& err);

    int fd_ = -1;
    uint32_t bufferCount_ = 4;
    double frameRate_ = 0;
    std::string path_, driver_, input_;
    bool streaming_ = false;
    bool overrideGuard_ = false;
    bool pollSource_ = true;
    bool broken_ = false;  // worker could not be stopped: do not touch the device again
    std::vector<Buffer> buffers_;
    CaptureFormat fmt_;
    Rect crop_;
    uint32_t reqFourcc_ = 0, reqW_ = 0, reqH_ = 0;

    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> workerDone_{true};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool mailboxFull_ = false;
    Frame mailbox_;
    std::vector<uint32_t> released_;
    bool restartNeeded_ = false;
    std::string restartReason_;

    uint32_t signalCtrlId_ = 0;  // "Signal Type" control, found by name at start()
    std::vector<std::pair<int64_t, std::string>> signalLabels_;
    int64_t noSignalValue_ = -1;
    std::atomic<int> signalValue_{-1};
    std::atomic<bool> signal_{true};

    mutable std::mutex ctrlMu_;  // guards the three members below
    std::vector<ControlInfo> controlsSnapshot_;
    std::vector<std::pair<uint32_t, int64_t>> pendingWrites_;
    Rect appliedCrop_;

    std::atomic<Clock> clock_{Clock::Unknown};
    std::atomic<uint64_t> delivered_{0}, overwritten_{0}, driverGaps_{0}, errorFrames_{0}, shortFrames_{0}, timeouts_{0}, seqJumps_{0};
};

}  // namespace fv

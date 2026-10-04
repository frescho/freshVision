// Records video (whatever raw pixel layout the capture path delivers,
// converted internally) and, optionally, one interleaved S16 PCM audio
// stream, to a Matroska (.mkv) file via FFmpeg's libav* libraries — linked
// directly, not shelled out to the `ffmpeg` binary.
//
//
// Matroska, not MP4: MP4's index is normally written at the end, so a crash
// or a hard stop mid-recording can leave the file unplayable; Matroska is
// designed to stay playable up to the last flushed cluster, which matters
// for "survives mode changes and source restarts".
//
// The encoder is chosen by trying candidates at runtime and taking the first
// one that actually opens (same "ask the system, never assume" pattern as
// the rest of this codebase) — hardware first (NVENC; VAAPI is tried too,
// harmless if it fails; it didn't work on the development machine),
// falling back to libx264, or FFV1 when lossless is explicitly requested.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "formats.h"

namespace fv {

enum class VideoEncoderKind { Auto, Nvenc, Vaapi, X264, Ffv1, Raw };
const char* videoEncoderKindName(VideoEncoderKind k);

// Quick real open/close probe (a few ms each) of Nvenc/Vaapi/X264/Ffv1,
// mirroring what openVideoEncoder(Auto) tries internally, so the Recorder
// panel's dropdown can list only what actually works on this machine
// instead of options that silently fall through (e.g. VAAPI) or
// fail outright. Auto is not included (it is not a real encoder, it is
// "pick one of these"). Meant to be called once at startup, not per frame.
std::vector<VideoEncoderKind> availableVideoEncoders();

struct RecorderCounters {
    uint64_t videoFrames = 0;
    uint64_t audioFrames = 0;
    uint64_t droppedVideoFrames = 0;  // the encoder was a full queue behind; dropped, not queued
    uint64_t droppedAudioBlocks = 0;  // same for audio (queue of 5 s full)
    int64_t bytesWritten = 0;
};

class Recorder {
public:
    ~Recorder();
    Recorder() = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // `width`/`height` fix the recording's canvas for its whole lifetime —
    // a mode change or source switch mid-recording (M1/M2 already make the
    // capture path itself survive these) does not resize the file; a source
    // frame of a different size is letterboxed (scaled, centered, black
    // bars) into this same canvas instead (a design decision:
    // "a mode change keeps the output canvas fixed ... so A/V sync never
    // drifts", chosen over starting a new file segment per mode change).
    // `srcLayout` is the pixel byte order pushVideoFrame() will receive
    // (same as Display::upload()); it cannot change mid-recording (a format
    // change, unlike a size change, does start a new segment — see M3 plan).
    // `audioRate`/`audioChannels` describe pushAudioFrame()'s input;
    // audioChannels = 0 means video-only. Tries `kind` (or, for Auto, Nvenc
    // then Vaapi then X264 in order) and reports which one actually opened.
    bool start(const std::string& path, uint32_t width, uint32_t height, Layout srcLayout, double fps,
              unsigned audioRate, int audioChannels, VideoEncoderKind kind, std::string& err);
    // Flushes both encoders and closes the file (safe to call even if a
    // recording is not active).
    void stop();
    // Matroska tags written into the next recording's header (key, value),
    // e.g. the composite capture's timing for freshCoder. Set before start().
    void setTags(std::vector<std::pair<std::string, std::string>> tags) { tags_ = std::move(tags); }
    bool active() const { return fmtCtx_ != nullptr; }
    std::string activeEncoderName() const;
    const std::string& path() const { return path_; }

    // Called from the display loop with the same raw buffer/stride that goes
    // to Display::upload(), plus that frame's own width/height (which may
    // differ from the canvas passed to start() — a mode change or source
    // switch mid-recording — in which case it's scaled and centered into
    // the fixed canvas, letterboxed with black bars, not resized). `ptsNs`
    // is the frame's capture time on CLOCK_MONOTONIC (the app's clock, also
    // used for audio). Only copies the frame into a free queue slot and
    // returns (well under a millisecond); a recorder thread converts and
    // encodes it. A frame is dropped (and counted) only when every slot is
    // still waiting to be encoded — the display loop is never held up by
    // the encoder (2026-09-28: encoding here cut GUI recordings to ~16 fps).
    void pushVideoFrame(const uint8_t* data, uint32_t srcWidth, uint32_t srcHeight, size_t stride, int64_t ptsNs);
    // Called from the audio mixer thread with its interleaved S16 output.
    // Only copies the block into a queue; a recorder thread encodes and
    // writes it, so a slow encoder or disk never holds up the mixer (which
    // would stall the monitor and, before a fix, the GUI).
    void pushAudioFrame(const int16_t* interleaved, size_t frames, int64_t ptsNs);

    RecorderCounters counters() const;

private:
    bool openVideoEncoder(VideoEncoderKind kind, std::string& err);
    bool openAudioEncoder(std::string& err);
    void encodeVideoFrame(bool flush);
    void encodeAudioFrame(bool flush);
    struct QueuedFrame {
        size_t slot = 0;
        uint32_t width = 0, height = 0;
        size_t stride = 0;
        int64_t ptsNs = 0;
    };
    void videoWorkerMain();
    void audioWorkerMain();
    void encodeQueuedAudio(const int16_t* interleaved, size_t frames, int64_t ptsNs);  // recorder thread, under aMu_
    void convertAndEncode(const QueuedFrame& q);  // recorder thread, under vMu_
    void stopWorker();                           // drains the queue and joins; call without vMu_/aMu_ held
    // stop()'s body, assuming vMu_/aMu_ are already held by the caller.
    // stop() takes both locks then calls this; start() takes both locks
    // once (for its own duration) and calls this directly instead of the
    // public stop() to avoid a self-deadlock re-locking a non-recursive
    // mutex. Needed because start()/stop() rewrite fmtCtx_/vCodecCtx_/
    // aCodecCtx_ — the same pointers pushVideoFrame/pushAudioFrame read —
    // and previously only the flush calls were lock-protected, leaving the
    // actual free()/nullptr teardown to run unlocked while the audio mixer
    // thread (always running, independent of whether a recording is
    // active — see main.cpp's AudioMixer::setSink) could still be inside
    // pushAudioFrame: a real use-after-free window on every Stop, not just
    // app shutdown. Found by review 2026-09-22, alongside the muxMu_ fix.
    void stopLocked();

    // FFmpeg contexts kept as void* so <libavcodec/...> stays out of every
    // other file that includes this header (same pattern as Capture's
    // snd_pcm_t*/SDL_GLContext elsewhere in this codebase).
    void* fmtCtx_ = nullptr;     // AVFormatContext*
    void* vCodecCtx_ = nullptr;  // AVCodecContext* (video)
    void* aCodecCtx_ = nullptr;  // AVCodecContext* (audio), null if audioChannels == 0
    void* swsCtx_ = nullptr;     // SwsContext*
    void* swrCtx_ = nullptr;     // SwrContext*
    void* vFrame_ = nullptr;     // AVFrame* (encoder input, in encPixFmt_)
    int encPixFmt_ = 0;          // AVPixelFormat the encoder takes: YUV420P, BGR0 for FFV1/raw (true lossless), GRAY8 for a g8 source
    void* aFrame_ = nullptr;     // AVFrame* (encoder input, resampled)
    // One AVPacket per stream, not shared: encodeVideoFrame/encodeAudioFrame
    // run on different threads (video thread / audio mixer thread) and a
    // single shared AVPacket* was mutated by both concurrently (receive/
    // rescale/unref) independent of the muxer write itself — found by the
    // same review as the muxMu_ fix above, 2026-09-22.
    void* vPkt_ = nullptr;  // AVPacket* (video)
    void* aPkt_ = nullptr;  // AVPacket* (audio)
    int vStreamIdx_ = -1, aStreamIdx_ = -1;

    std::string encoderName_, path_;
    std::vector<std::pair<std::string, std::string>> tags_;
    uint32_t canvasWidth_ = 0, canvasHeight_ = 0;  // fixed for the recording's lifetime
    uint32_t srcWidth_ = 0, srcHeight_ = 0;        // what swsCtx_ is currently built for (recreated on change)
    int letterboxX_ = 0, letterboxY_ = 0, letterboxW_ = 0, letterboxH_ = 0;  // where srcWidth_/Height_ lands in the canvas
    Layout srcLayout_ = Layout::Bgrx8;
    double fps_ = 0;
    unsigned audioRate_ = 0;
    int audioChannels_ = 0;
    // Set once, by whichever of pushVideoFrame/pushAudioFrame is called
    // first; those two run on different threads under different mutexes
    // (vMu_/aMu_), so this one shared field needs its own synchronization
    // (a plain int64_t here was a genuine check-then-set data race across
    // the two locks — found by review 2026-09-22, fixed with a CAS below).
    std::atomic<int64_t> firstVideoPtsNs_{-1};
    int64_t lastVideoPts_ = -1;  // encoder time base; recorder thread only, keeps pts strictly increasing
    int64_t videoFrameCount_ = 0, audioSampleCount_ = 0;
    // Next audio pts (samples, audio time base), continuous from block to
    // block so the track has no gaps or overlaps; -1 = not started. Under aMu_.
    int64_t audioNextPts_ = -1;
    int64_t audioApartSamples_ = 0;  // how long the count and the clock have been >40 ms apart (samples); under aMu_

    mutable std::mutex vMu_, aMu_;  // video and audio pushed from different threads
    // av_interleaved_write_frame() mutates fmtCtx_'s shared interleaving
    // queue and is not thread-safe for concurrent callers; vMu_/aMu_ each
    // guard only their own stream's encode state, not that shared call, so
    // encodeVideoFrame/encodeAudioFrame each additionally take this one
    // around the actual write. Found via a real SIGSEGV inside
    // av_interleaved_write_frame (gdb backtrace, 2026-09-22) — concurrent
    // video/audio writes corrupting libavformat's internal muxer state;
    // also the likely cause of the earlier intermittent "Can't write packet
    // with unknown timestamp" warning (a milder symptom of the same race).
    mutable std::mutex muxMu_;
    std::atomic<uint64_t> videoFrames_{0}, audioFrames_{0}, droppedVideoFrames_{0};

    // Frame queue between the display loop and the recorder thread.
    static constexpr size_t kQueueSlots = 6;  // 120 ms at 50 fps of headroom for a slow encode
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::vector<std::vector<uint8_t>> slots_;  // frame copies, reused
    std::vector<size_t> freeSlots_;
    std::deque<QueuedFrame> queue_;
    bool workerStop_ = false;               // under qMu_
    std::atomic<bool> accepting_{false};    // pushVideoFrame fast path when not recording
    std::thread vWorker_;

    // Audio block queue between the mixer thread and the audio recorder thread.
    struct QueuedAudio {
        std::vector<int16_t> samples;  // interleaved
        size_t frames = 0;
        int64_t ptsNs = 0;
    };
    static constexpr size_t kAudioQueueMax = 250;  // 5 s of 20 ms blocks
    std::mutex aqMu_;
    std::condition_variable aqCv_;
    std::deque<QueuedAudio> audioQueue_;
    bool audioWorkerStop_ = false;  // under aqMu_
    std::thread aWorker_;
    std::atomic<uint64_t> droppedAudioBlocks_{0};
    std::atomic<int64_t> bytesWritten_{0};
};

}  // namespace fv

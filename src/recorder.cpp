// SPDX-License-Identifier: GPL-3.0-or-later
#include "recorder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fv {

namespace {

// Maps this codebase's Layout (formats.h; the byte order the driver actually
// delivers, already resolved by the caller) to the matching FFmpeg pixel
// format, so swscale converts correctly without this file re-deriving the
// byte order itself.
AVPixelFormat toAvPixFmt(Layout l) {
    switch (l) {
        case Layout::Bgrx8: return AV_PIX_FMT_BGRA;
        case Layout::Rgbx8: return AV_PIX_FMT_RGBA;
        case Layout::Xrgb8: return AV_PIX_FMT_ARGB;
        case Layout::Xbgr8: return AV_PIX_FMT_ABGR;
        case Layout::Rgb24: return AV_PIX_FMT_RGB24;
        case Layout::Bgr24: return AV_PIX_FMT_BGR24;
        case Layout::Uyvy: return AV_PIX_FMT_UYVY422;
        case Layout::Grey8: return AV_PIX_FMT_GRAY8;
    }
    return AV_PIX_FMT_BGRA;
}

// Where a `srcW x srcH` frame lands inside a fixed `canvasW x canvasH`
// canvas, preserving aspect ratio (scaled to fit, centered, the rest left as
// black bars): the picture size stays fixed across a mode change.
//
struct Letterbox {
    int x = 0, y = 0, w = 0, h = 0;
};
Letterbox computeLetterbox(uint32_t srcW, uint32_t srcH, uint32_t canvasW, uint32_t canvasH) {
    double scale = std::min(double(canvasW) / double(srcW), double(canvasH) / double(srcH));
    Letterbox r;
    r.w = std::max(1, int(std::lround(double(srcW) * scale)));
    r.h = std::max(1, int(std::lround(double(srcH) * scale)));
    r.x = (int(canvasW) - r.w) / 2;
    r.y = (int(canvasH) - r.h) / 2;
    r.x -= r.x % 2;  // even offsets: chroma planes are half resolution
    r.y -= r.y % 2;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    return r;
}

const char* encoderNameFor(VideoEncoderKind k) {
    switch (k) {
        case VideoEncoderKind::Nvenc: return "h264_nvenc";
        case VideoEncoderKind::Vaapi: return "h264_vaapi";
        case VideoEncoderKind::X264: return "libx264";
        case VideoEncoderKind::Ffv1: return "ffv1";
        case VideoEncoderKind::Raw: return "rawvideo";
        case VideoEncoderKind::Auto: return nullptr;
    }
    return nullptr;
}

}  // namespace

const char* videoEncoderKindName(VideoEncoderKind k) {
    switch (k) {
        case VideoEncoderKind::Auto: return "Auto";
        case VideoEncoderKind::Nvenc: return "NVENC";
        case VideoEncoderKind::Vaapi: return "VAAPI";
        case VideoEncoderKind::X264: return "x264";
        case VideoEncoderKind::Ffv1: return "FFV1";
        case VideoEncoderKind::Raw: return "Raw";
    }
    return "?";
}

std::vector<VideoEncoderKind> availableVideoEncoders() {
    std::vector<VideoEncoderKind> out;
    for (VideoEncoderKind kind : {VideoEncoderKind::Nvenc, VideoEncoderKind::Vaapi, VideoEncoderKind::X264,
                                  VideoEncoderKind::Ffv1, VideoEncoderKind::Raw}) {
        const char* name = encoderNameFor(kind);
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) continue;  // not built into this FFmpeg
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;
        // Minimal params, same shape as openVideoEncoder() but without a
        // real AVFormatContext (avcodec_open2 doesn't need one) — just
        // enough for a real open to succeed or fail the way it would for
        // an actual recording (e.g. VAAPI fails here for the same reason
        // it fails in openVideoEncoder: no working hw device on this
        // machine).
        // NVENC rejects very small frames ("Frame Dimension less than the
        // minimum supported value" at 64x64, found while testing this probe
        // -- 256x256 clears that floor for every encoder here.
        ctx->width = 256;
        ctx->height = 256;
        ctx->pix_fmt = (kind == VideoEncoderKind::Ffv1 || kind == VideoEncoderKind::Raw) ? AV_PIX_FMT_BGR0
                                                                                          : AV_PIX_FMT_YUV420P;
        ctx->time_base = AVRational{1, 50};
        ctx->framerate = AVRational{50, 1};
        if (kind == VideoEncoderKind::Ffv1) {
            ctx->flags |= AV_CODEC_FLAG_QSCALE;
            ctx->global_quality = 0;
        }
        if (avcodec_open2(ctx, codec, nullptr) >= 0) out.push_back(kind);
        avcodec_free_context(&ctx);
    }
    return out;
}

Recorder::~Recorder() { stop(); }

std::string Recorder::activeEncoderName() const { return encoderName_; }

bool Recorder::openVideoEncoder(VideoEncoderKind kind, std::string& err) {
    std::vector<std::string> candidates;
    if (kind == VideoEncoderKind::Auto) {
        // The graphics card first (NVENC; VAAPI where it works), x264 as the
        // fallback. x264 was put first for a while because NVENC seemed to
        // stall the GUI; the stall was the GUI waiting on the audio mixer's
        // lock (since fixed), so NVENC is first again.
        //
        candidates = {"h264_nvenc", "h264_vaapi", "libx264"};
    } else {
        const char* n = encoderNameFor(kind);
        if (!n) {
            err = "unknown encoder kind";
            return false;
        }
        candidates = {n};
    }

    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmtCtx_);
    for (const std::string& name : candidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) {
            err += name + ": not built into this FFmpeg; ";
            continue;
        }
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;
        ctx->width = int(canvasWidth_);
        ctx->height = int(canvasHeight_);
        // FFV1 gets RGB (bgr0) so "lossless" means lossless against the
        // captured pixels; converting to 4:2:0 first quartered the colour
        // resolution. The H.264
        // encoders stay on 4:2:0, the format every player handles.
        // A g8 (Sync-on-Green composite) source is one channel of raw
        // samples: the lossless encoders keep it as exactly that, one plane,
        // so freshCoder gets the card's own samples back (
        // step 5).
        const bool lossless = name == "ffv1" || name == "rawvideo";
        AVPixelFormat pixFmt = !lossless                      ? AV_PIX_FMT_YUV420P
                               : srcLayout_ == Layout::Grey8 ? AV_PIX_FMT_GRAY8
                                                             : AV_PIX_FMT_BGR0;
        ctx->pix_fmt = pixFmt;
        ctx->time_base = AVRational{1, 90000};
        ctx->framerate = AVRational{int(fps_ > 0 ? std::lround(fps_) : 50), 1};
        ctx->gop_size = int(fps_ > 0 ? std::lround(fps_) * 2 : 100);
        // No B-frames: this records a live, occasionally-irregular capture
        // (frames can be dropped, PTS spacing isn't perfectly even), and
        // B-frame reordering combined with that produced real "pts < dts"
        // muxer warnings in testing. Not worth the extra compression here.
        ctx->max_b_frames = 0;
        if (name == "ffv1") {
            ctx->flags |= AV_CODEC_FLAG_QSCALE;
            ctx->global_quality = 0;  // lossless
            // Version 3 with slices lets FFV1 encode on several cores.
            ctx->level = 3;
            av_opt_set_int(ctx->priv_data, "slices", 12, 0);
            ctx->thread_count = 0;  // auto
        }
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        // VAAPI needs a hw device/frames context to succeed at all; this
        // machine's doesn't have a working one, so this is
        // a best-effort attempt that fails cleanly and falls through, not a
        // full VAAPI implementation.
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            err += name + ": failed to open; ";
            avcodec_free_context(&ctx);
            continue;
        }

        AVStream* st = avformat_new_stream(fmt, nullptr);
        if (!st || avcodec_parameters_from_context(st->codecpar, ctx) < 0) {
            avcodec_free_context(&ctx);
            err = "avformat_new_stream/parameters_from_context failed";
            return false;
        }
        st->time_base = ctx->time_base;
        vStreamIdx_ = st->index;
        vCodecCtx_ = ctx;
        encoderName_ = name;
        encPixFmt_ = int(pixFmt);
        return true;
    }
    return false;
}

bool Recorder::openAudioEncoder(std::string& err) {
    if (audioChannels_ <= 0) return true;  // video-only recording
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);  // no lossy compression
    if (!codec) {
        err = "no PCM S16LE encoder in this FFmpeg build";
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = int(audioRate_);
    ctx->ch_layout.nb_channels = audioChannels_;
    av_channel_layout_default(&ctx->ch_layout, audioChannels_);
    ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    ctx->time_base = AVRational{1, int(audioRate_)};
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmtCtx_);
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        err = "PCM encoder failed to open";
        avcodec_free_context(&ctx);
        return false;
    }
    AVStream* st = avformat_new_stream(fmt, nullptr);
    if (!st || avcodec_parameters_from_context(st->codecpar, ctx) < 0) {
        avcodec_free_context(&ctx);
        err = "audio avformat_new_stream failed";
        return false;
    }
    st->time_base = ctx->time_base;
    aStreamIdx_ = st->index;
    aCodecCtx_ = ctx;
    return true;
}

bool Recorder::start(const std::string& path, uint32_t width, uint32_t height, Layout srcLayout, double fps,
                     unsigned audioRate, int audioChannels, VideoEncoderKind kind, std::string& err) {
    // Held for this whole call, not just stopLocked(): openVideoEncoder/
    // openAudioEncoder and the frame/packet allocation below all write
    // fmtCtx_/vCodecCtx_/aCodecCtx_/vFrame_/aFrame_/vPkt_/aPkt_, which
    // pushVideoFrame/pushAudioFrame read as soon as they're non-null — the
    // audio mixer thread keeps calling pushAudioFrame regardless of
    // recording state (main.cpp's AudioMixer::setSink), so a half-built
    // Recorder must stay invisible to it until start() finishes or fails.
    stop();  // any recording in progress is finished (worker drained and joined) first
    std::scoped_lock<std::mutex, std::mutex> lk(vMu_, aMu_);
    canvasWidth_ = width;
    canvasHeight_ = height;
    srcWidth_ = srcHeight_ = 0;  // forces pushVideoFrame to (re)build the letterbox/sws context on first frame
    srcLayout_ = srcLayout;
    fps_ = fps;
    audioRate_ = audioRate;
    audioChannels_ = audioChannels;
    firstVideoPtsNs_ = -1;
    lastVideoPts_ = -1;
    videoFrameCount_ = audioSampleCount_ = 0;
    audioNextPts_ = -1;
    audioApartSamples_ = 0;
    videoFrames_ = audioFrames_ = droppedVideoFrames_ = 0;
    bytesWritten_ = 0;

    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "matroska", path.c_str()) < 0 || !fmt) {
        err = "avformat_alloc_output_context2 failed";
        return false;
    }
    fmtCtx_ = fmt;

    if (!openVideoEncoder(kind, err)) {
        std::string videoErr = err;
        stopLocked();
        err = "no working video encoder (" + videoErr + ")";
        return false;
    }
    err.clear();
    if (!openAudioEncoder(err)) {
        std::string e = err;
        stopLocked();
        err = "audio: " + e;
        return false;
    }

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            err = "avio_open failed for " + path;
            stopLocked();
            return false;
        }
    }
    for (const auto& [key, value] : tags_) av_dict_set(&fmt->metadata, key.c_str(), value.c_str(), 0);
    if (avformat_write_header(fmt, nullptr) < 0) {
        err = "avformat_write_header failed";
        stopLocked();
        return false;
    }

    // swsCtx_ is built lazily in pushVideoFrame() once the first frame's
    // actual size is known (and rebuilt if that size ever changes — a mode
    // change or source switch mid-recording); the encoder's frame is always
    // canvas-sized regardless.
    AVFrame* vf = av_frame_alloc();
    vf->format = encPixFmt_;
    vf->width = int(canvasWidth_);
    vf->height = int(canvasHeight_);
    if (av_frame_get_buffer(vf, 0) < 0) {
        av_frame_free(&vf);
        err = "video frame buffer allocation failed";
        stopLocked();
        return false;
    }
    vFrame_ = vf;

    if (audioChannels_ > 0) {
        AVCodecContext* actx = static_cast<AVCodecContext*>(aCodecCtx_);
        AVFrame* af = av_frame_alloc();
        af->format = AV_SAMPLE_FMT_S16;
        av_channel_layout_copy(&af->ch_layout, &actx->ch_layout);
        af->sample_rate = int(audioRate_);
        aFrame_ = af;
    }

    vPkt_ = av_packet_alloc();
    aPkt_ = av_packet_alloc();
    path_ = path;

    {
        std::lock_guard<std::mutex> q(qMu_);
        slots_.assign(kQueueSlots, {});
        freeSlots_.clear();
        for (size_t i = 0; i < kQueueSlots; ++i) freeSlots_.push_back(i);
        queue_.clear();
        workerStop_ = false;
    }
    vWorker_ = std::thread(&Recorder::videoWorkerMain, this);
    {
        std::lock_guard<std::mutex> q(aqMu_);
        audioQueue_.clear();
        audioWorkerStop_ = false;
    }
    droppedAudioBlocks_ = 0;
    aWorker_ = std::thread(&Recorder::audioWorkerMain, this);
    accepting_ = true;
    return true;
}

void Recorder::encodeVideoFrame(bool flush) {
    AVCodecContext* ctx = static_cast<AVCodecContext*>(vCodecCtx_);
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmtCtx_);
    AVPacket* pkt = static_cast<AVPacket*>(vPkt_);
    int rc = avcodec_send_frame(ctx, flush ? nullptr : static_cast<AVFrame*>(vFrame_));
    if (rc < 0) return;
    while (avcodec_receive_packet(ctx, pkt) == 0) {
        pkt->stream_index = vStreamIdx_;
        av_packet_rescale_ts(pkt, ctx->time_base, fmt->streams[vStreamIdx_]->time_base);
        bytesWritten_ += pkt->size;
        {
            std::lock_guard<std::mutex> muxLk(muxMu_);  // fmtCtx_ is shared with the audio thread
            av_interleaved_write_frame(fmt, pkt);
        }
        av_packet_unref(pkt);
    }
}

void Recorder::pushVideoFrame(const uint8_t* data, uint32_t srcWidth, uint32_t srcHeight, size_t stride,
                              int64_t ptsNs) {
    if (!accepting_ || !data || !srcWidth || !srcHeight) return;
    std::lock_guard<std::mutex> lk(qMu_);
    if (freeSlots_.empty()) {
        ++droppedVideoFrames_;  // the encoder is behind by kQueueSlots frames
        return;
    }
    size_t slot = freeSlots_.back();
    freeSlots_.pop_back();
    slots_[slot].assign(data, data + stride * srcHeight);
    queue_.push_back(QueuedFrame{slot, srcWidth, srcHeight, stride, ptsNs});
    qCv_.notify_one();
}

void Recorder::videoWorkerMain() {
    for (;;) {
        QueuedFrame q;
        {
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait(lk, [&] { return workerStop_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stop requested and everything queued has been encoded
            q = queue_.front();
            queue_.pop_front();
        }
        {
            std::lock_guard<std::mutex> lk(vMu_);
            convertAndEncode(q);
        }
        std::lock_guard<std::mutex> lk(qMu_);
        freeSlots_.push_back(q.slot);
    }
}

void Recorder::stopWorker() {
    accepting_ = false;
    {
        std::lock_guard<std::mutex> lk(qMu_);
        workerStop_ = true;
    }
    qCv_.notify_all();
    if (vWorker_.joinable()) vWorker_.join();
    {
        std::lock_guard<std::mutex> lk(aqMu_);
        audioWorkerStop_ = true;
    }
    aqCv_.notify_all();
    if (aWorker_.joinable()) aWorker_.join();
    {
        std::lock_guard<std::mutex> lk(aqMu_);
        audioQueue_.clear();
        audioWorkerStop_ = false;
    }
    std::lock_guard<std::mutex> lk(qMu_);
    queue_.clear();
    freeSlots_.clear();
    slots_.clear();
    workerStop_ = false;
}

void Recorder::convertAndEncode(const QueuedFrame& q) {
    if (!fmtCtx_ || !vCodecCtx_) return;
    const uint8_t* data = slots_[q.slot].data();  // owned by this thread until returned to freeSlots_
    const uint32_t srcWidth = q.width, srcHeight = q.height;
    // CAS, not "if < 0 then =": pushAudioFrame() can race this same field
    // from its own thread/mutex (see recorder.h). Whichever call wins
    // becomes pts=0; audio starting before the first video frame is a
    // normal, handled case.
    int64_t expectedAnchor = -1;
    firstVideoPtsNs_.compare_exchange_strong(expectedAnchor, q.ptsNs);

    // A mode change or source switch mid-recording changes the source size;
    // rebuild the scaler and the letterbox rect for the new size, but never
    // the canvas itself (Recorder::start's doc comment).
    const AVPixelFormat encFmt = AVPixelFormat(encPixFmt_);
    if (srcWidth != srcWidth_ || srcHeight != srcHeight_) {
        if (swsCtx_) sws_freeContext(static_cast<SwsContext*>(swsCtx_));
        Letterbox lb = computeLetterbox(srcWidth, srcHeight, canvasWidth_, canvasHeight_);
        letterboxX_ = lb.x;
        letterboxY_ = lb.y;
        letterboxW_ = lb.w;
        letterboxH_ = lb.h;
        swsCtx_ = sws_getContext(int(srcWidth), int(srcHeight), toAvPixFmt(srcLayout_), letterboxW_, letterboxH_,
                                 encFmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        srcWidth_ = srcWidth;
        srcHeight_ = srcHeight;
    }
    if (!swsCtx_) return;

    AVFrame* vf = static_cast<AVFrame*>(vFrame_);
    if (av_frame_make_writable(vf) < 0) return;
    // Cleared every frame (not just when the letterbox geometry changes): a
    // shrinking source would otherwise leave stale pixels outside the new,
    // smaller letterboxed rect.
    uint8_t* dstPlanes[4] = {nullptr, nullptr, nullptr, nullptr};
    int dstStride[4] = {vf->linesize[0], vf->linesize[1], vf->linesize[2], 0};
    if (encFmt == AV_PIX_FMT_GRAY8 && letterboxW_ == int(srcWidth) && letterboxH_ == int(srcHeight)) {
        // Same size, same format: a plain copy, so the samples are
        // bit-exact without depending on the scaler's same-size behaviour.
        for (uint32_t r = 0; r < srcHeight; ++r)
            std::memcpy(vf->data[0] + size_t(letterboxY_ + int(r)) * size_t(vf->linesize[0]) + size_t(letterboxX_),
                        data + size_t(r) * q.stride, srcWidth);
        dstPlanes[0] = nullptr;
    } else if (encFmt == AV_PIX_FMT_GRAY8) {
        std::memset(vf->data[0], 0, size_t(vf->linesize[0]) * canvasHeight_);
        dstPlanes[0] = vf->data[0] + size_t(letterboxY_) * size_t(vf->linesize[0]) + size_t(letterboxX_);
    } else if (encFmt == AV_PIX_FMT_BGR0) {
        std::memset(vf->data[0], 0, size_t(vf->linesize[0]) * canvasHeight_);
        dstPlanes[0] = vf->data[0] + size_t(letterboxY_) * size_t(vf->linesize[0]) + size_t(letterboxX_) * 4;
    } else {
        // Limited-range YUV black (Y=16, U=V=128), not 0.
        std::memset(vf->data[0], 16, size_t(vf->linesize[0]) * canvasHeight_);
        std::memset(vf->data[1], 128, size_t(vf->linesize[1]) * ((canvasHeight_ + 1) / 2));
        std::memset(vf->data[2], 128, size_t(vf->linesize[2]) * ((canvasHeight_ + 1) / 2));
        dstPlanes[0] = vf->data[0] + size_t(letterboxY_) * size_t(vf->linesize[0]) + size_t(letterboxX_);
        dstPlanes[1] = vf->data[1] + size_t(letterboxY_ / 2) * size_t(vf->linesize[1]) + size_t(letterboxX_ / 2);
        dstPlanes[2] = vf->data[2] + size_t(letterboxY_ / 2) * size_t(vf->linesize[2]) + size_t(letterboxX_ / 2);
    }
    const uint8_t* srcSlices[1] = {data};
    int srcStride[1] = {int(q.stride)};
    if (dstPlanes[0])
        sws_scale(static_cast<SwsContext*>(swsCtx_), srcSlices, srcStride, 0, int(srcHeight), dstPlanes, dstStride);

    AVCodecContext* ctx = static_cast<AVCodecContext*>(vCodecCtx_);
    int64_t pts = av_rescale_q(q.ptsNs - firstVideoPtsNs_.load(), AVRational{1, 1000000000}, ctx->time_base);
    if (pts < 0) return;  // captured before the recording's time origin (set by an earlier audio chunk)
    if (pts <= lastVideoPts_) pts = lastVideoPts_ + 1;  // strictly increasing, whatever the clock rounding
    lastVideoPts_ = pts;
    vf->pts = pts;
    encodeVideoFrame(false);
    ++videoFrames_;
    ++videoFrameCount_;
}

void Recorder::encodeAudioFrame(bool flush) {
    AVCodecContext* ctx = static_cast<AVCodecContext*>(aCodecCtx_);
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmtCtx_);
    AVPacket* pkt = static_cast<AVPacket*>(aPkt_);
    int rc = avcodec_send_frame(ctx, flush ? nullptr : static_cast<AVFrame*>(aFrame_));
    if (rc < 0) return;
    while (avcodec_receive_packet(ctx, pkt) == 0) {
        pkt->stream_index = aStreamIdx_;
        av_packet_rescale_ts(pkt, ctx->time_base, fmt->streams[aStreamIdx_]->time_base);
        bytesWritten_ += pkt->size;
        {
            std::lock_guard<std::mutex> muxLk(muxMu_);  // fmtCtx_ is shared with the video thread
            av_interleaved_write_frame(fmt, pkt);
        }
        av_packet_unref(pkt);
    }
}

void Recorder::pushAudioFrame(const int16_t* interleaved, size_t frames, int64_t ptsNs) {
    if (!accepting_ || audioChannels_ <= 0 || !interleaved || !frames) return;
    std::lock_guard<std::mutex> lk(aqMu_);
    if (audioQueue_.size() >= kAudioQueueMax) {
        ++droppedAudioBlocks_;  // the recorder is 5 s behind; drop rather than hold up the mixer
        return;
    }
    QueuedAudio q;
    q.samples.assign(interleaved, interleaved + frames * size_t(audioChannels_));
    q.frames = frames;
    q.ptsNs = ptsNs;
    audioQueue_.push_back(std::move(q));
    aqCv_.notify_one();
}

void Recorder::audioWorkerMain() {
    for (;;) {
        QueuedAudio q;
        {
            std::unique_lock<std::mutex> lk(aqMu_);
            aqCv_.wait(lk, [&] { return audioWorkerStop_ || !audioQueue_.empty(); });
            if (audioQueue_.empty()) return;  // stop requested and everything queued has been written
            q = std::move(audioQueue_.front());
            audioQueue_.pop_front();
        }
        std::lock_guard<std::mutex> lk(aMu_);
        encodeQueuedAudio(q.samples.data(), q.frames, q.ptsNs);
    }
}

void Recorder::encodeQueuedAudio(const int16_t* interleaved, size_t frames, int64_t ptsNs) {
    if (!fmtCtx_ || !aCodecCtx_ || audioChannels_ <= 0 || !frames) return;
    // Anchored to the same clock as video (both CLOCK_MONOTONIC/steady_clock,
    // cross-comparable — already relied on for the drift measurement):
    // firstVideoPtsNs_ may not be set yet if audio starts before the first
    // video frame, in which case this chunk is simply the time origin.
    int64_t expectedAnchor = -1;
    firstVideoPtsNs_.compare_exchange_strong(expectedAnchor, ptsNs);

    AVFrame* af = static_cast<AVFrame*>(aFrame_);
    AVCodecContext* ctx = static_cast<AVCodecContext*>(aCodecCtx_);
    // av_frame_unref() (below, end of the previous call) clears format/
    // sample_rate/ch_layout along with the buffer, not just the buffer —
    // found by a real crash (null af->data[0] from an unchecked
    // av_frame_get_buffer failure): they must be re-set every call, not
    // only once in start().
    af->format = AV_SAMPLE_FMT_S16;
    af->sample_rate = int(audioRate_);
    av_channel_layout_copy(&af->ch_layout, &ctx->ch_layout);
    af->nb_samples = int(frames);
    if (av_frame_get_buffer(af, 0) < 0 || !af->data[0]) {
        av_frame_unref(af);
        return;
    }
    std::memcpy(af->data[0], interleaved, frames * size_t(audioChannels_) * sizeof(int16_t));
    // Each block starts exactly where the previous one ended (sample count),
    // not at the clock time the mixer thread handled it: those clock times
    // jitter, and the track came out with half its blocks overlapping and
    // the rest leaving small gaps. The mixer produces
    // blocks on a fixed clock grid, so the count stays in step with the
    // clock. It is re-anchored to the clock only if they stay more than
    // 40 ms apart for 0.5 s in a row (e.g. the mixer's timing was reset by a
    // monitor switch); a brief stall that the mixer then catches up on must
    // not cause a jump (seen at recording start).
    int64_t clockPts = av_rescale_q(ptsNs - firstVideoPtsNs_.load(), AVRational{1, 1000000000}, ctx->time_base);
    int64_t maxDrift = int64_t(audioRate_) / 25;  // 40 ms in samples
    bool apart = audioNextPts_ >= 0 && (clockPts - audioNextPts_ > maxDrift || audioNextPts_ - clockPts > maxDrift);
    audioApartSamples_ = apart ? audioApartSamples_ + int64_t(frames) : 0;
    if (audioNextPts_ < 0 || audioApartSamples_ >= int64_t(audioRate_) / 2) {
        audioNextPts_ = std::max<int64_t>(0, clockPts);
        audioApartSamples_ = 0;
    }
    af->pts = audioNextPts_;
    audioNextPts_ += int64_t(frames);

    encodeAudioFrame(false);
    av_frame_unref(af);
    audioSampleCount_ += int64_t(frames);
    ++audioFrames_;
}

void Recorder::stop() {
    stopWorker();  // must not hold vMu_: the worker takes it for every frame it drains
    std::scoped_lock<std::mutex, std::mutex> lk(vMu_, aMu_);
    stopLocked();
}

void Recorder::stopLocked() {
    if (!fmtCtx_) return;
    if (vCodecCtx_) encodeVideoFrame(true);  // flush
    if (aCodecCtx_) encodeAudioFrame(true);  // flush
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmtCtx_);
    av_write_trailer(fmt);
    if (vCodecCtx_) {
        AVCodecContext* c = static_cast<AVCodecContext*>(vCodecCtx_);
        avcodec_free_context(&c);
        vCodecCtx_ = nullptr;
    }
    if (aCodecCtx_) {
        AVCodecContext* c = static_cast<AVCodecContext*>(aCodecCtx_);
        avcodec_free_context(&c);
        aCodecCtx_ = nullptr;
    }
    if (swsCtx_) {
        sws_freeContext(static_cast<SwsContext*>(swsCtx_));
        swsCtx_ = nullptr;
    }
    if (swrCtx_) {
        SwrContext* s = static_cast<SwrContext*>(swrCtx_);
        swr_free(&s);
        swrCtx_ = nullptr;
    }
    if (vFrame_) {
        AVFrame* f = static_cast<AVFrame*>(vFrame_);
        av_frame_free(&f);
        vFrame_ = nullptr;
    }
    if (aFrame_) {
        AVFrame* f = static_cast<AVFrame*>(aFrame_);
        av_frame_free(&f);
        aFrame_ = nullptr;
    }
    if (vPkt_) {
        AVPacket* p = static_cast<AVPacket*>(vPkt_);
        av_packet_free(&p);
        vPkt_ = nullptr;
    }
    if (aPkt_) {
        AVPacket* p = static_cast<AVPacket*>(aPkt_);
        av_packet_free(&p);
        aPkt_ = nullptr;
    }
    if (!(fmt->oformat->flags & AVFMT_NOFILE) && fmt->pb) avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    fmtCtx_ = nullptr;
    vStreamIdx_ = aStreamIdx_ = -1;
    encoderName_.clear();
}

RecorderCounters Recorder::counters() const {
    RecorderCounters c;
    c.videoFrames = videoFrames_;
    c.audioFrames = audioFrames_;
    c.droppedVideoFrames = droppedVideoFrames_;
    c.droppedAudioBlocks = droppedAudioBlocks_;
    c.bytesWritten = bytesWritten_;
    return c;
}

}  // namespace fv

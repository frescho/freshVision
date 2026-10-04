#include "recording.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pal.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace fc {

int samplesPerCycleFor(int horizontalSize, pal::Standard standard) {
    // PAL: ~283.6 cycles per line (283.75 standard, 283.5 C64); NTSC ~227.75 (227.5 / 228).
    const double q = double(horizontalSize) / pal::standardInfo(standard).cyclesPerLine;
    if (q < 3.5 || std::abs(q - std::round(q)) >= 0.01 * q) return 0;
    return int(std::lround(q));
}

void activeWindowFor(int horizontalSize, int position, int& activeStart, int& activeLen, pal::Standard standard) {
    const pal::StandardInfo& si = pal::standardInfo(standard);
    const double fs = double(horizontalSize) * si.lineRate;
    // The active picture starts 10.5 us (PAL) / 10.9 us (NTSC) after the sync edge.
    activeStart = std::max(0, int(std::lround(si.activeStart * fs)) - position);
    activeLen = int(std::lround(si.activeLen * fs));
}

Recording::~Recording() { close(); }

void Recording::close() {
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_);
    AVCodecContext* dec = static_cast<AVCodecContext*>(dec_);
    AVPacket* pkt = static_cast<AVPacket*>(pkt_);
    AVFrame* frame = static_cast<AVFrame*>(frame_);
    if (frame) av_frame_free(&frame);
    if (sws_) sws_freeContext(static_cast<SwsContext*>(sws_));
    sws_ = nullptr;
    if (pkt) av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    fmt_ = dec_ = pkt_ = frame_ = nullptr;
    vIdx_ = -1;
    eof_ = false;
}

bool Recording::open(const std::string& path, std::string& err) {
    close();
    info_ = RecordingInfo{};
    info_.path = path;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        err = "cannot open " + path;
        return false;
    }
    fmt_ = fmt;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        err = "cannot read the streams of " + path;
        return false;
    }
    vIdx_ = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx_ < 0) {
        err = "no video stream in " + path;
        return false;
    }
    AVStream* vs = fmt->streams[vIdx_];
    const AVCodec* codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) {
        err = "no decoder for the video stream";
        return false;
    }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, vs->codecpar);
    dec->thread_count = 0;
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        err = "cannot open the video decoder";
        return false;
    }
    dec_ = dec;
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();

    info_.width = vs->codecpar->width;
    info_.height = vs->codecpar->height;
    info_.videoCodec = codec->name;
    AVRational fr = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
    info_.fps = fr.num && fr.den ? av_q2d(fr) : 50.0;
    tb_ = av_q2d(vs->time_base);
    startPts_ = vs->start_time != AV_NOPTS_VALUE ? vs->start_time : 0;
    info_.durationSec = fmt->duration != AV_NOPTS_VALUE ? double(fmt->duration) / AV_TIME_BASE : 0;
    fileStartSec_ = fmt->start_time != AV_NOPTS_VALUE ? double(fmt->start_time) / AV_TIME_BASE : 0.0;
    info_.videoStartSec = double(startPts_) * tb_ - fileStartSec_;
    counter_ = 0;
    counterFromPts_ = false;
    info_.frames = int64_t(std::lround(info_.durationSec * info_.fps));
    const AVPixelFormat pf = AVPixelFormat(vs->codecpar->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pf);
    const bool rgb = desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
    if (pf != AV_PIX_FMT_GRAY8 && !rgb) {
        err = "the video is neither one channel of raw samples (gray8, composite) nor RGB (S-Video): not a freshVision raw "
              "recording";
        return false;
    }

    int aIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aIdx >= 0) {
        info_.hasAudio = true;
        info_.audioRate = fmt->streams[aIdx]->codecpar->sample_rate;
        info_.audioChannels = fmt->streams[aIdx]->codecpar->ch_layout.nb_channels;
    }
    const AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_get(fmt->metadata, "", e, AV_DICT_IGNORE_SUFFIX))) info_.tags[e->key] = e->value;
    auto tagInt = [&](const char* k) {
        auto it = info_.tags.find(k);
        return it == info_.tags.end() ? 0 : std::atoi(it->second.c_str());
    };
    info_.horizontalSize = tagInt("CTRL_HORIZONTAL_SIZE");
    info_.horizontalPosition = tagInt("CTRL_HORIZONTAL_POSITION");
    if (rgb) {
        // S-Video: which inputs carry brightness and colour (freshVision
        // writes the tag). Without it the recording is not decoded as
        // S-Video unless chosen (chooseInputs): the green input as composite.
        info_.rgb = true;
        auto it = info_.tags.find("FRESHVISION_SVIDEO");
        if (it != info_.tags.end() && it->second.size() == 3) {
            auto ch = [](char c) { return c == 'R' ? 0 : c == 'G' ? 1 : c == 'B' ? 2 : -1; };
            const int l = ch(it->second[0]), cc = ch(it->second[2]);
            if (l >= 0 && cc >= 0 && l != cc) info_.svideoTagged = true, info_.tagLuma = l, info_.tagChroma = cc;
        }
        chooseInputs(kSvideoFromRecording, 1, 2);
        sws_ = sws_getContext(info_.width, info_.height, pf, info_.width, info_.height, AV_PIX_FMT_GBRP, SWS_POINT, nullptr,
                              nullptr, nullptr);
        if (!sws_) {
            err = "cannot convert the RGB video to planes";
            return false;
        }
    }
    {
        auto it = info_.tags.find("FRESHVISION_STANDARD");
        if (it != info_.tags.end()) {
            info_.standardTagged = true;
            info_.taggedStandard = it->second == "NTSC" ? pal::Standard::Ntsc : pal::Standard::Pal;
        }
    }
    chooseStandard(0);
    return true;
}

void Recording::chooseStandard(int mode) {
    info_.standard = mode == 1 ? pal::Standard::Pal : mode == 2 ? pal::Standard::Ntsc : info_.taggedStandard;
    info_.samplesPerCycle = samplesPerCycleFor(info_.horizontalSize, info_.standard);
    info_.activeStart = info_.activeLen = 0;
    if (info_.samplesPerCycle)
        activeWindowFor(info_.horizontalSize, info_.horizontalPosition, info_.activeStart, info_.activeLen, info_.standard);
}

void Recording::chooseInputs(int mode, int luma, int chroma) {
    if (!info_.rgb) return;
    luma = std::clamp(luma, 0, 2), chroma = std::clamp(chroma, 0, 2);
    if (mode == kSvideoOn && luma != chroma) {
        info_.svideo = true, info_.lumaChannel = luma, info_.chromaChannel = chroma;
    } else if (mode == kSvideoOff || mode == kSvideoOn) {
        info_.svideo = false, info_.lumaChannel = luma;
    } else if (info_.svideoTagged) {
        info_.svideo = true, info_.lumaChannel = info_.tagLuma, info_.chromaChannel = info_.tagChroma;
    } else {
        info_.svideo = false, info_.lumaChannel = 1;
    }
}

bool Recording::decodeOne(std::vector<uint8_t>& samples, int64_t& index) {
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_);
    AVCodecContext* dec = static_cast<AVCodecContext*>(dec_);
    AVPacket* pkt = static_cast<AVPacket*>(pkt_);
    AVFrame* frame = static_cast<AVFrame*>(frame_);
    for (;;) {
        int rc = avcodec_receive_frame(dec, frame);
        if (rc == 0) {
            int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            timeSec_ = double(pts) * tb_ - fileStartSec_;
            if (counterFromPts_) {
                counter_ = int64_t(std::lround(double(pts - startPts_) * tb_ * info_.fps));
                counterFromPts_ = false;
            }
            index = counter_++;
            const size_t plane = size_t(info_.width) * info_.height;
            if (info_.rgb) {
                // Planar G, B, R (lossless), then the plane(s) the decoder needs.
                planar_.resize(plane * 3);
                uint8_t* dst[4] = {planar_.data(), planar_.data() + plane, planar_.data() + 2 * plane, nullptr};
                int dstStride[4] = {info_.width, info_.width, info_.width, 0};
                sws_scale(static_cast<SwsContext*>(sws_), frame->data, frame->linesize, 0, info_.height, dst, dstStride);
                auto planeOf = [&](int ch) { return planar_.data() + plane * size_t(ch == 1 ? 0 : ch == 2 ? 1 : 2); };  // G,B,R
                samples.resize(plane * (info_.svideo ? 2 : 1));
                std::memcpy(samples.data(), planeOf(info_.lumaChannel), plane);
                if (info_.svideo) std::memcpy(samples.data() + plane, planeOf(info_.chromaChannel), plane);
            } else {
                samples.resize(plane);
                for (int r = 0; r < info_.height; ++r)
                    std::memcpy(&samples[size_t(r) * info_.width], frame->data[0] + size_t(r) * frame->linesize[0],
                                size_t(info_.width));
            }
            av_frame_unref(frame);
            return true;
        }
        if (rc == AVERROR_EOF) return false;
        if (eof_) {
            avcodec_send_packet(dec, nullptr);
            continue;
        }
        rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            eof_ = true;
            continue;
        }
        if (pkt->stream_index == vIdx_) avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
    }
}

bool Recording::next(std::vector<uint8_t>& samples, int64_t& index) {
    if (!dec_) return false;
    for (;;) {
        if (!decodeOne(samples, index)) return false;
        if (skipBefore_ < 0 || index >= skipBefore_) {
            skipBefore_ = -1;
            return true;
        }
    }
}

bool Recording::seek(int64_t index) {
    if (!fmt_) return false;
    AVFormatContext* fmt = static_cast<AVFormatContext*>(fmt_);
    index = std::max<int64_t>(0, std::min(index, std::max<int64_t>(0, info_.frames - 1)));
    int64_t ts = startPts_ + int64_t(std::llround(double(index) / info_.fps / tb_));
    if (av_seek_frame(fmt, vIdx_, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(static_cast<AVCodecContext*>(dec_));
    eof_ = false;
    skipBefore_ = index;
    counterFromPts_ = true;
    return true;
}

}  // namespace fc

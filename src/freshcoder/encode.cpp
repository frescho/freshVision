// SPDX-License-Identifier: GPL-3.0-or-later
#include "encode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include "recording.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace fc {

namespace {

// What each encoder takes and how its quality is set. Lower quality numbers
// are better for all of them.
struct CodecSpec {
    const char* id;
    const char* label;
    const char* qualityName;
    int qMin, qMax, qDefault;
    std::vector<std::string> presets;
    const char* presetDefault;
    AVPixelFormat fmt444, fmt420;  // AV_PIX_FMT_NONE: not offered
    bool lossless;
};

const std::vector<CodecSpec>& specs() {
    static const std::vector<std::string> x26x = {"ultrafast", "superfast", "veryfast", "faster", "fast",
                                                  "medium",    "slow",      "slower",   "veryslow"};
    static const std::vector<std::string> nv = {"p1", "p2", "p3", "p4", "p5", "p6", "p7"};
    static const std::vector<std::string> svt = {"2", "4", "6", "8", "10", "12"};
    static const std::vector<CodecSpec> s = {
        {"libx264", "H.264 (x264)", "CRF", 0, 51, 14, x26x, "slow", AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P, false},
        {"libx265", "H.265 / HEVC (x265)", "CRF", 0, 51, 16, x26x, "slow", AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P, false},
        {"h264_nvenc", "H.264 (NVENC, graphics card)", "CQ", 1, 51, 18, nv, "p6", AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P, false},
        {"hevc_nvenc", "H.265 / HEVC (NVENC, graphics card)", "CQ", 1, 51, 18, nv, "p6", AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P, false},
        {"libsvtav1", "AV1 (SVT-AV1)", "CRF", 1, 63, 20, svt, "6", AV_PIX_FMT_NONE, AV_PIX_FMT_YUV420P, false},
        {"prores_ks", "ProRes 4444", "q", 1, 32, 4, {}, "", AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_NONE, false},
        {"ffv1", "FFV1 (lossless RGB)", "", 0, 0, 0, {}, "", AV_PIX_FMT_BGR0, AV_PIX_FMT_NONE, true},
    };
    return s;
}

const CodecSpec* specFor(const std::string& id) {
    for (const auto& s : specs())
        if (id == s.id) return &s;
    return nullptr;
}

// Sets up `ctx` for `spec` with the chosen quality/preset; returns false if
// the encoder rejects it.
void applyQuality(AVCodecContext* ctx, const CodecSpec& spec, int quality, const std::string& preset) {
    const std::string id = spec.id;
    auto setOpt = [&](const char* k, const std::string& v) { av_opt_set(ctx->priv_data, k, v.c_str(), 0); };
    if (id == "libx264" || id == "libx265" || id == "libsvtav1") {
        setOpt("crf", std::to_string(quality));
        if (!preset.empty()) setOpt("preset", preset);
        if (id == "libx265") setOpt("x265-params", "log-level=error");
    } else if (id == "h264_nvenc" || id == "hevc_nvenc") {
        setOpt("rc", "vbr");
        setOpt("cq", std::to_string(quality));
        if (!preset.empty()) setOpt("preset", preset);
        setOpt("tune", "hq");
        ctx->bit_rate = 0;
    } else if (id == "prores_ks") {
        setOpt("profile", "4444");
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ctx->global_quality = quality * FF_QP2LAMBDA;
    } else if (id == "ffv1") {
        ctx->level = 3;
        av_opt_set_int(ctx->priv_data, "slices", 12, 0);
    }
}

// The formats an encoder takes; FFmpeg 7.1 (libavcodec 61.13) replaced the
// AVCodec fields with avcodec_get_supported_config(). Null = any.
const AVPixelFormat* supportedPixelFormats(const AVCodec* c) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* out = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &out, &n) < 0) return nullptr;
    return static_cast<const AVPixelFormat*>(out);
#else
    return c->pix_fmts;
#endif
}
const AVSampleFormat* supportedSampleFormats(const AVCodec* c) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* out = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &out, &n) < 0) return nullptr;
    return static_cast<const AVSampleFormat*>(out);
#else
    return c->sample_fmts;
#endif
}

bool probe(const CodecSpec& spec, AVPixelFormat fmt) {
    if (fmt == AV_PIX_FMT_NONE) return false;
    const AVCodec* c = avcodec_find_encoder_by_name(spec.id);
    if (!c) return false;
    // Software encoders are there if FFmpeg has them and takes the format; only
    // hardware encoders need a test open (is the GPU/driver there?). Opening
    // and closing software encoders here hung in a Debian 12 container (276
    // threads waiting on each other, FFmpeg 5.1).
    const std::string id = spec.id;
    const bool hardware = id.find("nvenc") != std::string::npos || id.find("vaapi") != std::string::npos ||
                          id.find("qsv") != std::string::npos || id.find("amf") != std::string::npos;
    if (!hardware) {
        const AVPixelFormat* fmts = supportedPixelFormats(c);
        if (!fmts) return true;
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == fmt) return true;
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(c);
    ctx->width = 256;
    ctx->height = 256;
    ctx->pix_fmt = fmt;
    ctx->time_base = AVRational{1, 50};
    ctx->framerate = AVRational{50, 1};
    applyQuality(ctx, spec, spec.qDefault, spec.presetDefault);
    av_log_set_level(AV_LOG_QUIET);
    bool ok = avcodec_open2(ctx, c, nullptr) >= 0;
    av_log_set_level(AV_LOG_ERROR);
    avcodec_free_context(&ctx);
    return ok;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

}  // namespace

const std::vector<VideoCodec>& availableCodecs() {
    static const std::vector<VideoCodec> list = [] {
        setenv("SVT_LOG", "1", 0);  // SVT-AV1: errors only, no banner on every open
        std::vector<VideoCodec> out;
        for (const auto& s : specs()) {
            VideoCodec v;
            v.id = s.id;
            v.label = s.label;
            v.qualityName = s.qualityName;
            v.qualityMin = s.qMin, v.qualityMax = s.qMax, v.qualityDefault = s.qDefault;
            v.presets = s.presets;
            v.presetDefault = s.presetDefault;
            v.lossless = s.lossless;
            v.can444 = probe(s, s.fmt444);
            v.can420 = probe(s, s.fmt420);
            if (v.can444 || v.can420) out.push_back(v);
        }
        return out;
    }();
    return list;
}

const VideoCodec* findCodec(const std::string& id) {
    for (const auto& c : availableCodecs())
        if (c.id == id) return &c;
    return nullptr;
}

bool transcode(const std::string& in, const EncodeSettings& st, const fv::PalDecodeUi& ui, const OutputGeometry& geometry,
               EncodeProgress& progress, std::string& err, const pal::GridEstimate* grid,
               const std::vector<pal::Colour>* palette, const pal::Calibration* calibration) {
    progress.ok = false;
    progress.finished = false;
    struct Done {
        EncodeProgress& p;
        ~Done() { p.finished = true; }
    } done{progress};

    const VideoCodec* vc = findCodec(st.codec);
    const CodecSpec* spec = specFor(st.codec);
    if (!vc || !spec) {
        err = "encoder " + st.codec + " is not available here";
        return false;
    }
    const bool use444 = vc->can444 && (st.chroma444 || !vc->can420);
    const AVPixelFormat encFmt = use444 ? spec->fmt444 : spec->fmt420;
    const bool even = encFmt == AV_PIX_FMT_YUV420P;

    Recording rec;
    if (!rec.open(in, err)) return false;
    applyUiChoices(rec, ui);
    const RecordingInfo info = rec.info();
    if (!info.samplesPerCycle) {
        err = "the recording's Horizontal Size (" + std::to_string(info.horizontalSize) +
              ") is not a multiple of the " + std::string(pal::standardName(info.standard)) + " subcarrier";
        return false;
    }
    progress.framesTotal = info.frames;

    // Pass 1: the PLL's carrier track over the whole recording.
    PllTrack track;
    const PllTrack* trackPtr = nullptr;
    if (ui.pll) {
        progress.pass = 1;
        progress.framesDone = 0;
        if (!measurePll(in, ui, track, progress.framesDone, progress.cancel, err)) {
            if (progress.cancel) err = "cancelled";
            return false;
        }
        track.smooth(ui.pllBandwidth);
        trackPtr = &track;
    }
    progress.pass = 2;
    progress.framesDone = 0;

    Processor proc;
    const int threads = std::max(1, int(std::thread::hardware_concurrency() / 2));
    proc.setCellCandidates(16);  // offline: try every colour per C64 cell
    proc.configure(info, ui, geometry, threads, even);
    if (ui.pixelGrid) {
        // One grid (and palette) for the whole video, so its size never
        // changes: the caller's, or the first one the recording yields.
        pal::GridEstimate g;
        std::vector<pal::Colour> pl;
        pal::Calibration cal;
        if (grid && grid->valid) {
            g = *grid;
            pl = palette ? *palette : std::vector<pal::Colour>{};
            if (calibration) cal = *calibration;
        } else if (!findGrid(in, ui, g, pl, cal)) {
            err = "no pixel grid found in the first 500 frames (try another machine profile, or switch the grid off)";
            return false;
        }
        if (ui.snap && pl.empty()) {
            // A grid from the preview without a palette yet: from the recording.
            pal::GridEstimate g2;
            findGrid(in, ui, g2, pl, cal);
        }
        proc.fixGrid(g, pl, cal);
        std::fprintf(stderr, "pixel grid: %.4f samples per pixel, phase %.2f; %zu palette colours; calibration: "
                             "saturation x%.2f, hue %+.1f deg, contrast x%.2f, offset %+.0f\n",
                     g.period, g.phase, pl.size(), cal.satGain, cal.hueRad * 180 / pal::kPi, cal.contrast, cal.offset);
    }

    // Output container and video stream. The size comes from the first frame.
    std::vector<uint8_t> samples, rgb;
    int64_t index = 0;
    if (ui.removeStripes) {
        // Learn the fixed clock stripes from the first 10 s before encoding,
        // so the first frames get them removed too (learning goes on after).
        Recording pre;
        std::string e2;
        if (pre.open(in, e2)) {
            applyUiChoices(pre, ui);
            for (int n = 0; n < 500 && !progress.cancel && pre.next(samples, index); ++n) proc.learnStripes(samples.data());
            std::fprintf(stderr, "clock stripes: %.2f codes rms, %.0f%% of the line known (from %ld flat segments)\n",
                         proc.stripes().rms(), proc.stripes().coverage() * 100, proc.stripes().segments());
        }
    }
    if (!rec.next(samples, index) || !proc.process(samples.data(), index, trackPtr, rgb)) {
        err = "cannot read or decode the first frame";
        return false;
    }
    const OutputSize os = proc.size();

    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, nullptr, st.outPath.c_str()) < 0 || !out)
        avformat_alloc_output_context2(&out, nullptr, "matroska", st.outPath.c_str());
    if (!out) {
        err = "cannot create the output file format";
        return false;
    }
    struct OutGuard {
        AVFormatContext*& f;
        ~OutGuard() {
            if (f) {
                if (f->pb && !(f->oformat->flags & AVFMT_NOFILE)) avio_closep(&f->pb);
                avformat_free_context(f);
            }
        }
    } outGuard{out};
    const std::string ofmt = lower(out->oformat->name);
    const bool isMkv = ofmt.find("matroska") != std::string::npos;

    const AVCodec* venc = avcodec_find_encoder_by_name(spec->id);
    AVCodecContext* vctx = avcodec_alloc_context3(venc);
    struct CtxGuard {
        AVCodecContext*& c;
        ~CtxGuard() { avcodec_free_context(&c); }
    } vGuard{vctx};
    const int fpsInt = int(std::lround(info.fps));
    vctx->width = os.canvasW;
    vctx->height = os.canvasH;
    vctx->pix_fmt = encFmt;
    vctx->time_base = AVRational{1, 1000};  // each frame keeps its capture timestamp (ms)
    vctx->framerate = AVRational{fpsInt, 1};
    vctx->gop_size = fpsInt * 2;
    vctx->max_b_frames = (st.codec == "libx264" || st.codec == "libx265") ? 3 : 0;
    vctx->thread_count = 0;
    if (encFmt != AV_PIX_FMT_BGR0) {
        vctx->colorspace = AVCOL_SPC_BT709;
        vctx->color_primaries = AVCOL_PRI_BT709;
        vctx->color_trc = AVCOL_TRC_BT709;
        vctx->color_range = AVCOL_RANGE_MPEG;
    }
    applyQuality(vctx, *spec, vc->lossless ? 0 : std::clamp(st.quality, vc->qualityMin, vc->qualityMax), st.preset);
    if (out->oformat->flags & AVFMT_GLOBALHEADER) vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(vctx, venc, nullptr) < 0) {
        err = "the video encoder " + st.codec + " did not open with these settings";
        return false;
    }
    AVStream* vst = avformat_new_stream(out, nullptr);
    avcodec_parameters_from_context(vst->codecpar, vctx);
    vst->time_base = vctx->time_base;

    // Audio: copied PCM, or decoded and re-encoded.
    AVFormatContext* ain = nullptr;
    struct InGuard {
        AVFormatContext*& f;
        ~InGuard() {
            if (f) avformat_close_input(&f);
        }
    } ainGuard{ain};
    int aIn = -1;
    AVStream* ast = nullptr;
    AVCodecContext* adec = nullptr;
    AVCodecContext* aenc = nullptr;
    struct ACtxGuard {
        AVCodecContext*& a;
        AVCodecContext*& b;
        ~ACtxGuard() {
            avcodec_free_context(&a);
            avcodec_free_context(&b);
        }
    } aGuard{adec, aenc};
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    // Auto: lossless sound where the container allows it. MKV: PCM next to
    // lossless video, FLAC otherwise; MOV: PCM; MP4: AAC (MP4 takes no PCM).
    const bool isMov = ofmt.find("mov") != std::string::npos && ofmt.find("mp4") == std::string::npos;
    std::string audioMode = st.audio;
    if (audioMode == "auto")
        audioMode = isMkv ? ((vc->lossless || st.codec == "prores_ks") ? "pcm" : "flac") : isMov ? "pcm" : "aac";
    if (audioMode == "pcm" && !isMkv && !isMov) audioMode = "aac";
    if (info.hasAudio && audioMode != "none") {
        if (avformat_open_input(&ain, in.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(ain, nullptr) < 0) {
            err = "cannot open the audio of " + in;
            return false;
        }
        aIn = av_find_best_stream(ain, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
    if (aIn >= 0) {
        AVStream* ais = ain->streams[aIn];
        ast = avformat_new_stream(out, nullptr);
        if (audioMode == "pcm") {
            avcodec_parameters_copy(ast->codecpar, ais->codecpar);
            ast->codecpar->codec_tag = 0;
            ast->time_base = AVRational{1, ais->codecpar->sample_rate};
        } else {
            const AVCodec* dc = avcodec_find_decoder(ais->codecpar->codec_id);
            adec = avcodec_alloc_context3(dc);
            avcodec_parameters_to_context(adec, ais->codecpar);
            avcodec_open2(adec, dc, nullptr);
            const AVCodec* ec = audioMode == "aac"    ? avcodec_find_encoder(AV_CODEC_ID_AAC)
                                : audioMode == "opus" ? avcodec_find_encoder_by_name("libopus")
                                                      : avcodec_find_encoder(AV_CODEC_ID_FLAC);
            if (!ec) {
                err = "no " + audioMode + " audio encoder";
                return false;
            }
            aenc = avcodec_alloc_context3(ec);
            aenc->sample_rate = adec->sample_rate;
            // A recording's layout can be "unspecified"; encoders like AAC
            // want a standard one for the channel count.
            if (adec->ch_layout.order != AV_CHANNEL_ORDER_NATIVE) {
                const int n = adec->ch_layout.nb_channels;
                av_channel_layout_uninit(&adec->ch_layout);
                av_channel_layout_default(&adec->ch_layout, n);
            }
            av_channel_layout_copy(&aenc->ch_layout, &adec->ch_layout);
            const AVSampleFormat* sfmts = supportedSampleFormats(ec);
            aenc->sample_fmt = sfmts ? sfmts[0] : AV_SAMPLE_FMT_S16;
            aenc->time_base = AVRational{1, aenc->sample_rate};
            if (audioMode != "flac") aenc->bit_rate = int64_t(st.audioKbps) * 1000;
            if (out->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(aenc, ec, nullptr) < 0) {
                err = "the " + audioMode + " audio encoder did not open";
                return false;
            }
            avcodec_parameters_from_context(ast->codecpar, aenc);
            ast->time_base = aenc->time_base;
            swr_alloc_set_opts2(&swr, &aenc->ch_layout, aenc->sample_fmt, aenc->sample_rate, &adec->ch_layout,
                                adec->sample_fmt, adec->sample_rate, 0, nullptr);
            swr_init(swr);
            fifo = av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, 4096);
        }
    }
    struct AudioGuard {
        SwrContext*& s;
        AVAudioFifo*& f;
        ~AudioGuard() {
            swr_free(&s);
            if (f) av_audio_fifo_free(f);
        }
    } audioGuard{swr, fifo};

    av_dict_set(&out->metadata, "ENCODER_APP", "freshCoder", 0);
    for (const auto& [k, v] : info.tags)
        if (k.rfind("CTRL_", 0) == 0 || k.rfind("SOURCE_", 0) == 0 || k.rfind("CAPTURE_", 0) == 0)
            av_dict_set(&out->metadata, ("RAW_" + k).c_str(), v.c_str(), 0);
    if (!(out->oformat->flags & AVFMT_NOFILE) && avio_open(&out->pb, st.outPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        err = "cannot write " + st.outPath;
        return false;
    }
    if (avformat_write_header(out, nullptr) < 0) {
        err = "cannot write the file header (container/codec combination?)";
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* vf = av_frame_alloc();
    AVFrame* af = av_frame_alloc();
    AVFrame* ef = av_frame_alloc();
    struct FrameGuard {
        AVPacket*& p;
        AVFrame*& a;
        AVFrame*& b;
        AVFrame*& c;
        ~FrameGuard() {
            av_packet_free(&p);
            av_frame_free(&a);
            av_frame_free(&b);
            av_frame_free(&c);
        }
    } fGuard{pkt, vf, af, ef};
    vf->format = encFmt;
    vf->width = os.canvasW;
    vf->height = os.canvasH;
    av_frame_get_buffer(vf, 0);
    SwsContext* sws = sws_getContext(os.canvasW, os.canvasH, AV_PIX_FMT_RGB24, os.canvasW, os.canvasH, encFmt,
                                     SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    if (encFmt != AV_PIX_FMT_BGR0) {
        const int* coef709 = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_DEFAULT), 1, coef709, 0, 0, 1 << 16, 1 << 16);
    }
    struct SwsGuard {
        SwsContext*& s;
        ~SwsGuard() { sws_freeContext(s); }
    } swsGuard{sws};

    auto writePackets = [&](AVCodecContext* ctx, AVStream* stream) {
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            pkt->stream_index = stream->index;
            av_packet_rescale_ts(pkt, ctx->time_base, stream->time_base);
            av_interleaved_write_frame(out, pkt);
        }
    };
    // Audio up to `untilSec` (seconds from the file start); all of it when < 0.
    const double fileStart = ain && ain->start_time != AV_NOPTS_VALUE ? double(ain->start_time) / AV_TIME_BASE : 0.0;
    int64_t audioNextPts = -1;  // encoder time base (samples)
    bool audioEof = false;
    auto pumpAudio = [&](double untilSec) {
        if (aIn < 0) return;
        while (!audioEof) {
            // Look at the next packet's time before taking it.
            if (av_read_frame(ain, pkt) < 0) {
                audioEof = true;
                break;
            }
            if (pkt->stream_index != aIn) {
                av_packet_unref(pkt);
                continue;
            }
            AVStream* ais = ain->streams[aIn];
            const double t = double(pkt->pts) * av_q2d(ais->time_base) - fileStart;
            if (audioMode == "pcm") {
                pkt->stream_index = ast->index;
                av_packet_rescale_ts(pkt, ais->time_base, ast->time_base);
                av_interleaved_write_frame(out, pkt);
            } else {
                if (audioNextPts < 0) audioNextPts = int64_t(std::llround(std::max(0.0, t) * aenc->sample_rate));
                avcodec_send_packet(adec, pkt);
                av_packet_unref(pkt);
                while (avcodec_receive_frame(adec, af) == 0) {
                    uint8_t** conv = nullptr;
                    int n = swr_get_out_samples(swr, af->nb_samples);
                    av_samples_alloc_array_and_samples(&conv, nullptr, aenc->ch_layout.nb_channels, n, aenc->sample_fmt, 0);
                    int got = swr_convert(swr, conv, n, (const uint8_t**)af->extended_data, af->nb_samples);
                    if (got > 0) av_audio_fifo_write(fifo, (void**)conv, got);
                    av_freep(&conv[0]);
                    av_freep(&conv);
                    av_frame_unref(af);
                    const int fs = aenc->frame_size > 0 ? aenc->frame_size : 4096;
                    while (av_audio_fifo_size(fifo) >= fs) {
                        ef->nb_samples = fs;
                        ef->format = aenc->sample_fmt;
                        av_channel_layout_copy(&ef->ch_layout, &aenc->ch_layout);
                        ef->sample_rate = aenc->sample_rate;
                        av_frame_get_buffer(ef, 0);
                        av_audio_fifo_read(fifo, (void**)ef->data, fs);
                        ef->pts = audioNextPts;
                        audioNextPts += fs;
                        avcodec_send_frame(aenc, ef);
                        av_frame_unref(ef);
                        writePackets(aenc, ast);
                    }
                }
            }
            av_packet_unref(pkt);
            if (untilSec >= 0 && t > untilSec) break;
        }
    };

    // Pass 2: every frame.
    int64_t lastPts = -1;
    for (;;) {
        if (progress.cancel) {
            err = "cancelled";
            break;
        }
        if (av_frame_make_writable(vf) < 0) break;
        const uint8_t* src[1] = {rgb.data()};
        int srcStride[1] = {os.canvasW * 3};
        sws_scale(sws, src, srcStride, 0, os.canvasH, vf->data, vf->linesize);
        // The frame's own capture time, so video and sound stay together over
        // any length (the source runs at ~50.12 fps, not exactly 50).
        int64_t pts = int64_t(std::llround(rec.timeSec() * 1000.0));
        if (pts <= lastPts) pts = lastPts + 1;
        lastPts = pts;
        vf->pts = pts;
        avcodec_send_frame(vctx, vf);
        writePackets(vctx, vst);
        progress.framesDone = index + 1;
        pumpAudio(rec.timeSec());
        if (!rec.next(samples, index)) break;
        if (!proc.process(samples.data(), index, trackPtr, rgb)) break;
        if (proc.size().canvasW != os.canvasW || proc.size().canvasH != os.canvasH) {
            err = "the picture size changed in the middle of the recording";
            break;
        }
    }
    const bool cancelled = progress.cancel;
    avcodec_send_frame(vctx, nullptr);
    writePackets(vctx, vst);
    if (!cancelled) pumpAudio(-1);
    if (aenc) {
        // The last, shorter audio frame, then the encoder's own tail.
        int left = av_audio_fifo_size(fifo);
        if (left > 0) {
            ef->nb_samples = left;
            ef->format = aenc->sample_fmt;
            av_channel_layout_copy(&ef->ch_layout, &aenc->ch_layout);
            ef->sample_rate = aenc->sample_rate;
            av_frame_get_buffer(ef, 0);
            av_audio_fifo_read(fifo, (void**)ef->data, left);
            ef->pts = audioNextPts;
            avcodec_send_frame(aenc, ef);
            av_frame_unref(ef);
        }
        avcodec_send_frame(aenc, nullptr);
        writePackets(aenc, ast);
    }
    av_write_trailer(out);
    if (!err.empty()) {
        // No half-written files left behind after a cancel or an error.
        std::error_code ec;
        std::filesystem::remove(st.outPath, ec);
        return false;
    }
    progress.ok = true;
    return true;
}

}  // namespace fc

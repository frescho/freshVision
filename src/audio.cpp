// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace fv {

namespace {
int64_t nowMonoNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Ticks (20 ms each) of silence queued in front of the monitor device; see
// AudioMixer::startMonitor.
constexpr size_t kMonitorPrefillTicks = 3;

void writeSilence(snd_pcm_t* pcm, size_t frames, size_t chunkFrames, int channels) {
    std::vector<int16_t> zeros(chunkFrames * size_t(channels), 0);
    while (frames > 0) {
        size_t n = std::min(frames, chunkFrames);
        if (snd_pcm_writei(pcm, zeros.data(), n) < 0) return;
        frames -= n;
    }
}

// -- Hardware mixer access (generic ALSA simple-mixer API) ------------------
// Finds, for a given "hw:CARD=X,DEV=Y" capture device, the ALSA simple-mixer
// elements that actually govern its capture level and (if any) its input
// source. There is no ALSA API for "which mixer control feeds PCM device Y" —
// that association is a driver/codec convention, not something libasound
// exposes directly. The heuristic used here: a card's own capture-capable
// simple-mixer elements (things like 'Capture',0 / 'Capture',1) appear in
// the same order as that card's own capture-capable PCM devices (ascending
// device number) — confirmed on this machine's Realtek ALC897 (PCM device 0
// -> 'Capture' index 0, PCM device 2 -> 'Capture' index 1). Not guaranteed
// universal (no generic guarantee exists), so every accessor below is
// best-effort and simply reports "not available" rather than ever guessing
// wrong and touching a different input's level.
struct HwCaptureMixer {
    snd_mixer_t* mixer = nullptr;
    snd_mixer_elem_t* volumeElem = nullptr;  // 'Capture'-like: has_capture_volume()
    snd_mixer_elem_t* sourceElem = nullptr;  // 'Input Source'-like: enumerated, name match
    long volMin = 0, volMax = 0;

    ~HwCaptureMixer() {
        if (mixer) snd_mixer_close(mixer);
    }
};

// Splits "hw:CARD=PCH,DEV=0" (or "hw:CARD=PCH,DEV=0,SUBDEV=0") into the card
// name and device number. Only the forms enumerateAudioDevices() itself
// produces need to be understood here.
bool parseHwCardDev(const std::string& id, std::string& card, int& dev) {
    auto cardPos = id.find("CARD=");
    if (cardPos == std::string::npos) return false;
    cardPos += 5;
    auto cardEnd = id.find(',', cardPos);
    card = id.substr(cardPos, cardEnd == std::string::npos ? std::string::npos : cardEnd - cardPos);
    auto devPos = id.find("DEV=");
    if (devPos == std::string::npos) return false;
    devPos += 4;
    dev = std::atoi(id.c_str() + devPos);
    return !card.empty();
}

// This device's ordinal (0-based) among its card's own capture-capable PCM
// devices, in ascending device-number order — see HwCaptureMixer's comment.
// -1 if it can't be determined (card open failure, device not found, ...).
int captureOrdinal(const std::string& card, int targetDev) {
    snd_ctl_t* ctl = nullptr;
    std::string ctlName = "hw:" + card;
    if (snd_ctl_open(&ctl, ctlName.c_str(), 0) < 0) return -1;
    std::vector<int> captureDevs;
    int devNum = -1;
    while (snd_ctl_pcm_next_device(ctl, &devNum) == 0 && devNum >= 0) {
        snd_pcm_info_t* info;
        snd_pcm_info_alloca(&info);
        snd_pcm_info_set_device(info, devNum);
        snd_pcm_info_set_stream(info, SND_PCM_STREAM_CAPTURE);
        if (snd_ctl_pcm_info(ctl, info) == 0) captureDevs.push_back(devNum);
    }
    snd_ctl_close(ctl);
    std::sort(captureDevs.begin(), captureDevs.end());
    for (size_t i = 0; i < captureDevs.size(); ++i)
        if (captureDevs[i] == targetDev) return int(i);
    return -1;
}

// Opens a mixer handle on `card` and picks out the `ordinal`-th
// capture-volume element and, separately, the `ordinal`-th enumerated
// element whose name suggests it selects an input source ("Input Source" is
// the standard ALSA/HDA convention name for this, not Datapath- or even
// codec-specific). Returns nullptr if the card/mixer can't be opened at all;
// individual fields inside may still be null if that specific piece wasn't
// found, which callers must check.
std::unique_ptr<HwCaptureMixer> openHwCaptureMixer(const std::string& card, int ordinal) {
    if (ordinal < 0) return nullptr;
    auto hm = std::make_unique<HwCaptureMixer>();
    std::string ctlName = "hw:" + card;
    if (snd_mixer_open(&hm->mixer, 0) < 0) return nullptr;
    if (snd_mixer_attach(hm->mixer, ctlName.c_str()) < 0 ||
        snd_mixer_selem_register(hm->mixer, nullptr, nullptr) < 0 || snd_mixer_load(hm->mixer) < 0) {
        snd_mixer_close(hm->mixer);
        hm->mixer = nullptr;
        return nullptr;
    }
    int volSeen = -1, srcSeen = -1;
    for (snd_mixer_elem_t* e = snd_mixer_first_elem(hm->mixer); e; e = snd_mixer_elem_next(e)) {
        const char* name = snd_mixer_selem_get_name(e);
        // Must match the *main* capture level control by name ("Capture" is
        // the standard ALSA/HDA convention), not just any capture-volume-
        // capable element: codecs like this one also expose separate boost
        // pre-amp stages ("Front Mic Boost", "Line Boost", ...) that also
        // report has_capture_volume() true but are a different control
        // entirely (matching on capability alone picked
        // a boost control, stuck at 0, instead of the real level — hardware
        // volume always read 0 regardless of the actual level).
        if (snd_mixer_selem_has_capture_volume(e) && name && std::strcmp(name, "Capture") == 0) {
            ++volSeen;
            if (volSeen == ordinal) hm->volumeElem = e;
        }
        if (snd_mixer_selem_is_enumerated(e) && name && std::strstr(name, "Input Source")) {
            ++srcSeen;
            if (srcSeen == ordinal) hm->sourceElem = e;
        }
    }
    if (hm->volumeElem) snd_mixer_selem_get_capture_volume_range(hm->volumeElem, &hm->volMin, &hm->volMax);
    return hm;
}

// -- Hardware mixer access for the monitor's playback device ----------------
// Unlike capture (each PCM device's own level control has to be found by the
// ordinal heuristic above), playback is simpler: ALSA/HDA convention puts one
// card-wide volume control named "Master" (falling back to "PCM" if a card
// has no "Master") ahead of any per-device routing, so this doesn't need
// AudioCapture's per-device ordinal matching — one mixer element per card is
// enough. Deliberately scoped to real hw: device overrides only:
// "default" is PipeWire/PulseAudio's own mix target, which
// has no single hardware level to control here and stays software-only
// (RoutedSource::gain, as before).
struct HwPlaybackMixer {
    snd_mixer_t* mixer = nullptr;
    snd_mixer_elem_t* volumeElem = nullptr;
    long volMin = 0, volMax = 0;

    ~HwPlaybackMixer() {
        if (mixer) snd_mixer_close(mixer);
    }
};

// Extracts the card identifier snd_mixer_attach("hw:" + card) needs from a
// playback device id as enumeratePlaybackDevices() produces it -- either
// "hw:CARD=NAME,DEV=N" or the plain-numeric "hw:N,M" form ALSA also hands
// out, depending on the card/driver.
bool parseHwCard(const std::string& id, std::string& card) {
    if (id.rfind("hw:", 0) != 0) return false;
    std::string rest = id.substr(3);
    auto cardPos = rest.find("CARD=");
    if (cardPos != std::string::npos) {
        cardPos += 5;
        auto end = rest.find(',', cardPos);
        card = rest.substr(cardPos, end == std::string::npos ? std::string::npos : end - cardPos);
        return !card.empty();
    }
    auto comma = rest.find(',');
    card = comma == std::string::npos ? rest : rest.substr(0, comma);
    return !card.empty();
}

std::unique_ptr<HwPlaybackMixer> openHwPlaybackMixer(const std::string& playbackId) {
    std::string card;
    if (!parseHwCard(playbackId, card)) return nullptr;
    auto hm = std::make_unique<HwPlaybackMixer>();
    std::string ctlName = "hw:" + card;
    if (snd_mixer_open(&hm->mixer, 0) < 0) return nullptr;
    if (snd_mixer_attach(hm->mixer, ctlName.c_str()) < 0 ||
        snd_mixer_selem_register(hm->mixer, nullptr, nullptr) < 0 || snd_mixer_load(hm->mixer) < 0) {
        snd_mixer_close(hm->mixer);
        hm->mixer = nullptr;
        return nullptr;
    }
    snd_mixer_elem_t* fallback = nullptr;
    for (snd_mixer_elem_t* e = snd_mixer_first_elem(hm->mixer); e; e = snd_mixer_elem_next(e)) {
        if (!snd_mixer_selem_has_playback_volume(e)) continue;
        const char* name = snd_mixer_selem_get_name(e);
        if (name && std::strcmp(name, "Master") == 0) {
            hm->volumeElem = e;
            break;
        }
        if (!fallback) fallback = e;  // e.g. "PCM" on cards with no "Master"
    }
    if (!hm->volumeElem) hm->volumeElem = fallback;
    if (!hm->volumeElem) return nullptr;  // nothing usable found: report unavailable, don't guess
    snd_mixer_selem_get_playback_volume_range(hm->volumeElem, &hm->volMin, &hm->volMax);
    return hm;
}
}  // namespace

const char* channelModeName(ChannelMode m) {
    switch (m) {
        case ChannelMode::Left: return "Left";
        case ChannelMode::Right: return "Right";
        case ChannelMode::Stereo: return "Stereo";
        case ChannelMode::SeparateLR: return "Separate L/R";
    }
    return "?";
}

const char* outputRouteName(OutputRoute r) {
    switch (r) {
        case OutputRoute::Left: return "Left";
        case OutputRoute::Right: return "Right";
        case OutputRoute::Stereo: return "Stereo";
    }
    return "?";
}

namespace {
// Shared by enumerateAudioDevices()/enumeratePlaybackDevices(): filters ALSA
// PCM hints to hw: names (never PipeWire/PulseAudio's "default"/"sysdefault"
// pseudo-devices — same "ask the system, never assume" spirit as elsewhere
// in this codebase) matching the wanted direction.
std::vector<AudioDeviceInfo> enumerateHwDevices(bool wantCapture) {
    std::vector<AudioDeviceInfo> out;
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return out;
    for (void** n = hints; *n != nullptr; ++n) {
        char* name = snd_device_name_get_hint(*n, "NAME");
        char* desc = snd_device_name_get_hint(*n, "DESC");
        char* ioid = snd_device_name_get_hint(*n, "IOID");  // "Input", "Output", or NULL (both)
        const char* wantIoid = wantCapture ? "Input" : "Output";
        bool wantIt = name && std::strncmp(name, "hw:", 3) == 0 && (!ioid || std::strcmp(ioid, wantIoid) == 0);
        if (wantIt) {
            AudioDeviceInfo info;
            info.id = name;
            info.name = desc ? desc : name;
            for (char& c : info.name)
                if (c == '\n') c = ' ';
            out.push_back(std::move(info));
        }
        if (name) free(name);
        if (desc) free(desc);
        if (ioid) free(ioid);
    }
    snd_device_name_free_hint(hints);
    std::sort(out.begin(), out.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) { return a.id < b.id; });
    return out;
}
}  // namespace

std::vector<AudioDeviceInfo> enumerateAudioDevices() { return enumerateHwDevices(true); }
std::vector<AudioDeviceInfo> enumeratePlaybackDevices() { return enumerateHwDevices(false); }

AudioCapture::~AudioCapture() { close(); }

bool AudioCapture::open(const std::string& alsaId, std::string& err) {
    close();
    snd_pcm_t* h = nullptr;
    int rc = snd_pcm_open(&h, alsaId.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        err = std::string("snd_pcm_open: ") + snd_strerror(rc);
        return false;
    }

    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(h, hw);
    snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if ((rc = snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S16_LE)) < 0) {
        err = std::string("S16_LE not supported: ") + snd_strerror(rc);
        snd_pcm_close(h);
        return false;
    }
    unsigned rate = 48000;
    snd_pcm_hw_params_set_rate_near(h, hw, &rate, nullptr);
    unsigned channels = 2;
    if (snd_pcm_hw_params_set_channels(h, hw, channels) < 0) {
        channels = 1;  // found at runtime, not assumed: fall back to whatever the device actually has
        if ((rc = snd_pcm_hw_params_set_channels(h, hw, channels)) < 0) {
            err = std::string("no usable channel count: ") + snd_strerror(rc);
            snd_pcm_close(h);
            return false;
        }
    }
    snd_pcm_uframes_t period = rate / 100;  // ~10 ms
    snd_pcm_hw_params_set_period_size_near(h, hw, &period, nullptr);
    if ((rc = snd_pcm_hw_params(h, hw)) < 0) {
        err = std::string("snd_pcm_hw_params: ") + snd_strerror(rc);
        snd_pcm_close(h);
        return false;
    }
    snd_pcm_hw_params_get_rate(hw, &rate, nullptr);
    snd_pcm_hw_params_get_channels(hw, &channels);
    if ((rc = snd_pcm_prepare(h)) < 0) {
        err = std::string("snd_pcm_prepare: ") + snd_strerror(rc);
        snd_pcm_close(h);
        return false;
    }

    handle_ = h;
    id_ = alsaId;
    rate_ = rate;
    devChannels_ = channels;
    framesRead_ = overruns_ = readErrors_ = 0;
    bufL_.clear();
    bufR_.clear();
    openHwMixer();

    stopRequested_ = false;
    workerDone_ = false;
    worker_ = std::thread(&AudioCapture::workerMain, this);
    return true;
}

void AudioCapture::openHwMixer() {
    std::string card;
    int dev = -1;
    if (!parseHwCardDev(id_, card, dev)) return;  // not a "hw:CARD=X,DEV=Y" id; leave hwMixer_ null
    int ordinal = captureOrdinal(card, dev);
    auto hm = openHwCaptureMixer(card, ordinal);
    hwMixer_ = hm.release();  // owned as void* per this header's own-pointer convention; freed in closeHwMixer()
}

void AudioCapture::closeHwMixer() {
    delete static_cast<HwCaptureMixer*>(hwMixer_);
    hwMixer_ = nullptr;
}

void AudioCapture::close() {
    if (worker_.joinable()) {
        stopRequested_ = true;
        worker_.join();
    }
    closeHwMixer();
    if (handle_) {
        snd_pcm_close(static_cast<snd_pcm_t*>(handle_));
        handle_ = nullptr;
    }
}

void AudioCapture::workerMain() {
    std::vector<int16_t> raw(size_t(devChannels_) * 1024);
    const size_t maxBuffered = size_t(rate_) * 2;  // cap ~2 s per logical channel

    while (!stopRequested_) {
        snd_pcm_sframes_t n = snd_pcm_readi(static_cast<snd_pcm_t*>(handle_), raw.data(), 1024);
        if (n == -EPIPE) {
            ++overruns_;
            snd_pcm_prepare(static_cast<snd_pcm_t*>(handle_));
            continue;
        }
        if (n < 0) {
            n = snd_pcm_recover(static_cast<snd_pcm_t*>(handle_), int(n), 1);
            if (n < 0) {
                ++readErrors_;
                continue;
            }
            continue;
        }
        if (n <= 0) continue;

        ChannelMode m = mode_.load();
        std::lock_guard<std::mutex> lk(mu_);
        for (snd_pcm_sframes_t i = 0; i < n; ++i) {
            int16_t l = raw[size_t(i) * devChannels_ + 0];
            int16_t r = devChannels_ >= 2 ? raw[size_t(i) * devChannels_ + 1] : l;
            switch (m) {
                case ChannelMode::Left: bufL_.push_back(l); break;
                case ChannelMode::Right: bufL_.push_back(r); break;
                case ChannelMode::Stereo: bufL_.push_back(int16_t((int(l) + int(r)) / 2)); break;
                case ChannelMode::SeparateLR:
                    bufL_.push_back(l);
                    bufR_.push_back(r);
                    break;
            }
        }
        while (bufL_.size() > maxBuffered) {
            bufL_.pop_front();
            ++overruns_;
        }
        while (bufR_.size() > maxBuffered) {
            bufR_.pop_front();
            ++overruns_;
        }
        framesRead_ += uint64_t(n);
    }
    workerDone_ = true;
}

void AudioCapture::flushQueued() {
    std::lock_guard<std::mutex> lk(mu_);
    bufL_.clear();
    bufR_.clear();
}

void AudioCapture::pull(int logicalIndex, int16_t* out, size_t frames) {
    std::lock_guard<std::mutex> lk(mu_);
    std::deque<int16_t>& buf = (logicalIndex == 0) ? bufL_ : bufR_;
    size_t avail = std::min(frames, buf.size());
    for (size_t i = 0; i < avail; ++i) {
        out[i] = buf.front();
        buf.pop_front();
    }
    if (avail < frames) {
        std::fill(out + avail, out + frames, int16_t(0));
        ++overruns_;  // the source could not keep up (or hasn't started yet): padded with silence
    }
}

AudioCounters AudioCapture::counters() const {
    AudioCounters c;
    c.framesRead = framesRead_;
    c.overruns = overruns_;
    c.readErrors = readErrors_;
    return c;
}

bool AudioCapture::hasHwControl() const {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    return hm && hm->volumeElem;
}

bool AudioCapture::hwCaptureMuted() const {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->volumeElem) return false;
    int l = 1, r = 1;  // ALSA convention: 1 = unmuted, 0 = muted, if the control has a switch at all
    if (snd_mixer_selem_has_capture_switch(hm->volumeElem)) {
        snd_mixer_selem_get_capture_switch(hm->volumeElem, SND_MIXER_SCHN_FRONT_LEFT, &l);
        snd_mixer_selem_get_capture_switch(hm->volumeElem, SND_MIXER_SCHN_FRONT_RIGHT, &r);
    }
    return l == 0 && r == 0;
}

void AudioCapture::setHwCaptureMuted(bool mute) {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->volumeElem || !snd_mixer_selem_has_capture_switch(hm->volumeElem)) return;
    snd_mixer_selem_set_capture_switch_all(hm->volumeElem, mute ? 0 : 1);
}

float AudioCapture::hwCaptureVolume() const {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->volumeElem || hm->volMax <= hm->volMin) return 0.0f;
    long l = hm->volMin, r = hm->volMin;
    snd_mixer_selem_get_capture_volume(hm->volumeElem, SND_MIXER_SCHN_FRONT_LEFT, &l);
    snd_mixer_selem_get_capture_volume(hm->volumeElem, SND_MIXER_SCHN_FRONT_RIGHT, &r);
    double avg = (double(l - hm->volMin) + double(r - hm->volMin)) / 2.0;
    return float(std::clamp(avg / double(hm->volMax - hm->volMin), 0.0, 1.0));
}

void AudioCapture::setHwCaptureVolume(float v0to1) {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->volumeElem || hm->volMax <= hm->volMin) return;
    long v = hm->volMin + std::lround(std::clamp(v0to1, 0.0f, 1.0f) * double(hm->volMax - hm->volMin));
    snd_mixer_selem_set_capture_volume_all(hm->volumeElem, v);
}

float AudioCapture::hwCaptureBalance() const {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->volumeElem || hm->volMax <= hm->volMin) return 0.0f;
    long l = hm->volMin, r = hm->volMin;
    snd_mixer_selem_get_capture_volume(hm->volumeElem, SND_MIXER_SCHN_FRONT_LEFT, &l);
    snd_mixer_selem_get_capture_volume(hm->volumeElem, SND_MIXER_SCHN_FRONT_RIGHT, &r);
    double range = double(hm->volMax - hm->volMin);
    if (range <= 0) return 0.0f;
    return float(std::clamp((double(r) - double(l)) / range, -1.0, 1.0));
}

void AudioCapture::setHwCaptureBalance(float balance) {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->volumeElem || hm->volMax <= hm->volMin) return;
    balance = std::clamp(balance, -1.0f, 1.0f);
    float overall = hwCaptureVolume();  // keep the current overall level, just redistribute L/R
    float l = std::clamp(overall * (1.0f - std::max(0.0f, balance)), 0.0f, 1.0f);
    float r = std::clamp(overall * (1.0f - std::max(0.0f, -balance)), 0.0f, 1.0f);
    long lv = hm->volMin + std::lround(l * double(hm->volMax - hm->volMin));
    long rv = hm->volMin + std::lround(r * double(hm->volMax - hm->volMin));
    snd_mixer_selem_set_capture_volume(hm->volumeElem, SND_MIXER_SCHN_FRONT_LEFT, lv);
    snd_mixer_selem_set_capture_volume(hm->volumeElem, SND_MIXER_SCHN_FRONT_RIGHT, rv);
}

std::vector<std::string> AudioCapture::hwInputSourceOptions() const {
    std::vector<std::string> out;
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->sourceElem) return out;
    int n = snd_mixer_selem_get_enum_items(hm->sourceElem);
    char buf[64];
    for (int i = 0; i < n; ++i) {
        if (snd_mixer_selem_get_enum_item_name(hm->sourceElem, i, sizeof buf, buf) == 0) out.push_back(buf);
    }
    return out;
}

std::string AudioCapture::hwInputSource() const {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->sourceElem) return {};
    unsigned idx = 0;
    if (snd_mixer_selem_get_enum_item(hm->sourceElem, SND_MIXER_SCHN_FRONT_LEFT, &idx) < 0) return {};
    char buf[64];
    if (snd_mixer_selem_get_enum_item_name(hm->sourceElem, idx, sizeof buf, buf) < 0) return {};
    return buf;
}

bool AudioCapture::setHwInputSource(const std::string& name) {
    auto* hm = static_cast<HwCaptureMixer*>(hwMixer_);
    if (!hm || !hm->sourceElem) return false;
    int n = snd_mixer_selem_get_enum_items(hm->sourceElem);
    char buf[64];
    for (int i = 0; i < n; ++i) {
        if (snd_mixer_selem_get_enum_item_name(hm->sourceElem, i, sizeof buf, buf) == 0 && name == buf)
            return snd_mixer_selem_set_enum_item(hm->sourceElem, SND_MIXER_SCHN_FRONT_LEFT, i) == 0;
    }
    return false;
}

AudioMixer::~AudioMixer() { stop(); }

bool AudioMixer::start(unsigned sampleRate, int outputChannels, std::string& err) {
    if (outputChannels < 1) {
        err = "outputChannels must be >= 1";
        return false;
    }
    stop();
    rate_ = sampleRate;
    outChannels_ = outputChannels;
    mixFrames_ = mixUnderruns_ = 0;
    stopRequested_ = false;
    running_ = true;
    worker_ = std::thread(&AudioMixer::mixWorkerMain, this);
    return true;
}

void AudioMixer::stop() {
    if (worker_.joinable()) {
        stopRequested_ = true;
        worker_.join();
    }
    {
        // Real teardown here, not stopMonitor() (which deliberately just
        // silences the stream, see its comment) — this is the mixer
        // actually going away, so the device must actually be released.
        std::lock_guard<std::mutex> lk(monitorMu_);
        closeMonitorHandleLocked();
    }
    stopRecording();  // patches the WAV header; without this a recording in progress would be left unreadable
    std::lock_guard<std::mutex> lk(srcMu_);
    sources_.clear();
    running_ = false;
}

bool AudioMixer::addOrUpdateSource(const RoutedSource& r, std::string& err) {
    {
        std::lock_guard<std::mutex> lk(srcMu_);
        for (auto& e : sources_) {
            if (e.route.deviceId == r.deviceId) {
                e.route = r;
                e.cap->setChannelMode(r.mode);
                return true;
            }
        }
    }
    // cap->open() (not yet holding srcMu_) does real ALSA/ioctl work —
    // negotiating the PCM, and opening a hardware-mixer
    // handle too. mixWorkerMain()'s real-time tick loop needs srcMu_ every
    // ~20ms; holding it for the whole open() call (as this used to do)
    // eats into that budget for no reason, since nothing else touches
    // `cap` until it's inserted below. Found while chasing a
    // "jerky audio on toggle" — see removeSource()'s comment for the much
    // larger version of the same mistake, which is the one that actually
    // mattered in practice.
    auto cap = std::make_unique<AudioCapture>();
    if (!cap->open(r.deviceId, err)) return false;
    cap->setChannelMode(r.mode);
    SourceEntry e;
    e.route = r;
    e.cap = std::move(cap);
    e.addedAtNs = nowMonoNs();
    std::lock_guard<std::mutex> lk(srcMu_);
    // Re-check: another call could have added this deviceId while open()
    // above ran unlocked (the GUI only ever calls this from one thread in
    // practice, so this is a correctness belt-and-braces, not a case this
    // codebase actually exercises concurrently).
    for (auto& existing : sources_) {
        if (existing.route.deviceId == r.deviceId) {
            existing.route = r;
            existing.cap->setChannelMode(r.mode);
            return true;  // e's AudioCapture (the one just opened) is discarded on return, closed by its destructor
        }
    }
    sources_.push_back(std::move(e));
    return true;
}

void AudioMixer::removeSource(const std::string& deviceId) {
    // The real fix for the "jerky audio on toggle": erasing a
    // SourceEntry here used to destroy its AudioCapture synchronously,
    // inside this function's own lock_guard — and ~AudioCapture() calls
    // close(), which does worker_.join(). That worker thread is very
    // likely blocked inside a *blocking* snd_pcm_readi() waiting for up to
    // one whole period (~21ms at this app's 1024-frame/48kHz capture
    // setup) to arrive, so join() could take nearly that long — all of it
    // spent holding srcMu_, the same mutex mixWorkerMain()'s real-time
    // tick loop (paced at 20ms) needs every single tick. That routinely
    // ate most or all of one tick's time budget, causing a late write to
    // the ALSA/PipeWire monitor stream and the first real xrun — which,
    // in testing, reliably cascaded into
    // ~10 seconds of continued underruns before things settled again,
    // matching the reported "jerky ... gets right if you let it run"
    // exactly. Fixed by moving the AudioCapture out of the vector under
    // the lock (a pointer move, effectively free) and letting it actually
    // be destroyed — and block in join() — only after the lock is
    // released, so mixWorkerMain() is never kept waiting for it.
    std::unique_ptr<AudioCapture> toDestroy;
    {
        std::lock_guard<std::mutex> lk(srcMu_);
        auto it = std::find_if(sources_.begin(), sources_.end(),
                               [&](SourceEntry& e) { return e.route.deviceId == deviceId; });
        if (it != sources_.end()) {
            toDestroy = std::move(it->cap);
            sources_.erase(it);
        }
    }
    // toDestroy's destructor (close(), worker_.join()) runs here, after
    // srcMu_ has already been released.
}

std::vector<RoutedSource> AudioMixer::sources() const {
    std::lock_guard<std::mutex> lk(srcMu_);
    std::vector<RoutedSource> out;
    for (auto& e : sources_) out.push_back(e.route);
    return out;
}

std::vector<std::pair<std::string, AudioCounters>> AudioMixer::sourceCounters() const {
    std::lock_guard<std::mutex> lk(srcMu_);
    std::vector<std::pair<std::string, AudioCounters>> out;
    for (auto& e : sources_) out.emplace_back(e.route.deviceId, e.cap->counters());
    return out;
}

namespace {
// Readers for the monitor's hardware mixer; the caller holds monitorMu_.
bool playbackMuted(const HwPlaybackMixer* hm) {
    if (!hm || !hm->volumeElem) return false;
    int l = 1, r = 1;
    if (snd_mixer_selem_has_playback_switch(hm->volumeElem)) {
        snd_mixer_selem_get_playback_switch(hm->volumeElem, SND_MIXER_SCHN_FRONT_LEFT, &l);
        snd_mixer_selem_get_playback_switch(hm->volumeElem, SND_MIXER_SCHN_FRONT_RIGHT, &r);
    }
    return !l && !r;
}
float playbackVolume(const HwPlaybackMixer* hm) {
    if (!hm || !hm->volumeElem || hm->volMax <= hm->volMin) return 0.0f;
    long l = hm->volMin, r = hm->volMin;
    snd_mixer_selem_get_playback_volume(hm->volumeElem, SND_MIXER_SCHN_FRONT_LEFT, &l);
    snd_mixer_selem_get_playback_volume(hm->volumeElem, SND_MIXER_SCHN_FRONT_RIGHT, &r);
    float lv = float(l - hm->volMin) / float(hm->volMax - hm->volMin);
    float rv = float(r - hm->volMin) / float(hm->volMax - hm->volMin);
    return (lv + rv) * 0.5f;
}
}  // namespace

namespace {
HwAudioInfo hwInfoOf(const AudioCapture& cap) {
    HwAudioInfo info;
    info.available = cap.hasHwControl();
    if (info.available) {
        info.muted = cap.hwCaptureMuted();
        info.volume = cap.hwCaptureVolume();
        info.balance = cap.hwCaptureBalance();
    }
    info.inputSources = cap.hwInputSourceOptions();
    info.currentInputSource = cap.hwInputSource();
    return info;
}
}  // namespace

HwAudioInfo AudioMixer::hwInfo(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lk(srcMu_);
    for (auto& e : sources_)
        if (e.route.deviceId == deviceId) return hwInfoOf(*e.cap);
    return HwAudioInfo{};
}

AudioMixer::DisplayState AudioMixer::displayState() const {
    DisplayState d;
    {
        std::lock_guard<std::mutex> lk(displayMu_);
        d = displayCache_;  // the last values seen, used for whatever lock is busy right now
    }
    {
        std::unique_lock<std::mutex> lk(srcMu_, std::try_to_lock);
        if (lk.owns_lock()) {
            d.sources.clear();
            d.sourceCounters.clear();
            d.hwInfo.clear();
            for (const auto& e : sources_) {
                d.sources.push_back(e.route);
                d.sourceCounters.emplace_back(e.route.deviceId, e.cap->counters());
                d.hwInfo.emplace_back(e.route.deviceId, hwInfoOf(*e.cap));
            }
            d.driftReference = sources_.empty() ? std::pair<uint64_t, int64_t>{0, 0}
                                                : std::pair<uint64_t, int64_t>{sources_.front().cap->counters().framesRead,
                                                                               sources_.front().addedAtNs};
        }
    }
    {
        std::unique_lock<std::mutex> lk(monitorMu_, std::try_to_lock);
        if (lk.owns_lock()) {
            auto* hm = static_cast<HwPlaybackMixer*>(monitorHwMixer_);
            d.monitorHwAvailable = hm != nullptr;
            d.monitorHwMuted = playbackMuted(hm);
            d.monitorHwVolume = playbackVolume(hm);
        }
    }
    {
        std::unique_lock<std::mutex> lk(recMu_, std::try_to_lock);
        if (lk.owns_lock()) {
            d.framesRecorded = wav_.framesWritten();
            d.recordingPath = wav_.path();
        }
    }
    std::lock_guard<std::mutex> lk(displayMu_);
    displayCache_ = d;
    return d;
}

void AudioMixer::setHwVolume(const std::string& deviceId, float v0to1) {
    std::lock_guard<std::mutex> lk(srcMu_);
    for (auto& e : sources_)
        if (e.route.deviceId == deviceId) {
            e.cap->setHwCaptureVolume(v0to1);
            break;
        }
}

void AudioMixer::setHwBalance(const std::string& deviceId, float balance) {
    std::lock_guard<std::mutex> lk(srcMu_);
    for (auto& e : sources_)
        if (e.route.deviceId == deviceId) {
            e.cap->setHwCaptureBalance(balance);
            break;
        }
}

void AudioMixer::setHwMuted(const std::string& deviceId, bool mute) {
    std::lock_guard<std::mutex> lk(srcMu_);
    for (auto& e : sources_)
        if (e.route.deviceId == deviceId) {
            e.cap->setHwCaptureMuted(mute);
            break;
        }
}

bool AudioMixer::setHwInputSource(const std::string& deviceId, const std::string& name) {
    std::lock_guard<std::mutex> lk(srcMu_);
    for (auto& e : sources_)
        if (e.route.deviceId == deviceId) return e.cap->setHwInputSource(name);
    return false;
}

std::pair<uint64_t, int64_t> AudioMixer::driftReference() const {
    std::lock_guard<std::mutex> lk(srcMu_);
    if (sources_.empty()) return {0, 0};
    const SourceEntry& e = sources_.front();
    return {e.cap->counters().framesRead, e.addedAtNs};
}

bool AudioMixer::startMonitor(const std::string& playbackId, std::string& err) {
    {
        std::lock_guard<std::mutex> lk(monitorMu_);
        if (monitorHandle_ && monitorDeviceId_ == playbackId) {
            // Already open on this exact device: just start writing real
            // audio again instead of paying a fresh connection's cost (see
            // this method's declaration comment).
            monitorActive_ = true;
            resyncRequested_ = true;
            return true;
        }
    }
    // The new device is opened and configured *before* the old stream is
    // closed, so a failed switch leaves the old one playing. Tearing down
    // first made a failed switch silently stop the monitor (
    // picking the hw: device PipeWire itself uses fails with EBUSY).
    snd_pcm_t* h = nullptr;
    int rc = snd_pcm_open(&h, playbackId.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        err = std::string("snd_pcm_open (playback): ") + snd_strerror(rc);
        if (rc == -EBUSY)
            err += " — probably in use by the system sound server (PipeWire); pick \"default\" to play through it";
        return false;
    }
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(h, hw);
    snd_pcm_hw_params_set_access(h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(h, hw, SND_PCM_FORMAT_S16_LE);
    // Exact rate, not _near: mixWorkerMain() paces its writes assuming the
    // device is really running at rate_ (48000) — _near silently accepts
    // whatever the device's closest supported rate is without telling us,
    // so a device that doesn't offer exactly rate_ would play back
    // pitch-/speed-shifted ("very strange", reported after
    // a stop/restart) with no error at all. Failing loudly here is safer
    // than silently mismatching; every device seen on this machine offers
    // 48000 exactly, so this should not make things harder in practice.
    if ((rc = snd_pcm_hw_params_set_rate(h, hw, rate_, 0)) < 0) {
        err = std::string("playback device does not offer exactly ") + std::to_string(rate_) +
              " Hz: " + snd_strerror(rc);
        snd_pcm_close(h);
        return false;
    }
    if ((rc = snd_pcm_hw_params_set_channels(h, hw, unsigned(outChannels_))) < 0) {
        err = std::string("playback device does not offer ") + std::to_string(outChannels_) +
              " channels: " + snd_strerror(rc);
        snd_pcm_close(h);
        return false;
    }
    // Explicit buffer headroom, not whatever PipeWire's default negotiation
    // picks: mixWorkerMain() writes exactly one ~20ms tick at a time, paced
    // by the clock rather than by write() blocking (see its own comment) —
    // with no buffer cushion, any brief scheduling jitter on this thread
    // (a mutex it's waiting on, a moment of system load, ...) has zero
    // margin before it shows up as a real xrun. Requesting several ticks
    // of buffer here (near, not exact — this is a "give yourself room"
    // request, not a correctness requirement the way the exact rate above
    // is) gives PipeWire's own buffer more to draw from during exactly
    // that kind of hiccup. Investigated chasing the "jerky audio";
    // failure to get this is not fatal, so errors here are not checked.
    unsigned framesPerTick = std::max<unsigned>(1, rate_ / 50);
    snd_pcm_uframes_t period = snd_pcm_uframes_t(framesPerTick) * 2;  // ~40ms: how often PipeWire services this stream
    snd_pcm_hw_params_set_period_size_near(h, hw, &period, nullptr);
    snd_pcm_uframes_t bufSize = snd_pcm_uframes_t(framesPerTick) * 8;  // ~160ms of cushion
    snd_pcm_hw_params_set_buffer_size_near(h, hw, &bufSize);
    if ((rc = snd_pcm_hw_params(h, hw)) < 0) {
        err = std::string("snd_pcm_hw_params (playback): ") + snd_strerror(rc);
        snd_pcm_close(h);
        return false;
    }
    // Belt and braces: confirm what was actually negotiated matches, in
    // case a future device/driver accepts set_rate() but still reports
    // something else back (shouldn't happen per the exact-rate request
    // above, but silent mismatches are exactly what caused this bug).
    unsigned actualRate = 0;
    snd_pcm_hw_params_get_rate(hw, &actualRate, nullptr);
    if (actualRate != rate_) {
        err = "playback negotiated " + std::to_string(actualRate) + " Hz, not the requested " +
              std::to_string(rate_) + " Hz";
        snd_pcm_close(h);
        return false;
    }
    snd_pcm_prepare(h);
    // Start with a few ticks of silence queued so the device runs that far
    // behind the clock-paced one-tick-at-a-time writer. Without it the
    // stream starts on the first 20 ms written and runs dry on any jitter;
    // measured: ~25 underruns/s on an onboard device, and the
    // same steady 25/s on "default" (PipeWire) for seconds at a time after
    // device switches.
    size_t prefill = kMonitorPrefillTicks * framesPerTick;
    writeSilence(h, prefill, framesPerTick, outChannels_);
    std::lock_guard<std::mutex> lk(monitorMu_);
    closeMonitorHandleLocked();  // the new device is ready: only now release the old one
    monitorPrefillFrames_ = prefill;
    monitorHandle_ = h;
    monitorDeviceId_ = playbackId;
    monitorActive_ = true;
    // Best-effort; null (not found/not a hw: device) just means
    // monitorHasHwControl() reports false and the caller stays software-only.
    monitorHwMixer_ = openHwPlaybackMixer(playbackId).release();
    resyncRequested_ = true;
    return true;
}

void AudioMixer::stopMonitor() {
    // Deliberately does not close monitorHandle_ (see startMonitor()'s
    // comment) — mixWorkerMain() keeps writing silence to it so the
    // connection stays alive and warmed up for a fast, glitch-free resume.
    monitorActive_ = false;
}

void AudioMixer::closeMonitorHandleLocked() {
    if (monitorHandle_) {
        snd_pcm_close(static_cast<snd_pcm_t*>(monitorHandle_));
        monitorHandle_ = nullptr;
    }
    delete static_cast<HwPlaybackMixer*>(monitorHwMixer_);
    monitorHwMixer_ = nullptr;
    monitorPrefillFrames_ = 0;
    monitorDeviceId_.clear();
    monitorActive_ = false;
}

bool AudioMixer::monitorHasHwControl() const {
    std::lock_guard<std::mutex> lk(monitorMu_);
    return monitorHwMixer_ != nullptr;
}


bool AudioMixer::monitorHwMuted() const {
    std::lock_guard<std::mutex> lk(monitorMu_);
    return playbackMuted(static_cast<HwPlaybackMixer*>(monitorHwMixer_));
}

void AudioMixer::setMonitorHwMuted(bool mute) {
    std::lock_guard<std::mutex> lk(monitorMu_);
    auto* hm = static_cast<HwPlaybackMixer*>(monitorHwMixer_);
    if (!hm || !hm->volumeElem || !snd_mixer_selem_has_playback_switch(hm->volumeElem)) return;
    snd_mixer_selem_set_playback_switch_all(hm->volumeElem, mute ? 0 : 1);
}

float AudioMixer::monitorHwVolume() const {
    std::lock_guard<std::mutex> lk(monitorMu_);
    return playbackVolume(static_cast<HwPlaybackMixer*>(monitorHwMixer_));
}

void AudioMixer::setMonitorHwVolume(float v0to1) {
    std::lock_guard<std::mutex> lk(monitorMu_);
    auto* hm = static_cast<HwPlaybackMixer*>(monitorHwMixer_);
    if (!hm || !hm->volumeElem) return;
    v0to1 = std::clamp(v0to1, 0.0f, 1.0f);
    long v = hm->volMin + std::lround(v0to1 * float(hm->volMax - hm->volMin));
    snd_mixer_selem_set_playback_volume_all(hm->volumeElem, v);
}

void AudioMixer::setSink(std::function<void(const int16_t*, size_t, int64_t)> sink) {
    std::lock_guard<std::mutex> lk(sinkMu_);
    sink_ = std::move(sink);
}

void AudioMixer::mixWorkerMain() {
    const size_t framesPerTick = std::max<unsigned>(1, rate_ / 50);  // ~20 ms
    std::vector<int16_t> outBuf(framesPerTick * size_t(outChannels_));
    std::vector<int16_t> tmp(framesPerTick);

    // Paced by the clock, not by the playback write blocking: measured
    // that the "default" PCM (routed through the desktop's PipeWire/
    // PulseAudio ALSA shim on this machine) does not reliably block at real
    // playback speed, so trusting snd_pcm_writei() alone to pace this loop
    // pulled samples far faster than sources could supply them.
    auto nextTick = std::chrono::steady_clock::now();
    const auto tickPeriod = std::chrono::microseconds(1000000 * int64_t(framesPerTick) / int64_t(rate_));

    while (!stopRequested_) {
        if (resyncRequested_.exchange(false)) {
            // Clean start after a monitor start/switch: drop queued input
            // and delay backlog, and stop "catching up" on any ticks the
            // schedule fell behind by. Costs a gap of a few tens of ms.
            nextTick = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(srcMu_);
            for (auto& e : sources_) {
                e.cap->flushQueued();
                e.delayL.clear();
                e.delayR.clear();
            }
        }
        std::fill(outBuf.begin(), outBuf.end(), int16_t(0));
        {
            std::lock_guard<std::mutex> lk(srcMu_);
            for (auto& e : sources_) {
                const RoutedSource& r = e.route;
                int logicalChannels = channelModeCount(r.mode);
                for (int li = 0; li < logicalChannels; ++li) {
                    e.cap->pull(li, tmp.data(), framesPerTick);

                    // An artificial extra delay per source. Feed
                    // this tick's freshly-pulled (real-time) samples into a
                    // FIFO, then take the output from its front so this
                    // source's contribution lags real-time by ~delayMs once
                    // the FIFO has filled to that depth. While filling
                    // (right after the delay is first set or increased),
                    // output is silence for this source, not garbage.
                    std::deque<int16_t>& delayFifo = (li == 0) ? e.delayL : e.delayR;
                    for (size_t f = 0; f < framesPerTick; ++f) delayFifo.push_back(tmp[f]);
                    size_t targetDepth = size_t(std::max(0, r.delayMs)) * rate_ / 1000;
                    const int16_t* src;
                    if (delayFifo.size() >= targetDepth + framesPerTick) {
                        for (size_t f = 0; f < framesPerTick; ++f) tmp[f] = delayFifo.front(), delayFifo.pop_front();
                        src = tmp.data();
                    } else {
                        static const std::vector<int16_t> silence(4096, 0);  // large enough for any realistic framesPerTick
                        src = silence.data();
                    }
                    // Cap the FIFO so a delay that was later reduced doesn't
                    // leave a permanently oversized buffer (would otherwise
                    // never catch back up to the new, smaller target).
                    while (delayFifo.size() > targetDepth + framesPerTick * 4) delayFifo.pop_front();

                    // Volume + left/right balance, then route to one or
                    // both output channels (Left/Right/Stereo, mirroring
                    // ChannelMode so input and output read the same way —
                    // user feedback). Stereo
                    // pans between the two output channels via `balance`;
                    // Left/Right send the full (post-gain) signal to just
                    // one, matching what the name says rather than being an
                    // arbitrary channel index nothing guarantees is audible.
                    float gain = std::clamp(r.gain, 0.0f, 4.0f);
                    float balance = std::clamp(r.balance, -1.0f, 1.0f);
                    float gL = 1.0f, gR = 1.0f;
                    if (outChannels_ >= 2) {
                        switch (r.route[li]) {
                            case OutputRoute::Left: gL = 1.0f; gR = 0.0f; break;
                            case OutputRoute::Right: gL = 0.0f; gR = 1.0f; break;
                            case OutputRoute::Stereo:
                                gL = std::clamp(1.0f - balance, 0.0f, 1.0f);
                                gR = std::clamp(1.0f + balance, 0.0f, 1.0f);
                                break;
                        }
                    }
                    for (size_t f = 0; f < framesPerTick; ++f) {
                        float s = float(src[f]) * gain;
                        int mixedL = int(outBuf[f * size_t(outChannels_) + 0]) + int(std::lround(s * gL));
                        outBuf[f * size_t(outChannels_) + 0] = int16_t(std::clamp(mixedL, -32768, 32767));
                        if (outChannels_ >= 2) {
                            int mixedR = int(outBuf[f * size_t(outChannels_) + 1]) + int(std::lround(s * gR));
                            outBuf[f * size_t(outChannels_) + 1] = int16_t(std::clamp(mixedR, -32768, 32767));
                        }
                    }
                }
            }
        }
        mixFrames_ += framesPerTick;

        {
            std::lock_guard<std::mutex> lk(recMu_);
            if (recording_) wav_.writeFrames(outBuf.data(), framesPerTick);
        }
        {
            std::lock_guard<std::mutex> lk(sinkMu_);
            if (sink_) sink_(outBuf.data(), framesPerTick, nowMonoNs());
        }

        {
            std::lock_guard<std::mutex> lk(monitorMu_);
            if (monitorHandle_) {
                // Keep writing even while "stopped" (monitorActive_ false)
                // — silence instead of outBuf — so the ALSA/PipeWire
                // connection never idles or gets torn down: that's the
                // whole point of startMonitor()'s reuse-if-already-open
                // path (chasing "stop then start sounds
                // choppy" — reopening cost ~20ms plus a settling-in burst
                // of xruns each time, both avoided by never closing at all
                // for a simple Stop/Start).
                static const std::vector<int16_t> silence(outBuf.size(), 0);
                const int16_t* toWrite = monitorActive_ ? outBuf.data() : silence.data();
                auto* pcm = static_cast<snd_pcm_t*>(monitorHandle_);
                snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, toWrite, framesPerTick);
                snd_pcm_sframes_t queued = 0;
                if (wrote >= 0 && monitorActive_ && snd_pcm_delay(pcm, &queued) == 0 && queued >= 0)
                    monitorOutDelayMs_ = double(queued) * 1000.0 / double(rate_);
                else if (!monitorActive_)
                    monitorOutDelayMs_ = 0.0;
                if (wrote < 0) {
                    ++mixUnderruns_;
                    if (wrote == -EPIPE) snd_pcm_prepare(pcm);
                    else snd_pcm_recover(pcm, int(wrote), 1);
                    // Restart with the same cushion as at open (see
                    // startMonitor), and write this tick after it instead
                    // of dropping it.
                    if (monitorPrefillFrames_) {
                        writeSilence(pcm, monitorPrefillFrames_, framesPerTick, outChannels_);
                        snd_pcm_writei(pcm, toWrite, framesPerTick);
                    }
                }
            }
        }

        nextTick += tickPeriod;
        // More than 3 ticks behind (a blocking write or a stalled sink held
        // this thread up): start the schedule over from now instead of
        // writing the missed ticks back-to-back. Racing to catch up meant
        // re-taking monitorMu_ without ever sleeping, which starved the GUI
        //
        auto nowTp = std::chrono::steady_clock::now();
        if (nowTp - nextTick > 3 * tickPeriod) {
            nextTick = nowTp;
            ++scheduleResets_;
        }
        std::this_thread::sleep_until(nextTick);
    }
}

AudioCounters AudioMixer::mixCounters() const {
    AudioCounters c;
    c.framesRead = mixFrames_;
    c.overruns = mixUnderruns_;
    return c;
}

bool AudioMixer::startRecording(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(recMu_);
    wav_.stop();
    if (!wav_.start(path, rate_, outChannels_, err)) {
        recording_ = false;
        return false;
    }
    recording_ = true;
    return true;
}

void AudioMixer::stopRecording() {
    std::lock_guard<std::mutex> lk(recMu_);
    recording_ = false;
    wav_.stop();
}

uint64_t AudioMixer::framesRecorded() const {
    std::lock_guard<std::mutex> lk(recMu_);
    return wav_.framesWritten();
}

std::string AudioMixer::recordingPath() const {
    std::lock_guard<std::mutex> lk(recMu_);
    return wav_.path();
}

// -- WavWriter --------------------------------------------------------------
// Standard 44-byte canonical RIFF/WAVE header (PCM, S16LE); the two size
// fields are placeholders until stop() patches them in, since the total
// length isn't known while still recording.
namespace {
struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t riffSize = 0;  // patched: 36 + dataSize
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;  // PCM
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;  // patched
};
}  // namespace

WavWriter::~WavWriter() { stop(); }

bool WavWriter::start(const std::string& path, unsigned sampleRate, int channels, std::string& err) {
    stop();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = std::string("fopen ") + path + ": " + std::strerror(errno);
        return false;
    }
    WavHeader h;
    h.numChannels = uint16_t(channels);
    h.sampleRate = sampleRate;
    h.blockAlign = uint16_t(channels * 2);
    h.byteRate = sampleRate * h.blockAlign;
    if (std::fwrite(&h, sizeof h, 1, f) != 1) {
        err = "short write on the WAV header";
        std::fclose(f);
        return false;
    }
    file_ = f;
    path_ = path;
    rate_ = sampleRate;
    channels_ = channels;
    framesWritten_ = 0;
    return true;
}

void WavWriter::writeFrames(const int16_t* interleaved, size_t frames) {
    if (!file_ || !frames) return;
    size_t samples = frames * size_t(channels_);
    if (std::fwrite(interleaved, sizeof(int16_t), samples, file_) == samples) framesWritten_ += frames;
}

void WavWriter::stop() {
    if (!file_) return;
    uint32_t dataSize = uint32_t(framesWritten_ * size_t(channels_) * sizeof(int16_t));
    uint32_t riffSize = 36 + dataSize;
    std::fseek(file_, offsetof(WavHeader, riffSize), SEEK_SET);
    std::fwrite(&riffSize, sizeof riffSize, 1, file_);
    std::fseek(file_, offsetof(WavHeader, dataSize), SEEK_SET);
    std::fwrite(&dataSize, sizeof dataSize, 1, file_);
    std::fclose(file_);
    file_ = nullptr;
}

}  // namespace fv

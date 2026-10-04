// ALSA audio capture, one worker thread per device, plus a small mixer that
// routes several sources' channels into one output stream:
// a fixed-channel router ("record the C64 and a microphone
// to separate channels"), not a full multi-track system.
//
// Clean-room: device names/descriptions come from ALSA's own device-list API
// at runtime (enumerateAudioDevices()); nothing about a specific card is
// hard-coded (the same rule as for the card's controls:
// ask the system, don't assume).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fv {

struct AudioDeviceInfo {
    std::string id;    // ALSA PCM name for snd_pcm_open, e.g. "hw:0,0"
    std::string name;  // human-readable description ALSA provides
};
// Enumerates capture-capable ALSA PCM devices via snd_device_name_hint.
std::vector<AudioDeviceInfo> enumerateAudioDevices();
// Enumerates playback-capable ALSA PCM devices, same way. Needed because
// PipeWire/PulseAudio's "default" sink is not necessarily what a human is
// actually listening to (found 2026-09-23: on this machine "default"
// resolves to the HDMI-monitor output, not the onboard analog jack real
// speakers/headphones are on — AudioMixer::startMonitor("default", ...)
// was therefore correctly playing audio, just to a device nobody could
// hear). Callers should let the user pick, not assume "default" is right.
std::vector<AudioDeviceInfo> enumeratePlaybackDevices();

enum class ChannelMode { Left, Right, Stereo, SeparateLR };
const char* channelModeName(ChannelMode m);
// How many independently-routable logical channels a mode produces.
inline int channelModeCount(ChannelMode m) { return m == ChannelMode::SeparateLR ? 2 : 1; }

// Where a logical (input) channel lands on output: mirrors ChannelMode's
// Left/Right/Stereo so routing reads the same way on both ends (user
// feedback, Phase B Step 2, 2026-09-23: "if you use left, right, stereo in
// input use the same on output"). Stereo here means "audible on both output
// channels", not "this source is stereo" — for a mono logical channel that's
// the deliberate default (see RoutedSource) so a newly added source is never
// silently inaudible because it landed on an output channel nothing is
// listening to.
enum class OutputRoute { Left, Right, Stereo };
const char* outputRouteName(OutputRoute r);

struct AudioCounters {
    uint64_t framesRead = 0;
    uint64_t overruns = 0;    // ring buffer had to drop old samples (reader too slow) or pad (source too slow)
    uint64_t readErrors = 0;  // snd_pcm_readi errors other than a handled overrun
};

// One ALSA capture device, read continuously on its own thread. Extracts
// mono sample(s) per the current ChannelMode from whatever channel count the
// device actually negotiated (found at runtime, never assumed: a mono
// device, for instance, makes Left/Right/Stereo all read its one channel).
class AudioCapture {
public:
    ~AudioCapture();
    AudioCapture() = default;
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool open(const std::string& alsaId, std::string& err);
    void close();
    bool opened() const { return handle_ != nullptr; }
    const std::string& id() const { return id_; }

    unsigned sampleRate() const { return rate_; }
    unsigned deviceChannels() const { return devChannels_; }  // what the hardware actually gave us

    void setChannelMode(ChannelMode m) { mode_.store(m); }
    ChannelMode channelMode() const { return mode_.load(); }

    // Hardware mixer access for this device's own capture level/routing,
    // via the generic ALSA simple-mixer API (snd_mixer_* — standard Linux
    // kernel/ALSA, nothing Datapath-specific). User feedback, 2026-09-23:
    // "I prefer [volume set on] the [hardware mixer]" and "make it
    // possible to switch between front and back microphone and line
    // inputs". ALSA has no generic API for "which mixer control feeds PCM
    // device N's capture", so the control is found by a best-effort
    // heuristic (see audio.cpp) matched against this specific machine's
    // codec; hasHwControl() reports whether that heuristic found something
    // usable for this device, so callers/UI can fall back to software-only
    // gain (RoutedSource::gain/balance) rather than silently adjusting the
    // wrong input.
    bool hasHwControl() const;
    bool hwCaptureMuted() const;
    void setHwCaptureMuted(bool mute);
    float hwCaptureVolume() const;          // 0..1, average of the L/R hardware levels
    void setHwCaptureVolume(float v0to1);   // sets both channels to the same level
    float hwCaptureBalance() const;         // -1(L only)..0(even)..+1(R only), derived from the L/R levels
    void setHwCaptureBalance(float balance);  // redistributes the current overall volume across L/R
    // Empty if this device has no selectable input source (most don't).
    std::vector<std::string> hwInputSourceOptions() const;
    std::string hwInputSource() const;
    bool setHwInputSource(const std::string& name);

    // Fills `out` with exactly `frames` mono S16 samples for logical channel
    // `logicalIndex` (0, or 1 for SeparateLR's second channel), from what's
    // been captured since the last pull. Zero-pads (and counts as overrun)
    // if not enough was available yet; never blocks.
    void pull(int logicalIndex, int16_t* out, size_t frames);
    // Drops everything captured but not yet pulled (clean start after a
    // monitor switch, see AudioMixer::startMonitor).
    void flushQueued();

    AudioCounters counters() const;

private:
    void workerMain();
    void openHwMixer();   // best-effort; called from open(), leaves hwMixer_ null on any doubt
    void closeHwMixer();

    void* handle_ = nullptr;  // snd_pcm_t*; kept as void* so <alsa/asoundlib.h> stays out of this header
    std::string id_;
    unsigned rate_ = 0;
    unsigned devChannels_ = 0;
    void* hwMixer_ = nullptr;  // HwCaptureMixer* (audio.cpp); null if none was found for this device

    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> workerDone_{true};
    std::atomic<ChannelMode> mode_{ChannelMode::Stereo};

    mutable std::mutex mu_;
    std::deque<int16_t> bufL_, bufR_;  // already-extracted mono samples, capped (old ones dropped = overrun)

    std::atomic<uint64_t> framesRead_{0}, overruns_{0}, readErrors_{0};
};

// One active source in the router: a capture device, its channel mode, where
// its logical channel(s) land on output, a volume/balance, and an artificial
// extra delay (M2 step 8) so a fast source can be pushed back to line up
// with video's own latency. Defaults are chosen so a freshly added source is
// audible immediately (route Stereo = both output channels, gain 1.0,
// balance centered) rather than silently landing nowhere — the exact
// failure the user hit in Phase B Step 2 (2026-09-23).
struct RoutedSource {
    std::string deviceId;
    ChannelMode mode = ChannelMode::Stereo;
    OutputRoute route[2] = {OutputRoute::Stereo, OutputRoute::Stereo};  // per logical channel (0, and 1 if SeparateLR)
    // Software fallback only, applied in AudioMixer::mixWorkerMain() —
    // used when this device has no usable hardware capture control (see
    // AudioCapture::hasHwControl()). When one is available, the Audio
    // panel drives that instead (AudioMixer::setHwVolume()/setHwBalance())
    // and these two stay at their neutral values, per the user's stated
    // preference for the hardware mixer over a software multiply
    // (2026-09-23: "I prefer the second option").
    float gain = 1.0f;     // linear multiplier, 0 = silent, 1 = unity, clamped to [0, 4] when mixed
    float balance = 0.0f;  // -1 = full left .. +1 = full right; only affects a Stereo route
    int delayMs = 0;
};

// A source's hardware mixer state, for the Audio panel — see
// AudioCapture's Hw* methods for what each field means and how reliable
// `available` is (best-effort, see audio.cpp).
struct HwAudioInfo {
    bool available = false;
    bool muted = false;
    float volume = 0.0f;
    float balance = 0.0f;
    std::vector<std::string> inputSources;  // empty: this device has none to pick from
    std::string currentInputSource;
};

// Writes an interleaved S16LE PCM stream to a standard RIFF/WAVE file (M2
// step 9): a fixed 44-byte header written up front with placeholder sizes,
// patched in stop() once the real length is known. 16-bit only for now (the
// capture path negotiates S16; 24-bit output would need capturing at that
// depth too — not done, left as an open item rather than faking precision).
class WavWriter {
public:
    ~WavWriter();
    bool start(const std::string& path, unsigned sampleRate, int channels, std::string& err);
    void writeFrames(const int16_t* interleaved, size_t frames);
    void stop();
    bool active() const { return file_ != nullptr; }
    uint64_t framesWritten() const { return framesWritten_; }
    const std::string& path() const { return path_; }

private:
    FILE* file_ = nullptr;
    std::string path_;
    unsigned rate_ = 0;
    int channels_ = 0;
    uint64_t framesWritten_ = 0;
};

// Combines the active sources into one interleaved S16 stream at a fixed
// rate/channel count; can play it live to a monitor device (M2 step 7) and/or
// record it to a WAV file (M2 step 9). Tracks the first source's hardware
// frame count against wall-clock time so main.cpp can report audio/video
// clock drift (M2 step 10) without this class knowing anything about video.
class AudioMixer {
public:
    ~AudioMixer();
    // outputChannels: width of the interleaved frame this mixer produces (2 = stereo).
    bool start(unsigned sampleRate, int outputChannels, std::string& err);
    void stop();
    bool running() const { return running_; }
    unsigned sampleRate() const { return rate_; }

    // Adds a source or updates an existing one for the same deviceId in place.
    bool addOrUpdateSource(const RoutedSource& r, std::string& err);
    void removeSource(const std::string& deviceId);
    std::vector<RoutedSource> sources() const;
    // Per-source counters for the GUI, keyed by deviceId.
    std::vector<std::pair<std::string, AudioCounters>> sourceCounters() const;
    // Pass-throughs to a source's own AudioCapture::Hw*() (see there) — the
    // GUI never touches an AudioCapture directly, same pattern as the rest
    // of this router. No-ops (false/{}) if deviceId isn't an active source.
    HwAudioInfo hwInfo(const std::string& deviceId) const;
    void setHwVolume(const std::string& deviceId, float v0to1);
    void setHwBalance(const std::string& deviceId, float balance);
    void setHwMuted(const std::string& deviceId, bool mute);
    bool setHwInputSource(const std::string& deviceId, const std::string& name);

    // Opens `playbackId` if it isn't already the open device (a fresh ALSA/
    // PipeWire connection measured 18-21 ms — see the .cpp — most of one
    // mixWorkerMain() tick, and starting it fresh also seems to cost a
    // brief burst of underruns while PipeWire's own buffering settles,
    // independent of anything in this codebase; found chasing "stop then
    // start monitor sounds choppy", 2026-09-23). If it's already open on
    // the same device, this is just a flag flip — no reconnect, no
    // reopen-triggered glitch. stopMonitor() likewise never closes the
    // device, just silences it, so a Stop/Start pair costs nothing.
    bool startMonitor(const std::string& playbackId, std::string& err);
    void stopMonitor();
    // Whether the monitor is currently outputting real audio (not just
    // whether a device happens to be held open — see above).
    bool monitoring() const { return monitorActive_; }

    // Hardware-mixer volume for the monitor's own playback device (2026-09-23
    // backlog item 4) — mirrors AudioCapture's Hw*() pattern, but scoped to a
    // real hw:-device override (see openHwPlaybackMixer() in audio.cpp);
    // "default" has no single hardware level to control and stays
    // software-only. false/0 if the current monitor device is "default" or
    // no usable playback mixer element was found.
    bool monitorHasHwControl() const;
    bool monitorHwMuted() const;
    void setMonitorHwMuted(bool mute);
    float monitorHwVolume() const;  // 0..1
    void setMonitorHwVolume(float v0to1);

    // Called with each tick's mixed output (interleaved S16, `outputChannels`
    // wide, `framesPerTick` frames, a CLOCK_MONOTONIC/steady_clock ns
    // timestamp) right after it's computed — the M3 Recorder hooks in here
    // via main.cpp so a recording's audio always matches exactly what the
    // monitor plays and the WAV writer records. Set to nullptr to detach.
    // Called from the mixer's own thread; must not block or throw.
    void setSink(std::function<void(const int16_t*, size_t, int64_t)> sink);

    bool startRecording(const std::string& path, std::string& err);
    void stopRecording();
    bool recording() const { return recording_.load(); }
    uint64_t framesRecorded() const;
    std::string recordingPath() const;

    AudioCounters mixCounters() const;
    // Hardware frames captured so far by the first active source, and the
    // wall-clock time (ns, CLOCK_MONOTONIC) that source was added — the pair
    // main.cpp needs to compute audio-vs-system-clock drift (M2 step 10).
    // {0, 0} if no source is active yet.
    std::pair<uint64_t, int64_t> driftReference() const;

    // Non-blocking snapshot for the GUI and the once-a-second log line. Each
    // lock is only *tried*; if the audio thread holds it right now, the
    // last values seen are returned instead. The GUI must never wait for
    // the audio thread: it did, and starved for seconds per frame while the
    // mixer held monitorMu_ in a blocking write (2026-09-28).
    // Button actions keep using the exact, blocking getters above.
    struct DisplayState {
        std::vector<RoutedSource> sources;
        std::vector<std::pair<std::string, AudioCounters>> sourceCounters;
        std::vector<std::pair<std::string, HwAudioInfo>> hwInfo;  // per active source
        bool monitorHwAvailable = false, monitorHwMuted = false;
        float monitorHwVolume = 0.0f;
        uint64_t framesRecorded = 0;
        std::string recordingPath;
        std::pair<uint64_t, int64_t> driftReference{0, 0};
    };
    DisplayState displayState() const;
    // How often the mixer gave up catching up and restarted its schedule.
    uint64_t scheduleResets() const { return scheduleResets_; }
    // What the monitor's output device reports as still queued ahead of the
    // loudspeaker (snd_pcm_delay after each write), in ms; 0 while the
    // monitor is off. Measured 112 ms on `default`, 80 ms on hw:.
    double monitorOutputDelayMs() const { return monitorOutDelayMs_; }

private:
    struct SourceEntry {
        RoutedSource route;
        std::unique_ptr<AudioCapture> cap;
        std::deque<int16_t> delayL, delayR;  // extra FIFO for RoutedSource::delayMs
        int64_t addedAtNs = 0;
    };
    void mixWorkerMain();

    unsigned rate_ = 0;
    int outChannels_ = 2;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};

    mutable std::mutex srcMu_;
    std::vector<SourceEntry> sources_;

    mutable std::mutex monitorMu_;
    void* monitorHandle_ = nullptr;  // snd_pcm_t*; stays open across stop/start once opened, see startMonitor()
    std::string monitorDeviceId_;    // which device monitorHandle_ is currently open on, "" if none
    std::atomic<bool> monitorActive_{false};  // true: write real audio; false: write silence, keep the stream alive
    void* monitorHwMixer_ = nullptr;  // HwPlaybackMixer* (audio.cpp); opened/closed alongside monitorHandle_
    size_t monitorPrefillFrames_ = 0;  // silence queued ahead of the monitor device at open/after an underrun
    // Set by every successful startMonitor(); mixWorkerMain() then empties the
    // capture and delay queues and restarts its tick schedule from "now", so
    // nothing queued before a switch is carried over (user request 2026-09-28).
    std::atomic<bool> resyncRequested_{false};
    void closeMonitorHandleLocked();  // real teardown; caller must hold monitorMu_

    mutable std::mutex recMu_;
    WavWriter wav_;
    std::atomic<bool> recording_{false};

    std::mutex sinkMu_;
    std::function<void(const int16_t*, size_t, int64_t)> sink_;

    std::atomic<uint64_t> mixFrames_{0}, mixUnderruns_{0};
    std::atomic<uint64_t> scheduleResets_{0};
    std::atomic<double> monitorOutDelayMs_{0.0};
    mutable std::mutex displayMu_;       // guards displayCache_ only; never held while waiting for another lock
    mutable DisplayState displayCache_;
};

}  // namespace fv

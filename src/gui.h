// ImGui panels: stats, the runtime-enumerated control list, and geometry
// (size/crop). M1 steps 5-7.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <SDL.h>
#include <imgui.h>

#include "audio.h"
#include "capture.h"
#include "controls.h"
#include "display.h"
#include "presets.h"
#include "recorder.h"

namespace fv {

struct GeometryState {
    uint32_t width = 0, height = 0;    // current capture size
    Rect crop;                          // Capture::appliedCrop()
    int maxScreenFactor = 0;            // Display::maxScreenFactor(): largest integer factor that fits the screen
    bool grey8 = false;                 // capture format is g8 (the PAL decode checkbox applies)
    bool rgbChannels = false;           // an RGB capture (S-Video decoding can apply)
    std::string palStatus;              // PAL decode result line for the Display panel, empty when off
    PalGridStatus palGrid;              // phase 3 pixel grid/palette state for the Display panel
    std::string autoStatus;             // auto-tune's last action or state, empty when off
    std::string rgbStatus;              // RGB auto-tune's last step (phase/position/crop), empty if never run
    bool rgbRunning = false;
    int rgbAreaW = 0, rgbAreaH = 0;     // picture area the RGB auto-tune measured last, 0 = none
};

// Machines section (M4): the machine files, what the source looks like, and
// which mode matches it.
struct MachinePanelState {
    struct Entry {
        std::string name, id, note, path, group;
        bool builtIn = false, mine = false;
        std::vector<std::string> modes;
        std::vector<bool> modeMatches;  // per mode: fits the source as it is now
        std::vector<bool> modeManual;   // per mode: only applied by a click
        std::vector<int> modeOrigin;    // per mode: 0 built-in, 1 mine, 2 mine replacing a built-in one
        std::vector<std::vector<std::string>> modeDiff;  // per mode (replacing ones): keys that differ from the built-in
        std::vector<std::string> hidden;                // built-in modes the user hid
    };
    struct Update {  // a newer built-in version of a mode the user replaced
        int machine = -1;
        std::string mode;
        int revision = 0;
        std::vector<std::string> diff;
    };
    std::vector<Update> updates;
    bool changesKnown = false;              // the active mode's starting point is taken
    std::vector<std::string> changes;       // changed since the active mode was applied
    std::vector<Entry> machines;
    int activeMachine = -1, activeMode = -1;        // last applied
    int matchMachine = -1, matchMode = -1;          // best match for the detected mode, -1 = none
    std::string matchWhy;                           // conditions that held
    std::string detected;                           // DetectedMode::describe()
    std::string hint;                               // the active mode's hint
    std::string status;                             // last action
    std::vector<std::string> errors;                // unreadable lines in the files
    bool autoSwitch = true;
    std::string folder;
};

// Everything the Audio panel needs to draw itself; main.cpp fills this each
// frame from its AudioMixer.
struct AudioPanelState {
    std::vector<AudioDeviceInfo> devices;                              // enumerateAudioDevices()
    std::vector<RoutedSource> activeSources;                           // mixer.sources()
    std::vector<std::pair<std::string, AudioCounters>> sourceCounters;  // mixer.sourceCounters()
    std::vector<std::pair<std::string, HwAudioInfo>> hwAudio;           // mixer.hwInfo() per active source
    bool monitoring = false;
    std::vector<AudioDeviceInfo> playbackDevices;  // enumeratePlaybackDevices(), for the monitor output picker
    std::string playbackDeviceId;                  // currently selected/active monitor output
    std::string monitorError;                      // last monitor start/switch failure, empty if none
    // Hardware-mixer volume for the monitor's own playback device (2026-09-23
    // backlog item 4) — mirrors a source's hwAudio entry above, but for
    // output; available is false for "default" or a hw: device with no
    // usable playback mixer element found (mixer.monitorHasHwControl()).
    bool monitorHwAvailable = false;
    bool monitorHwMuted = false;
    float monitorHwVolume = 0.0f;
    AudioCounters mixCounters;
    int outputChannels = 2;
    bool recording = false;
    std::string recordingPath;
    uint64_t framesRecorded = 0;
    double driftMs = 0;       // audio hardware clock vs. wall clock, over driftSeconds (M2 step 10)
    // Monitor sound vs. picture (backlog 6): how late each reaches you
    // (ms after the source), and how many frames the picture is held back.
    double monitorSoundMs = 0, pictureMs = 0;
    int pictureDelayFrames = 0;
    double driftSeconds = 0;  // 0 = no source active yet / not enough time elapsed
};

// M3: the Recorder panel's state, filled by main.cpp each frame.
struct RecorderPanelState {
    bool recording = false;
    std::string path;
    std::string lastRecording;  // the last finished recording (for "Open in freshCoder"), empty if none
    std::string encoderName;  // active encoder once recording ("libx264", "h264_nvenc", ...), empty otherwise
    VideoEncoderKind kind = VideoEncoderKind::Auto;  // the pending choice for the next Start
    RecorderCounters counters;
    // Probed once at startup (availableVideoEncoders(), recorder.h) so the
    // dropdown below only offers what actually opens on this machine
    // (2026-09-23 backlog item: e.g. VAAPI silently falls through today
    // rather than being hidden).
    std::vector<VideoEncoderKind> availableKinds;
    uint32_t canvasWidth = 0, canvasHeight = 0;  // what a recording started now would use (current capture size)
    double fps = 0;                              // capture frame rate the recording uses
    double elapsedSec = 0;                       // while recording
    std::string defaultFolder;                   // pre-fills the folder field (the directory freshVision was started in)
    std::string error;                           // last start failure (bad folder, file exists, encoder), empty if none
    bool folderDialogOpen = false;               // the Browse... dialog is showing
    std::string pickedFolder;                    // last folder chosen in the dialog ...
    int pickedSeq = 0;                           // ... and a counter, so the panel copies it into its field once
};

// Presets panel state — the list of saved presets (main.cpp calls
// listPresets() each frame; cheap, a directory listing of a handful of
// small files/subdirectories).
//
struct PresetPanelState {
    std::vector<PresetEntry> entries;
    // The group/name last successfully loaded (both empty: nothing loaded
    // yet this run) — the Presets panel pre-fills its Save fields with
    // these once, right after a Load, so "load a preset, tweak it, save it
    // back" doesn't need retyping the name.
    std::string lastLoadedGroup, lastLoadedName;
};

// Callbacks the GUI uses to act on user input; main.cpp supplies these so the
// GUI module itself never touches the capture device directly (only the
// worker thread and, for controls, the queued Capture::requestControlChange
// path may do that while streaming).
struct GuiActions {
    std::function<void(uint32_t id, int64_t value)> setControl;
    std::function<void(uint32_t w, uint32_t h)> applySize;   // request a capture-size restart
    std::function<void(Rect crop)> applyCrop;                 // request a crop restart (crop.set=false: reset)
    std::function<void(const std::string& path)> selectSource;  // switch to a different /dev/videoN

    // Single per-device on/off (folds add+remove into one control; user
    // feedback, Phase B Step 1, 2026-09-23).
    std::function<void(const std::string& deviceId, bool on)> setAudioSourceEnabled;
    std::function<void(const std::string& deviceId, ChannelMode mode)> setAudioChannelMode;
    std::function<void(const std::string& deviceId, int logicalIndex, OutputRoute route)> setAudioOutputRoute;
    std::function<void(const std::string& deviceId, float gain)> setAudioGain;
    std::function<void(const std::string& deviceId, float balance)> setAudioBalance;
    // Hardware-mixer equivalents, used instead of the two above whenever
    // AudioPanelState::hwAudio says a device has one (user preference,
    // 2026-09-23: "I prefer the [hardware mixer over software volume]").
    std::function<void(const std::string& deviceId, float v0to1)> setAudioHwVolume;
    std::function<void(const std::string& deviceId, float balance)> setAudioHwBalance;
    std::function<void(const std::string& deviceId, bool mute)> setAudioHwMuted;
    std::function<void(const std::string& deviceId, const std::string& name)> setAudioInputSource;
    std::function<void(const std::string& deviceId, int delayMs)> setAudioDelay;
    std::function<void(bool on)> toggleAudioMonitor;
    std::function<void(const std::string& playbackId)> setAudioPlaybackDevice;  // switches monitor output
    // Hardware-mixer volume for the monitor's own playback device (see
    // AudioPanelState::monitorHwAvailable above); no-ops when unavailable.
    std::function<void(float v0to1)> setMonitorHwVolume;
    std::function<void(bool mute)> setMonitorHwMuted;
    std::function<void(bool on)> toggleAudioRecording;  // uses main.cpp's default/--record-audio path

    std::function<void(bool on)> toggleVideoRecording;  // uses main.cpp's default/--record-video path
    // Start recording into `folder`/`name` (name empty = freshvision-<date>_<time>.mkv).
    std::function<void(const std::string& folder, const std::string& name)> startVideoRecordingTo;
    std::function<void(const std::string& startFolder)> browseRecordFolder;  // opens the folder dialog
    std::function<void(const std::string& folder)> openRecordFolder;         // opens it in the file browser
    std::function<void(const std::string& path)> openInFreshCoder;           // starts freshCoder (path may be empty)
    std::function<void(VideoEncoderKind)> setVideoEncoderKind;

    // Presets: save the app's current video+audio+recorder settings under
    // `group`/`name` (group may be empty — ungrouped), or apply a
    // previously-saved one.
    //
    std::function<void(const std::string& group, const std::string& name)> savePreset;
    std::function<void(const std::string& group, const std::string& name)> loadPreset;
    std::function<void(const std::string& group, const std::string& name)> deletePreset;

    // Controls: reset one control, or every currently-settable one, to the
    // driver's own reported default. User feedback, 2026-09-23: "it was
    // not that obvious how to get back to defaults".
    std::function<void(uint32_t id)> resetControl;
    std::function<void()> resetAllControls;

    // Sync-on-Green PAL timing: 4/8/10 = sample at that multiple of the
    // colour subcarrier (DisplayOptions::palMultiple/palLine; window and
    // capture width 1024 per 4x), 0 = back to the driver's own Sync-on-Green
    // baseline (524/113/384).
    std::function<void(int mode)> setPalTiming;
    // S-Video ticked on a single-channel (g8) capture: switch to an RGB
    // format at the same size (one full reopen), since it needs two inputs.
    std::function<void()> switchToRgbForSvideo;

    // Machines (M4). mode -1: the machine's best-matching mode (or its first).
    std::function<void(int machine, int mode)> applyMachineMode;
    // Into My modes: `conditions` = what the source looks like now (else
    // click-only); `sound` = the sound sources too.
    std::function<void(int machine, const std::string& modeName, bool conditions, bool sound)> saveCurrentAsMode;
    std::function<void(int machine, const std::string& modeName)> removeMyMode;  // delete / back to the built-in mode
    std::function<void(int machine, const std::string& modeName, bool hide)> hideBuiltInMode;
    std::function<void(int machine, const std::string& modeName)> keepMyMode;    // "keep mine" for the newer built-in
    std::function<void()> importOtherInstallation;
    std::function<void(const std::string& name, const std::string& group)> newMachine;
    std::function<void(int machine)> editMachine;  // opens the file in the desktop's text editor
    std::function<void()> openMachinesFolder;
    std::function<void()> reloadMachines;
    std::function<void(bool on)> setAutoSwitch;
    // RGB auto-tune (phase, position, crop) once now.
    std::function<void()> runRgbAutoTune;
    // Takes the last measured picture area as the machine's screen size.
    std::function<void()> useMeasuredScreen;
};

class Gui {
public:
    bool init(SDL_Window* window, SDL_GLContext ctx, std::string& err);
    void shutdown();

    // Feed every SDL event here before your own handling; check
    // wantCaptureKeyboard()/wantCaptureMouse() before acting on a key/click
    // yourself so typing into a panel field doesn't also trigger app shortcuts.
    void processEvent(const SDL_Event& e);
    bool wantCaptureKeyboard() const;
    bool wantCaptureMouse() const;

    void newFrame();
    // Draws the stats line, source selector, control panel, geometry panel,
    // audio panel and recorder panel. `visible` false skips drawing (the
    // window keeps showing only the video, Tab toggles this in main.cpp).
    void draw(bool visible, const std::string& statsText, const std::vector<ControlInfo>& controls,
              DisplayOptions& dispOpts, const GeometryState& geom, const std::vector<SourceInfo>& sources,
              const std::string& activePath, const AudioPanelState& audio, const RecorderPanelState& recorder,
              const PresetPanelState& presets, const MachinePanelState& machines, const GuiActions& actions);
    void render();  // must be called after your own GL draw calls, before SwapWindow
    // Width of the control column on the left (the picture uses the rest).
    float panelWidth() const { return panelW_; }

private:
    void drawMachines(const MachinePanelState& m, const GuiActions& actions);
    bool initialized_ = false;
    // The Presets panel's Save fields — plain widget text, but pre-filled
    // from PresetPanelState::lastLoaded{Group,Name} right after a Load, so
    // this needs to persist and be updated in response to an event, not
    // just hold whatever the user is mid-typing (a local `static` inside
    // draw() can't do that). presetSeenLoadedName_/Group_ track what
    // lastLoaded* looked like the previous frame, so the pre-fill only
    // happens once, right when it changes, not on every frame (which would
    // stomp on the user's own edits after a Load).
    char presetGroupBuf_[64] = "";
    char presetNameBuf_[64] = "";
    std::string presetSeenLoadedGroup_, presetSeenLoadedName_;
    // Recorder panel's folder/name fields; the folder is filled from
    // RecorderPanelState::defaultFolder the first time it is known.
    char recFolderBuf_[512] = "";
    char recNameBuf_[256] = "";
    bool recFolderInit_ = false;
    int recPickedSeqSeen_ = 0;

    // Machines section fields.
    char machineNameBuf_[64] = "";
    char modeNameBuf_[64] = "";
    int saveTarget_ = -1;  // machine the "Save current as mode" goes into (-1: the active one)
    char machineGroupBuf_[64] = "";
    char machineSearchBuf_[64] = "";
    bool saveConditions_ = true, saveSound_ = false;
    int prefillMachine_ = -2, prefillMode_ = -2;  // the active mode the save row was last filled from

    float panelW_ = 460;  // width of the control column (the user can drag its edge)
};

}  // namespace fv

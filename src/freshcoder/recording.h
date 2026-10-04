// freshCoder: reads a freshVision raw composite recording (Matroska with one
// gray video stream of the card's samples, PCM audio, and tags describing
// how the samples were taken).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pal.hpp"

namespace fc {

struct RecordingInfo {
    std::string path;
    int width = 0, height = 0;  // samples per row, rows per frame
    double fps = 50;
    int64_t frames = 0;         // estimated from the duration
    double durationSec = 0;
    double videoStartSec = 0;   // the video stream's first timestamp, relative to the file's start
    std::string videoCodec;
    bool hasAudio = false;
    int audioRate = 0, audioChannels = 0;
    std::map<std::string, std::string> tags;
    // From the tags (0 when missing):
    int horizontalSize = 0;     // CTRL_HORIZONTAL_SIZE: samples per line
    int horizontalPosition = 0; // CTRL_HORIZONTAL_POSITION: window start after the sync edge
    // Derived: samples per subcarrier cycle (0 = not a PAL sampling rate) and
    // the decoder's active-picture window.
    int samplesPerCycle = 0;
    int activeStart = 0, activeLen = 0;
    // An RGB recording (three inputs). S-Video: brightness and colour on two
    // of them (tag FRESHVISION_SVIDEO, e.g. "G,B"); each frame's samples then
    // hold the brightness plane followed by the colour plane
    // (2 x width x height). Without S-Video one input is decoded as
    // composite (`lumaChannel`, one plane).
    // The colour standard (tag FRESHVISION_STANDARD; recordings from before
    // NTSC have none and are PAL), and whether the tag was there.
    pal::Standard standard = pal::Standard::Pal;
    bool standardTagged = false;
    pal::Standard taggedStandard = pal::Standard::Pal;
    bool rgb = false;
    bool svideoTagged = false;             // the recording carries the tag
    int tagLuma = 1, tagChroma = 2;        // its channels (when tagged)
    bool svideo = false;
    int lumaChannel = 1, chromaChannel = 2;  // 0 red, 1 green, 2 blue
};

// How an RGB recording's inputs are read (freshCoder's choice, PalDecodeUi):
// 0 = as the recording says (S-Video with the tag's inputs; without the tag
// the green input as composite), 1 = S-Video with the given inputs,
// 2 = the given brightness input as composite.
enum SvideoMode { kSvideoFromRecording = 0, kSvideoOn = 1, kSvideoOff = 2 };

// For the decoder: the colour plane of a frame's samples (null: composite).
inline const uint8_t* chromaPlaneOf(const RecordingInfo& i, const uint8_t* samples) {
    return i.svideo ? samples + size_t(i.width) * i.height : nullptr;
}

// Sequential and seekable access to the video frames (rows of `width`
// samples, no padding).
class Recording {
public:
    Recording() = default;
    ~Recording();
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;

    bool open(const std::string& path, std::string& err);
    void close();
    const RecordingInfo& info() const { return info_; }
    // RGB recordings: which inputs to decode (SvideoMode; ignored for gray8).
    // Takes effect from the next frame read.
    void chooseInputs(int mode, int luma, int chroma);
    // 0 = as recorded, 1 = PAL, 2 = NTSC; sets the sampling and window anew.
    void chooseStandard(int mode);

    // The next frame; false at the end or on an error. `index` counts frames
    // in order (after a seek: from the sought frame on); timeSec() is that
    // frame's own timestamp, seconds from the file's start (the capture's
    // real timing: a C64 runs at ~50.12 Hz, not 50).
    bool next(std::vector<uint8_t>& samples, int64_t& index);
    double timeSec() const { return timeSec_; }
    // The next call to next() returns frame `index` (clamped to the file).
    bool seek(int64_t index);

private:
    bool decodeOne(std::vector<uint8_t>& samples, int64_t& index);

    RecordingInfo info_;
    void* fmt_ = nullptr;   // AVFormatContext*
    void* dec_ = nullptr;   // AVCodecContext*
    void* pkt_ = nullptr;   // AVPacket*
    void* frame_ = nullptr; // AVFrame*
    void* sws_ = nullptr;   // SwsContext*: an RGB recording to planar G, B, R
    std::vector<uint8_t> planar_;
    int vIdx_ = -1;
    int64_t startPts_ = 0;
    double tb_ = 0;         // video stream time base, seconds
    int64_t skipBefore_ = -1;  // after a seek: drop frames before this index
    int64_t counter_ = 0;      // index of the next frame decoded in order
    bool counterFromPts_ = false;  // after a seek: take the next index from the timestamp
    double timeSec_ = 0;
    double fileStartSec_ = 0;
    bool eof_ = false;
};

// Samples per subcarrier cycle for a Horizontal Size (0 = not a multiple of
// the PAL subcarrier), and the active picture for a window starting
// `position` samples after the sync edge. Same rules as freshVision's live
// decoder (src/main.cpp).
int samplesPerCycleFor(int horizontalSize, pal::Standard standard = pal::Standard::Pal);
void activeWindowFor(int horizontalSize, int position, int& activeStart, int& activeLen,
                     pal::Standard standard = pal::Standard::Pal);

}  // namespace fc

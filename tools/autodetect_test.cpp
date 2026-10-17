// SPDX-License-Identifier: GPL-3.0-or-later
// autodetect-test: the auto-detect rules (src/autodetect.*) against synthetic
// signals of known machines, measured the way the app measures them (bursts
// per wire, pixel period on the decoded picture, pixel repetition), plus
// negative controls that must not be recognised. Exit code = failures.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "autodetect.h"
#include "formats.h"
#include "grid.hpp"
#include "pal.hpp"

using namespace fv;
using namespace fv::detect;

namespace {

int failures = 0;

void check(const std::string& name, bool ok, const std::string& detail = "") {
    if (!ok) ++failures;
    std::printf("%-66s %s%s%s\n", name.c_str(), ok ? "PASS" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
}

// A picture made of machine pixels: blocks of a few colours, changing every
// pixel (at `clockHz`) and every other line, so a pixel grid is there to find.
struct PixelPicture {
    double clockHz;
    double activeLen;
    pal::Rgb operator()(double xFrac, int line) const {
        static const pal::Rgb cols[6] = {{0, 0, 0}, {.75, .75, .75}, {.75, 0, 0}, {0, .6, .75}, {.75, .75, 0}, {.2, .2, .75}};
        const long px = long(std::floor(xFrac * activeLen * clockHz));
        unsigned h = unsigned(px * 2654435761u) ^ unsigned((line / 2) * 40503u);
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return cols[h % 6];
    }
};

struct Source {
    const char* name;
    bool ntsc;
    double cycles;     // carrier cycles per line of the source
    double refresh;
    double pixelClock;
    bool chroma;
    bool svideo;
};

// Measures a synthetic Sync-on-Green source the way the app does: an 8x
// capture whose Horizontal Size comes from the driver's line rate.
Measurements measureComposite(const Source& s, double reportedLineRate, bool noise = false) {
    const pal::StandardInfo& si = pal::standardInfo(s.ntsc ? pal::Standard::Ntsc : pal::Standard::Pal);
    const double lineRate = si.fsc / s.cycles;
    const int K = 8;
    const int hs = int(std::lround(si.fsc / (reportedLineRate > 0 ? reportedLineRate / (reportedLineRate > 25000 ? 2 : 1) : lineRate) * K));
    pal::GenParams p;
    p.samplesPerCycle = K;
    p.samplesPerLine = hs;
    p.lineRate = lineRate;
    p.windowStart = s.ntsc ? 128 : 180;  // the app's 8x windows: PAL 90, NTSC 64 at 4x
    p.width = s.ntsc ? 1664 : 2048;
    p.lines = 120;
    p.chroma = s.chroma;
    p.standard = s.ntsc ? pal::Standard::Ntsc : pal::Standard::Pal;
    p.noiseCodes = 1.0;
    const PixelPicture pic{s.pixelClock, si.activeLen};
    Measurements m;
    m.input = "Analog Sync On Green";
    m.signal = "Video";
    m.lineRateHz = reportedLineRate;
    m.refreshHz = s.refresh;
    m.horizontalSize = hs;
    m.samplesPerCycle = K;
    std::vector<uint8_t> green, blue;
    if (noise) {
        green.resize(size_t(p.width) * p.lines);
        unsigned r = 7;
        for (auto& v : green) r = r * 1103515245u + 12345u, v = uint8_t(r >> 24);
        blue = green;
        m.refreshHz = 0, m.lineRateHz = 0;
    } else if (s.svideo) {
        const pal::YcPlanes yc = pal::generateYC(p, pic);
        green = yc.y, blue = yc.c;
    } else {
        green = pal::generate(p, pic);
        blue.assign(green.size(), 16);  // nothing on the blue wire
    }
    const bool ntsc = s.ntsc;
    const int burstAt = int(std::lround(si.burstStart * hs * lineRate)) - p.windowStart;
    m.burst[1] = measureBurst(green.data(), p.width, p.lines, K, ntsc, burstAt);
    m.burst[2] = measureBurst(blue.data(), p.width, p.lines, K, ntsc, burstAt);
    // Pixel period on the decoded picture.
    pal::DecodeParams dp;
    dp.samplesPerCycle = K;
    dp.samplesPerLine = hs;
    dp.luma = s.chroma ? pal::LumaFilter::Notch : pal::LumaFilter::Off;
    dp.standard = p.standard;
    if (s.svideo) dp.chromaPlane = blue.data();
    if (!s.chroma) dp.mono = true;
    pal::DecodeInfo info;
    const std::vector<uint8_t> rgb = pal::decode(green.data(), p.width, p.lines, dp, &info);
    const int ow = int(rgb.size() / 3 / size_t(p.lines));
    double lo = 0, hi = 0;
    pixelSearchRange(si.fsc * K, lineRate, lo, hi);
    m.pixelPeriod = pixelPeriodRgb(rgb.data(), ow, p.lines, lo, hi, &m.pixelScore);
    return m;
}

std::string periodNote(const Measurements& m) { return std::to_string(m.pixelPeriod).substr(0, 5) + " samples measured, not shown"; }

std::string reasonValue(const Proposal& pr, const std::string& what) {
    for (const auto& r : pr.reasons)
        if (r.what == what) return r.value;
    return "";
}

void compositeCase(const Source& s, double reportedLineRate, Path wantPath, int wantLine4, bool wantMono, int wantChroma = -1) {
    const Measurements m = measureComposite(s, reportedLineRate);
    const Proposal pr = propose(m, nullptr);
    const auto& d = pr.settings.display;
    // No pixel clock line for composite/S-Video even with a grid measured.
    const bool ok = reasonValue(pr, "Machine pixels (estimate)").empty() && m.pixelPeriod > 0 && pr.recognised && pr.path == wantPath &&
                    d.palLine == wantLine4 && d.palUi.mono == wantMono &&
                    d.videoStandard == (s.ntsc ? 1 : 0) && (wantChroma < 0 || (d.palSvideo && d.svideoChroma == wantChroma));
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s, line %d, %s, step %.1f deg, pixels %s, conf %.2f", pathName(pr.path), d.palLine,
                  d.palUi.mono ? "B/W" : "colour", m.burst[s.svideo ? 2 : 1].phaseStepDeg,
                  periodNote(m).c_str(), pr.confidence);
    check(s.name, ok, buf);
}

// Digital frame: machine pixels repeated rx x ry, inside black bars.
std::vector<uint8_t> digitalFrame(int W, int H, int rx, int ry, int bar, bool noise) {
    std::vector<uint8_t> f(size_t(W) * H * 4, 0);
    unsigned r = 3;
    for (int y = bar; y < H - bar; ++y)
        for (int x = bar; x < W - bar; ++x) {
            uint8_t* p = &f[(size_t(y) * W + x) * 4];
            if (noise) {
                for (int c = 0; c < 3; ++c) r = r * 1103515245u + 12345u, p[c] = uint8_t(r >> 24);
                continue;
            }
            unsigned h = unsigned((x - bar) / rx) * 2654435761u ^ unsigned((y - bar) / ry) * 40503u;
            h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
            p[0] = (h & 1) ? 200 : 20, p[1] = (h & 2) ? 220 : 30, p[2] = (h & 4) ? 210 : 10;
        }
    return f;
}

}  // namespace

// A real recording's raw frames (ffmpeg -f rawvideo, gray or bgr0): measured
// as the app measures them live; prints the proposal.
int fileMode(int argc, char** argv) {
    if (argc < 10) {
        std::fprintf(stderr, "usage: autodetect-test --raw FILE W H gray|bgr0 INPUT HSIZE POSITION REFRESH [LINERATE]\n");
        return 2;
    }
    const char* path = argv[2];
    const int W = std::atoi(argv[3]), H = std::atoi(argv[4]);
    const bool grey = std::string(argv[5]) == "gray";
    const std::string input = argv[6];
    const int hs = std::atoi(argv[7]), pos = std::atoi(argv[8]);
    const double refresh = std::atof(argv[9]), lineRate = argc > 10 ? std::atof(argv[10]) : 0;
    const size_t bpp = grey ? 1 : 4, frameBytes = size_t(W) * H * bpp;
    std::vector<uint8_t> frame(frameBytes);
    FILE* f = std::fopen(path, "rb");
    if (!f || std::fread(frame.data(), 1, frameBytes, f) != frameBytes) {
        std::fprintf(stderr, "cannot read one frame from %s\n", path);
        return 2;
    }
    std::fclose(f);
    const FrameView v{frame.data(), uint32_t(W), uint32_t(H), uint32_t(W * bpp), grey ? Layout::Grey8 : Layout::Bgrx8};
    Measurements m;
    m.input = input;
    m.signal = "Video";
    m.width = uint32_t(W), m.height = uint32_t(H), m.lineTotal = hs, m.horizontalSize = hs;
    m.refreshHz = refresh, m.lineRateHz = lineRate;
    const Path path0 = pathFor(m.input, m.signal);
    if (path0 == Path::Composite) {
        const bool ntsc = refresh > 55;
        const auto& si = pal::standardInfo(ntsc ? pal::Standard::Ntsc : pal::Standard::Pal);
        const double q = hs / si.cyclesPerLine;
        const int K = int(std::lround(q));
        m.samplesPerCycle = K;
        const double fs = si.fsc * K;  // samples per second, near enough for the burst's place
        const int burstAt = int(std::lround(si.burstStart * fs)) - pos;
        std::vector<uint8_t> wires[3];
        for (int w = 0; w < 3; ++w) {
            if (grey && w != 1) continue;
            wires[w] = extractWire(v, w);
            m.burst[w] = measureBurst(wires[w].data(), W, H, K, ntsc, burstAt);
        }
        pal::DecodeParams dp;
        dp.samplesPerCycle = K;
        dp.samplesPerLine = hs;
        dp.standard = ntsc ? pal::Standard::Ntsc : pal::Standard::Pal;
        dp.luma = pal::LumaFilter::Notch;
        if (!grey && m.burst[2].colour == 1) dp.chromaPlane = wires[2].data();
        pal::DecodeInfo info;
        const std::vector<uint8_t> rgb = pal::decode(wires[1].data(), W, H, dp, &info);
        double lo = 0, hi = 0;
        pixelSearchRange(si.fsc * K, si.lineRate, lo, hi);
        m.pixelPeriod = pixelPeriodRgb(rgb.data(), int(rgb.size() / 3 / size_t(H)), H, lo, hi, &m.pixelScore);
    } else {
        const std::vector<uint8_t> rgb = toRgb(v);
        double lo = 1.2, hi = 8;
        if (lineRate > 0) pixelSearchRange(lineRate * hs, lineRate, lo, hi);
        m.pixelPeriod = pixelPeriodRgb(rgb.data(), W, H, lo, hi, &m.pixelScore);
        m.area = findPictureArea(v);
        pixelRepetition(v, m.repeatX, m.repeatY);
    }
    const Proposal pr = propose(m, nullptr);
    std::printf("%s\n", pr.summary().c_str());
    for (const auto& r : pr.reasons) std::printf("  %-24s %-28s %s\n", r.what.c_str(), r.value.c_str(), r.why.c_str());
    for (const auto& w : pr.warnings) std::printf("  warning: %s\n", w.c_str());
    std::printf("  bursts R/G/B: %d %d %d (amplitude %.1f %.1f %.1f), pixel period %.3f (score %.2f)\n", m.burst[0].colour,
                m.burst[1].colour, m.burst[2].colour, m.burst[0].amplitude, m.burst[1].amplitude, m.burst[2].amplitude, m.pixelPeriod,
                m.pixelScore);
    return pr.recognised ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--raw") return fileMode(argc, argv);
    // ---- composite and S-Video (synthetic Sync-on-Green sources) ----
    const double fscPal = pal::standardInfo(pal::Standard::Pal).fsc, fscNtsc = pal::standardInfo(pal::Standard::Ntsc).fsc;
    const Source c64{"C64 PAL composite (283.5 cycles, 50.125 Hz)", false, 283.5, 50.125, fscPal * 16 / 9, true, false};
    const Source c64y{"C64 PAL S-Video (colour on Blue)", false, 283.5, 50.125, fscPal * 16 / 9, true, true};
    const Source ted{"C16/Plus/4 TED (285 cycles, 49.861 Hz)", false, 285, 49.861, fscPal * 1.6, true, false};
    const Source zx{"ZX Spectrum (283.75 cycles, 50.08 Hz)", false, 283.75, 50.08, 7e6, true, false};
    const Source a2{"Apple II NTSC (228 cycles, 59.92 Hz)", true, 228, 59.92, fscNtsc * 2, true, false};
    const Source c64n{"C64 NTSC (227.5 cycles, 59.83 Hz)", true, 227.5, 59.83, fscNtsc * 16 / 7, true, false};
    const Source bw{"Black and white (no burst, 50.08 Hz)", false, 283.75, 50.08, 6e6, false, false};
    compositeCase(c64, fscPal / 283.5, Path::Composite, 1134, false);
    compositeCase(c64y, fscPal / 283.5, Path::SVideo, 1134, false, 2);
    compositeCase(ted, fscPal / 285, Path::Composite, 1140, false);
    compositeCase(zx, fscPal / 283.75, Path::Composite, 1135, false);
    compositeCase(a2, fscNtsc / 228, Path::Composite, 912, false);
    compositeCase(c64n, fscNtsc / 227.5, Path::Composite, 910, false);
    compositeCase(bw, fscPal / 283.75, Path::Composite, 1135, true);
    // The line rate missing from the driver: the sampling still gives it.
    compositeCase({"C64, no line rate from the driver", false, 283.5, 50.125, fscPal * 16 / 9, true, false}, 0, Path::Composite, 1134,
                  false);
    // The pixel estimate never changes a composite setting (it can be half or
    // twice the real pixel): the C64 proposal keeps pixel aspect 1.
    {
        const Proposal pr = propose(measureComposite(c64, fscPal / 283.5), nullptr);
        check("C64: pixel estimate does not change the pixel aspect", pr.settings.display.pixelAspect == 1.0);
    }
    // "Timing like": machines whose modes state this timing are named (click-only
    // ones too), others not; the proposal's settings are its own, not theirs.
    {
        auto machine = [](const char* id, const char* name, double hz, double cycles, bool manual) {
            Machine mc;
            mc.id = id, mc.name = name;
            MachineMode md;
            md.name = "Composite 8x";
            md.match.input = "Sync On Green";
            md.match.refreshHz = hz, md.match.refreshTol = 0.06;
            md.match.cyclesPerLine = cycles;
            md.match.manual = manual;
            mc.modes.push_back(md);
            return mc;
        };
        const std::vector<Machine> ms = {machine("zx", "ZX Spectrum", 50.08, 283.75, false), machine("bbc", "BBC Micro", 50.08, 283.75, true),
                                         machine("c64", "Commodore 64", 50.125, 283.5, false)};
        const Proposal pr = propose(measureComposite(zx, fscPal / 283.75), &ms);
        check("timing like: ZX Spectrum and BBC Micro, not the C64", pr.timingLike == "ZX Spectrum, BBC Micro", pr.timingLike);
        const Proposal pc = propose(measureComposite(c64, fscPal / 283.5), &ms);
        check("timing like: C64 source names only the C64", pc.timingLike == "Commodore 64", pc.timingLike);
        check("timing like is a hint only (line from the measurement)", pr.settings.display.palLine == 1135);
    }
    // The card locked at twice the line rate: warned, still the right line.
    {
        const Measurements m = measureComposite(c64, 2 * fscPal / 283.5);
        const Proposal pr = propose(m, nullptr);
        check("double line rate (31 kHz on a 50 Hz source): warning", !pr.warnings.empty() && pr.settings.display.palLine == 1134,
              pr.warnings.empty() ? "" : pr.warnings[0].substr(0, 40));
    }
    // Negative controls.
    {
        const Measurements m = measureComposite(c64, 0, true);
        const Proposal pr = propose(m, nullptr);
        check("noise on Sync-on-Green: not recognised", !pr.recognised, pr.summary());
        check("noise: no colour burst found", m.burst[1].colour != 1);
    }
    {
        const Measurements m = measureComposite(c64, fscPal / 283.5);
        const Proposal pr = propose(m, nullptr);
        check("composite (burst on Green only) is not taken for S-Video", pr.path == Path::Composite && !pr.settings.display.palSvideo);
    }
    {
        Measurements m;
        m.noSignal = true;
        check("no signal: not recognised", !propose(m, nullptr).recognised);
    }
    // The old fold toward the PAL standard would have made the TED 284.x.
    check("resolveCyclesPerLine: TED step 0 deg near 285", std::abs(resolveCyclesPerLine(285.02, 0, false) - 285) < 1e-9);
    check("resolveCyclesPerLine: NTSC 180 deg is .5", std::abs(resolveCyclesPerLine(227.4, 180, true) - 227.5) < 1e-9);

    // ---- digital ----
    {
        const int W = 720, H = 576;
        std::vector<uint8_t> f = digitalFrame(W, H, 2, 2, 40, false);
        FrameView v{f.data(), uint32_t(W), uint32_t(H), uint32_t(W * 4), Layout::Bgrx8};
        Measurements m;
        m.input = "DVI-I SL";
        m.signal = "DVI";
        m.width = W, m.height = H, m.refreshHz = 50;
        pixelRepetition(v, m.repeatX, m.repeatY);
        m.area = findPictureArea(v);
        const Proposal pr = propose(m, nullptr);
        check("digital 720x576 with black bars: 2x2 reported, captured 1:1, no crop",
              pr.path == Path::Digital && m.repeatX == 2 && m.repeatY == 2 && !pr.settings.crop.set && pr.settings.width == uint32_t(W) &&
                  pr.settings.height == uint32_t(H),
              std::to_string(m.repeatX) + "x" + std::to_string(m.repeatY) + ", crop " + (pr.settings.crop.set ? "set" : "none"));
        std::vector<uint8_t> g = digitalFrame(W, H, 3, 1, 0, false);
        FrameView v3{g.data(), uint32_t(W), uint32_t(H), uint32_t(W * 4), Layout::Bgrx8};
        int rx = 0, ry = 0;
        pixelRepetition(v3, rx, ry);
        check("digital: 3x1 repetition", rx == 3 && ry == 1, std::to_string(rx) + "x" + std::to_string(ry));
        std::vector<uint8_t> n = digitalFrame(W, H, 1, 1, 0, true);
        FrameView vn{n.data(), uint32_t(W), uint32_t(H), uint32_t(W * 4), Layout::Bgrx8};
        pixelRepetition(vn, rx, ry);
        check("digital noise: no repetition (negative control)", rx == 1 && ry == 1, std::to_string(rx) + "x" + std::to_string(ry));
    }

    // ---- analog RGB ----
    {
        // A 15 kHz RGB machine sampled at 2.25 samples per pixel (Horizontal
        // Size 2048 for a 910-pixel line); each sample averages the analog
        // signal over its own interval, as the card's ADC sees it.
        const int W = 1440, H = 288;
        const double P = 2.25;
        std::vector<uint8_t> f(size_t(W) * H * 4, 0);
        auto level = [](int px, int y, int c) {
            unsigned h = unsigned(px) * 2654435761u ^ unsigned(y / 2) * 40503u;
            h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
            return (h >> c) & 1 ? 200.0 : 0.0;
        };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c) {
                    double acc = 0;
                    for (int k = 0; k < 8; ++k) acc += level(int(std::floor((x + (k + 0.5) / 8) / P)), y, c);
                    f[(size_t(y) * W + x) * 4 + size_t(c)] = uint8_t(std::lround(acc / 8));
                }
        FrameView v{f.data(), uint32_t(W), uint32_t(H), uint32_t(W * 4), Layout::Bgrx8};
        Measurements m;
        m.input = "Analog 5 Wires (DVI-I SL)";
        m.signal = "Video";
        m.width = W, m.height = H, m.lineTotal = 2048, m.horizontalSize = 2048;
        m.lineRateHz = 15625, m.refreshHz = 50.08;
        const std::vector<uint8_t> rgb = toRgb(v);
        double lo = 0, hi = 0;
        pixelSearchRange(15625.0 * 2048, 15625, lo, hi);
        m.pixelPeriod = pixelPeriodRgb(rgb.data(), W, H, lo, hi, &m.pixelScore);
        m.area = findPictureArea(v);
        const Proposal pr = propose(m, nullptr);
        int64_t hs = 0;
        for (const auto& c : pr.settings.controls)
            if (c.name == "Horizontal Size") hs = c.value;
        check("analog RGB at 2.25 samples per pixel: Horizontal Size 910", pr.path == Path::AnalogRgb && hs == 910,
              "period " + std::to_string(m.pixelPeriod) + " score " + std::to_string(m.pixelScore) + ", size " + std::to_string(hs));
        std::vector<uint8_t> flat(size_t(W) * H * 4, 90);
        FrameView vf{flat.data(), uint32_t(W), uint32_t(H), uint32_t(W * 4), Layout::Bgrx8};
        const std::vector<uint8_t> frgb = toRgb(vf);
        m.pixelPeriod = pixelPeriodRgb(frgb.data(), W, H, 1.2, 8, &m.pixelScore);
        const Proposal pf = propose(m, nullptr);
        hs = 0;
        for (const auto& c : pf.settings.controls)
            if (c.name == "Horizontal Size") hs = c.value;
        check("analog RGB flat frame: no grid, size kept, lower confidence", m.pixelPeriod == 0 && hs == 2048 && pf.confidence < 1);
    }
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "all passed", failures);
    return failures;
}

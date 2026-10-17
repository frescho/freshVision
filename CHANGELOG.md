# Changelog

All notable changes to freshVision and freshCoder. Versions follow
[Semantic Versioning](https://semver.org/); before 1.0 anything may change.

## 0.1.1 (2026-10-17)

Experimental feature release.

### freshVision
- **Detect unknown machine (experimental):** measures the source for a few
  seconds and proposes settings with a confidence and a reason for each, for
  digital (DVI/HDMI), analog RGB, composite and S-Video sources; "Apply" or
  "Save as My mode" (group "Detected"); option `--detect`. Tested on a C64
  (composite, S-Video, through an RGB scan converter and through an HDMI line
  doubler). Digital sources are proposed 1:1 at the detected size.
- Machine files from documentation for Commodore 16 and Plus/4, Amiga 500, 600
  and 1200, HT 1080Z and ZX81: click-only and not yet measured on the card.
- Microkey Primo mode (black and white composite), the "Card's detected
  timing" button (`--pal-timing card`) and black-and-white decoding
  (`--pal-mono`; freshCoder `--mono`).

## 0.1.0 (2026-10-03)

First public release.

### freshVision
- Live capture and display from Datapath Vision cards through the Linux V4L2 driver
  (an independent program, not affiliated with Datapath), with integer scaling,
  fullscreen and low latency.
- Card controls found at runtime (picture, timing and per-colour levels),
  capture size and crop.
- Software composite decoding (PAL and NTSC) from the Sync-on-Green input, and
  S-Video decoding from two inputs: sampling at 4x, 8x or 10x the colour carrier
  (NTSC also 6x and 12x), line averaging, carrier-tracking PLL, auto-tune of the
  levels, removal of the fixed stripes a computer's clock leaves in its video,
  re-alignment of sources the card can't hold vertically.
- Pixel grid and palette snapping with documented palettes (C64, VIC-20, C16/Plus/4,
  ZX Spectrum, MSX1, BBC Micro), soft two-colour text for MSX1.
- Machines and modes (C64 PAL/NTSC, ZX Spectrum, BBC Micro, Apple II, MSX1 and
  generic PAL/NTSC) that switch by themselves when the source changes; RGB
  auto-tune. Built-in modes ship with the program and are updated by upgrades;
  your own modes and tuned versions ("My modes") are never touched by an
  upgrade, and a newer built-in version is offered for you to take or keep.
  Saving stores only what you changed since the mode was applied. Machines
  grouped by maker, with search. Settings of earlier development versions and
  presets are converted once (backups kept).
- Sound: capture from ALSA devices, mixing, monitoring with delay, WAV recording.
- Recording to Matroska with sound: NVENC, x264 or lossless FFV1; raw composite
  recordings for freshCoder.
- Starts on the last input that worked (else the first with a signal), shows a
  help window when no card can be opened; `--list-devices`.
- Folder and file dialogs through the desktop's file-chooser portal.

### freshCoder
- Opens freshVision's raw composite and S-Video recordings, decodes them with the
  same decoder and picture controls, crops, scales and encodes them (x264, x265,
  NVENC, SVT-AV1, ProRes, FFV1) with sound; GUI and batch command line.

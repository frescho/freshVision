# Changelog

All notable changes to freshVision and freshCoder. Versions follow
[Semantic Versioning](https://semver.org/); before 1.0 anything may change.

## 0.1.0 (unreleased)

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

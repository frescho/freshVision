# freshVision 0.1.0

The first public release of freshVision and freshCoder: capture, display and
recording for Datapath Vision capture cards on Linux, made for retro
computers. An independent project, not affiliated with Datapath.

## Highlights

- Live picture with integer scaling and low latency; RGB with auto-tune.
- Composite and S-Video decoded in software (PAL and NTSC) from the card's
  Sync-on-Green input, with auto-tune, carrier-tracking PLL and clock-stripe
  removal; machine pixels and palettes.
- Machine modes for the Commodore 64, ZX Spectrum, BBC Micro, MSX1, Apple II
  and generic PAL/NTSC that switch by themselves; your own modes and tuned
  versions are kept apart from the built-in ones, so upgrades never overwrite
  them.
- Recording with sound (NVENC, VAAPI, x264, lossless FFV1); raw composite
  recordings that freshCoder decodes again later.
- freshCoder: decode, crop, scale (up to 2160p or pixel-exact) and encode
  recordings; GUI and batch.

Tested with a VisionHD4 and a Commodore 64 (composite, S-Video, RGB through a
converter), an MSX1 (Casio MX-10) and an Apple IIc. The ZX Spectrum and BBC
Micro modes are prepared from documentation and untested.

## Install

- From source: `./build.sh --user-install` (Debian/Ubuntu/Mint, Fedora, Arch,
  openSUSE).
- Flatpak: download `freshVision.flatpak` below, then
  `flatpak install --user freshVision.flatpak`.

The card's own Linux driver must be installed separately. On some Intel
computers RGB capture needs the kernel option `intel_iommu=off`. See the
[user guide](docs/user/README.md).

## Known limits

- PAL composite sampling up to 10x the colour carrier (the card doesn't
  deliver whole frames above that).
- The Apple IIc's colour graphics arrive at about 8 frames/s (the card doesn't
  hold its vertical sync).
- Windows is not supported yet, but will be ready in the near future.

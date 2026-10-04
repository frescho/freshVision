# freshVision

**freshVision** shows and records the picture of a **Datapath Vision** capture
card on Linux, made with retro computers in mind. **freshCoder**, which comes
with it, turns freshVision's raw composite recordings into finished colour
videos.

Version 0.1.0. Free software under the GPL-3.0-or-later. **An independent
project, not affiliated with or endorsed by Datapath.**

![freshVision showing an MSX1 on composite, decoded in software](docs/user/images/freshvision-msx-window.png)

## What it does

- **Live picture** from any input of the card, with sharp integer scaling,
  fullscreen and low latency.
- **RGB** (digital and analog), with an auto-tune for analog RGB (sampling
  phase, centring).
- **Composite and S-Video, decoded in software** from the card's
  Sync-on-Green input: PAL and NTSC, sampled at 4x-10x the colour carrier,
  with auto-tune of the levels, line-pair colour averaging, a carrier-tracking
  PLL, removal of the fixed stripes a computer's clock leaves in its video,
  and optional snapping to the machine's own pixels and palette.
- **Machine modes** for the Commodore 64 (PAL/NTSC), ZX Spectrum, BBC Micro,
  MSX1, Apple II and generic PAL/NTSC: freshVision recognises the signal and
  switches to the right mode by itself. Your own machines and modes are plain
  text files.
- **Sound**: capture from the PC's sound inputs, monitoring with delay, WAV.
- **Recording** to Matroska with sound: NVENC, VAAPI, x264 or lossless FFV1;
  raw composite recordings that freshCoder can decode again later with other
  settings, at sizes up to 2160p or pixel-exact.

Tested with a VisionHD4 card, a Commodore 64 (composite, S-Video, RGB through
a scan converter), an MSX1 (Casio MX-10) and an Apple IIc.

## Requirements

- A Datapath Vision capture card and **its Linux driver, installed
  separately** (it is not part of freshVision).
- A 64-bit Linux desktop with OpenGL 3.2.
- On some Intel computers, RGB capture needs the kernel option
  `intel_iommu=off` (see the user guide).

## Install

From source, on Debian/Ubuntu/Mint, Fedora, Arch or openSUSE:

```sh
git clone https://github.com/frescho/freshvision.git
cd freshvision
./build.sh --user-install
```

The script checks the build dependencies (it shows the install command and
asks first), builds both programs and installs them with their menu entries
for your user. A Flatpak is attached to the releases.

## Documentation

The [user guide](docs/user/README.md): getting started (driver, IOMMU, first
start), building, connecting your computer, the freshVision and freshCoder
references, troubleshooting and FAQ.

## Licence and credits

GPL-3.0-or-later, see [LICENSE](LICENSE). Third-party software and notices:
[THIRD-PARTY.md](THIRD-PARTY.md). Changes: [CHANGELOG.md](CHANGELOG.md).

freshVision contains no code, headers or drivers from Datapath. It talks to
the card only through the standard Linux video interface (V4L2) and finds the
card's controls at run time. "Datapath" and "Vision" are names of Datapath
Ltd, used only to say which cards freshVision works with.

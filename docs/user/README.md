# freshVision user guide

freshVision shows and records the picture of a **Datapath Vision** capture card
on Linux, made with retro computers in mind. **freshCoder**, which comes with
it, turns freshVision's raw composite recordings into finished colour videos.

freshVision is an independent, free program (GPL-3.0-or-later). It is not
made, endorsed or supported by Datapath.

## Contents

1. [Getting started](getting-started.md): what you need, installing the card's
   driver, the IOMMU setting some Intel computers need, installing and starting
   freshVision.
2. [Building from source](building.md): the build script, the developer
   build, the Flatpak.
3. [Connecting your computer](wiring.md): RGB, composite and S-Video on the
   card's inputs, and sound.
4. [freshVision reference](freshvision.md): every section of the control
   column, the keys, machines and modes (built-in and your own), recording,
   the command line.
5. [freshCoder reference](freshcoder.md): the window and the batch command
   line.
6. [Troubleshooting and FAQ](troubleshooting.md).
7. [How the composite and S-Video decoding works](decoding.md) (technical,
   with raw captures and decoded pictures of a C64).

## In one minute

1. Install the card's Linux driver from the card maker (it isn't part of
   freshVision) and check that `/dev/video*` devices appear.
2. On an Intel computer, check whether RGB capture needs `intel_iommu=off`
   ([Getting started](getting-started.md#2-the-iommu-setting-intel-computers)).
3. Build and install freshVision (`./build.sh --user-install`) or install the
   Flatpak from the release page.
4. Connect the computer ([Connecting your computer](wiring.md)) and start
   freshVision from the menu. It opens the card input with a signal and, for
   the machines it knows (C64, ZX Spectrum, BBC Micro, MSX1, Apple II, ...),
   switches to the right mode by itself.

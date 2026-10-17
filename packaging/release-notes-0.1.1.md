# freshVision 0.1.1

An experimental feature release. An independent project, not affiliated with
Datapath.

## New

- **Detect unknown machine (experimental).** The new button in the Machines
  section (or `--detect`) measures the source for a few seconds and proposes
  settings for it: for digital (DVI/HDMI), analog RGB, composite and S-Video
  sources. Each setting comes with its reason and a confidence. Nothing changes
  until you click **Apply detected settings** or **Save as My mode**. Digital
  sources are proposed 1:1 at the size the card detects. The known machines
  with the same timing are listed as a hint ("Timing like"); no machine is
  picked from them.
- **More machines, prepared from documentation:** Commodore 16 and Plus/4,
  Amiga 500, 600 and 1200, HT 1080Z and ZX81. These modes are applied by a
  click and are not yet measured on the card; corrections follow in the next
  releases.
- **Microkey Primo** mode (black and white composite), measured on the card.
- **Black-and-white decoding** for sources without a colour burst
  (`--pal-mono`; freshCoder `--mono`), and the **Card's detected timing**
  button (`--pal-timing card`), which sets the card back to the timing it
  detects for the signal.

Detection was tested with a Commodore 64 on composite, on S-Video, through an
RGB scan converter and through an HDMI line doubler.

## Install

- From source: `./build.sh --user-install` (Debian/Ubuntu/Mint, Fedora, Arch,
  openSUSE).
- Flatpak: download `freshVision.flatpak` below, then
  `flatpak install --user freshVision.flatpak`.

The card's own Linux driver must be installed separately. See the
[user guide](docs/user/README.md).

## Known limits

- Detection doesn't tell apart machines with the same timing (for example the
  C64 and the Amigas on PAL) and shows no pixel clock for composite and
  S-Video.
- As in 0.1.0: PAL composite sampling up to 10x the colour carrier; the Apple
  IIc's colour graphics at about 8 frames/s; Windows is not supported yet.

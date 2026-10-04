# Troubleshooting and FAQ

## The computer freezes when RGB capture starts

Seen on an Intel PC with the IOMMU on: RGB capture froze the whole computer
within seconds, every time. Set the kernel option `intel_iommu=off`
([Getting started](getting-started.md#2-the-iommu-setting-intel-computers)).
freshVision refuses RGB capture without it unless you start it with
`--i-know-it-may-freeze`. YUV formats are not affected.

## "No capture card could be opened"

- Is the card's driver installed and loaded? `ls /dev/video*` must show
  devices; `freshvision --list-devices` must show `driver Vision`.
- After a kernel update the driver may need installing again.
- Permission denied: add your user to the `video` group
  (`sudo usermod -aG video $USER`, log out and in).
- The window lists the devices it found; **Use** opens one.

## No picture, or "no signal"

- `freshvision --list-devices` shows which input has a signal; pick that one
  in the Source list (Stats and source section).
- Composite and S-Video go into the **green** input (see
  [Connecting](wiring.md)); the input then reads "Analog Sync On Green".
- If the picture is there but black or white, run **Auto-tune** (Display).

## Composite: no colour, or the colours are wrong

- **Decode** must be ticked, with the right standard (PAL or NTSC).
- Check the sampling and the line length (**samples/line at 4x**: 1134 for a
  C64, 1135 standard PAL, 910 NTSC, 912 Apple II/MSX1). The machine modes set
  them.
- **The card detects the source at 31,250 Hz instead of ~15,600 Hz** (seen
  after a reboot): the colour can't be decoded then. Unplug the cable and plug
  it in again; the card then detects the right line rate.
- Colour fringes on fine detail are part of composite video (a TV shows them
  too). For text screens, **Two-colour text (soft)** removes them; **colour
  smoothing** reduces them. S-Video has none.

## A rolling band of noise moves through the picture

On the C64 used for development, a faint band rolls slowly through the
picture. It comes from the computer itself (mains hum in its own video
circuit, about a 100 Hz beat); it stayed with a new power supply. It isn't a
capture problem. The fixed vertical stripes of the computer's clock, on the
other hand, can be removed (**Remove fixed clock stripes**).

## Apple IIc: colour graphics show only ~8 frames per second

The card locks to the Apple IIc's picture fully (60 frames/s) in text mode,
but in colour graphics it delivers only about 8 frames/s, each starting at a
different line. The likely cause: the Apple's colour burst continues through
its vertical sync, and the card's firmware then rejects most vertical syncs.
freshVision re-aligns the frames it gets (**Re-align rolling frames**, on in
the Apple II mode), so the picture is steady, but the frame rate stays low.
The IIc's video expansion port has a clean sync signal that might help; this
is untested. An MSX1 with the same timing runs at the full 60 frames/s.

## The settings changed by themselves

The card keeps its controls (levels, timing, size) between runs and shares
them between programs. Another program using the card at the same time can
change them under freshVision. Use **Reset all to defaults** (Controls) or
apply the machine mode again.

## "Device or resource busy" for a sound input

Two programs can't record from the same ALSA hardware input at once (for
example two freshVision windows). Close the other one.

## NVENC is missing in the Flatpak

The Flatpak brings its own FFmpeg, and NVIDIA's encoder needs a driver at
least as new as that FFmpeg expects. On the development PC (driver 580.178)
the Flatpak's FFmpeg asked for NVENC API 13.1 and the driver offers 13.0, so
NVENC isn't offered there; x264, x265, SVT-AV1, ProRes and FFV1 work. A newer
NVIDIA driver, or the version built from source (which uses the system's
FFmpeg), has NVENC.

## A recording stopped by itself

If the computer can't keep up (each frame takes over a second for 3 seconds),
the recording is stopped and finished so the file stays usable; the reason is
shown in the Recorder section. Try another encoder or a smaller size.

## The machine mode is wrong

- Machines with the same timing can't be told apart (ZX Spectrum and BBC
  Micro; Apple II and MSX1): their modes are "(manual)", click the right one.
- From a cold start the C64's S-Video mode is tried first while freshVision
  can't yet tell whether the blue input carries colour; with plain composite it
  moves on to the composite mode within a second or two.
- Turn off **Switch automatically** to keep a mode you chose.
- A built-in mode doesn't suit your machine (a different crystal, other
  levels): tune it and **Save**; your version is used from then on, and
  **back to built-in** undoes it.
- While a recording runs, freshVision doesn't switch modes. To record a
  machine's mode from the start, name it: `--machine "NAME/MODE" --record-video FILE`
  (the recording starts once the mode is in place).

## FAQ

**Is freshVision made by Datapath?** No. It's an independent free program
that works with Datapath Vision cards through their Linux driver.

**Does it work with other capture cards or webcams?** It is built for the
Vision cards' driver and its controls; other video devices are not supported.

**Windows?** Not supported yet, but it will be ready in the near future.

**SECAM?** Not supported (no SECAM source to test with).

**Why 4x, 8x, 10x sampling and not more for PAL?** The card samples a PAL
composite signal correctly up to 10x the colour carrier (44 MHz). At 12x
and 14x it breaks the lower part of many frames, and above that it delivers no
usable picture. (NTSC at 12x is only 43 MHz and works.)

**Where are my settings?** `~/.config/freshvision/` (or `$XDG_CONFIG_HOME/freshvision/`): your machine modes,
palettes, freshCoder settings, the last device and the window size. Inside the
Flatpak: `~/.var/app/io.github.frescho.freshVision/config/freshvision/`.

**How do I report a problem?** Open an issue on GitHub with the output of
`freshvision --version`, `freshvision --list-devices`, your distribution, and
what the terminal printed (start `freshvision` from a terminal to see it).

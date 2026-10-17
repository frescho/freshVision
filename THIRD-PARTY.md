# Third-party software and notices

freshVision and freshCoder are free software under the GNU General Public
License version 3 or later (`LICENSE`).

**freshVision is an independent project. It is not affiliated with, endorsed
by or supported by Datapath Ltd.** "Datapath" and "Vision" are named only to
say which capture cards the programs work with. freshVision contains no code,
headers or other material from Datapath; it talks to the cards only through the
standard Linux video interface (V4L2) of the driver the user installs.
The card driver is not part of freshVision and is not distributed with it.

## Included in this source tree

| Component | Version | Licence | Where |
|---|---|---|---|
| Dear ImGui (core, SDL2 and OpenGL 3 backends) | 1.90.1 | MIT, (c) 2014-2024 Omar Cornut | `third_party/imgui/` (`LICENSE.txt` there) |
| stb_truetype, stb_rect_pack, stb_textedit (bundled with Dear ImGui as `imstb_*.h`) | as bundled with ImGui 1.90.1 | MIT or public domain (Unlicense), (c) Sean Barrett; notice in each file | `third_party/imgui/imstb_*.h` |

## Used at build and run time (not included; from the system or the Flatpak runtime)

| Library | Licence | Used for |
|---|---|---|
| SDL2 | zlib | window, input, OpenGL context |
| libepoxy | MIT | OpenGL function loading |
| OpenGL (Mesa or the GPU vendor's driver) | various | display |
| ALSA library (libasound) | LGPL-2.1-or-later | sound capture and playback |
| libdbus | AFL-2.1 or GPL-2.0-or-later | the desktop's file-chooser portal (folder and file dialogs) |
| FFmpeg libraries (libavcodec, libavformat, libavutil, libswscale, libswresample) | LGPL-2.1-or-later, or GPL-2.0-or-later when built with GPL parts | recording, encoding, decoding |
| x264, through FFmpeg (if FFmpeg was built with it) | GPL-2.0-or-later | H.264 software encoding |
| NVIDIA NVENC, through FFmpeg (if available on the system) | NVIDIA driver licence | H.264/HEVC hardware encoding |

Distribution builds of FFmpeg differ: on Debian, Ubuntu and Mint FFmpeg is
built with GPL parts including x264; Fedora's own FFmpeg leaves some codecs
out. All of the above are compatible with distributing freshVision under the
GPL-3.0-or-later.

## Included in the Windows zip

The Windows zip (`freshVision-<version>-windows-x64.zip`) carries these
libraries as DLLs next to the program, unchanged; their licence texts are in
its `licences` folder.

| Component | Version | Licence | Source |
|---|---|---|---|
| SDL2 | 2.32.10 (MinGW release package) | zlib | https://github.com/libsdl-org/SDL/releases |
| FFmpeg, shared GPL build for win64 (includes x264, x265, SVT-AV1 and others) | 8.1 | GPL-3.0-or-later as built | https://github.com/BtbN/FFmpeg-Builds (build scripts and the exact upstream versions), https://ffmpeg.org |
| libepoxy | 1.5.10 (MSYS2 mingw64 package) | MIT | https://github.com/anholt/libepoxy, https://packages.msys2.org |
| GCC runtime (libstdc++, libgcc) | 13 (MinGW-w64, Ubuntu 24.04) | GPL-3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org |
| winpthreads | MinGW-w64 11 (Ubuntu 24.04) | MIT/BSD-style | https://www.mingw-w64.org |

## Optional programs called at run time (not linked)

- `zenity` (folder dialog, fallback when the desktop portal isn't available), LGPL-2.1-or-later
- `xdg-open` (opens the recordings folder), from xdg-utils, MIT

## References

The composite decoding follows the published PAL and NTSC standards (ITU-R
BT.470, SMPTE 170M) and general signal-processing practice. Machine palettes
and timings come from published documentation of each machine; the palettes are
starting points and are calibrated from the picture.

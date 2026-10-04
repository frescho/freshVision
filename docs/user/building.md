# Building from source

## With the build script

```sh
git clone https://github.com/frescho/freshvision.git
cd freshvision
./build.sh                 # check/install dependencies, build into build-release/
./build.sh --user-install  # ... and install for your user (~/.local, no sudo)
./build.sh --install       # ... or system-wide (/usr/local, asks for sudo)
```

What the script does:

1. **Dependencies.** It checks for a C++17 compiler, CMake, pkg-config and the
   development files of SDL2, libepoxy, ALSA, D-Bus, OpenGL and FFmpeg. If
   something is missing it recognises the distribution family (apt: Debian,
   Ubuntu, Mint; dnf: Fedora; pacman: Arch, Manjaro; zypper: openSUSE), prints
   the exact install command and runs it with `sudo` **only after you answer
   yes** (`-y` answers yes for you; `--no-deps` skips the check).
2. **Build** with CMake in `build-release/` (`--build-dir` changes it).
3. **Install** if asked: the two programs, their menu entries, icons and the
   documentation. The uninstall command is printed at the end.

The Debian/Ubuntu/Mint package list is the tested one. The lists for Fedora,
Arch and openSUSE follow those distributions' package names; if one is wrong
on your version, install the libraries listed above yourself and run
`./build.sh --no-deps`.

Fedora's own FFmpeg leaves out some encoders (H.264 software encoding among
them). freshVision still records with the lossless FFV1 encoder; for x264,
FFmpeg from RPM Fusion is needed.

## By hand

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build          # optional; prefix /usr/local
```

## The developer build

`./build.sh --with-tools` (or CMake `-DFRESHVISION_TOOLS=ON`) also builds:

- `paltool`: the composite signal generator and decoder used for development,
  with its test suite (`paltool test`);
- `machines-test`: checks the built-in machine files and the automatic mode choice;
- `rgbtune-test`: the RGB auto-tune against a simulated source.

Users don't need them.

## The Flatpak

The manifest is `packaging/flatpak/io.github.frescho.freshVision.yml`. With
`flatpak-builder` installed:

```sh
flatpak-builder --user --install --force-clean build-flatpak \
    packaging/flatpak/io.github.frescho.freshVision.yml
flatpak run io.github.frescho.freshVision
```

It uses the Freedesktop 25.08 runtime (FFmpeg with x264 comes with it). The
Flatpak needs access to all devices (`--device=all`) to reach the capture card
and the sound devices. The card's driver and the IOMMU setting are still set up
on the system itself ([Getting started](getting-started.md)).

## Third-party code

Dear ImGui 1.90.1 (MIT) is included in `third_party/imgui`. Everything else
comes from the system or the Flatpak runtime; see `THIRD-PARTY.md`.

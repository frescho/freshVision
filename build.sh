#!/bin/sh
# Builds (and optionally installs) freshVision and freshCoder from source on
# the common desktop Linux distributions.
#
#   ./build.sh                  check/install the build dependencies, then build
#   ./build.sh --user-install   ... and install for this user (~/.local, no sudo)
#   ./build.sh --install        ... and install system-wide (/usr/local, uses sudo)
#
# Options:
#   --with-tools     also build the developer tools (paltool, test programs)
#   --prefix DIR     install prefix for --install (default /usr/local)
#   --no-deps        don't check or install the dependencies
#   --deps-only      only check/install the dependencies
#   -y, --yes        install missing dependencies without asking
#   --build-dir DIR  build folder (default: build-release)
#   -h, --help       this text
#
# The dependencies come from the distribution (apt, dnf, pacman or zypper);
# the script shows the exact command and runs it with sudo only after a yes.
# The capture card's own driver is not installed by this script: see
# docs/user/getting-started.md.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tools=OFF
install=none
prefix=/usr/local
deps=yes
depsonly=no
assume=no
builddir="$here/build-release"

usage() { sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --with-tools) tools=ON ;;
        --user-install) install=user ;;
        --install) install=system ;;
        --prefix) shift; prefix=$1 ;;
        --no-deps) deps=no ;;
        --deps-only) depsonly=yes ;;
        -y|--yes) assume=yes ;;
        --build-dir) shift; builddir=$1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\n== %s\n' "$*"; }

# root (e.g. in a container) needs no sudo
if [ "$(id -u)" = 0 ]; then SUDO=""; else SUDO="sudo "; fi

# ---- dependencies -----------------------------------------------------------
# pkg-config modules the build needs (CMakeLists.txt).
modules="sdl2 epoxy alsa dbus-1 libavcodec libavformat libavutil libswscale libswresample gl"

missing_now() {
    miss=""
    command -v cmake >/dev/null 2>&1 || miss="$miss cmake"
    command -v c++ >/dev/null 2>&1 || miss="$miss c++"
    command -v pkg-config >/dev/null 2>&1 || { miss="$miss pkg-config"; echo "$miss"; return; }
    for m in $modules; do
        pkg-config --exists "$m" 2>/dev/null || miss="$miss $m"
    done
    echo "$miss"
}

distro_family() {
    id=""; like=""
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        id=${ID:-}; like=${ID_LIKE:-}
    fi
    for x in $id $like; do
        case "$x" in
            debian|ubuntu|linuxmint|pop|elementary|zorin|neon) echo apt; return ;;
            fedora|rhel|centos|nobara) echo dnf; return ;;
            arch|manjaro|endeavouros|garuda|cachyos) echo pacman; return ;;
            opensuse|opensuse-tumbleweed|opensuse-leap|suse|sles) echo zypper; return ;;
        esac
    done
    echo unknown
}

# Package lists per family (checked in containers: Ubuntu 24.04, Debian 12,
# Fedora, Arch, openSUSE Tumbleweed). dnf and zypper install by pkg-config
# name, which follows renamed packages (e.g. SDL2 -> sdl2-compat).
install_cmd() {
    case "$1" in
        apt) echo "${SUDO}apt-get install -y build-essential cmake pkg-config libsdl2-dev libepoxy-dev libasound2-dev libdbus-1-dev libgl-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev" ;;
        dnf) echo "${SUDO}dnf install -y gcc-c++ cmake pkgconf-pkg-config 'pkgconfig(sdl2)' 'pkgconfig(epoxy)' 'pkgconfig(alsa)' 'pkgconfig(dbus-1)' 'pkgconfig(gl)' 'pkgconfig(libavcodec)' 'pkgconfig(libavformat)' 'pkgconfig(libavutil)' 'pkgconfig(libswscale)' 'pkgconfig(libswresample)'" ;;
        pacman) echo "${SUDO}pacman -S --needed --noconfirm base-devel cmake pkgconf sdl2 libepoxy alsa-lib dbus mesa ffmpeg" ;;
        zypper) echo "${SUDO}zypper --non-interactive install gcc-c++ cmake pkgconf 'pkgconfig(sdl2)' 'pkgconfig(epoxy)' 'pkgconfig(alsa)' 'pkgconfig(dbus-1)' 'pkgconfig(gl)' 'pkgconfig(libavcodec)' 'pkgconfig(libavformat)' 'pkgconfig(libavutil)' 'pkgconfig(libswscale)' 'pkgconfig(libswresample)'" ;;
    esac
}

if [ "$deps" = yes ]; then
    say "Checking build dependencies"
    miss=$(missing_now)
    if [ -z "$miss" ]; then
        echo "all found"
    else
        echo "missing:$miss"
        fam=$(distro_family)
        if [ "$fam" = unknown ]; then
            echo "Unknown distribution: install a C++17 compiler, cmake, pkg-config and the development files of" >&2
            echo "SDL2, libepoxy, ALSA, D-Bus, OpenGL and FFmpeg (libavcodec, libavformat, libavutil," >&2
            echo "libswscale, libswresample), then run this script again with --no-deps." >&2
            exit 1
        fi
        cmd=$(install_cmd "$fam")
        echo "This command installs them ($fam):"
        echo "  $cmd"
        if [ "$assume" = yes ]; then
            answer=y
        else
            printf 'Run it now? [y/N] '
            read -r answer || answer=n
        fi
        case "$answer" in
            y|Y|yes|YES) sh -c "$cmd" ;;
            *) echo "Not installed. Install them yourself, then run this script again." >&2; exit 1 ;;
        esac
        miss=$(missing_now)
        if [ -n "$miss" ]; then
            echo "still missing:$miss" >&2
            echo "(package names differ between versions; please report this with your distribution's name)" >&2
            exit 1
        fi
    fi
fi
[ "$depsonly" = yes ] && exit 0

# ---- build ------------------------------------------------------------------
case "$install" in
    user) instprefix="$HOME/.local" ;;
    *) instprefix="$prefix" ;;
esac
jobs=$(nproc 2>/dev/null || echo 4)
say "Building in $builddir (tools: $tools)"
cmake -S "$here" -B "$builddir" -DCMAKE_BUILD_TYPE=Release -DFRESHVISION_TOOLS="$tools" -DCMAKE_INSTALL_PREFIX="$instprefix"
cmake --build "$builddir" -j"$jobs"
echo "built: $builddir/freshvision and $builddir/freshcoder"

# ---- install ----------------------------------------------------------------
case "$install" in
    user)
        say "Installing for this user into $instprefix"
        cmake --install "$builddir"
        update-desktop-database "$instprefix/share/applications" 2>/dev/null || true
        gtk-update-icon-cache -q -t "$instprefix/share/icons/hicolor" 2>/dev/null || true
        case ":$PATH:" in
            *":$instprefix/bin:"*) ;;
            *) echo "Note: $instprefix/bin is not in your PATH yet; it usually is after logging in again." ;;
        esac
        echo "Uninstall: xargs rm -f < \"$builddir/install_manifest.txt\""
        ;;
    system)
        say "Installing system-wide into $instprefix (sudo)"
        # shellcheck disable=SC2086  # $SUDO is "sudo " or empty: split on purpose
        ${SUDO}cmake --install "$builddir"
        echo "Uninstall: sudo xargs rm -f < \"$builddir/install_manifest.txt\""
        ;;
    none)
        echo "Run without installing: $builddir/freshvision   (or ./build.sh --user-install)"
        ;;
esac

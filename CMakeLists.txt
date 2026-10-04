cmake_minimum_required(VERSION 3.20)
project(freshvision VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(GNUInstallDirs)

# Developer flavour: paltool (the composite generator/decoder/test suite) and
# the test programs. Off for users; developers configure with
# -DFRESHVISION_TOOLS=ON (build.sh --with-tools).
option(FRESHVISION_TOOLS "Build paltool and the test programs" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Dear ImGui 1.90.1 (MIT), vendored in third_party/imgui with its SDL2 and
# OpenGL 3 backends (third_party/imgui/README.md): no distro package needed.
add_library(imgui_backends STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_demo.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_sdl2.cpp
  third_party/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_backends PUBLIC third_party/imgui third_party/imgui/backends)
target_link_libraries(imgui_backends PUBLIC PkgConfig::SDL2 OpenGL::GL ${CMAKE_DL_LIBS})

# freshvision: capture-to-display program (named freshvision-spike until 0.1.0). M0 was CLI-only; M1 adds the
# ImGui shell (src/gui.cpp) and a shader/PBO display path (src/display.cpp)
# on top of it, keeping every command-line option of the first version
# so the existing test recipes keep working.
add_executable(freshvision
  src/main.cpp
  src/capture.cpp
  src/controls.cpp
  src/display.cpp
  src/gui.cpp
  src/formats.cpp
  src/safety.cpp
  src/audio.cpp
  src/recorder.cpp
  src/presets.cpp
  src/machines.cpp
  src/rgbtune.cpp
  src/desktop.cpp
  src/portal.cpp
  src/nodevice.cpp
  src/palui.cpp)
target_compile_options(freshvision PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(freshvision PRIVATE FRESHVISION_VERSION="${PROJECT_VERSION}"
                                           FRESHVISION_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(freshvision PRIVATE tools/pal)  # pal.hpp: the PAL decoder, shared with paltool
target_link_libraries(freshvision PRIVATE PkgConfig::SDL2 PkgConfig::EPOXY PkgConfig::ALSA PkgConfig::DBUS OpenGL::GL
                                                Threads::Threads imgui_backends PkgConfig::AVCODEC
                                                PkgConfig::AVFORMAT PkgConfig::AVUTIL PkgConfig::SWSCALE
                                                PkgConfig::SWRESAMPLE)

# freshcoder (display name freshCoder): offline decoder/encoder for freshVision's
# raw composite recordings; shares the PAL decoder (tools/pal/pal.hpp), the
# display and the PAL controls with freshVision.
add_executable(freshcoder
  src/freshcoder/main.cpp
  src/freshcoder/recording.cpp
  src/freshcoder/process.cpp
  src/freshcoder/encode.cpp
  src/freshcoder/settings.cpp
  src/display.cpp
  src/formats.cpp
  src/palui.cpp
  src/desktop.cpp
  src/portal.cpp)
target_compile_options(freshcoder PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(freshcoder PRIVATE FRESHVISION_VERSION="${PROJECT_VERSION}")
target_include_directories(freshcoder PRIVATE src tools/pal)
target_link_libraries(freshcoder PRIVATE PkgConfig::SDL2 PkgConfig::EPOXY PkgConfig::DBUS OpenGL::GL Threads::Threads imgui_backends
                                         PkgConfig::AVCODEC PkgConfig::AVFORMAT PkgConfig::AVUTIL PkgConfig::SWSCALE
                                         PkgConfig::SWRESAMPLE)

if(FRESHVISION_TOOLS)
  # paltool: PAL composite test-signal generator + software decoder prototype
  # for the Sync-on-Green capture path (tools/pal/pal.hpp). Standalone, no
  # app code or libraries; `paltool test` runs its accuracy suite.
  add_executable(paltool tools/pal/paltool.cpp)
  target_compile_options(paltool PRIVATE -Wall -Wextra -Wpedantic)

  # rgbtune-test: the RGB auto-tune (src/rgbtune.*) against a simulated source
  # (phase-dependent sharpness, position-dependent placement).
  add_executable(rgbtune-test tools/rgbtune_test.cpp src/rgbtune.cpp)
  target_compile_options(rgbtune-test PRIVATE -Wall -Wextra -Wpedantic)
  target_include_directories(rgbtune-test PRIVATE src)

  # machines-test: the built-in machine files and the automatic mode choice
  # (src/machines.*), in a temporary HOME.
  add_executable(machines-test tools/machines_test.cpp src/machines.cpp src/presets.cpp src/formats.cpp)
  target_compile_options(machines-test PRIVATE -Wall -Wextra -Wpedantic)
  target_include_directories(machines-test PRIVATE src)
  target_compile_definitions(machines-test PRIVATE FRESHVISION_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  install(TARGETS paltool rgbtune-test machines-test RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Installation (release 0.1.0): the two programs, their menu entries, icons
# and the AppStream description; the IDs match the Flatpak app ID.
install(TARGETS freshvision freshcoder RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/io.github.frescho.freshVision.desktop
              packaging/io.github.frescho.freshVision.freshCoder.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/io.github.frescho.freshVision.svg
              packaging/io.github.frescho.freshVision.freshCoder.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(DIRECTORY data/machines DESTINATION ${CMAKE_INSTALL_DATADIR}/freshvision FILES_MATCHING PATTERN "*.conf")
install(FILES packaging/io.github.frescho.freshVision.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES LICENSE README.md CHANGELOG.md THIRD-PARTY.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

cmake_minimum_required(VERSION 3.16)
project(gridtv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ----------------------------------------------------------------------------
# Dependencies
# ----------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(RTMIDI REQUIRED IMPORTED_TARGET rtmidi)
pkg_check_modules(LIBLO REQUIRED IMPORTED_TARGET liblo)

# CoreMIDI/CoreFoundation are macOS-only. RtMidi links them transitively on
# macOS and selects the native backend on every OS (Core on macOS, ALSA on
# Linux, WinMM on Windows) via the __MACOSX_CORE__/__LINUX_ALSA__/__WINDOWS_MM__
# macros that pkg-config injects. We only link the Apple frameworks explicitly.
if(APPLE)
    find_library(COREMIDI CoreMIDI)
    find_library(COREFOUNDATION CoreFoundation)
endif()

# ----------------------------------------------------------------------------
# Core library: device abstraction + frame processing (no VLC dependency).
# This is exactly the layer the future native VLC vout module will reuse.
# ----------------------------------------------------------------------------
add_library(gridtv STATIC
    src/frame_processor.cpp
    src/devices/launchpad_mini_mk3.cpp
    src/devices/launchpad_classic.cpp
    src/devices/monome_128.cpp
)
target_include_directories(gridtv PUBLIC include)
# libgridtv is linked into the shared VLC plugin, so it must be position-
# independent. macOS is lenient about this; Linux x86-64 requires -fPIC.
set_target_properties(gridtv PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(gridtv
    PUBLIC
        PkgConfig::RTMIDI
        PkgConfig::LIBLO
)
if(APPLE)
    target_link_libraries(gridtv PUBLIC ${COREMIDI} ${COREFOUNDATION})
endif()

# ----------------------------------------------------------------------------
# Standalone device test: proves the MIDI link + grid orientation on hardware,
# using the verified per-pad SysEx form, then a batched full-frame.
# ----------------------------------------------------------------------------
add_executable(gridtv_device_test test/device_test.cpp)
target_link_libraries(gridtv_device_test PRIVATE gridtv)

add_executable(gridtv_probe test/interactive_probe.cpp)
target_link_libraries(gridtv_probe PRIVATE gridtv)

add_executable(gridtv_midi_probe test/midi_probe.cpp)
target_link_libraries(gridtv_midi_probe PRIVATE gridtv)

# Mock serialosc broker+device for validating the monome OSC path without hardware.
add_executable(gridtv_monome_mock test/monome_mock.cpp)
target_link_libraries(gridtv_monome_mock PRIVATE gridtv)

# ----------------------------------------------------------------------------
# libVLC harness: feeds real, A/V-synced video to a GridDevice.
# Only needs the libVLC public headers (present in the VLC.app bundle).
# ----------------------------------------------------------------------------
# VLC install location. Overridable per platform; sensible defaults below.
#   macOS : the VLC.app bundle
#   Linux : /usr (VLC installed system-wide from your distro's package)
#   Windows: the VLC program folder (with the SDK for the harness)
if(APPLE)
    set(_gridtv_vlc_default "/Applications/VLC.app")
elseif(WIN32)
    set(_gridtv_vlc_default "C:/Program Files/VideoLAN/VLC")
else()
    set(_gridtv_vlc_default "/usr")
endif()
set(VLC_APP "${_gridtv_vlc_default}" CACHE PATH "Path to the VLC install (the .app bundle on macOS, /usr on Linux, the VLC folder on Windows)")
if(APPLE)
    set(VLC_LIBDIR "${VLC_APP}/Contents/MacOS/lib")
    set(VLC_INCDIR "${VLC_APP}/Contents/MacOS/include")
elseif(WIN32)
    set(VLC_LIBDIR "${VLC_APP}")
    set(VLC_INCDIR "${VLC_APP}/sdk/include")
else()
    set(VLC_LIBDIR "${VLC_APP}/lib")
    set(VLC_INCDIR "${VLC_APP}/include")
endif()

if(EXISTS "${VLC_INCDIR}/vlc/vlc.h")
    add_executable(gridtv_vlc harness/gridtv_vlc.cpp)
    target_include_directories(gridtv_vlc PRIVATE "${VLC_INCDIR}")
    target_link_directories(gridtv_vlc PRIVATE "${VLC_LIBDIR}")
    target_link_libraries(gridtv_vlc PRIVATE gridtv vlc)
    # Let the binary find VLC's dylibs at runtime.
    target_link_options(gridtv_vlc PRIVATE "SHELL:-Wl,-rpath,${VLC_LIBDIR}")
else()
    message(WARNING
        "VLC public headers not found at ${VLC_INCDIR} -- skipping gridtv_vlc.\n"
        "Set -DVLC_APP=/path/to/VLC.app to enable the libVLC harness.")
endif()

# ----------------------------------------------------------------------------
# Native VLC video filter: libgridtv_plugin.dylib
# Drop into VLC.app/Contents/MacOS/plugins/ and run:
#   VLC --video-filter gridtv video.mp4
# ----------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/vlc-src/include/vlc_plugin.h")
    add_library(gridtv_plugin MODULE module_vlc/gridtv_plugin.cpp)
    target_include_directories(gridtv_plugin PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/vlc-src/include")
    target_link_libraries(gridtv_plugin PRIVATE gridtv) # pulls RtMidi (+CoreMIDI on macOS)
    # VLC core symbols (vlc_*, picture_*) resolve at runtime from VLC itself.
    if(APPLE)
        target_link_options(gridtv_plugin PRIVATE
            "-Wl,-undefined,dynamic_lookup"
            "-Wl,-rpath,/opt/homebrew/opt/rtmidi/lib")
    elseif(UNIX)
        # Linux/BSD: shared object may leave VLC symbols undefined (resolved at load).
        target_link_options(gridtv_plugin PRIVATE "-Wl,--allow-shlib-undefined")
    endif()
    # Plugin filename per platform: lib...dylib (macOS), lib...so (Linux), ...dll (Windows).
    if(APPLE)
        set(_gridtv_plug_prefix "lib")
        set(_gridtv_plug_suffix ".dylib")
    elseif(WIN32)
        # VLC requires the lib<name>_plugin.<ext> naming convention on EVERY OS
        # (yes, including Windows: libaccess_http_plugin.dll) or it won't load a
        # module — so keep the "lib" prefix here too.
        set(_gridtv_plug_prefix "lib")
        set(_gridtv_plug_suffix ".dll")
    else()
        set(_gridtv_plug_prefix "lib")
        set(_gridtv_plug_suffix ".so")
    endif()
    set_target_properties(gridtv_plugin PROPERTIES
        PREFIX "${_gridtv_plug_prefix}" SUFFIX "${_gridtv_plug_suffix}" OUTPUT_NAME "gridtv_plugin")
else()
    message(STATUS "VLC module headers not vendored (third_party/vlc-src) -- skipping gridtv_plugin.")
endif()

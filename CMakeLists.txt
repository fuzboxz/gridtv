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
# RtMidi + liblo: prefer pkg-config (Homebrew on macOS, apt on Linux), which
# also injects RtMidi's backend define (__MACOSX_CORE__/__LINUX_ALSA__). When
# pkg-config is absent (e.g. a vcpkg-only Windows build) fall back to locating
# the raw libs + headers and set the backend define per platform ourselves.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(RTMIDI IMPORTED_TARGET rtmidi)
    pkg_check_modules(LIBLO  IMPORTED_TARGET liblo)
endif()

if(TARGET PkgConfig::RTMIDI)
    set(GRIDTV_RTMIDI_LIB PkgConfig::RTMIDI)
    set(GRIDTV_LIBLO_LIB   PkgConfig::LIBLO)
else()
    # pkg-config absent (e.g. a vcpkg-only Windows build): locate the raw libs +
    # headers and supply RtMidi's backend define + the backend's system libs
    # ourselves (static libs don't carry transitive deps the way a .pc would).
    if(WIN32)
        set(_rtmidi_backend __WINDOWS_MM__)
        set(_rtmidi_syslib winmm)
        set(_liblo_syslib  ws2_32)   # liblo OSC over UDP needs winsock
    elseif(APPLE)
        set(_rtmidi_backend __MACOSX_CORE__)
    else()
        set(_rtmidi_backend __LINUX_ALSA__)
    endif()

    find_library(RTMIDI_LIB NAMES rtmidi RtMidi REQUIRED)
    find_path(RTMIDI_INC NAMES RtMidi.h PATH_SUFFIXES rtmidi REQUIRED)
    add_library(gridtv_rtmidi INTERFACE IMPORTED)
    set_target_properties(gridtv_rtmidi PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${RTMIDI_INC}"
        INTERFACE_LINK_LIBRARIES      "${RTMIDI_LIB};${_rtmidi_syslib}"
        INTERFACE_COMPILE_DEFINITIONS "${_rtmidi_backend}")

    find_library(LIBLO_LIB NAMES lo liblo REQUIRED)
    find_path(LIBLO_INC NAMES lo/lo.h REQUIRED)   # code does #include <lo/lo.h> -> need the dir above lo/
    add_library(gridtv_liblo INTERFACE IMPORTED)
    set_target_properties(gridtv_liblo PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIBLO_INC}"
        INTERFACE_LINK_LIBRARIES      "${LIBLO_LIB};${_liblo_syslib}")

    set(GRIDTV_RTMIDI_LIB gridtv_rtmidi)
    set(GRIDTV_LIBLO_LIB   gridtv_liblo)
endif()

# CoreMIDI/CoreFoundation are macOS-only. RtMidi links them transitively on
# macOS; we only link them explicitly on Apple.
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
        ${GRIDTV_RTMIDI_LIB}
        ${GRIDTV_LIBLO_LIB}
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
    # MinGW: VLC's headers call poll() / use struct pollfd (vlc_threads.h), which
    # MinGW doesn't declare by default. Newer MinGW-w64 ships <poll.h> providing
    # both, so force-include it before the VLC headers.
    if(MINGW)
        target_compile_options(gridtv_plugin PRIVATE -include poll.h)
    endif()
    # VLC core symbols (vlc_*, picture_*) resolve at runtime from VLC itself.
    if(APPLE)
        target_link_options(gridtv_plugin PRIVATE
            "-Wl,-undefined,dynamic_lookup"
            "-Wl,-rpath,/opt/homebrew/opt/rtmidi/lib")
    elseif(MINGW)
        # Windows DLL: leave vlc_* undefined; VLC (libvlccore) resolves them at load.
        target_link_options(gridtv_plugin PRIVATE "-Wl,--unresolved-symbols=ignore-all")
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

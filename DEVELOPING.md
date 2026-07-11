# Developing GridTV

This document is for **contributors and people building GridTV from source**. If
you just want to use it, see the **[README](README.md)** (download a prebuilt
plugin instead of building).

---

## Repository layout

```
include/gridtv/          GridDevice abstraction, colour/RGB8 types, frame processor
src/devices/             Device drivers: launchpad_mini_mk3, launchpad_classic, monome_128
src/frame_processor.cpp  Area-averaging downscaler (harness / standalone paths)
module_vlc/              The native VLC video filter (Open/Filter/Close, worker, settings UI)
harness/                 Standalone libVLC player (dev/test; --mirror routes through the filter)
test/                    CLI tools: device_test, probe, midi_probe, monome_mock
scripts/                 install-plugin.sh, gridtv-vlc launcher, check-ci.sh workflow linter
third_party/vlc-src/     VLC 3.0.x module headers (NOT committed — fetched on demand; gitignored)
```

| File | Role |
|---|---|
| `module_vlc/gridtv_plugin.cpp` | VLC video filter: `Open`/`Filter`/`Close`, worker thread, all settings |
| `src/devices/*.cpp` | device drivers (Launchpad mk2/3, classic, monome) |
| `src/frame_processor.cpp` | downscaler used by the harness/standalone paths |
| `harness/gridtv_vlc.cpp` | standalone libVLC player for dev/test |

---

## Build from source

### macOS (primary, tested)

```sh
xcode-select --install
brew install cmake rtmidi liblo
# (only to drive a monome grid)
brew install serialosc && brew services start serialosc

cmake -S . -B build
cmake --build build -j
```

Then install the freshly built filter into VLC.app (one auth prompt — it copies
through Finder to bypass macOS "App Management"):

```sh
scripts/install-plugin.sh
```

### Linux (Debian / Ubuntu)

```sh
sudo apt install build-essential cmake pkg-config ninja-build \
                 librtmidi-dev liblo-dev libvlc-dev vlc
# (only for a monome grid)
sudo apt install serialosc        # then: serialoscd &  (or your distro's service)

cmake -S . -B build -G Ninja
cmake --build build -j            # -> build/libgridtv_plugin.so
```

Install: copy the `.so` into VLC's `plugins/video_filter/` dir and run
`vlc --reset-plugins-cache` (paths in the README).

### Windows

- Visual Studio 2022 (C++) or MinGW-w64, CMake, Git
- **RtMidi** + **liblo**: [vcpkg](https://vcpkg.io). Build them with the **MinGW**
  triplet to match the compiler: `vcpkg install rtmidi liblo --triplet x64-mingw-static
  --overlay-triplets=<vcpkg>/triplets/community`. (vcpkg's default `x64-windows`
  triplet is MSVC and its ABI clashes with MinGW-compiled code.)
- **VLC** from videolan.org (its `sdk/` has the headers + import libs)

```sh
cmake -S . -B build -G Ninja ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
      -DVCPKG_TARGET_TRIPLET=x64-mingw-static ^
      -DVCPKG_OVERLAY_TRIPLETS=<vcpkg>/triplets/community ^
      -DVLC_APP="C:/Program Files/VideoLAN/VLC"
cmake --build build -j          # -> build/gridtv_device_test.exe, ... (the CLI tools)
```

> **Windows status.** The **core library and all CLI tools build** on Windows
> (CI produces `gridtv_device_test.exe`, `gridtv_probe.exe`, …). The **VLC
> plugin `.dll` does not yet build**: VLC's headers use POSIX `poll()`, which
> MinGW doesn't declare, and a Windows plugin also needs VLC's import library
> (`libvlccore.lib`) from the VLC `sdk/`. Closing this needs the VLC Windows
> SDK wired into the build (the roadmap item) — the MinGW path gets everything
> *except* the plugin.

### VLC module headers

The plugin compiles against VLC's *internal* module headers (`vlc_plugin.h`,
`vlc_filter.h`, …). They live under `third_party/vlc-src/include/` locally but
are **gitignored** (large, platform-independent). If you don't have them, fetch
just `include/` from the 3.0.23 tag:

```sh
git clone --depth 1 --branch 3.0.23 --filter=blob:none --sparse \
  https://github.com/videolan/vlc.git third_party/vlc-src
git -C third_party/vlc-src sparse-checkout set include
```

(CI does this automatically — see below.)

---

## Build targets

`cmake --build build --target <name>`:

| Target | What |
|---|---|
| `gridtv` | the static core library (device abstraction + frame processor) |
| `gridtv_plugin` | the VLC video filter — `libgridtv_plugin.dylib/.so/.dll` |
| `gridtv_vlc` | standalone libVLC player (the harness; needs libvlc) |
| `gridtv_device_test` | lights a pad, sweeps, blits a rainbow — verifies the hardware link |
| `gridtv_probe` / `gridtv_midi_probe` | interactive / MIDI-port probes |
| `gridtv_monome_mock` | mock serialosc broker + device for testing the OSC path without hardware |

---

## First light — verify the link

```sh
./build/gridtv_device_test
```

Prints the MIDI ports, then on the hardware: lights **only the top-left pad**
(confirm orientation), sweeps + fills the grid red (confirm every pad), shows a
batched rainbow frame (the exact video hot path), and clears. If step 1 lights
the *bottom-left* pad instead, the grid is vertically flipped — flip the sign in
`note_for()` in `src/devices/launchpad_mini_mk3.cpp`.

The **harness** plays real A/V-synced video through the library:

```sh
VLC_PLUGIN_PATH=/Applications/VLC.app/Contents/MacOS/plugins \
DYLD_LIBRARY_PATH=/Applications/VLC.app/Contents/MacOS/lib \
  ./build/gridtv_vlc video.mp4 --mirror --gridtv-device monome-128
```

`--mirror` routes the harness's frames through the VLC filter (so both code paths
get exercised). Without it, the harness drives a device directly.

To test the monome driver **without hardware**, run the mock (it grabs
`localhost:12002`), then point the filter at `monome-128`:

```sh
MOCK_HOLD=20 ./build/gridtv_monome_mock &    # stays alive 20s; logs the blank on close
# … then drive the filter; watch it log /serialosc/device + level/map stats
```

---

## Architecture

```
VLC video thread ──picture──▶ Filter() ──8×8 RGB──▶ worker thread ──▶ GridDevice ──▶ hardware
   (downscale via VLC image converter)              (colour pipeline + MIDI/OSC blit)
        └── original picture returned unchanged (on-screen playback never affected)
```

- **`GridDevice`** — abstract controller: `{cols, rows, color_model, max_fps}` +
  `blit(RGB8*)`. Each driver reduces the RGB8 frame to its hardware palette.
- **`FrameProcessor`** — high-quality **area-averaging** downscaler (nearest-
  neighbour at 8×8 looks terrible; this is the single biggest quality lever).
- **Two colour families:** RGB (Launchpad mk2/3, future APC/Push) vs luminance
  (Launchpad classic, monome). The processor always outputs normalized RGB8;
  luminance drivers compute luma + ordered dither.
- **Thread split:** `Filter()` runs on VLC's video thread (downscale only —
  `image_Convert` must stay there, it creates VLC sub-objects and deadlocks
  off-thread). `filter_worker()` runs on its own thread (colour LUT + device
  I/O), so a grid/device stall can never affect on-screen video.
- **Colour pipeline** is a 256-entry LUT (`gamma → contrast → lift → grid-gain →
  posterize`) rebuilt only when a param changes, so `pow()` never runs per pixel.
- **Anti-flicker:** `GridDevice::frame_changed()` skips the transport send when
  per-channel delta ≤ the change threshold.
- **Lifecycle:** `Open()` creates the device + worker (async connect, 5s retry);
  `Close()` stops the worker, blanks the grid, disconnects, and frees everything.

### Cross-platform build internals

- **MIDI backend** is chosen by RtMidi via the `__MACOSX_CORE__`/`__LINUX_ALSA__`/
  `__WINDOWS_MM__` macros pkg-config injects. The Apple frameworks
  (`CoreMIDI`/`CoreFoundation`) are linked only `if(APPLE)`.
- **VLC location** is overridable: `-DVLC_APP=/path` (defaults to
  `/Applications/VLC.app`, `/usr`, or the Windows program folder per platform).
- **Plugin filename** is platform-aware: `lib…dylib` (macOS), `lib….so` (Linux),
  `….dll` (Windows).

---

## Device protocols (driver reference)

### Launchpad (Mini MK3 / X / Pro MK3 / MK2 / Pro gen 1) — RGB SysEx

All SysEx carry the Novation header `F0 00 20 29 02 <family> … F7`.

| Family byte | Model | RGB cmd | Entry | RGB range | Port |
|---|---|---|---|---|---|
| `0x0D` | Mini MK3 | `03 03` | `0E 01` (programmer) | 0–63 | `LPMiniMK3 DAW In` / `LPMiniMK3 MIDI In` |
| `0x0C` | X | `03 03` | `0E 01` | 0–127 | `LPX MIDI` |
| `0x0E` | Pro MK3 | `03 03` | `0E 01` | 0–127 | `LPProMK3 MIDI` |
| `0x18` | MK2 | `0B` | `22 00` (Session) | 0–63 | `Mk2` |
| `0x10` | Pro gen1 | `0B` | `21 00` (Ableton) | 0–63 | `Launchpad Pro` (excl. MK3) |

Grid note (origin = physical top-left): `note = 81 − 10·y + x`.
Sources: Novation Programmer's References; cross-checked against `FMMT666/launchpad.py`.

### Launchpad classic (original / S / Mini gen 1) — bi-colour Note-On

- Note = `(y<<4)|x` (X-Y layout, origin top-left).
- Velocity = `(green<<4)|red|0x0C` (red+green 0–3, 16 colours, **no blue**).
- Reset: `B0 00 00`. ~400 msg/s limit → max_fps 6.

### monome grid 128 — serialosc / OSC

- Discovery: send `/serialosc/list si <host> <port>` to `localhost:12002`; broker
  replies `/serialosc/device ssi <id> <type> <port>`.
- This grid **ignores un-prefixed** `/grid/...` — set a prefix and address pads as
  `/monome/grid/...`.
- Full frame = 2× `/monome/grid/led/level/map` (quads x_off 0 and 8), 64 levels
  each (0–15). Luminance `0.299/0.587/0.114` + 4×4 ordered Bayer dither → 0–15.
- Clear: `/monome/grid/led/level/all i 0`.

---

## Continuous integration

`.github/workflows/build.yml` rebuilds for **Linux, macOS and Windows** on every
source change (push to `main`/`master`, or a PR).

| OS | Builds | Artifact |
|---|---|---|
| **Linux** | core lib + CLI tools + harness + VLC plugin | `libgridtv_plugin.so` |
| **macOS** | core lib + CLI tools + harness + VLC plugin | `libgridtv_plugin.dylib` |
| **Windows** | core lib + CLI tools _(plugin not yet)_ | CLI tool `.exe`s (plugin blocked on VLC `poll`/SDK) |

Each run uploads its artifacts (Actions → run → Artifacts). A **`v*` tag**
publishes them to a GitHub **Release** with auto-generated notes. The VLC module
headers are fetched via sparse-checkout of the `3.0.23` tag (see above).

### Lint the workflow locally

A fast `lint` job gates every run with [`actionlint`](https://github.com/rhysd/actionlint)
plus project-specific checks: every CMake `--target` named in the workflow must
exist in `CMakeLists.txt`, and every `uses:` action must be pinned to a tag/SHA
(never `@main`). Run the same checks before pushing:

```sh
scripts/check-ci.sh     # needs actionlint (brew install actionlint) + python3
```

> **Badge:** the README build badge reflects the live `fuzboxz/gridtv` CI status.
> with your GitHub user/org once the repo is pushed.

---

## Roadmap

- [x] GridDevice abstraction, FrameProcessor, all current drivers
- [x] Native VLC video filter — screen + grid, async non-blocking tap
- [x] Cross-platform build + CI with per-OS release artifacts
- [ ] APC40 mkII, Ableton Push, monome 256 drivers
- [ ] Multi-device fan-out; universal auto-detect (Launchpad + monome)
- [ ] Correct batched-SysEx hot path; palette LUT + Floyd–Steinberg
- [ ] Wire the Windows VLC import library so the `.dll` builds in CI

## Contributing

Patches welcome. Please run `scripts/check-ci.sh` and a clean
`cmake --build build -j` (no new warnings in our code) before opening a PR.
Device drivers are one file under `src/devices/` implementing the `GridDevice`
interface — see an existing driver for the shape.

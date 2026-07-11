# gridtv — lo-fi video on grid controllers

[![build](https://github.com/fuzboxz/gridtv/actions/workflows/build.yml/badge.svg)](https://github.com/fuzboxz/gridtv/actions/workflows/build.yml)

**GridTV shows your video on a music grid controller** — a Novation Launchpad or
a monome grid — as a low-resolution light display, **at the same time** as normal
on-screen playback. It is a **non-blocking additive tap**: your on-screen video
and your audio are never changed, slowed down, or replaced. The grid simply
mirrors a downscaled copy of every frame.

It runs inside **VLC media player** as a video filter: turn it on once and every
video you play is mirrored to the grid. There is no separate app to run.

> Building from source, the architecture, CI, and device protocols are covered in
> **[DEVELOPING.md](DEVELOPING.md)**. This README is the end-user guide — you only
> need it if you're *using* GridTV, not building it.

---

## Supported controllers

| Family | Models | Display |
|---|---|---|
| **Launchpad (RGB)** | Mini MK3, X, Pro MK3, MK2, Pro (gen 1) | 8×8 full RGB over USB-MIDI |
| **Launchpad (classic)** | Original Launchpad, Launchpad S, Mini (gen 1) | 8×8 red/green bi-colour (no blue) |
| **monome grid** | grid 128 | 16×8 monochrome, 16 brightness levels (serialosc/OSC) |
| _Planned_ | APC40 mkII, Ableton Push, monome 256 | — |

> **Platform status.** **macOS** (Apple Silicon) is the primary, fully tested
> platform. **Linux** and **Windows** build the same VLC plugin in CI (Windows
> via MinGW); both are lighter-tested on real hardware — please report what
> you find.

---

## Install

You don't need to build anything. Grab the prebuilt plugin for your OS **and
the matching installer** from the **[GitHub Releases](../../releases)** page,
then run the installer — it installs the plugin plus its runtime deps
(rtmidi/liblo) into the right place for you:

```sh
# macOS / Linux
chmod +x install.sh && ./install.sh libgridtv_plugin.dylib   # or .so
# Windows (PowerShell)
powershell -ExecutionPolicy Bypass -File windows.ps1 -Plugin libgridtv_plugin.dll
```

| OS | Plugin | Installer | What it does |
|---|---|---|---|
| **macOS** (Apple Silicon) | `libgridtv_plugin.dylib` | `install.sh` | deps via Homebrew → `~/Library/Application Support/gridtv/` + `VLC_PLUGIN_PATH` (no modifying the signed VLC.app) |
| **Linux** | `libgridtv_plugin.so` | `install.sh` | deps via apt → VLC's system `plugins/video_filter/` (auto-scanned) |
| **Windows** | `libgridtv_plugin.dll` | `windows.ps1` | `%LOCALAPPDATA%\gridtv\` + `VLC_PLUGIN_PATH` |

Then verify: `vlc --list | grep -i gridtv`, and enable GridTV in **VLC → Settings
(Show All) → Video → Filters → GridTV**. (Manual alternative: VLC loads modules
named `lib<name>_plugin.<ext>` from its `plugins/` folder or `VLC_PLUGIN_PATH`;
run `vlc --reset-plugins-cache` if it doesn't pick up a hand-copied file.)

### macOS

1. Download `libgridtv_plugin.dylib` (Apple Silicon / arm64 — on an Intel Mac,
   [build from source](DEVELOPING.md)).
2. In **Finder**, right-click **VLC.app → Show Package Contents → Contents →
   MacOS → plugins**, and drag the file in. Authenticate when prompted.
3. If the filter doesn't appear in VLC, refresh the cache once:
   ```sh
   /Applications/VLC.app/Contents/MacOS/VLC --reset-plugins-cache
   ```

### Linux

VLC loads plugins from its system folder (varies by distro — commonly
`/usr/lib/vlc/plugins/video_filter/`, or `/usr/lib/x86_64-linux-gnu/vlc/plugins/video_filter/`
on Debian/Ubuntu multiarch):

```sh
sudo cp libgridtv_plugin.so /usr/lib/x86_64-linux-gnu/vlc/plugins/video_filter/ \
  || sudo cp libgridtv_plugin.so /usr/lib/vlc/plugins/video_filter/
vlc --reset-plugins-cache
vlc --list | grep -i gridtv     # confirm it loaded
```

…or, without sudo, keep it in a folder you own and point VLC at it:

```sh
mkdir -p ~/vlc-plugins/video_filter && cp libgridtv_plugin.so ~/vlc-plugins/video_filter/
VLC_PLUGIN_PATH=~/vlc-plugins vlc --reset-plugins-cache
# (launch VLC with VLC_PLUGIN_PATH set from then on — e.g. export it in ~/.profile)
```

### Windows

Copy `libgridtv_plugin.dll` into VLC's install `plugins\` folder (needs admin),
then refresh the cache:

```powershell
copy libgridtv_plugin.dll "C:\Program Files\VideoLAN\VLC\plugins\"
& "C:\Program Files\VideoLAN\VLC\vlc.exe" --reset-plugins-cache
```

Without admin, set a user `VLC_PLUGIN_PATH` environment variable to a folder
containing a `video_filter\` subfolder with the plugin inside.

---

## Run

Enable GridTV once: **VLC → Settings (Show All) → Video → Filters → GridTV**.
From then on, just open any video — it plays on your screen **and** the grid,
with audio through your speakers, all in sync.

> **Monome user?** Set up the **serialosc** daemon first (see _Set up your
> controller_ below) — the grid stays dark without it.

Or launch VLC with the filter on directly:

```sh
# macOS
/Applications/VLC.app/Contents/MacOS/VLC --video-filter gridtv video.mp4
# Linux
vlc --video-filter gridtv video.mp4
# Windows
vlc --video-filter gridtv video.mp4
```

---

## Set up your controller

### Launchpad (Mini MK3 / X / Pro MK3 / MK2 / Pro gen 1) — RGB

1. Plug it in over USB. No driver needed on any OS.
2. Leave **Device** on **Auto-detect** (it finds any of these automatically).
3. Play a video. The grid lights up within a second or two.

### Launchpad classic (original / S / Mini gen 1) — red/green only

These older pads have **no blue** and a slower refresh, so they aren't
auto-detected. Set **Device → Launchpad classic** explicitly. Colours map to
red + green only.

### monome grid 128 — 16×8 monochrome

The monome talks over a small helper daemon called **serialosc** (it bridges the
grid's USB link to the network). Install and start it once:

- **macOS:** `brew install serialosc && brew services start serialosc`
- **Linux:** install `serialosc` (+ `libmonome`) if your distro packages it, else build from source — see [monome.org/docs/serialosc/linux](https://monome.org/docs/serialosc/linux); then run `serialoscd`
- **Windows:** get `serialosc.exe` from [monome.org/docs/serialosc](https://monome.org/docs/serialosc) and run it

Then plug in the grid, set **Device → monome grid 128**, and play. Video is
converted to luminance and 16-level dithered.

> If a manually-started `serialoscd` left orphaned `serialosc-device` processes
> holding the serial port, kill them (`pkill -9 -f serialosc` on macOS/Linux)
> and restart the service cleanly.

---

## Switching between devices

GridTV drives **one** controller at a time. To switch:

1. **Stop** the current video.
2. In GridTV settings, pick the new **Device**.
3. **Play** again.

**Device** and **Custom MIDI port** take effect when a video opens, so a switch
needs that one stop-and-replay. **Everything else** updates **live** while video
plays — no restart needed.

> For Launchpads you can usually just leave **Device** on Auto-detect and swap
> the USB cable; GridTV reconnects to whatever Launchpad it finds.

---

## Settings

All settings live in **VLC → Settings (Show All) → Video → Filters → GridTV**,
grouped into **Device · Picture · Colour & tone · Output · Advanced**, and can
also be passed as `--gridtv-<name>` flags. Most update **live** while video plays
(only **Device** / **Custom MIDI port** need a stop-and-replay).

| Setting | Type | Default | What it does |
|---|---|---|---|
| **Device** | dropdown | `auto` | Which controller: `auto` (detect any Launchpad), a specific model, `monome grid 128`, `Launchpad classic`, or `custom` |
| **Custom MIDI port** | text | — | Used only when Device = `custom`; a substring of the MIDI port name (see Tools → Messages) |
| **Aspect / fit** | dropdown | `Fill` | `Fill` (crop to fill), `Fit` (letterbox / show all), `Stretch` (distort to fill) |
| **Colour pick** | dropdown | `Dominant` | How each pad's colour is sampled: `Dominant` (crisp, default), `Average` (smooth), `Brightest`, `Median`, `Centre` |
| **Video brightness (gamma)** | float 0.2–3 | `1.0` | Gamma on the source — above 1 brightens dark video / reveals shadow; below 1 darkens |
| **Contrast** | float 0–3 | `1.0` | Punch (<1 softer, >1 punchier) |
| **Saturation** | float 0–3 | `1.0` | Colour saturation (0 = greyscale; no effect on monochrome devices) |
| **Black lift** | float 0–1 | `0.0` | Raises the black floor so "off" reads dim-grey instead of fully dark |
| **Bit depth** | int 1–8 | `8` | Posterize each channel for a chunky retro look (8 = full colour) |
| **LED display curve (gamma)** | float 1–3 | `2.0` | Makes grid colours/brightness match the screen. LEDs are ~linear while video is gamma-encoded, so raw values look flat/washed — this deepens mid-tones (2.0–2.4 = vibrant & screen-accurate; 1.0 = flat/raw). Applies to every device |
| **Grid brightness** | float 0–1 | `1.0` | Master dim of the whole grid (dims the lift floor too) |
| **Max FPS** | int 0–60 | `0` | Frame-rate cap (0 = device default; the grid rarely needs >25) |
| **Change threshold** | int 0–64 | `0` | Anti-flicker: re-send only when a channel changes by more than this (raise to 2–4 to freeze near-static content) |
| **Colour order** | dropdown | `RGB` | Leave on `RGB`; switch to `BGR` only if red/blue look swapped (debug) |
| **Debug logging** | bool | off | Log one line per frame to the VLC message log (Advanced) |

---

## If the grid stays dark

1. **Confirm VLC sees the filter** — it must be enabled under
   **Settings → Video → Filters → GridTV**.
2. **Check the device** — open **VLC → Tools → Messages**. GridTV prints every
   MIDI port it can see (copy the exact name if you use **Custom**) and a
   plain-English reason if a device can't be reached.
3. **monome?** Make sure `serialosc` is running and the grid is plugged in.
4. **Launchpad?** Check the USB cable and that **Device** matches a port from the
   message log (or use **Auto-detect**).

### Colours look washed out / not as vibrant as the screen

This is expected with the default raw mapping and is what **LED display curve
(gamma)** fixes. Controller LEDs are driven ~linearly while video is gamma-
encoded, so straight values come out flat. While a familiar scene plays, raise
**LED display curve** to **2.0–2.4** (the default) so greys and skin tones look
about as bright on the grid as on screen; bump **Saturation** a little (e.g.
1.2–1.4) for extra pop. (LEDs also have a smaller colour gamut than a monitor,
so a perfect match isn't possible — but this gets close.)

When you close VLC, GridTV automatically blanks the grid and disconnects cleanly.

---

_Building GridTV from source, the internals, and contributing are documented in
[DEVELOPING.md](DEVELOPING.md)._

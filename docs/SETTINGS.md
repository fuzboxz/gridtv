# GridTV settings — full guide

Every GridTV option lives in **VLC → Settings → Show All → Video → Filters →
GridTV**, and can also be passed as a `--gridtv-<name>` flag on the command line.
This document explains what each one actually does, why it exists, and how to
use it.

> **All settings except *Device* and *Custom MIDI port* are live:** change them
> while a video is playing and the grid updates instantly — no restart. When you
> change any of them, GridTV rebuilds its colour pipeline and forces one clean
> full redraw of the grid, so you never get a half-updated, glitchy frame.
> (*Device* and *Custom MIDI port* are read when a media item opens; stop and
> replay to switch hardware.)

The settings are grouped the same way VLC shows them.

---

## Device

### Device
`gridtv-device` · dropdown · default `Auto-detect`

Which controller receives the video.

- **Auto-detect** — finds any connected RGB Launchpad (Mini MK3, X, Pro MK3,
  MK2, or Pro gen 1). Start here.
- A specific model — pick this if you have more than one Launchpad and want to
  target a particular one, or if auto-detect picks the wrong port.
- **monome grid 128** — for a monome grid (reached through the `serialosc`
  daemon, which must be running).
- **Launchpad classic** — for the original Launchpad, Launchpad S, or Mini gen 1
  (red/green only, no blue).
- **Custom** — type the MIDI output port name into *Custom MIDI port* below.

If the grid stays dark, switch to **Auto-detect** and check **VLC → Tools →
Messages**, where GridTV lists every MIDI port it can see.

### Custom MIDI port
`gridtv-port` · text · default _(empty)_

Used **only** when *Device = Custom*. Type any unique part of the MIDI output
port name shown in Tools → Messages — a substring match is enough (e.g.
`Launchpad`).

---

## Picture (fit & quality)

How the video is framed and sampled onto the grid.

### Aspect / fit
`gridtv-aspect` · dropdown · default `Fill`

How the video is fitted to the (usually non-square) grid shape.

- **Fill (default)** — zoom in so every pad is covered, cropping the edges. Best
  when you want the whole grid lit with no dead area.
- **Fit** — shrink to show the entire frame, with black bars (letterbox) on the
  sides/top. Best when you must see the whole image.
- **Stretch** — distort the frame to fill every pad (ignores aspect ratio). Use
  only if you don't mind squashed geometry.

### Colour pick
`gridtv-colorpick` · dropdown · default `Salient`

How each pad's colour is derived from the block of video behind it. This is the
single biggest lever for how the grid "reads."

- **Salient (default)** — an area-average weighted toward **detail / subject**
  pixels, so the subject shows through instead of being flattened into the
  background. Good all-rounder.
- **Dominant** — the most common colour in the block (crisp; avoids the muddy
  average look on busy scenes).
- **Vibrant** — like Dominant but biased toward **saturated** colours, so vivid
  colours win over large grey areas. Punchy.
- **Average** — a true area-mean (every source pixel behind the pad is averaged).
  The smoothest, most colour-accurate, but can look soft/muddy on busy scenes.
- **Brightest** — the brightest pixel in the block. Emphasises highlights
  (explosions, lights).
- **Median** — the median colour. A robust middle that ignores outliers.
- **Centre** — the single centre pixel. Sharpest and noisiest.

> For natural video, **Salient** or **Dominant** usually look best. Switch to
> **Vibrant** for animation or high-contrast graphics.

### Smoothing (temporal)
`gridtv-smoothing` · dropdown · default `Off`

Blends each grid frame with the previous one (an exponential moving average) to
steady the colours and suppress per-frame shimmer/flicker.

- **Off** — every frame raw (sharpest in time; can shimmer on noisy video).
- **Light / Medium / Strong** — progressively more smoothing. **Strong** can
  leave a brief trailing smear on very fast motion.

This is a big readability win on noisy or fast-moving video and pairs well with a
low *Change threshold*. It is independent of *Change threshold* (a hard
send/don't-send gate); smoothing is a soft temporal blend.

### Dither
`gridtv-dither` · dropdown · default `Off`

Spreads the device's coarse brightness steps (and the *Bit depth* posterize)
into a fine, stable checkerboard (**Bayer** pattern) so gradients look smooth
instead of banded. The pattern is spatially fixed, so it's flicker-free.

- **Off** — raw levels.
- **Ordered (Bayer)** — dithered.

Mainly helps **Launchpads** (64 or 128 brightness levels). It has **no effect on
the monome** (which already dithers internally) or where the device is
effectively continuous. Turn it on if you see visible banding in skies,
gradients, or skin tones.

### Sharpen (local contrast)
`gridtv-sharpen` · dropdown · default `Light`

A mild **unsharp mask** on the grid (`grid + k·(grid − 3×3 blur)`) that crisps
pad-to-pad edges the downscale softened, so the image reads a little
sharper/more defined.

- **Off** — no sharpening.
- **Light (default)** — subtle.
- **Strong** — punchy; can emphasise noise on already-busy video.

Runs after temporal smoothing (so it isn't smoothed away) and before dither. No
effect on a 1×N grid.

---

## Colour & tone

The "look" of the image on the grid.

### Video brightness (gamma)
`gridtv-gamma` · float 0.2–3.0 · default `1.0`

A **gamma curve applied to the source content** before anything else. Above 1.0
brightens dark video and lifts shadow detail; below 1.0 darkens it. This is a
creative control — separate from *Grid brightness* (which scales the final
output) and from *LED display curve* (which matches the device).

### Contrast
`gridtv-contrast` · float 0.0–3.0 · default `1.0`

Contrast of the grid image: `1.0` = unchanged, below `1.0` = softer/flatter,
above `1.0` = punchier.

### Saturation
`gridtv-saturation` · float 0.0–3.0 · default `1.0`

Colour saturation: `1.0` = unchanged, `0` = greyscale, above `1.0` = more vivid.
Boost this (e.g. 1.2–1.4) if colours look muted — controller LEDs have a smaller
colour gamut than a monitor, so a little extra saturation recovers perceived
punch. **No effect on monochrome devices** (monome, Launchpad classic).

### Black lift
`gridtv-lift` · float 0.0–1.0 · default `0.0`

Raises the black floor so "off" reads as a dim grey instead of fully dark.
Useful where total black makes the grid look broken (some scenes read as "the
grid is off"). `0` = pure black.

### Bit depth
`gridtv-bits` · int 1–8 · default `8`

Posterize each colour channel to this many bits: `8` = full colour, lower =
fewer, chunkier colours for a retro look. Combine with **Dither = Ordered** to
smooth the resulting bands.

### LED display curve (gamma)
`gridtv-ledgamma` · float 1.0–3.0 · default `2.0`

**Compensates for the controller's near-linear LED response so the grid's
colours and brightness match your screen.** LED levels are roughly linear in
light output, but video is gamma-encoded — so without correction (1.0) mid-tones
look flat/washed out and colours lose punch. `2.0–2.4` deepens them to look
vibrant and screen-accurate; raise it for a punchier, contrasty look, lower
toward 1.0 for a flatter raw look. Applies to every device.

> **Tip:** play a familiar scene and set this so the grid's greys and skin tones
> look about as bright as on screen. `2.0` is a good start; `2.2` is exact sRGB.

---

## Output

Final output level, speed, and stability.

### Grid brightness
`gridtv-brightness` · float 0.0–1.0 · default `1.0`

Master output gain of the grid. `1.0` = full brightness; lower dims everything
(including the black-lift floor). Use this to protect your eyes in a dark room
or to match room lighting.

### Max FPS
`gridtv-fps` · int 0–60 · default `0`

Frame-rate cap for the grid. `0` = use the device's safe default (Launchpads
30, monome higher). Lower values save USB bandwidth and reduce flicker; the grid
rarely needs more than ~25 fps.

### Change threshold
`gridtv-delta` · int 0–64 · default `0`

Anti-flicker: only send a frame to the grid if some colour channel changed by
more than this since the last frame.

- `0` — send every frame (most accurate; can flicker on static sensor noise).
- `2–4` — freeze near-static content so a paused or still image stops
  shimmering, while still updating on real motion.

### Colour order
`gridtv-color` · dropdown · default `RGB`

Byte order of the colour data. Leave on **RGB** for normal use. Switch to **BGR**
only if red and blue look swapped on your setup — a debugging control that is
almost never needed.

---

## Advanced

### Debug logging
`gridtv-debug` · bool · default `off`

Log one line per displayed frame to the VLC message log (**Tools → Messages**),
including grid size and a sample colour. Useful for confirming the tap is
running. (Requires a filter re-open to take effect.)

---

## Recipes

A few known-good starting points:

- **Natural video (default look):** Colour pick `Salient`, LED display curve
  `2.0`, Sharpen `Light`. Add Smoothing `Light` on noisy footage.
- **Vivid / animation / graphics:** Colour pick `Vibrant`, Saturation `1.3`,
  LED display curve `2.2`, Dither `Ordered`.
- **Retro / chunky:** Bit depth `3`, Dither `Ordered`, Colour pick `Dominant`.
- **Dark room, easy on the eyes:** Grid brightness `0.6`, Black lift `0.05`,
  LED display curve `2.2`.

## How settings are applied (and why live changes are safe)

Every live setting is read on VLC's own thread (in `Filter`), clamped, and handed
to the grid worker as an immutable **snapshot** together with each frame. The
worker never touches VLC's configuration directly — which is why changing a
setting while a video plays cannot crash or race VLC (an earlier build did crash
here; the snapshot design fixed it). On any change the worker rebuilds its colour
LUT, drops the device's cached "last frame" (forcing one full redraw), and resets
the temporal-smoothing history, so the grid snaps cleanly to the new look.

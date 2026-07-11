#pragma once

#include <cstdint>

namespace gridtv {

// A grid device's representable color model. The FrameProcessor always produces
// a normalized RGB8 image at device geometry; each driver reduces that image to
// whatever its hardware can actually show:
//   - RGB devices  (Launchpad mk2/mk3, APC40mk2, Push): palette/quantize per pad
//   - Luma devices (Launchpad mk1, monome):            luminance + dither to N levels
// This single branch is the heart of the portability story.
enum class ColorModel {
    RGB,
    Luminance,
};

struct RGB8 {
    std::uint8_t r, g, b;
};

} // namespace gridtv

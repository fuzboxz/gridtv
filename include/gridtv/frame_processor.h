#pragma once

#include "gridtv/color.h"

#include <cstdint>

namespace gridtv {

// How the source frame's aspect ratio is mapped onto the (usually non-matching)
// grid geometry.
enum class AspectMode {
    Stretch,    // ignore aspect; fill every cell (may distort)
    Cover,      // crop source to grid aspect, then fill (default; looks best)
    Letterbox,  // fit inside grid, leaving cells black (TODO)
};

enum class ChannelOrder {
    RGB, // memory order: R, G, B
    BGR, // memory order: B, G, R  (libVLC "RV24" vmem output on little-endian)
};

// High-quality downscaler. Area-averaging (box) filter from any source
// resolution down to a target grid geometry, producing a normalized RGB8 image
// suitable for any GridDevice::blit(). Area-averaging is the single biggest
// determinant of perceived quality at 8x8 -- nearest-neighbour looks like noise.
class FrameProcessor {
public:
    FrameProcessor(int target_cols, int target_rows, AspectMode mode = AspectMode::Cover);

    // `src` is a packed pixel buffer of `height` rows, each `src_stride` bytes
    // wide (>= width * bytes_per_pixel; allows VLC's padded picture pitches),
    // each pixel `bytes_per_pixel` bytes in the given channel order.
    // `out` receives cols() * rows() RGB8 values, row-major (y == 0 first).
    void process(const std::uint8_t* src, int src_stride, int width, int height,
                 int bytes_per_pixel, ChannelOrder order, RGB8* out) const;

    int cols() const { return cols_; }
    int rows() const { return rows_; }

private:
    int cols_;
    int rows_;
    AspectMode aspect_;
};

} // namespace gridtv

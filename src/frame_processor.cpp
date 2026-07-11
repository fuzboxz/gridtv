#include "gridtv/frame_processor.h"

#include <algorithm>
#include <cstdint>

namespace gridtv {

FrameProcessor::FrameProcessor(int target_cols, int target_rows, AspectMode mode)
    : cols_(target_cols), rows_(target_rows), aspect_(mode) {}

// Average the source pixels within [x0,x1) x [y0,y1) into a single RGB8 value.
static void box_average(const std::uint8_t* src, int src_stride, int bpp,
                        ChannelOrder order, int x0, int y0, int x1, int y1,
                        RGB8* out) {
    std::uint64_t sr = 0, sg = 0, sb = 0, n = 0;
    const int ri = (order == ChannelOrder::RGB) ? 0 : 2;
    const int bi = (order == ChannelOrder::RGB) ? 2 : 0;
    for (int y = y0; y < y1; ++y) {
        const std::uint8_t* row = src + static_cast<std::size_t>(y) * src_stride;
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t* p = row + x * bpp;
            sr += p[ri];
            sg += p[1];
            sb += p[bi];
            ++n;
        }
    }
    if (n == 0) n = 1;
    out->r = static_cast<std::uint8_t>(sr / n);
    out->g = static_cast<std::uint8_t>(sg / n);
    out->b = static_cast<std::uint8_t>(sb / n);
}

void FrameProcessor::process(const std::uint8_t* src, int src_stride, int width,
                             int height, int bytes_per_pixel, ChannelOrder order,
                             RGB8* out) const {
    if (cols_ <= 0 || rows_ <= 0 || width <= 0 || height <= 0 || src == nullptr) {
        if (cols_ > 0 && rows_ > 0)
            for (int i = 0; i < cols_ * rows_; ++i) out[i] = {0, 0, 0};
        return;
    }

    // Source crop region (only Cover does any cropping).
    int rx0 = 0, ry0 = 0, rw = width, rh = height;
    if (aspect_ == AspectMode::Cover) {
        const double src_aspect = static_cast<double>(width) / height;
        const double dst_aspect = static_cast<double>(cols_) / rows_;
        if (src_aspect > dst_aspect) {
            rw = std::max(1, static_cast<int>(height * dst_aspect));
            rx0 = (width - rw) / 2;
        } else {
            rh = std::max(1, static_cast<int>(width / dst_aspect));
            ry0 = (height - rh) / 2;
        }
    }

    for (int oy = 0; oy < rows_; ++oy) {
        int sy0 = ry0 + (oy * rh) / rows_;
        int sy1 = ry0 + ((oy + 1) * rh) / rows_;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > ry0 + rh) sy1 = ry0 + rh;

        for (int ox = 0; ox < cols_; ++ox) {
            int sx0 = rx0 + (ox * rw) / cols_;
            int sx1 = rx0 + ((ox + 1) * rw) / cols_;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > rx0 + rw) sx1 = rx0 + rw;

            box_average(src, src_stride, bytes_per_pixel, order, sx0, sy0, sx1, sy1,
                        &out[oy * cols_ + ox]);
        }
    }
}

} // namespace gridtv

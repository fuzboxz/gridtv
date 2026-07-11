#pragma once

#include "gridtv/color.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gridtv {

// Abstract target grid. Every supported controller implements this; the rest of
// the program never knows whether frames travel over MIDI/SysEx, OSC, or USB.
//
// Coordinate system: (x, y), origin top-left physical pad, x in [0, cols()),
// y in [0, rows()).
class GridDevice {
public:
    virtual ~GridDevice() = default;

    virtual std::string name() const = 0;
    // Human-readable connection summary for logs (e.g. "Launchpad ... via 'port'").
    virtual std::string describe() const { return name(); }
    virtual int cols() const = 0;
    virtual int rows() const = 0;
    virtual ColorModel color_model() const = 0;

    // Sustained full-frame rate the device can absorb without choking. 0 = unlimited.
    // Callers should drop frames faster than this rather than flooding the transport.
    virtual int max_fps() const = 0;

    // Open the underlying transport (MIDI / OSC). Throws std::runtime_error on failure.
    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    virtual void blit(const RGB8* px) = 0;
    virtual void clear() = 0;

    // --- change detection (flicker reduction) ---
    // Frames whose per-channel delta vs the previous frame is <= the threshold
    // are treated as unchanged and skipped at the transport. 0 = require an exact
    // match (fully static on static content). Applied by each driver's blit().
    void set_change_threshold(int t) { change_threshold_ = t < 0 ? 0 : t; }
    int change_threshold() const { return change_threshold_; }

    // Drop the cached last frame so the next blit() always sends the full grid.
    // Call after an external change (a setting tweak, a reconnect) that alters
    // the rendered colours, so frame_changed() can't suppress the refresh by
    // comparing against stale, pre-change colours.
    void invalidate_last_frame() { last_frame_.clear(); }

protected:
    // True if `px` differs from the cached last frame beyond the threshold (and
    // caches it). Drivers call this at the top of blit().
    bool frame_changed(const RGB8* px) {
        const std::size_t n = static_cast<std::size_t>(cols()) * rows();
        if (last_frame_.size() != n) { last_frame_.assign(px, px + n); return true; }
        const int t = change_threshold_;
        for (std::size_t i = 0; i < n; ++i) {
            if (std::abs(static_cast<int>(last_frame_[i].r) - px[i].r) > t ||
                std::abs(static_cast<int>(last_frame_[i].g) - px[i].g) > t ||
                std::abs(static_cast<int>(last_frame_[i].b) - px[i].b) > t) {
                std::memcpy(last_frame_.data(), px, n * sizeof(RGB8));
                return true;
            }
        }
        return false;
    }

private:
    std::vector<RGB8> last_frame_;
    int change_threshold_ = 0;
};

} // namespace gridtv

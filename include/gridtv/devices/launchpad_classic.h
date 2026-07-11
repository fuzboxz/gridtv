#pragma once

#include "gridtv/grid_device.h"

#include <RtMidi.h>

#include <memory>
#include <string>
#include <vector>

namespace gridtv {

// Original Novation Launchpad, Launchpad S, and Launchpad Mini (gen1) -- the
// pre-MK2 bi-colour generation. 8x8 grid of red+green LEDs (0..3 each => 16
// colours; no blue element).
//
// Transport: USB-MIDI channel 1 (RtMidi).
// X-Y layout (default): MIDI key = 16*row + column, origin = top-left pad.
// LED colour is set by note-on velocity = (green<<4) | red | 0x0C, where the
// 0x0C sets the Copy+Clear bits (normal, non-flashing use).
// All-off / reset: controller B0h 00h 00h.
//
// Hardware limit: ~400 MIDI messages/second, so per-pad updates (64/frame) cap
// the sustainable rate around 6 fps -- max_fps() reflects that.
class LaunchpadClassic : public GridDevice {
public:
    explicit LaunchpadClassic(std::string port_query = "");
    ~LaunchpadClassic() override;

    std::string name() const override { return "Launchpad (classic)"; }
    std::string describe() const override;
    int cols() const override { return 8; }
    int rows() const override { return 8; }
    ColorModel color_model() const override { return ColorModel::RGB; }
    int max_fps() const override { return 6; }  // 400 msg/s / 64 pads

    void connect() override;
    void disconnect() override;
    bool is_connected() const override { return connected_; }

    void blit(const RGB8* px) override;
    void clear() override;

    static std::vector<std::string> list_ports();
    const std::string& connected_port() const { return connected_port_; }

private:
    void select_port();
    void send(std::uint8_t a, std::uint8_t b, std::uint8_t c);
    static bool looks_classic(const std::string& port);  // a Launchpad, but not MK2/MK3/X

    std::unique_ptr<RtMidiOut> midi_;
    std::string port_query_;
    std::string connected_port_;
    bool connected_ = false;
};

} // namespace gridtv

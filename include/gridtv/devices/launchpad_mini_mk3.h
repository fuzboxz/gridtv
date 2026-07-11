#pragma once

#include "gridtv/grid_device.h"

#include <RtMidi.h>

#include <memory>
#include <string>
#include <vector>

namespace gridtv {

// Novation Launchpad MK3 generation: the Mini MK3 and Pro MK3 are both 8x8 RGB
// grids driven by the same SysEx family. Only three things differ between them:
//
//   Mini MK3 : family 0x0D, Programmer-mode port "LPMiniMK3 DAW In", RGB 0..63
//   Pro  MK3 : family 0x0E, Programmer-mode port "LPProMK3 MIDI In", RGB 0..127
//
// Transport: USB-MIDI via CoreMIDI (RtMidi).
// Protocol (Programmer layout), SysEx carry the Novation header
//   F0 00 20 29 02 <family> ... F7
//   enter programmer mode : ... 0E 01 F7
//   set pad RGB (single)   : ... 03 03 <note> <r> <g> <b> F7   (r,g,b scaled to range)
// Grid note numbering (origin = physical top-left pad):
//   note = 81 - 10*y + x     for x,y in [0,7]
class LaunchpadMk3 : public GridDevice {
public:
    enum class Model { Mini, X, Pro, Mk2, ProGen1, None };

    // Defaults to the Mini model (back-compat). `port_query` selects a MIDI
    // output port by case-insensitive substring; empty -> the model's default.
    explicit LaunchpadMk3(std::string port_query = "");
    LaunchpadMk3(Model model, std::string port_query = "");
    ~LaunchpadMk3() override;

    std::string name() const override { return name_; }
    std::string describe() const override;
    int cols() const override { return 8; }
    int rows() const override { return 8; }
    ColorModel color_model() const override { return ColorModel::RGB; }
    int max_fps() const override { return 30; }

    void connect() override;
    void disconnect() override;
    bool is_connected() const override { return connected_; }

    // Full-frame update as per-pad RGB SysEx.
    void blit(const RGB8* px) override;
    void clear() override;

    // Per-pad setter using the verified single-LED SysEx form (channels scaled
    // to the device range). Used by the device test to confirm the link.
    void set_led(int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b);

    // --- lower-level / diagnostics ---
    void select_programmer_layout();
    void note_on(int note, int velocity);
    void send_raw(std::uint8_t status, std::uint8_t d1 = 0, std::uint8_t d2 = 0);

    // Enumerate MIDI output port names visible to the system.
    static std::vector<std::string> list_ports();
    // Scan the ports and return which mk3 Launchpad (Mini/X/Pro) is connected
    // (Model::None if none). Used by the "auto" device setting.
    static Model detect(std::string* out_port = nullptr);
    // Name of the port the driver opened (valid after connect()).
    const std::string& connected_port() const { return connected_port_; }

private:
    void send_sysex(const std::vector<unsigned char>& body);  // body without F0/F7
    void select_port();
    void init(Model model);
    std::uint8_t to_device(std::uint8_t v) const;  // scale 0..255 -> 0..rgb_max_

    std::unique_ptr<RtMidiOut> midi_;
    std::string port_query_;
    std::string connected_port_;
    std::string name_;
    std::string default_needle_;
    std::string default_exclude_;  // substring to reject when auto-selecting (e.g. "MK3" for the gen1 Pro)
    std::vector<unsigned char> rgb_prefix_;   // bytes between family and <note> in the RGB SysEx
    std::vector<unsigned char> entry_body_;   // bytes after family to enter Programmer/Session mode
    unsigned char family_ = 0x0D;
    int rgb_max_ = 63;
    bool connected_ = false;
};

// Back-compat alias; constructing it defaults to the Mini model.
using LaunchpadMiniMK3 = LaunchpadMk3;

} // namespace gridtv

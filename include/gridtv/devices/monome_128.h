#pragma once

#include "gridtv/grid_device.h"

#include <lo/lo.h>

#include <string>

namespace gridtv {

// monome grid 128: 16x8 monochrome, 16-level varibright LEDs. Transport is OSC
// over UDP to the serialosc daemon (discovery on localhost:12002), which proxies
// to the physical device. This is the Luminance colour family: RGB frames are
// reduced to perceptual luminance + ordered dithering to 0..15.
//
// Reference: https://monome.org/docs/serialosc/osc/
class Monome128 : public GridDevice {
public:
    Monome128() = default;
    ~Monome128() override;

    std::string name() const override { return "monome grid 128"; }
    std::string describe() const override {
        return name() + (id_.empty() ? std::string() : (" (" + id_ + ")"));
    }
    int cols() const override { return 16; }
    int rows() const override { return 8; }
    ColorModel color_model() const override { return ColorModel::Luminance; }
    int max_fps() const override { return 30; }

    void connect() override;
    void disconnect() override;
    bool is_connected() const override { return connected_; }

    void blit(const RGB8* px) override;
    void clear() override;

private:
    lo_address dev_addr_ = nullptr;  // address of the per-device serialosc port
    std::string id_;                 // serialosc device id (e.g. "m1234567")
    bool connected_ = false;
};

} // namespace gridtv

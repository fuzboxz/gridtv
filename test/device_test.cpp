// Standalone device test. Run this to confirm the MIDI link to your Launchpad
// Mini MK3 and verify grid orientation BEFORE driving it with video.
//
//   ./gridtv_device_test
//
// Sequence:
//   1. light ONLY pad (0,0) red -> confirms which physical pad maps to top-left
//   2. sweep+fill the whole grid red -> confirms every pad responds
//   3. batched full-frame rainbow -> confirms the video hot path (single SysEx)
//   4. clear

#include "gridtv/devices/launchpad_mini_mk3.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace gridtv;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    const std::string query = (argc > 1) ? argv[1] : "";
    std::printf("MIDI output ports visible to the system:\n");
    for (const auto& p : LaunchpadMiniMK3::list_ports()) std::printf("  %s\n", p.c_str());
    std::printf("\n");

    LaunchpadMiniMK3 lp(query);
    try {
        lp.connect();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "connect failed: %s\n", e.what());
        return 1;
    }
    std::printf("Connected to %s via port '%s' (%dx%d, %s).\n", lp.name().c_str(),
                lp.connected_port().c_str(), lp.cols(), lp.rows(),
                lp.color_model() == ColorModel::RGB ? "RGB" : "Luminance");

    // 1) Orientation check: top-left pad only.
    std::printf("[1/4] lighting top-left pad (0,0) red for 2.5s... (is it physical top-left?)\n");
    lp.set_led(0, 0, 63, 0, 0);
    std::this_thread::sleep_for(2500ms);
    lp.clear();

    // 2) Fill sweep: light each pad in turn and leave it on.
    std::printf("[2/4] sweeping+filling grid...\n");
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            lp.set_led(x, y, 63, 0, 0);
            std::this_thread::sleep_for(120ms);
        }
    }
    std::this_thread::sleep_for(800ms);
    lp.clear();

    // 3) Batched full-frame rainbow (the exact path video frames take).
    std::printf("[3/4] full-frame rainbow (per-pad) for 3s... (smooth gradient = video path OK)\n");
    std::vector<RGB8> frame(64);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            frame[y * 8 + x] = {static_cast<std::uint8_t>(x * 32),
                                static_cast<std::uint8_t>(y * 32),
                                static_cast<std::uint8_t>(128)};
        }
    }
    lp.blit(frame.data());
    std::this_thread::sleep_for(3000ms);

    // 4) Clear.
    std::printf("[4/4] clearing.\n");
    lp.clear();
    lp.disconnect();
    std::printf("done.\n");
    return 0;
}

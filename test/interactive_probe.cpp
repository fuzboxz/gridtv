// Interactive probe: holds each pattern long enough to observe and tries several
// independent lighting methods on each MIDI port, so we can tell whether the
// issue is the port, programmer-mode entry, the batched SysEx form, or MIDI
// delivery itself.
//
//   ./gridtv_probe            # tries "DAW" then "MIDI" ports
//   ./gridtv_probe MIDI       # one port only

#include "gridtv/devices/launchpad_mini_mk3.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace gridtv;

static void hold(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static void say(const std::string& s) { std::printf("\n==== %s ====\n", s.c_str()); std::fflush(stdout); }

static void perpad_white(LaunchpadMiniMK3& lp) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) lp.set_led(x, y, 63, 63, 63);
}
static void noteon_all(LaunchpadMiniMK3& lp, int vel) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) lp.note_on(81 - 10 * y + x, vel);
}

int main(int argc, char** argv) {
    std::vector<std::string> queries = (argc > 1)
        ? std::vector<std::string>{argv[1]}
        : std::vector<std::string>{"DAW", "MIDI"};

    for (const auto& q : queries) {
        say("PORT QUERY: \"" + q + "\"");
        LaunchpadMiniMK3 lp(q);
        try {
            lp.connect();
        } catch (const std::exception& e) {
            std::printf("  connect failed: %s -- skipping\n", e.what());
            continue;
        }
        std::printf("  opened output port: '%s'\n", lp.connected_port().c_str());

        say("[1] Note-On palette, all pads (vel 120) -- hold 6s");
        noteon_all(lp, 120);
        hold(6000);
        lp.clear();

        say("[2] programmer MODE + per-pad RGB SysEx white (verified form) -- hold 6s");
        perpad_white(lp);
        hold(6000);
        lp.clear();

        say("[3] + programmer LAYOUT (0x7F) + per-pad white -- hold 6s");
        lp.select_programmer_layout();
        hold(80);
        perpad_white(lp);
        hold(6000);
        lp.clear();

        say("[4] programmer mode + BATCHED full-frame white (video hot path) -- hold 6s");
        std::vector<RGB8> white(64, {63, 63, 63});
        lp.blit(white.data());
        hold(6000);
        lp.clear();

        lp.disconnect();
        hold(500);
    }

    std::printf("\nprobe complete.\n");
    return 0;
}

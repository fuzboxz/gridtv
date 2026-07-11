// Bidirectional MIDI probe for the Launchpad. Answers the key question: is the
// device actually talking to the host over CoreMIDI?
//   1. sends a MIDI Identity Request (F0 7E 7F 06 01 F7) -- every device replies
//   2. listens 1.5s for the reply
//   3. asks you to press a pad, listens 4s for button messages
// If both are silent, the MIDI link itself is broken (permissions/driver). If the
// reply comes back but LEDs stay dark, the problem is in the lighting commands.

#include <RtMidi.h>

#include <chrono>
#include <cctype>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static int find_port(RtMidi& m, const std::string& needle) {
    const std::string nl = lower(needle);
    for (unsigned i = 0; i < m.getPortCount(); ++i)
        if (lower(m.getPortName(i)).find(nl) != std::string::npos) return static_cast<int>(i);
    return -1;
}

static void drain(RtMidiIn& in, int ms, const char* label) {
    std::printf("%s", label);
    std::fflush(stdout);
    bool any = false;
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        std::vector<unsigned char> msg;
        in.getMessage(&msg);
        if (!msg.empty()) {
            any = true;
            std::printf("  RX(%zu):", msg.size());
            for (auto b : msg) std::printf(" %02X", b);
            std::printf("\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    if (!any) std::printf("  (nothing received)\n");
}

int main() {
    try {
        RtMidiOut out;
        RtMidiIn in;

        std::printf("OUT (host -> device) ports:\n");
        for (unsigned i = 0; i < out.getPortCount(); ++i)
            std::printf("  [%u] %s\n", i, out.getPortName(i).c_str());
        std::printf("IN (device -> host) ports:\n");
        for (unsigned i = 0; i < in.getPortCount(); ++i)
            std::printf("  [%u] %s\n", i, in.getPortName(i).c_str());

        // Host -> device output: prefer the "...MIDI In" endpoint.
        int oi = find_port(out, "LPMiniMK3 MIDI In");
        if (oi < 0) oi = find_port(out, "Launchpad Mini MK3");
        // Device -> host input: prefer the "...MIDI Out" endpoint.
        int ii = find_port(in, "LPMiniMK3 MIDI Out");
        if (ii < 0) ii = find_port(in, "Launchpad Mini MK3");

        if (oi < 0 || ii < 0) {
            std::printf("Could not find both Launchpad endpoints (oi=%d ii=%d).\n", oi, ii);
            return 1;
        }
        std::printf("\nUsing OUT [%d] %s\n", oi, out.getPortName(oi).c_str());
        std::printf("Using IN  [%d] %s\n", ii, in.getPortName(ii).c_str());

        out.openPort(oi, "probe-out");
        in.openPort(ii, "probe-in");
        in.ignoreTypes(false, false, false); // receive sysex, active-sensing, clock

        std::printf("\n[1] Sending Identity Request, listening 1.5s...\n");
        std::vector<unsigned char> idreq = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
        out.sendMessage(&idreq);
        drain(in, 1500, "  reply: ");

        std::printf("\n[2] Press ANY pad on the Launchpad now (4s window)...\n");
        drain(in, 4000, "  buttons: ");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

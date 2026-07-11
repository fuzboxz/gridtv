#include "gridtv/devices/launchpad_classic.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace gridtv {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Scale an 8-bit channel to the classic's 0..3 brightness, rounded.
std::uint8_t q3(std::uint8_t v) {
    int x = (static_cast<int>(v) * 3 + 127) / 255;
    return static_cast<std::uint8_t>(x > 3 ? 3 : x);
}

} // namespace

LaunchpadClassic::LaunchpadClassic(std::string port_query)
    : port_query_(std::move(port_query)) {
    try {
        midi_ = std::make_unique<RtMidiOut>();
    } catch (...) {
        midi_.reset();
    }
}

LaunchpadClassic::~LaunchpadClassic() { disconnect(); }

std::string LaunchpadClassic::describe() const {
    return name() + (connected_port_.empty() ? std::string()
                                             : (" via '" + connected_port_ + "'"));
}

std::vector<std::string> LaunchpadClassic::list_ports() {
    std::vector<std::string> names;
    try {
        RtMidiOut m;
        for (unsigned i = 0; i < m.getPortCount(); ++i) names.push_back(m.getPortName(i));
    } catch (...) {
    }
    return names;
}

bool LaunchpadClassic::looks_classic(const std::string& port) {
    // Real port names: "Launchpad" (original), "Launchpad S", "Launchpad Mini"
    // (gen1). All are substrings of the newer devices too ("Launchpad Mini MK3"
    // etc.), so we match "Launchpad" then exclude every newer generation -- and
    // "Pro", since the gen1 Pro is RGB and uses a different protocol.
    const std::string p = lower(port);
    if (p.find("launchpad") == std::string::npos) return false;
    return p.find("mk2") == std::string::npos &&
           p.find("mk3") == std::string::npos &&
           p.find("lpx")  == std::string::npos &&
           p.find("pro")  == std::string::npos;
}

void LaunchpadClassic::select_port() {
    if (!midi_) throw std::runtime_error("RtMidi is unavailable on this system.");

    int chosen = -1;
    if (!port_query_.empty()) {
        const std::string nl = lower(port_query_);
        for (unsigned i = 0; i < midi_->getPortCount(); ++i)
            if (lower(midi_->getPortName(i)).find(nl) != std::string::npos) {
                chosen = static_cast<int>(i);
                break;
            }
    } else {
        for (unsigned i = 0; i < midi_->getPortCount(); ++i)
            if (looks_classic(midi_->getPortName(i))) {
                chosen = static_cast<int>(i);
                break;
            }
    }
    if (chosen < 0) {
        std::string avail;
        for (unsigned i = 0; i < midi_->getPortCount(); ++i)
            avail += "\n  [" + std::to_string(i) + "] " + midi_->getPortName(i);
        throw std::runtime_error(
            "No classic Launchpad (original/S/Mini gen1) port found. "
            "Available ports:" + (avail.empty() ? " (none)" : avail));
    }
    connected_port_ = midi_->getPortName(static_cast<unsigned>(chosen));
    midi_->openPort(static_cast<unsigned>(chosen), "gridtv");
}

void LaunchpadClassic::send(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    if (!midi_ || !midi_->isPortOpen()) return;
    const std::vector<unsigned char> msg{a, b, c};
    midi_->sendMessage(&msg);
}

void LaunchpadClassic::connect() {
    select_port();
    send(0xB0, 0x00, 0x01);  // select X-Y layout (the grid-friendly mapping)
    clear();
    connected_ = true;
}

void LaunchpadClassic::disconnect() {
    if (connected_) {
        try { clear(); } catch (...) {}
        connected_ = false;
    }
    if (midi_ && midi_->isPortOpen()) {
        try { midi_->closePort(); } catch (...) {}
    }
}

void LaunchpadClassic::clear() {
    invalidate_last_frame();
    send(0xB0, 0x00, 0x00);  // reset: all LEDs off, mapping/buffers to defaults
}

void LaunchpadClassic::blit(const RGB8* px) {
    if (!midi_ || !midi_->isPortOpen() || !px) return;
    if (!frame_changed(px)) return;  // respect the 400 msg/s ceiling on static frames
    // Per-pad note-on: key = 16*row + column (X-Y layout, origin top-left),
    // velocity = (green<<4)|red|0x0C. Blue has no LED on this generation.
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const RGB8& c = px[y * 8 + x];
            const std::uint8_t vel = static_cast<std::uint8_t>((q3(c.g) << 4) | q3(c.r) | 0x0C);
            send(0x90, static_cast<std::uint8_t>((y << 4) | x), vel);
        }
    }
}

} // namespace gridtv

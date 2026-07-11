#include "gridtv/devices/launchpad_mini_mk3.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <thread>

namespace gridtv {

LaunchpadMk3::LaunchpadMk3(std::string port_query)
    : LaunchpadMk3(Model::Mini, std::move(port_query)) {}

LaunchpadMk3::LaunchpadMk3(Model model, std::string port_query)
    : port_query_(std::move(port_query)) {
    init(model);
    try {
        midi_ = std::make_unique<RtMidiOut>();
    } catch (...) {
        midi_.reset();
    }
}

LaunchpadMk3::~LaunchpadMk3() { disconnect(); }

void LaunchpadMk3::init(Model model) {
    // Defaults are the mk3-generation protocol (Mini/X/Pro): RGB SysEx is
    // "... 03 03 <note> r g b" and Programmer mode is entered with "0E 01".
    rgb_prefix_ = {0x03, 0x03};
    entry_body_ = {0x0E, 0x01};
    default_exclude_.clear();

    if (model == Model::Pro) {
        name_ = "Launchpad Pro MK3";
        default_needle_ = "LPProMK3 MIDI";
        family_ = 0x0E;
        rgb_max_ = 127;
    } else if (model == Model::X) {
        name_ = "Launchpad X";
        default_needle_ = "LPX MIDI";
        family_ = 0x0C;
        rgb_max_ = 127;
    } else if (model == Model::Mk2) {
        // Older MK2: family 0x18, RGB SysEx "... 0B <note> r g b" (0..63),
        // entered by selecting the Session layout "22 00". Same note map.
        name_ = "Launchpad MK2";
        default_needle_ = "Mk2";
        family_ = 0x18;
        rgb_max_ = 63;
        rgb_prefix_ = {0x0B};
        entry_body_ = {0x22, 0x00};
    } else if (model == Model::ProGen1) {
        // Original Launchpad Pro (gen1, 2015): family 0x10, RGB SysEx
        // "... 0B <note> r g b" (0..63). Entered by switching to Ableton/Live
        // mode "21 00" (command 0x21), which enables SysEx LED control.
        // Same 81-10y+x note map as the rest of the family. Port name
        // "Launchpad Pro" collides with the Pro MK3, so exclude "MK3".
        name_ = "Launchpad Pro (gen1)";
        default_needle_ = "Launchpad Pro";
        default_exclude_ = "MK3";
        family_ = 0x10;
        rgb_max_ = 63;
        rgb_prefix_ = {0x0B};
        entry_body_ = {0x21, 0x00};
    } else {
        name_ = "Launchpad Mini MK3";
        default_needle_ = "LPMiniMK3 DAW In";
        family_ = 0x0D;
        rgb_max_ = 63;
    }
}

std::string LaunchpadMk3::describe() const {
    return name() + (connected_port_.empty() ? std::string()
                                             : (" via '" + connected_port_ + "'"));
}

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Physical grid -> MIDI note. (0,0) is the top-left pad; identical across the
// mk3 generation and the MK2 (Session layout).
int note_for(int x, int y) { return 81 - 10 * y + x; }

} // namespace

std::uint8_t LaunchpadMk3::to_device(std::uint8_t v) const {
    int s = (static_cast<int>(v) * rgb_max_ + 127) / 255;  // rounded, 0..rgb_max_
    return static_cast<std::uint8_t>(s > rgb_max_ ? rgb_max_ : s);
}

std::vector<std::string> LaunchpadMk3::list_ports() {
    std::vector<std::string> names;
    try {
        RtMidiOut m;
        for (unsigned i = 0; i < m.getPortCount(); ++i) names.push_back(m.getPortName(i));
    } catch (...) {
    }
    return names;
}

LaunchpadMk3::Model LaunchpadMk3::detect(std::string* out_port) {
    struct Cand { Model model; const char* needle; };
    static const Cand cands[] = {
        {Model::Mini,    "LPMiniMK3"},
        {Model::X,       "LPX"},
        {Model::Pro,     "LPProMK3"},
        {Model::Mk2,     "Mk2"},
        {Model::ProGen1, "Launchpad Pro"},  // only reached if no Pro MK3 (LPProMK3) is present
    };
    const std::vector<std::string> ports = list_ports();
    for (const Cand& c : cands) {
        const std::string nl = to_lower(c.needle);
        for (const std::string& p : ports)
            if (to_lower(p).find(nl) != std::string::npos) {
                if (out_port) *out_port = p;
                return c.model;
            }
    }
    return Model::None;
}

void LaunchpadMk3::select_port() {
    if (!midi_) throw std::runtime_error("RtMidi is unavailable on this system.");

    const bool use_default = port_query_.empty();
    const std::string needle = to_lower(use_default ? default_needle_ : port_query_);
    const std::string excl = use_default ? to_lower(default_exclude_) : std::string();

    int chosen = -1;
    for (unsigned i = 0; i < midi_->getPortCount(); ++i) {
        const std::string p = to_lower(midi_->getPortName(i));
        if (p.find(needle) != std::string::npos &&
            (excl.empty() || p.find(excl) == std::string::npos)) {
            chosen = static_cast<int>(i);
            break;
        }
    }
    if (chosen < 0) {
        std::string avail;
        for (unsigned i = 0; i < midi_->getPortCount(); ++i)
            avail += "\n  [" + std::to_string(i) + "] " + midi_->getPortName(i);
        throw std::runtime_error(
            "No MIDI output port matching \"" + port_query_ + "\"." +
            " Available ports:" + (avail.empty() ? " (none)" : avail));
    }
    connected_port_ = midi_->getPortName(static_cast<unsigned>(chosen));
    midi_->openPort(static_cast<unsigned>(chosen), "gridtv");
}

void LaunchpadMk3::connect() {
    select_port();

    // Enter Programmer mode (mk3) / Session layout (MK2).
    std::vector<unsigned char> body = {0x00, 0x20, 0x29, 0x02, family_};
    body.insert(body.end(), entry_body_.begin(), entry_body_.end());
    send_sysex(body);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    clear();
    connected_ = true;
}

void LaunchpadMk3::disconnect() {
    if (connected_) {
        try { clear(); } catch (...) {}
        connected_ = false;
    }
    if (midi_ && midi_->isPortOpen()) {
        try { midi_->closePort(); } catch (...) {}
    }
}

void LaunchpadMk3::send_sysex(const std::vector<unsigned char>& body) {
    if (!midi_ || !midi_->isPortOpen()) return;
    std::vector<unsigned char> msg;
    msg.reserve(body.size() + 2);
    msg.push_back(0xF0);
    msg.insert(msg.end(), body.begin(), body.end());
    msg.push_back(0xF7);
    midi_->sendMessage(&msg);
}

void LaunchpadMk3::set_led(int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x < 0 || x > 7 || y < 0 || y > 7) return;
    std::vector<unsigned char> body = {0x00, 0x20, 0x29, 0x02, family_};
    body.insert(body.end(), rgb_prefix_.begin(), rgb_prefix_.end());
    body.push_back(static_cast<unsigned char>(note_for(x, y)));
    body.push_back(to_device(r));
    body.push_back(to_device(g));
    body.push_back(to_device(b));
    send_sysex(body);
}

void LaunchpadMk3::select_programmer_layout() {
    std::vector<unsigned char> body = {0x00, 0x20, 0x29, 0x02, family_};
    body.insert(body.end(), entry_body_.begin(), entry_body_.end());
    send_sysex(body);
}

void LaunchpadMk3::note_on(int note, int velocity) {
    send_raw(0x90, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity));
}

void LaunchpadMk3::send_raw(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    if (!midi_ || !midi_->isPortOpen()) return;
    d1 &= 0x7F;  // MIDI data bytes must not set the high bit (else they read as status)
    d2 &= 0x7F;
    std::vector<unsigned char> msg{status, d1, d2};
    midi_->sendMessage(&msg);
}

void LaunchpadMk3::clear() {
    invalidate_last_frame();
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            set_led(x, y, 0, 0, 0);
}

void LaunchpadMk3::blit(const RGB8* px) {
    if (!midi_ || !midi_->isPortOpen() || !px) return;
    if (!frame_changed(px)) return;  // static frame -> skip the SysEx barrage (anti-flicker)
    // Per-pad RGB SysEx. (The mk3 firmware rejects a batched multi-LED form, so a
    // frame is 64 small SysEx messages; well within USB-MIDI bandwidth. The MK2
    // could batch, but per-pad keeps one code path for the whole family.)
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const RGB8& c = px[y * 8 + x];
            std::vector<unsigned char> body = {0x00, 0x20, 0x29, 0x02, family_};
            body.insert(body.end(), rgb_prefix_.begin(), rgb_prefix_.end());
            body.push_back(static_cast<unsigned char>(note_for(x, y)));
            body.push_back(to_device(c.r));
            body.push_back(to_device(c.g));
            body.push_back(to_device(c.b));
            send_sysex(body);
        }
    }
}

} // namespace gridtv

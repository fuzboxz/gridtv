#include "gridtv/devices/monome_128.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace gridtv {

namespace {

// 4x4 ordered (Bayer) dither matrix, entries 0..15. Spreads the 16-level
// quantization error spatially so gradients read smoothly on a coarse grid.
constexpr int kBayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}};

void lo_error_handler(int num, const char* msg, const char* /*where*/) {
    std::fprintf(stderr, "gridtv monome: liblo error %d: %s\n", num, msg ? msg : "?");
}

// Reply target for the discovery handshake.
struct DiscoveryCtx {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    int port = 0;
    std::string id;
    std::string type;
};

// Handler for "/serialosc/device ssi <id> <type> <port>".
int on_device(const char* /*path*/, const char* types, lo_arg** argv, int argc,
              lo_message /*msg*/, void* user) {
    auto* c = static_cast<DiscoveryCtx*>(user);
    if (argc >= 3 && types && types[0] == 's' && types[1] == 's' && types[2] == 'i') {
        const int port = argv[2]->i;
        if (port < 1 || port > 65535) return 0;  // ignore malformed discovery replies
        {
            std::lock_guard<std::mutex> lk(c->mtx);
            c->id = &argv[0]->s;
            c->type = &argv[1]->s;
            c->port = port;
            c->done = true;
        }
        c->cv.notify_one();
    }
    return 0;
}

} // namespace

Monome128::~Monome128() {
    try { disconnect(); } catch (...) {}
}

void Monome128::connect() {
    if (connected_) return;
    if (dev_addr_) { lo_address_free(dev_addr_); dev_addr_ = nullptr; }  // clear any stale address

    // Open an ephemeral UDP socket to receive serialosc's discovery reply.
    lo_server_thread st = lo_server_thread_new(nullptr, lo_error_handler);
    if (!st)
        throw std::runtime_error("serialosc: could not open OSC receive socket");
    int my_port = lo_server_thread_get_port(st);

    DiscoveryCtx ctx;
    lo_server_thread_add_method(st, "/serialosc/device", "ssi", on_device, &ctx);
    lo_server_thread_start(st);

    // Ask the serialosc server (port 12002) to list devices, replying to us.
    lo_address srv = lo_address_new("127.0.0.1", "12002");
    lo_send(srv, "/serialosc/list", "si", "127.0.0.1", my_port);
    lo_address_free(srv);

    {
        std::unique_lock<std::mutex> lk(ctx.mtx);
        if (!ctx.cv.wait_for(lk, std::chrono::seconds(2),
                             [&] { return ctx.done; })) {
            lo_server_thread_stop(st);
            lo_server_thread_free(st);
            throw std::runtime_error(
                "serialosc: no monome device replied within 2s "
                "(is serialosc running and a grid connected?)");
        }
    }

    id_ = ctx.id;
    dev_addr_ = lo_address_new("127.0.0.1", std::to_string(ctx.port).c_str());

    lo_server_thread_stop(st);
    lo_server_thread_free(st);

    // This grid ignores un-prefixed /grid/... messages, so set a real prefix and
    // address pads as /monome/grid/... . Blank the grid after.
    lo_send(dev_addr_, "/sys/prefix", "s", "/monome");
    clear();
    connected_ = true;
}

void Monome128::disconnect() {
    if (connected_) {
        try { clear(); } catch (...) {}
        connected_ = false;
    }
    if (dev_addr_) {
        lo_address_free(dev_addr_);
        dev_addr_ = nullptr;
    }
}

void Monome128::clear() {
    invalidate_last_frame();
    if (dev_addr_) lo_send(dev_addr_, "/monome/grid/led/level/all", "i", 0);
}

void Monome128::blit(const RGB8* px) {
    if (!connected_ || !dev_addr_ || !px) return;
    if (!frame_changed(px)) return;  // static frame -> skip the OSC blit (anti-flicker)

    // The 16-wide grid is two 8x8 quads (x offsets 0 and 8). Each /level/map
    // carries x_off, y_off, then 64 levels (0..15) in row-major order.
    for (int q = 0; q < 2; ++q) {
        lo_message m = lo_message_new();
        if (!m) { connected_ = false; throw std::runtime_error("serialosc: message alloc failed"); }
        lo_message_add_int32(m, q * 8);  // x_off
        lo_message_add_int32(m, 0);      // y_off
        for (int y = 0; y < 8; ++y) {
            for (int xi = 0; xi < 8; ++xi) {
                int x = q * 8 + xi;
                const RGB8& p = px[static_cast<std::size_t>(y) * 16 + x];
                // Perceptual luminance -> 0..15, with 4x4 ordered dithering.
                float luma = 0.299f * p.r + 0.587f * p.g + 0.114f * p.b;  // 0..255
                float f = luma * (15.0f / 255.0f);
                float d = kBayer4[y & 3][x & 3] / 16.0f;
                int lvl = static_cast<int>(std::floor(f + d));
                if (lvl < 0) lvl = 0;
                else if (lvl > 15) lvl = 15;
                lo_message_add_int32(m, lvl);
            }
        }
        int sent = lo_send_message(dev_addr_, "/monome/grid/led/level/map", m);
        lo_message_free(m);
        if (sent < 0) { connected_ = false; throw std::runtime_error("serialosc: send failed"); }
    }
}

} // namespace gridtv

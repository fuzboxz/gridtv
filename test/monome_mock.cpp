// Mock serialosc broker + device: validates the monome driver's OSC path
// (discovery handshake + /grid/led/level/map blit + luminance/dither) without
// hardware. Run it (it grabs :12002), then drive the filter with
//   --gridtv-device monome-128
// and watch it log the discovery reply and the per-quad level stats.

#include <lo/lo.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static int g_device_port = 0;
static std::atomic<int> g_maps{0};

static int on_list(const char*, const char* types, lo_arg** argv, int argc,
                   lo_message, void*) {
    if (argc >= 2 && types && types[0] == 's' && types[1] == 'i') {
        const char* host = &argv[0]->s;
        int port = argv[1]->i;
        lo_address to = lo_address_new(host, std::to_string(port).c_str());
        lo_send(to, "/serialosc/device", "ssi", "mMOCK123", "grid", g_device_port);
        lo_address_free(to);
        std::printf("[mock] /serialosc/list -> replied /serialosc/device "
                    "(id=mMOCK123 type=grid port=%d) to %s:%d\n",
                    g_device_port, host, port);
    }
    return 0;
}

static int on_map(const char*, const char*, lo_arg** argv, int argc,
                  lo_message, void*) {
    if (argc >= 66) {
        int xo = argv[0]->i, yo = argv[1]->i;
        int mn = 99, mx = -1, sum = 0;
        for (int i = 2; i < 66; ++i) {
            int v = argv[i]->i;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
        }
        std::printf("[mock] /grid/led/level/map off=(%d,%d) 64 levels: "
                    "min=%d max=%d avg=%.1f\n", xo, yo, mn, mx, sum / 64.0);
        ++g_maps;
    }
    return 0;
}

// Observability: log the "all off" blank command so a close/cleanup test can
// confirm the driver blanks the grid on shutdown.
static int on_all(const char*, const char*, lo_arg** argv, int, lo_message, void*) {
    int v = (argv && argv[0]) ? argv[0]->i : -1;
    std::printf("[mock] /grid/led/level/all = %d  (clear/blank)\n", v);
    return 0;
}
static int noop(const char*, const char*, lo_arg**, int, lo_message, void*) { return 0; }

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    lo_server_thread dev = lo_server_thread_new(nullptr, nullptr);
    g_device_port = lo_server_thread_get_port(dev);
    lo_server_thread_add_method(dev, "/monome/grid/led/level/map", nullptr, on_map, nullptr);
    lo_server_thread_add_method(dev, "/monome/grid/led/level/all", nullptr, on_all, nullptr);
    lo_server_thread_add_method(dev, "/sys/prefix", nullptr, noop, nullptr);
    lo_server_thread_start(dev);

    lo_server_thread brk = lo_server_thread_new("12002", nullptr);
    lo_server_thread_add_method(brk, "/serialosc/list", "si", on_list, nullptr);
    lo_server_thread_start(brk);

    std::printf("[mock] broker=:12002  device-mock=:%d  (waiting for gridtv)\n",
                g_device_port);
    // MOCK_HOLD=<seconds> keeps the mock alive (no early exit) so a caller can
    // observe shutdown-time messages (e.g. the close-time grid blank).
    int hold = 0;
    if (const char* h = std::getenv("MOCK_HOLD")) hold = std::atoi(h);
    int iters = (hold > 0) ? hold : 30;
    for (int i = 0; i < iters; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (hold == 0 && g_maps.load() > 4) break;
    }
    std::printf("[mock] done: received %d level/map frames\n", g_maps.load());
    return 0;
}

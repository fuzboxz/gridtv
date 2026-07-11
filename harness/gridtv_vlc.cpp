// libVLC harness: plays a media file's VIDEO onto a grid controller while VLC
// plays its AUDIO through the normal audio output. Because libVLC owns the
// master clock, audio and video stay in sync for free -- we only ever see
// already-timed decoded frames in the display callback.
//
//   ./gridtv_vlc /path/to/video.mp4            # default channel order (RGB)
//   ./gridtv_vlc /path/to/video.mp4 --bgr      # if red/blue look swapped

#include "gridtv/devices/launchpad_mini_mk3.h"
#include "gridtv/frame_processor.h"

#include <vlc/vlc.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace gridtv;
using Clock = std::chrono::steady_clock;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct Harness {
    GridDevice* dev = nullptr;
    FrameProcessor* proc = nullptr;
    ChannelOrder order = ChannelOrder::BGR;

    std::vector<std::uint8_t> src; // libVLC writes decoded frames here (RV24)
    std::vector<RGB8> grid;        // proc output: cols()*rows()
    int width = 0, height = 0, pitch = 0;
    long disp_n = 0;               // diagnostics: display-callback count

    std::mutex m; // guards src/width/height across vmem + main threads
    Clock::time_point last_blit;
};

// libVLC vmem callbacks ------------------------------------------------------

static unsigned vlc_format(void** opaque, char* chroma, unsigned* width,
                           unsigned* height, unsigned* pitches, unsigned* lines) {
    auto* h = static_cast<Harness*>(*opaque);
    std::memcpy(chroma, "RV24", 4); // 24-bit packed RGB
    h->width = static_cast<int>(*width);
    h->height = static_cast<int>(*height);
    *pitches = *width * 3;
    *lines = *height;
    h->pitch = static_cast<int>(*pitches);
    {
        std::lock_guard<std::mutex> lk(h->m);
        h->src.assign(static_cast<std::size_t>(*pitches) * (*lines), 0);
    }
    if (std::getenv("GRIDTV_DEBUG"))
        std::fprintf(stderr, "[fmt] %ux%u pitch=%u\n", *width, *height, *pitches);
    return 1; // one buffer
}

static void* vlc_lock(void* opaque, void** planes) {
    auto* h = static_cast<Harness*>(opaque);
    planes[0] = h->src.data();
    return h->src.data();
}

static void vlc_unlock(void* /*opaque*/, void* /*picture*/, void* const* /*planes*/) {}

static void vlc_display(void* opaque, void* /*picture*/) {
    auto* h = static_cast<Harness*>(opaque);
    if (!h || !h->dev) return;  // mirror mode: the filter owns the grid; nothing to do

    // Respect the device's sustainable frame rate; drop (never queue) if ahead.
    const int cap = h->dev->max_fps();
    if (cap > 0) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - h->last_blit).count();
        if (elapsed_ms < (1000 / cap) - 1) return;
    }
    h->last_blit = Clock::now();

    std::lock_guard<std::mutex> lk(h->m);
    if (!h->dev || h->src.empty()) return;
    h->proc->process(h->src.data(), h->pitch, h->width, h->height, 3, h->order, h->grid.data());
    h->dev->blit(h->grid.data());
    if (std::getenv("GRIDTV_DEBUG") && ++h->disp_n % 15 == 0) {
        const std::uint8_t* p = h->src.data();
        int r = (h->order == ChannelOrder::BGR) ? p[2] : p[0];
        int g = p[1];
        int b = (h->order == ChannelOrder::BGR) ? p[0] : p[2];
        const RGB8& o = h->grid[0];
        std::fprintf(stderr, "[disp #%ld] %dx%d src px0=(%d,%d,%d) -> grid0=(%d,%d,%d)\n",
                     h->disp_n, h->width, h->height, r, g, b, o.r, o.g, o.b);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <video> [--bgr]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    bool bgr = false;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--bgr") == 0) bgr = true;

    std::signal(SIGINT, on_signal);

    LaunchpadMiniMK3 lp;
    FrameProcessor proc(lp.cols(), lp.rows(), AspectMode::Cover);

    Harness h;
    h.dev = nullptr;
    h.proc = &proc;
    h.order = bgr ? ChannelOrder::BGR : ChannelOrder::RGB;
    h.grid.resize(static_cast<std::size_t>(lp.cols()) * lp.rows());
    h.last_blit = Clock::now() - std::chrono::seconds(1);

    // --mirror: route through the gridtv-mirror video FILTER (screen + grid)
    // instead of the harness doing its own blit. We still use vmem as the vout
    // so frames pump through the filter chain.
    bool mirror = false;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--mirror") == 0) mirror = true;

    std::vector<const char*> vlc_args;
    if (mirror) {
        vlc_args.push_back("--video-filter");
        vlc_args.push_back("gridtv");
    }
    // Forward any --gridtv-* settings (bool flags, or option+value pairs).
    for (int i = 2; i < argc; ++i) {
        if (std::strncmp(argv[i], "--gridtv-", 9) != 0) continue;
        vlc_args.push_back(argv[i]);
        if (std::strcmp(argv[i], "--gridtv-debug") != 0 && i + 1 < argc && argv[i + 1][0] != '-')
            vlc_args.push_back(argv[++i]);
    }

    if (!mirror) {
        try {
            lp.connect();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "device error: %s\n", e.what());
            return 1;
        }
        h.dev = &lp;
    }

    libvlc_instance_t* inst = libvlc_new(static_cast<int>(vlc_args.size()), vlc_args.data());
    if (!inst) {
        std::fprintf(stderr, "libvlc_new() failed\n");
        return 1;
    }
    libvlc_media_t* media = libvlc_media_new_path(inst, path.c_str());
    libvlc_media_player_t* mp = libvlc_media_player_new_from_media(media);
    libvlc_media_release(media);

    libvlc_video_set_callbacks(mp, vlc_lock, vlc_unlock, vlc_display, &h);
    libvlc_video_set_format_callbacks(mp, vlc_format, nullptr);
    libvlc_media_player_play(mp);

    std::printf("Playing '%s' on %s (%dx%d, %s). Ctrl-C to stop.\n", path.c_str(),
                lp.name().c_str(), lp.cols(), lp.rows(),
                h.order == ChannelOrder::RGB ? "RGB" : "BGR");

    while (!g_stop) {
        const auto st = libvlc_media_player_get_state(mp);
        if (st == libvlc_Ended || st == libvlc_Error) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    libvlc_media_player_stop(mp);
    libvlc_media_player_release(mp);
    libvlc_release(inst);

    lp.clear();
    lp.disconnect();
    std::printf("stopped.\n");
    return 0;
}

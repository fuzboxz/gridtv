// Native VLC "video filter" module -> grid controller.
//
// Builds to libgridtv_plugin.dylib. Drop it into
//   VLC.app/Contents/MacOS/plugins/
// and enable it:  VLC → Preferences → Video → Filters → GridTV, or
//   /Applications/VLC.app/Contents/MacOS/VLC --video-filter gridtv video.mp4
//
// This is a *filter*, not an output: VLC's normal on-screen display is
// untouched. The filter taps each decoded frame for the grid as a side-effect
// and passes the original picture straight through. The grid work (the MIDI
// blit) runs on its own worker thread, so a grid/device stall can never block or
// drop frames in the on-screen playback.
//
// Settings appear in VLC → Preferences (Show All) → Video → Filters → GridTV,
// and as --gridtv-<name> flags.

#define __PLUGIN__ 1
#define MODULE_STRING "gridtv"

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_image.h>
#include <vlc_fourcc.h>
#include <vlc_messages.h>
#include <vlc_dialog.h>
#include <vlc_picture.h>

#include "gridtv/devices/launchpad_mini_mk3.h"
#include "gridtv/devices/launchpad_classic.h"
#include "gridtv/devices/monome_128.h"
#include "gridtv/frame_processor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifndef N_
#define N_(s) (s)
#endif
#ifndef _
#define _(s) (s)
#endif

using namespace gridtv;
using Clock = std::chrono::steady_clock;

struct filter_sys_t {
    GridDevice* dev = nullptr;
    image_handler_t* image = nullptr;
    filter_t* self = nullptr;

    // Per-channel colour LUT (gamma/contrast/lift/grid-gain/posterize baked in).
    // Rebuilt on the worker thread whenever any live setting changes; the hot
    // path is a table lookup, so pow() never runs per pixel. Whether a setting
    // changed is tracked by the worker's local `prev` snapshot (filter_worker).
    std::uint8_t lut[256] = {};
    bool debug = false;

    // The grid is driven by an independent worker thread so the on-screen
    // display path is never blocked by grid work: Filter() just downscales to a
    // tiny buffer and hands it off, returning the original picture immediately.
    std::mutex mtx;
    std::condition_variable cv;
    picture_t* pending = nullptr;   // newest 8x8 RGB24 frame held for the worker
    std::atomic<bool> stop{false};
    std::atomic<bool> connected{false};     // set by the worker once the device is live
    std::atomic<bool> fail_announced{false};
    std::thread worker;
};

static picture_t* Filter(filter_t*, picture_t*);

// Clear, actionable connect-failure dialog (shown once per failure streak).
// Thread-safe; a no-op if VLC has no dialog provider (e.g. the libVLC harness).
static void gridtv_connect_error_dialog(filter_t* f, GridDevice* dev, const char* detail) {
    if (dynamic_cast<Monome128*>(dev) != nullptr) {
        vlc_dialog_display_error(f, "GridTV: monome not connected",
            "GridTV can't reach the monome grid - the serialosc helper app isn't running.\n\n"
            "To fix: open Terminal and run\n    brew services start serialosc\n"
            "then make sure the grid is plugged in. GridTV keeps retrying and will "
            "light up automatically once serialosc is up.\n\n(Detail: %s)", detail);
    } else {
        vlc_dialog_display_error(f, "GridTV: Launchpad not connected",
            "GridTV can't open the Launchpad MIDI port. Check the controller is "
            "plugged in and that the Device setting matches one of the ports listed "
            "in VLC's Tools -> Messages.\n\n(Detail: %s)", detail);
    }
}

// Snapshot of every live-tunable setting, compared each worker iteration. When
// it changes, the colour LUT is rebuilt and the device's last-frame cache is
// dropped so the grid redraws cleanly under the new setting instead of keeping
// stale, old-setting colours -- the cause of glitches when switching fit / pick
// / tone modes mid-playback. Max FPS is intentionally excluded: it only paces.
struct Settings {
    int pick, aspect, color_order;
    float gamma, contrast, saturation, lift, brightness;
    int bits, delta;
    bool operator!=(const Settings& o) const {
        return std::tie(pick, aspect, color_order, gamma, contrast, saturation,
                        lift, brightness, bits, delta)
             != std::tie(o.pick, o.aspect, o.color_order, o.gamma, o.contrast,
                         o.saturation, o.lift, o.brightness, o.bits, o.delta);
    }
};

static void filter_worker(filter_sys_t* sys) {
    auto last = Clock::now() - std::chrono::seconds(1);
    Settings prev{};   // zero-init: the first real frame always differs -> full redraw
    while (!sys->stop.load()) {
        // (Re)connect on THIS thread so a slow device probe (the monome's OSC
        // discovery can take ~2s) never stalls the VLC video path. Retry quietly
        // until it works, so starting serialosc / plugging in late just works.
        if (!sys->connected.load()) {
            try {
                sys->dev->connect();
                sys->connected.store(true);
                sys->fail_announced.store(false);
                msg_Info(sys->self, "gridtv: connected - tapping video to %s",
                         sys->dev->describe().c_str());
            } catch (const std::exception& e) {
                if (!sys->fail_announced.exchange(true)) {
                    msg_Err(sys->self, "gridtv: device connect failed: %s", e.what());
                    gridtv_connect_error_dialog(sys->self, sys->dev, e.what());
                }
                std::unique_lock<std::mutex> lk(sys->mtx);
                sys->cv.wait_for(lk, std::chrono::seconds(5),
                                 [sys] { return sys->stop.load(); });
                continue;
            }
        }

        picture_t* raw = nullptr;
        {
            std::unique_lock<std::mutex> lk(sys->mtx);
            sys->cv.wait(lk, [sys] { return sys->stop.load() || sys->pending != nullptr; });
            if (sys->stop.load() && !sys->pending) break;
            raw = sys->pending;
            sys->pending = nullptr;
        }
        if (!raw) continue;
        // RAII: always release the frame, even if processing below throws.
        std::unique_ptr<picture_t, void(*)(picture_t*)> pic(raw, &picture_Release);

        try {  // never let a per-frame exception terminate the worker (and VLC)
        // Pace to the device's sustainable rate.
        const int user_fps = var_InheritInteger(sys->self, "gridtv-fps");
        const int cap = (user_fps > 0) ? std::min(user_fps, sys->dev->max_fps())
                                       : sys->dev->max_fps();
        if (cap > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - last).count();
            const int need = 1000 / cap;
            if (elapsed < need)
                std::this_thread::sleep_for(std::chrono::milliseconds(need - elapsed));
        }
        last = Clock::now();

        // --- snapshot every live-tunable setting, clamped to its valid range ---
        // pic is already a cols x rows RGB24, downscaled on the VLC thread.
        float gamma = var_InheritFloat(sys->self, "gridtv-gamma");
        if (gamma < 0.2f) gamma = 0.2f; else if (gamma > 3.0f) gamma = 3.0f;
        float contrast = var_InheritFloat(sys->self, "gridtv-contrast");
        if (contrast < 0.0f) contrast = 0.0f;
        float saturation = var_InheritFloat(sys->self, "gridtv-saturation");
        if (saturation < 0.0f) saturation = 0.0f;
        float lift = var_InheritFloat(sys->self, "gridtv-lift");
        if (lift < 0.0f) lift = 0.0f;
        if (lift > 1.0f) lift = 1.0f;
        float bright = var_InheritFloat(sys->self, "gridtv-brightness");
        if (bright < 0.0f) bright = 0.0f;
        if (bright > 1.0f) bright = 1.0f;
        int bits = var_InheritInteger(sys->self, "gridtv-bits");
        if (bits < 1) bits = 1; else if (bits > 8) bits = 8;
        const int delta   = var_InheritInteger(sys->self, "gridtv-delta");
        const int order_i = var_InheritInteger(sys->self, "gridtv-color");
        // gridtv-colorpick / gridtv-aspect drive the downscale on the VLC thread
        // (Filter), but we re-read them here so a change in EITHER (or in any
        // colour/tone knob) is detected and forces a clean redraw.
        const int pick_i   = var_InheritInteger(sys->self, "gridtv-colorpick");
        const int aspect_i = var_InheritInteger(sys->self, "gridtv-aspect");
        sys->dev->set_change_threshold(delta);

        Settings cur{pick_i, aspect_i, order_i,
                     gamma, contrast, saturation, lift, bright, bits, delta};
        if (cur != prev) {
            prev = cur;
            // A live setting changed. Rebuild the per-channel LUT (bakes in
            //   gamma -> contrast -> lift -> grid brightness -> posterize)
            // AND drop the device's last-frame cache. The cache holds colours
            // computed under the OLD setting, so without this the first frame
            // under the new setting can fall within the change threshold and be
            // suppressed by frame_changed() -> a half-updated, glitchy grid.
            sys->dev->invalidate_last_frame();
            const float inv_gamma = 1.0f / gamma;
            const float inv255 = 1.0f / 255.0f;
            const float gain = 1.0f - lift;
            const float levels = static_cast<float>((1 << bits) - 1);
            for (int i = 0; i < 256; ++i) {
                float v = std::pow(static_cast<float>(i) * inv255, inv_gamma);  // video gamma
                v = (v - 0.5f) * contrast + 0.5f;                              // contrast
                v = lift + gain * v;                                          // black lift
                v *= bright;                                                 // grid brightness
                if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
                v = std::rint(v * levels) / levels;                          // posterize
                sys->lut[i] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
            }
        }
        const ChannelOrder order = (order_i == 1) ? ChannelOrder::BGR : ChannelOrder::RGB;
        const bool desaturate = saturation != 1.0f;
        auto proc = [&](std::uint8_t R, std::uint8_t G, std::uint8_t B) -> RGB8 {
            if (!desaturate)
                return {sys->lut[R], sys->lut[G], sys->lut[B]};
            const float y = 0.299f * R + 0.587f * G + 0.114f * B;  // input luma
            auto sc = [&](std::uint8_t v) -> std::uint8_t {
                float o = y + (v - y) * saturation;
                int oi = (o < 0.0f) ? 0 : (o > 255.0f) ? 255 : static_cast<int>(o + 0.5f);
                return sys->lut[oi];
            };
            return {sc(R), sc(G), sc(B)};
        };

        const int cols = sys->dev->cols(), rows = sys->dev->rows();
        std::vector<RGB8> grid(static_cast<std::size_t>(cols) * rows);
        const std::uint8_t* base = pic->p[0].p_pixels;
        const int pitch = pic->p[0].i_pitch;
        for (int y = 0; y < rows; ++y) {
            const std::uint8_t* row = base + static_cast<std::size_t>(y) * pitch;
            for (int x = 0; x < cols; ++x) {
                const std::uint8_t* px = row + x * 3;
                std::uint8_t r = (order == ChannelOrder::RGB) ? px[0] : px[2];
                std::uint8_t g = px[1];
                std::uint8_t b = (order == ChannelOrder::RGB) ? px[2] : px[0];
                grid[static_cast<std::size_t>(y) * cols + x] = proc(r, g, b);
            }
        }
        try {
            sys->dev->blit(grid.data());
            if (sys->debug) {
                const RGB8& o = grid[0];
                msg_Info(sys->self, "gridtv: tapped %dx%d bright=%.2f grid0=(%d,%d,%d)",
                         cols, rows, bright, o.r, o.g, o.b);
            }
        } catch (const std::exception& e) {
            // Device vanished mid-stream: drop and reconnect next iteration.
            msg_Warn(sys->self, "gridtv: blit failed (%s); reconnecting", e.what());
            sys->connected.store(false);
            try { sys->dev->disconnect(); } catch (...) {}
        } catch (...) {
            sys->connected.store(false);
            try { sys->dev->disconnect(); } catch (...) {}
        }
        } catch (const std::exception& e) {  // any other per-frame throw: log + continue
            msg_Warn(sys->self, "gridtv: worker iteration error: %s", e.what());
        }
        // `pic` releases the frame here (RAII).
    }
}

static int Open(vlc_object_t* obj) {
    filter_t* f = (filter_t*)obj;
    auto* sys = new (std::nothrow) filter_sys_t();
    if (!sys) return VLC_ENOMEM;
    sys->self = f;
    sys->debug = var_InheritBool(f, "gridtv-debug");

    // Resolve the device type + port from the --gridtv-device preset.
    char* devc = var_InheritString(f, "gridtv-device");
    enum class Kind { Launchpad, Monome, Classic };
    Kind kind = Kind::Launchpad;
    LaunchpadMk3::Model lp_model = LaunchpadMk3::Model::Mini;
    std::string pq;
    if (devc) {
        const std::string d = devc;
        if (d == "launchpad-mk3-daw")       { pq = "LPMiniMK3 DAW In"; }
        else if (d == "launchpad-mk3-midi") { pq = "LPMiniMK3 MIDI In"; }
        else if (d == "launchpad-x")        { lp_model = LaunchpadMk3::Model::X; }
        else if (d == "launchpad-pro-mk3")  { lp_model = LaunchpadMk3::Model::Pro; }
        else if (d == "launchpad-mk2")       { lp_model = LaunchpadMk3::Model::Mk2; }
        else if (d == "launchpad-pro-gen1")   { lp_model = LaunchpadMk3::Model::ProGen1; }
        else if (d == "launchpad-classic")   { kind = Kind::Classic; }
        else if (d == "monome-128")         { kind = Kind::Monome; }
        else if (d == "custom") {
            char* port = var_InheritString(f, "gridtv-port");
            if (port && port[0]) pq = port;
            free(port);
        }
        else {  // "auto" / unknown -> detect whichever mk3 Launchpad is connected
            LaunchpadMk3::Model m = LaunchpadMk3::detect();
            if (m != LaunchpadMk3::Model::None) lp_model = m;
        }
    }
    free(devc);

    if (kind == Kind::Launchpad) {
        msg_Info(f, "gridtv: MIDI output ports available:");
        for (const std::string& name : LaunchpadMiniMK3::list_ports())
            msg_Info(f, "    %s", name.c_str());
    } else {
        msg_Info(f, "gridtv: monome selected; connecting via serialosc in the background");
    }

    // Create the device object only; connect() runs on the worker thread so a
    // slow probe never blocks the VLC video path. Wrap init so a throw (e.g.
    // std::thread under resource exhaustion, or bad_alloc) can't escape the C
    // ABI and terminate VLC -- clean up and fail gracefully instead.
    try {
        std::unique_ptr<GridDevice> device;
        if (kind == Kind::Monome)        device = std::make_unique<Monome128>();
        else if (kind == Kind::Classic)  device = std::make_unique<LaunchpadClassic>(pq);
        else                             device = std::make_unique<LaunchpadMk3>(lp_model, pq);
        sys->dev = device.release();

        sys->image = image_HandlerCreate(f);
        if (!sys->image) throw std::runtime_error("image handler creation failed");

        sys->worker = std::thread(filter_worker, sys);
    } catch (const std::exception& e) {
        msg_Err(f, "gridtv: init failed: %s", e.what());
        if (sys->image) image_HandlerDelete(sys->image);
        delete sys->dev;  // nullptr-safe
        delete sys;
        return VLC_EGENERIC;
    }
    f->p_sys = sys;
    f->pf_video_filter = Filter;
    msg_Info(f, "gridtv: filter ready (connects to %s asynchronously)",
             sys->dev->name().c_str());
    return VLC_SUCCESS;
}

static void Close(vlc_object_t* obj) {
    filter_t* f = (filter_t*)obj;
    auto* sys = (filter_sys_t*)f->p_sys;
    if (!sys) return;
    {
        std::lock_guard<std::mutex> lk(sys->mtx);
        sys->stop.store(true);
        if (sys->pending) { picture_Release(sys->pending); sys->pending = nullptr; }
    }
    sys->cv.notify_one();
    if (sys->worker.joinable()) sys->worker.join();

    if (sys->dev) {
        try { sys->dev->clear(); sys->dev->disconnect(); } catch (...) {}
        delete sys->dev;
        msg_Info(f, "gridtv: stopped (device disconnected, grid cleared)");
    }
    if (sys->image) image_HandlerDelete(sys->image);
    delete sys;
    f->p_sys = nullptr;
}

// --- per-pad colour selection (anti-"mush") ---
// How a pad's colour is derived from its source block. Average is the smooth
// area-mean (also the fast image_Convert path); the others sample a cols*K x
// rows*K intermediate so each pad collapses a K*K block of real pixels.
enum ColorPick { CP_AVERAGE = 0, CP_DOMINANT = 1, CP_BRIGHTEST = 2, CP_MEDIAN = 3, CP_CENTER = 4 };

// How source content is fitted to the (non-1:1) device grid.
enum Fit { FIT_STRETCH = 0, FIT_COVER = 1, FIT_CONTAIN = 2 };

static RGB8 pick_block(const std::uint8_t* base, int pitch, int bx, int by, int K, int mode) {
    if (K < 1) K = 1;
    if (K > 8) K = 8;  // K is always 8 today; this guards the 64-wide CP_MEDIAN buffers
    const int n = K * K;
    switch (mode) {
    case CP_CENTER: {
        const std::uint8_t* p = base + static_cast<std::size_t>(by + K / 2) * pitch + (bx + K / 2) * 3;
        return {p[0], p[1], p[2]};
    }
    case CP_BRIGHTEST: {
        const std::uint8_t* best = base + static_cast<std::size_t>(by) * pitch + bx * 3;
        float best_l = -1.0f;
        for (int y = 0; y < K; ++y)
            for (int x = 0; x < K; ++x) {
                const std::uint8_t* p = base + static_cast<std::size_t>(by + y) * pitch + (bx + x) * 3;
                float l = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
                if (l > best_l) { best_l = l; best = p; }
            }
        return {best[0], best[1], best[2]};
    }
    case CP_MEDIAN: {  // K==8 -> 64 samples
        std::uint8_t rs[64], gs[64], bs[64];
        int i = 0;
        for (int y = 0; y < K; ++y)
            for (int x = 0; x < K; ++x) {
                const std::uint8_t* p = base + static_cast<std::size_t>(by + y) * pitch + (bx + x) * 3;
                rs[i] = p[0]; gs[i] = p[1]; bs[i] = p[2]; ++i;
            }
        std::nth_element(rs, rs + n / 2, rs + n);
        std::nth_element(gs, gs + n / 2, gs + n);
        std::nth_element(bs, bs + n / 2, bs + n);
        return {rs[n / 2], gs[n / 2], bs[n / 2]};
    }
    case CP_DOMINANT: {  // mode of an 8x8x8 colour cube (512 bins)
        int cnt[512] = {0}, sr[512] = {0}, sg[512] = {0}, sb[512] = {0};
        for (int y = 0; y < K; ++y)
            for (int x = 0; x < K; ++x) {
                const std::uint8_t* p = base + static_cast<std::size_t>(by + y) * pitch + (bx + x) * 3;
                int bin = (p[0] >> 5) * 64 + (p[1] >> 5) * 8 + (p[2] >> 5);
                cnt[bin]++; sr[bin] += p[0]; sg[bin] += p[1]; sb[bin] += p[2];
            }
        int best = 0;
        for (int b = 1; b < 512; ++b) if (cnt[b] > cnt[best]) best = b;
        if (cnt[best] == 0) {
            const std::uint8_t* p = base + static_cast<std::size_t>(by + K / 2) * pitch + (bx + K / 2) * 3;
            return {p[0], p[1], p[2]};
        }
        return {static_cast<std::uint8_t>(sr[best] / cnt[best]),
                static_cast<std::uint8_t>(sg[best] / cnt[best]),
                static_cast<std::uint8_t>(sb[best] / cnt[best])};
    }
    default: {  // CP_AVERAGE
        long R = 0, G = 0, B = 0;
        for (int y = 0; y < K; ++y)
            for (int x = 0; x < K; ++x) {
                const std::uint8_t* p = base + static_cast<std::size_t>(by + y) * pitch + (bx + x) * 3;
                R += p[0]; G += p[1]; B += p[2];
            }
        return {static_cast<std::uint8_t>(R / n), static_cast<std::uint8_t>(G / n),
                static_cast<std::uint8_t>(B / n)};
    }
    }
}

// --- RGB24 picture helpers (VLC thread only; image_Convert is not thread-safe) ---
static picture_t* new_rgb(int W, int H) {
    video_format_t f;
    video_format_Init(&f, VLC_CODEC_RGB24);
    f.i_width = W;          f.i_height = H;
    f.i_visible_width = W;  f.i_visible_height = H;
    f.i_sar_num = f.i_sar_den = 1;
    picture_t* p = picture_NewFromFormat(&f);
    video_format_Clean(&f);
    return p;
}
static picture_t* convert_rgb(image_handler_t* image, picture_t* src, int W, int H) {
    video_format_t f;
    video_format_Init(&f, VLC_CODEC_RGB24);
    f.i_width = W;          f.i_height = H;
    f.i_visible_width = W;  f.i_visible_height = H;
    f.i_sar_num = f.i_sar_den = 1;
    picture_t* p = image_Convert(image, src, &src->format, &f);
    video_format_Clean(&f);
    return p;
}
static void blacken(picture_t* p) {
    std::memset(p->p[0].p_pixels, 0,
                static_cast<std::size_t>(p->p[0].i_pitch) * p->p[0].i_lines);
}
static void copy_rect(picture_t* dst, int dx, int dy, int w, int h,
                      const picture_t* src, int sx, int sy) {
    auto vw = [](const picture_t* p) { int v = p->format.i_visible_width;  return v > 0 ? v : p->format.i_width; };
    auto vh = [](const picture_t* p) { int v = p->format.i_visible_height; return v > 0 ? v : p->format.i_height; };
    const int sw = vw(src), sh = vh(src), dw = vw(dst), dh = vh(dst);
    // Clip the source rect to the source's actual dims, map onto the destination,
    // then clip to the destination. Guarantees we never overrun a plane buffer
    // (image_Convert / picture_NewFromFormat dims don't always match requests).
    int x0 = std::max(sx, 0), y0 = std::max(sy, 0);
    int cw = std::min(sx + w, sw) - x0;
    int ch = std::min(sy + h, sh) - y0;
    int ddx = dx + (x0 - sx), ddy = dy + (y0 - sy);
    if (ddx < 0) { x0 -= ddx; cw += ddx; ddx = 0; }
    if (ddy < 0) { y0 -= ddy; ch += ddy; ddy = 0; }
    cw = std::min(cw, dw - ddx);
    ch = std::min(ch, dh - ddy);
    if (cw <= 0 || ch <= 0) return;
    const int sp = src->p[0].i_pitch, dp = dst->p[0].i_pitch;
    const std::uint8_t* sb = src->p[0].p_pixels;
    std::uint8_t* db = dst->p[0].p_pixels;
    for (int y = 0; y < ch; ++y)
        std::memcpy(db + static_cast<std::size_t>(ddy + y) * dp + static_cast<std::size_t>(ddx) * 3,
                    sb + static_cast<std::size_t>(y0 + y) * sp + static_cast<std::size_t>(x0) * 3,
                    static_cast<std::size_t>(cw) * 3);
}

// Fit src into a WxH RGB24 picture honouring the aspect mode. Stretch resizes
// directly (distorts); cover scales to fill then centre-crops; contain scales
// to fit then centre-pads with black. Cover/contain scale aspect-preserving then
// memcpy crop/pad, so we never rely on image_Convert's own cropping.
static picture_t* fit_to(image_handler_t* image, picture_t* src, int W, int H, int fit) {
    const video_format_t* sf = &src->format;
    const int sw = sf->i_visible_width  > 0 ? sf->i_visible_width  : sf->i_width;
    const int sh = sf->i_visible_height > 0 ? sf->i_visible_height : sf->i_height;
    if (sw <= 0 || sh <= 0 || fit == FIT_STRETCH)
        return convert_rgb(image, src, W, H);

    if (fit == FIT_COVER) {
        const double s = std::max(static_cast<double>(W) / sw, static_cast<double>(H) / sh);
        const int cap = std::max(W, H) * 4;  // bound the transient allocation (DoS hardening)
        const int iw = std::min(cap, std::max(1, static_cast<int>(std::round(sw * s))));
        const int ih = std::min(cap, std::max(1, static_cast<int>(std::round(sh * s))));
        picture_t* inter = convert_rgb(image, src, iw, ih);  // aspect preserved (then centre-cropped)
        if (!inter) return nullptr;
        picture_t* out = new_rgb(W, H);
        if (!out) { picture_Release(inter); return nullptr; }
        copy_rect(out, 0, 0, W, H, inter, (iw - W) / 2, (ih - H) / 2);
        picture_Release(inter);
        return out;
    }

    // FIT_CONTAIN
    const double s = std::min(static_cast<double>(W) / sw, static_cast<double>(H) / sh);
    int iw = std::max(1, static_cast<int>(std::round(sw * s)));
    int ih = std::max(1, static_cast<int>(std::round(sh * s)));
    if (iw > W) iw = W;
    if (ih > H) ih = H;
    picture_t* inner = convert_rgb(image, src, iw, ih);
    picture_t* out = new_rgb(W, H);
    if (!out) { if (inner) picture_Release(inner); return nullptr; }
    blacken(out);
    if (inner) {
        copy_rect(out, (W - iw) / 2, (H - ih) / 2, iw, ih, inner, 0, 0);
        picture_Release(inner);
    }
    return out;
}

// Downscale src to cols x rows RGB24 with a non-average pick mode.
static picture_t* downscale_custom(image_handler_t* image, picture_t* src,
                                   int cols, int rows, int mode, int fit) {
    constexpr int K = 8;  // samples per pad edge (must match pick_block's buffers)
    picture_t* mid = fit_to(image, src, cols * K, rows * K, fit);
    if (!mid) return nullptr;
    picture_t* small = new_rgb(cols, rows);
    if (!small) { picture_Release(mid); return nullptr; }
    const std::uint8_t* mbase = mid->p[0].p_pixels;
    const int mpitch = mid->p[0].i_pitch;
    std::uint8_t* sbase = small->p[0].p_pixels;
    const int spitch = small->p[0].i_pitch;
    for (int gy = 0; gy < rows; ++gy)
        for (int gx = 0; gx < cols; ++gx) {
            RGB8 c = pick_block(mbase, mpitch, gx * K, gy * K, K, mode);
            std::uint8_t* d = sbase + static_cast<std::size_t>(gy) * spitch + gx * 3;
            d[0] = c.r; d[1] = c.g; d[2] = c.b;
        }
    picture_Release(mid);
    return small;
}

// Display-path call: downscale to a tiny RGB24 on the VLC thread (image_Convert
// is not safe off-thread), hand it to the worker for the MIDI blit, and return
// the original picture immediately. The display path never blocks on MIDI.
static picture_t* Filter(filter_t* f, picture_t* p_pic) {
    auto* sys = (filter_sys_t*)f->p_sys;
    if (p_pic && sys && sys->image && sys->dev && sys->connected.load()) {
        const int cols = sys->dev->cols(), rows = sys->dev->rows();
        const int mode = var_InheritInteger(f, "gridtv-colorpick");
        const int fit = var_InheritInteger(f, "gridtv-aspect");
        picture_t* small = (mode == CP_AVERAGE)
            ? fit_to(sys->image, p_pic, cols, rows, fit)
            : downscale_custom(sys->image, p_pic, cols, rows, mode, fit);
        if (small) {
            std::lock_guard<std::mutex> lk(sys->mtx);
            if (sys->pending) picture_Release(sys->pending);  // drop stale frame
            sys->pending = small;
            sys->cv.notify_one();
        }
    }
    return p_pic;
}

// Device presets for the --gridtv-device dropdown (value = internal id,
// text = shown in VLC Preferences). "custom" defers to the --gridtv-port field.
static const char* const gridtv_device_values[] = {
    "auto", "launchpad-mk3-daw", "launchpad-mk3-midi", "launchpad-x", "launchpad-pro-mk3", "launchpad-mk2", "launchpad-pro-gen1", "launchpad-classic", "monome-128", "custom"
};
static const char* const gridtv_device_texts[] = {
    "Auto-detect (any Launchpad)",
    "Launchpad Mini MK3 -- DAW port",
    "Launchpad Mini MK3 -- MIDI port",
    "Launchpad X (MIDI port)",
    "Launchpad Pro MK3 (MIDI port)",
    "Launchpad MK2 (Session layout)",
    "Launchpad Pro (gen1, Ableton mode)",
    "Launchpad classic (original / S / Mini gen1)",
    "monome grid 128 (serialosc)",
    "Custom (use the port name below)"
};

// Colour-pick modes for the --gridtv-colorpick dropdown.
static const int gridtv_colorpick_values[] = { 0, 1, 2, 3, 4 };
static const char* const gridtv_colorpick_texts[] = {
    "Average (smooth)", "Dominant colour (crisp)", "Brightest pixel", "Median", "Centre pixel"
};

// Aspect/fit modes for the --gridtv-aspect dropdown.
static const int gridtv_aspect_values[] = { 0, 1, 2 };
static const char* const gridtv_aspect_texts[] = {
    "Stretch (distort to fill)", "Fill (crop to fill)", "Fit (letterbox / show all)"
};

// Colour-byte order for the --gridtv-color dropdown. Frames are RGB after VLC's
// image conversion, so RGB is correct in normal use; BGR is a debug fallback for
// the rare case colours look swapped on a particular setup.
static const int gridtv_color_values[] = { 0, 1 };
static const char* const gridtv_color_texts[] = {
    "RGB (normal)", "BGR (only if red/blue look swapped)"
};

vlc_module_begin()
    set_shortname(N_("GridTV"))
    set_description(N_("Grid controller video filter (screen + grid)"))
    set_help(N_(
        "GridTV mirrors your video onto a music grid controller (a Novation "
        "Launchpad or a monome grid) as a low-resolution light display, at the "
        "same time as normal on-screen playback. The grid is a non-blocking "
        "additive tap: your on-screen video and your audio are never changed, "
        "slowed or replaced.\n\n"
        "GETTING STARTED\n"
        "  1. Connect your controller. Launchpads are USB plug-and-play. A "
        "monome grid needs the serialosc daemon running (install + enable it "
        "once - see 'Device families' below).\n"
        "  2. Set 'Device' to match your hardware, or leave it on Auto-detect "
        "(works for any Launchpad: Mini MK3, X, Pro MK3, MK2 or Pro gen1).\n"
        "  3. Play any video. The grid lights up within a second or two.\n\n"
        "WHAT YOU CAN CHANGE LIVE\n"
        "  Brightness, gamma, contrast, saturation, lift, bit depth, max FPS, "
        "colour-pick and fit all update instantly while the video plays - no "
        "need to restart playback.\n\n"
        "WHAT NEEDS A RE-OPEN\n"
        "  'Device' and 'Custom MIDI port' are read when a media item opens. "
        "To switch hardware, stop the current item, choose the new device, then "
        "play again.\n\n"
        "IF THE GRID STAYS DARK\n"
        "  Open VLC -> Tools -> Messages. GridTV prints every MIDI output port "
        "it can see (copy the exact name if you need Custom) and a plain-English "
        "reason if a device can't be reached - for example: start the serialosc "
        "daemon, or check the USB cable.\n\n"
        "DEVICE FAMILIES\n"
        "  - Launchpad (Mini MK3 / X / Pro MK3 / MK2 / Pro gen1): full RGB over "
        "USB-MIDI. Auto-detect finds any of these.\n"
        "  - Launchpad classic (original Launchpad, Launchpad S, Mini gen1): "
        "red + green only (no blue), slower refresh - pick it explicitly.\n"
        "  - monome grid 128 (16x8): monochrome, 16 brightness levels, reached "
        "through the serialosc daemon. macOS: 'brew services start serialosc'. "
        "Linux: start serialoscd from your package manager. Windows: run "
        "serialosc.exe.\n\n"
        "See the project README for the full settings table."))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_shortcut("gridtv")

    set_section(N_("Device"), N_("Which controller receives the video."))
    add_string("gridtv-device", "auto",
               N_("Device"), N_("Choose your controller. Auto-detect finds any "
               "connected Launchpad (Mini MK3, X, Pro MK3, MK2 or Pro gen1). For "
               "a monome grid pick 'monome grid 128'. For an original Launchpad, "
               "Launchpad S or Mini gen1 pick 'Launchpad classic'. For anything "
               "else pick 'Custom' and type the MIDI port name into 'Custom MIDI "
               "port' below. Tip: when in doubt, use Auto-detect, and check "
               "Tools -> Messages for the list of ports GridTV can see."), false)
        change_string_list(gridtv_device_values, gridtv_device_texts)
    add_string("gridtv-port", "",
               N_("Custom MIDI port"), N_("Used only when Device = Custom. Type "
               "any unique part of the MIDI output port name shown in Tools -> "
               "Messages (a substring match is enough, e.g. 'Launchpad')."), false)

    set_section(N_("Picture (fit & quality)"), N_("How the video is framed and sampled onto the grid."))
    add_integer("gridtv-aspect", FIT_COVER,
                N_("Aspect / fit"), N_("How the video is fitted to the grid shape. "
                "Fill (default) zooms in so the whole grid is covered (crops the "
                "edges). Fit shrinks to show the entire frame with black bars. "
                "Stretch distorts to fill every pad. Works for any grid size (8x8 "
                "Launchpad, 16x8 monome, etc.)."), false)
        change_integer_list(gridtv_aspect_values, gridtv_aspect_texts)
        change_safe()
    add_integer("gridtv-colorpick", CP_DOMINANT,
                N_("Colour pick"), N_("How each pad's colour is chosen from the "
                "block of video behind it. Dominant (default) picks the most common "
                "colour - crisp, and avoids the muddy 'average' look on busy scenes. "
                "Average is smoother. Brightest emphasises highlights. Median is a "
                "robust middle. Centre uses the single centre pixel (sharpest, "
                "noisiest)."), false)
        change_integer_list(gridtv_colorpick_values, gridtv_colorpick_texts)
        change_safe()

    set_section(N_("Colour & tone"), N_("The look of the image on the grid."))
    add_float_with_range("gridtv-gamma", 1.0f, 0.2f, 3.0f,
                         N_("Video brightness (gamma)"), N_("Gamma applied to the "
                         "source content before anything else. Above 1.0 brightens "
                         "dark video and lifts shadow detail; below 1.0 darkens it. "
                         "This is separate from 'Grid brightness', which dims the "
                         "final output."), false)
        change_safe()
    add_float_with_range("gridtv-contrast", 1.0f, 0.0f, 3.0f,
                         N_("Contrast"), N_("Contrast of the grid image: 1.0 = "
                         "unchanged, below 1.0 = softer/flatter, above 1.0 = "
                         "punchier."), false)
        change_safe()
    add_float_with_range("gridtv-saturation", 1.0f, 0.0f, 3.0f,
                         N_("Saturation"), N_("Colour saturation: 1.0 = unchanged, "
                         "0 = greyscale, above 1.0 = more vivid. No effect on "
                         "monochrome devices (monome, Launchpad classic)."), false)
        change_safe()
    add_float_with_range("gridtv-lift", 0.0f, 0.0f, 1.0f,
                         N_("Black lift"), N_("Raises the black floor so 'off' reads "
                         "as a dim grey instead of fully dark - useful where total "
                         "black makes the grid look broken. 0 = pure black."), false)
        change_safe()
    add_integer_with_range("gridtv-bits", 8, 1, 8,
                           N_("Bit depth"), N_("Posterize each colour channel to "
                           "this many bits: 8 = full colour, lower = fewer, chunkier "
                           "colours for a retro look."), false)
        change_safe()

    set_section(N_("Output"), N_("Final output level, speed and stability."))
    add_float_with_range("gridtv-brightness", 1.0f, 0.0f, 1.0f,
                         N_("Grid brightness"), N_("Master output gain of the grid. "
                         "1.0 = full brightness; lower dims everything including the "
                         "black-lift floor. Use this to protect your eyes in a dark "
                         "room or to match room lighting."), false)
        change_safe()
    add_integer_with_range("gridtv-fps", 0, 0, 60,
                           N_("Max FPS"), N_("Frame-rate cap for the grid (0 = use "
                           "the device's safe default). Lower values save USB "
                           "bandwidth and reduce flicker; the grid rarely needs more "
                           "than about 25 fps."), false)
        change_safe()
    add_integer_with_range("gridtv-delta", 0, 0, 64,
                           N_("Change threshold"), N_("Anti-flicker: only send a frame "
                           "to the grid if some colour channel changed by more than "
                           "this since the last frame. 0 = send every frame (most "
                           "accurate, can flicker on static noise). Raise it to 2-4 "
                           "to freeze near-static content so a paused or still image "
                           "stops shimmering."), false)
        change_safe()
    add_integer("gridtv-color", 0,
                N_("Colour order"), N_("Byte order of the colour data. Leave on RGB "
                "for normal use. Switch to BGR only if red and blue look swapped on "
                "your setup - a debugging control that is almost never needed."), false)
        change_integer_list(gridtv_color_values, gridtv_color_texts)
        change_safe()

    set_section(N_("Advanced"), NULL)
    add_bool("gridtv-debug", false,
             N_("Debug logging"), N_("Log one line per displayed frame to the VLC "
             "message log (Tools -> Messages), including grid size and a sample "
             "colour. Useful for confirming the tap is running."), true)
    set_callbacks(Open, Close)
vlc_module_end()

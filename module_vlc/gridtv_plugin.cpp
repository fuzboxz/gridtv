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

// Snapshot of every live-tunable setting. Filter() reads + clamps all of them
// on the VLC thread and hands a copy to the worker, so the worker NEVER touches
// VLC's variable system -- a non-VLC thread doing var_Inherit* during a config
// save / filter reconfigure is unsafe and crashed VLC on "save while playing".
// Compared each worker iteration; on change the LUT is rebuilt and the device's
// last-frame cache + temporal history are dropped for a clean redraw.
struct Settings {
    int pick, aspect, color_order, fps;
    float gamma, contrast, saturation, lift, brightness, led_gamma;
    int bits, delta, smoothing, dither, sharpen;
    bool operator!=(const Settings& o) const {
        return std::tie(pick, aspect, color_order, fps, gamma, contrast, saturation,
                        lift, brightness, led_gamma, bits, delta, smoothing, dither,
                        sharpen)
             != std::tie(o.pick, o.aspect, o.color_order, o.fps, o.gamma, o.contrast,
                         o.saturation, o.lift, o.brightness, o.led_gamma, o.bits,
                         o.delta, o.smoothing, o.dither, o.sharpen);
    }
};

// Read + clamp every live setting into a snapshot (VLC thread only). Centralised
// so the worker can stay free of VLC variable access entirely.
static Settings snapshot_settings(filter_t* f) {
    Settings s{};
    s.pick = var_InheritInteger(f, "gridtv-colorpick");
    s.aspect = var_InheritInteger(f, "gridtv-aspect");
    s.color_order = var_InheritInteger(f, "gridtv-color");
    s.fps = var_InheritInteger(f, "gridtv-fps");
    s.gamma = var_InheritFloat(f, "gridtv-gamma");
    if (s.gamma < 0.2f) s.gamma = 0.2f; else if (s.gamma > 3.0f) s.gamma = 3.0f;
    s.contrast = var_InheritFloat(f, "gridtv-contrast");
    if (s.contrast < 0.0f) s.contrast = 0.0f;
    s.saturation = var_InheritFloat(f, "gridtv-saturation");
    if (s.saturation < 0.0f) s.saturation = 0.0f;
    s.lift = var_InheritFloat(f, "gridtv-lift");
    if (s.lift < 0.0f) s.lift = 0.0f; else if (s.lift > 1.0f) s.lift = 1.0f;
    s.brightness = var_InheritFloat(f, "gridtv-brightness");
    if (s.brightness < 0.0f) s.brightness = 0.0f; else if (s.brightness > 1.0f) s.brightness = 1.0f;
    s.led_gamma = var_InheritFloat(f, "gridtv-ledgamma");
    if (s.led_gamma < 1.0f) s.led_gamma = 1.0f; else if (s.led_gamma > 3.0f) s.led_gamma = 3.0f;
    s.bits = var_InheritInteger(f, "gridtv-bits");
    if (s.bits < 1) s.bits = 1; else if (s.bits > 8) s.bits = 8;
    s.delta = var_InheritInteger(f, "gridtv-delta");
    s.smoothing = var_InheritInteger(f, "gridtv-smoothing");
    s.dither = var_InheritInteger(f, "gridtv-dither");
    s.sharpen = var_InheritInteger(f, "gridtv-sharpen");
    return s;
}

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

    // Temporal-smoothing accumulator (worker thread only): the previous displayed
    // grid, EMA-blended into the current one. Reset (history_valid=false) on any
    // setting change so a mode switch snaps to the new look in one frame instead
    // of blending with stale, old-setting colours.
    std::vector<RGB8> history_;
    bool history_valid = false;

    // The grid is driven by an independent worker thread so the on-screen
    // display path is never blocked by grid work: Filter() just downscales to a
    // tiny buffer and hands it off, returning the original picture immediately.
    std::mutex mtx;
    std::condition_variable cv;
    picture_t* pending = nullptr;        // newest cols x rows RGB24 frame held for the worker
    Settings pending_snap;               // settings snapshot captured with `pending` (VLC thread)
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

// Temporal smoothing: EMA blend of the current grid with the previous one.
enum Smooth { SM_OFF = 0, SM_LIGHT = 1, SM_MEDIUM = 2, SM_STRONG = 3 };

// Ordered dither applied just before the device blit (smooths banding from the
// device's coarse levels / posterize). Off everywhere by default.
enum Dither { DT_OFF = 0, DT_ORDERED = 1 };

// Local-contrast (unsharp) sharpening of the tiny grid: crisps pad-to-pad edges
// that averaging softened. Light by default.
enum Sharpen { SH_OFF = 0, SH_LIGHT = 1, SH_STRONG = 2 };

// 4x4 Bayer threshold matrix (values 0..15); spatially fixed -> flicker-free.
static const int BAYER4[4][4] = {
    { 0, 8, 2,10}, {12, 4,14, 6}, { 3,11, 1, 9}, {15, 7,13, 5}
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
        Settings cur;
        {
            std::unique_lock<std::mutex> lk(sys->mtx);
            sys->cv.wait(lk, [sys] { return sys->stop.load() || sys->pending != nullptr; });
            if (sys->stop.load() && !sys->pending) break;
            raw = sys->pending;
            cur = sys->pending_snap;   // immutable settings snapshot (captured on the VLC thread)
            sys->pending = nullptr;
        }
        if (!raw) continue;
        // RAII: always release the frame, even if processing below throws.
        std::unique_ptr<picture_t, void(*)(picture_t*)> pic(raw, &picture_Release);

        try {  // never let a per-frame exception terminate the worker (and VLC)
        // Pace to the device's sustainable rate. fps comes from the snapshot
        // (read on the VLC thread) -- the worker never touches VLC variables,
        // so a config save / filter reconfigure can't race with per-frame reads.
        const int cap = (cur.fps > 0) ? std::min(cur.fps, sys->dev->max_fps())
                                      : sys->dev->max_fps();
        if (cap > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - last).count();
            const int need = 1000 / cap;
            if (elapsed < need)
                std::this_thread::sleep_for(std::chrono::milliseconds(need - elapsed));
        }
        last = Clock::now();

        if (cur != prev) {
            prev = cur;
            // A live setting changed: rebuild the per-channel LUT and force a
            // clean redraw. The device's last-frame cache holds colours from the
            // OLD setting, so without invalidate_last_frame() the first new frame
            // could fall within the change threshold and be suppressed by
            // frame_changed() -> a half-updated, glitchy grid. History resets so
            // temporal smoothing snaps to the new look in one frame.
            sys->dev->set_change_threshold(cur.delta);
            sys->dev->invalidate_last_frame();
            sys->history_valid = false;
            const float gamma = cur.gamma, contrast = cur.contrast, lift = cur.lift,
                        bright = cur.brightness, led_gamma = cur.led_gamma;
            const int bits = cur.bits;
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
                v = std::pow(v, led_gamma);                                  // LED display curve (match the screen)
                v = std::rint(v * levels) / levels;                          // posterize
                sys->lut[i] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
            }
        }
        const float saturation = cur.saturation;
        const ChannelOrder order = (cur.color_order == 1) ? ChannelOrder::BGR : ChannelOrder::RGB;
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

        // --- temporal smoothing (EMA): blend with the previous grid to steady
        //     the palette and suppress per-frame shimmer. Disabled modes, or the
        //     first frame after any setting change (history_valid==false), pass
        //     through unblended and re-seed the history. ---
        if (cur.smoothing != SM_OFF && sys->history_valid &&
            sys->history_.size() == grid.size()) {
            const float a = (cur.smoothing == SM_LIGHT) ? 0.50f
                          : (cur.smoothing == SM_MEDIUM) ? 0.30f : 0.15f;
            auto mix = [&](std::uint8_t c, std::uint8_t o) -> std::uint8_t {
                float f = a * c + (1.0f - a) * o;
                return static_cast<std::uint8_t>(f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : std::rint(f));
            };
            for (std::size_t i = 0; i < grid.size(); ++i) {
                grid[i].r = mix(grid[i].r, sys->history_[i].r);
                grid[i].g = mix(grid[i].g, sys->history_[i].g);
                grid[i].b = mix(grid[i].b, sys->history_[i].b);
            }
        }
        sys->history_.assign(grid.begin(), grid.end());  // smoothed colour = new history
        sys->history_valid = true;

        // --- local-contrast (unsharp): crisp pad-to-pad edges that averaging
        //     softened. grid + k*(grid - 3x3 box blur). Runs after temporal (so
        //     the sharpening isn't smoothed away) and before dither. ---
        if (cur.sharpen != SH_OFF) {
            const float k = (cur.sharpen == SH_LIGHT) ? 0.5f : 1.0f;
            std::vector<RGB8> sharp(grid.size());
            auto us = [](int v, float bv, float kk) -> std::uint8_t {
                float f = v + kk * (v - bv);
                return static_cast<std::uint8_t>(f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : std::rint(f));
            };
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < cols; ++x) {
                    long br = 0, bg = 0, bb = 0; int c = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const RGB8& n = grid[static_cast<std::size_t>(std::clamp(y + dy, 0, rows - 1)) * cols
                                                + std::clamp(x + dx, 0, cols - 1)];
                            br += n.r; bg += n.g; bb += n.b; ++c;
                        }
                    const float bbr = static_cast<float>(br) / c, bbg = static_cast<float>(bg) / c,
                                bbb = static_cast<float>(bb) / c;
                    const RGB8& o = grid[static_cast<std::size_t>(y) * cols + x];
                    sharp[static_cast<std::size_t>(y) * cols + x] = { us(o.r, bbr, k), us(o.g, bbg, k), us(o.b, bbb, k) };
                }
            grid.swap(sharp);
        }

        // --- ordered dither (last, so it isn't smoothed away): spread the
        //     device's coarse levels / posterize into a stable Bayer pattern so
        //     gradients read smooth instead of banded. Skipped for devices that
        //     report >=256 levels (continuous, or they dither themselves). ---
        if (cur.dither != DT_OFF) {
            const int dl = sys->dev->dither_levels();
            if (dl > 1 && dl < 256) {
                const float dstep = 255.0f / (dl - 1);
                auto add = [](std::uint8_t v, float o) -> std::uint8_t {
                    float f = v + o;
                    return static_cast<std::uint8_t>(f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : std::rint(f));
                };
                for (int y = 0; y < rows; ++y)
                    for (int x = 0; x < cols; ++x) {
                        const float off = (static_cast<float>(BAYER4[y & 3][x & 3]) / 15.0f - 0.5f) * dstep;
                        RGB8& c = grid[static_cast<std::size_t>(y) * cols + x];
                        c.r = add(c.r, off); c.g = add(c.g, off); c.b = add(c.b, off);
                    }
            }
        }
        try {
            sys->dev->blit(grid.data());
            if (sys->debug) {
                const RGB8& o = grid[0];
                msg_Info(sys->self, "gridtv: tapped %dx%d bright=%.2f grid0=(%d,%d,%d)",
                         cols, rows, cur.brightness, o.r, o.g, o.b);
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
enum ColorPick { CP_AVERAGE = 0, CP_DOMINANT = 1, CP_BRIGHTEST = 2, CP_MEDIAN = 3, CP_CENTER = 4, CP_VIBRANT = 5, CP_SALIENT = 6 };

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
    case CP_VIBRANT: {  // dominant colour weighted by saturation (vivid wins over grey)
        float wcnt[512] = {0};
        int cnt[512] = {0}; long sr[512] = {0}, sg[512] = {0}, sb[512] = {0};
        for (int y = 0; y < K; ++y)
            for (int x = 0; x < K; ++x) {
                const std::uint8_t* p = base + static_cast<std::size_t>(by + y) * pitch + (bx + x) * 3;
                int bin = (p[0] >> 5) * 64 + (p[1] >> 5) * 8 + (p[2] >> 5);
                int mx = std::max(p[0], std::max(p[1], p[2]));
                int mn = std::min(p[0], std::min(p[1], p[2]));
                float sat = mx > 0 ? static_cast<float>(mx - mn) / mx : 0.0f;
                wcnt[bin] += 0.15f + sat;          // floor so grey still scores a little
                cnt[bin]++; sr[bin] += p[0]; sg[bin] += p[1]; sb[bin] += p[2];
            }
        int best = 0;
        for (int b = 1; b < 512; ++b) if (wcnt[b] > wcnt[best]) best = b;
        if (cnt[best] == 0) {
            const std::uint8_t* p = base + static_cast<std::size_t>(by + K / 2) * pitch + (bx + K / 2) * 3;
            return {p[0], p[1], p[2]};
        }
        return {static_cast<std::uint8_t>(sr[best] / cnt[best]),
                static_cast<std::uint8_t>(sg[best] / cnt[best]),
                static_cast<std::uint8_t>(sb[best] / cnt[best])};
    }
    case CP_SALIENT: {  // area-average weighted toward high-contrast (detail/subject) pixels
        std::uint8_t rs[64], gs[64], bs[64];
        int i = 0;
        long R = 0, G = 0, B = 0;
        for (int y = 0; y < K; ++y)
            for (int x = 0; x < K; ++x, ++i) {
                const std::uint8_t* p = base + static_cast<std::size_t>(by + y) * pitch + (bx + x) * 3;
                rs[i] = p[0]; gs[i] = p[1]; bs[i] = p[2];
                R += p[0]; G += p[1]; B += p[2];
            }
        const float mr = static_cast<float>(R) / i, mg = static_cast<float>(G) / i,
                    mb = static_cast<float>(B) / i;
        double wR = 0, wG = 0, wB = 0, wsum = 0;
        for (int j = 0; j < i; ++j) {
            float dr = rs[j] - mr, dg = gs[j] - mg, db = bs[j] - mb;
            float w = 1.0f + std::sqrt(dr * dr + dg * dg + db * db) * 0.02f;  // detail weighs more
            wR += rs[j] * w; wG += gs[j] * w; wB += bs[j] * w; wsum += w;
        }
        return {static_cast<std::uint8_t>(wR / wsum + 0.5),
                static_cast<std::uint8_t>(wG / wsum + 0.5),
                static_cast<std::uint8_t>(wB / wsum + 0.5)};
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

// Mean colour of the source RGB24 rectangle [x0,x1) x [y0,y1). Used by the
// area-average downscaler: each pad = true mean of every pixel behind it, so the
// grid preserves the frame's actual colour distribution rather than aliasing.
static void area_mean(const std::uint8_t* base, int pitch, int x0, int y0, int x1, int y1,
                      std::uint8_t* out3) {
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    long R = 0, G = 0, B = 0;
    for (int y = y0; y < y1; ++y) {
        const std::uint8_t* r = base + static_cast<std::size_t>(y) * pitch;
        for (int x = x0; x < x1; ++x) { R += r[x * 3]; G += r[x * 3 + 1]; B += r[x * 3 + 2]; }
    }
    const long cnt = static_cast<long>(x1 - x0) * (y1 - y0);
    out3[0] = static_cast<std::uint8_t>(R / cnt);
    out3[1] = static_cast<std::uint8_t>(G / cnt);
    out3[2] = static_cast<std::uint8_t>(B / cnt);
}

// Downscale by true area-averaging: convert to a capped-resolution RGB24 (so the
// per-pad sum stays cheap), then each pad = mean of the source pixels mapped to
// it under the aspect mode. Independent of VLC's scaler quality -> no aliasing,
// so the grid keeps the frame's real colours instead of moire artefacts.
static picture_t* downscale_area_average(image_handler_t* image, picture_t* src,
                                         int cols, int rows, int fit) {
    const int sw = src->format.i_visible_width  > 0 ? src->format.i_visible_width  : src->format.i_width;
    const int sh = src->format.i_visible_height > 0 ? src->format.i_visible_height : src->format.i_height;
    if (sw <= 0 || sh <= 0) return fit_to(image, src, cols, rows, fit);  // nothing to average
    const int CAP = 256;  // bound the working-resolution convert + sum
    double s = static_cast<double>(CAP) / std::max(sw, sh);
    if (s > 1.0) s = 1.0;
    const int cw = std::max(1, static_cast<int>(std::round(sw * s)));
    const int ch = std::max(1, static_cast<int>(std::round(sh * s)));
    picture_t* rgb = convert_rgb(image, src, cw, ch);  // averages when downscaling
    if (!rgb) return fit_to(image, src, cols, rows, fit);
    picture_t* out = new_rgb(cols, rows);
    if (!out) { picture_Release(rgb); return nullptr; }
    blacken(out);
    const std::uint8_t* base = rgb->p[0].p_pixels;
    const int rp = rgb->p[0].i_pitch;
    const int dp = out->p[0].i_pitch;
    std::uint8_t* db = out->p[0].p_pixels;

    if (fit == FIT_CONTAIN) {  // letterbox: source fits inside the grid
        const double sc = std::min(static_cast<double>(cols) / cw, static_cast<double>(rows) / ch);
        const int iw = std::max(1, static_cast<int>(std::round(cw * sc)));
        const int ih = std::max(1, static_cast<int>(std::round(ch * sc)));
        const int gx0 = (cols - iw) / 2, gy0 = (rows - ih) / 2;
        for (int gy = 0; gy < rows; ++gy)
            for (int gx = 0; gx < cols; ++gx) {
                std::uint8_t* d = db + static_cast<std::size_t>(gy) * dp + static_cast<std::size_t>(gx) * 3;
                if (gx < gx0 || gx >= gx0 + iw || gy < gy0 || gy >= gy0 + ih) continue;  // black bar
                area_mean(base, rp, (gx - gx0) * cw / iw, (gy - gy0) * ch / ih,
                          ((gx - gx0) + 1) * cw / iw, ((gy - gy0) + 1) * ch / ih, d);
            }
    } else {
        int rx, ry, rw, rh;
        if (fit == FIT_COVER) {  // centre-crop the working image to the grid aspect
            const double ga = static_cast<double>(cols) / rows;
            if (static_cast<double>(cw) / ch > ga) { rh = ch; rw = static_cast<int>(std::round(ch * ga)); }
            else { rw = cw; rh = static_cast<int>(std::round(cw / ga)); }
            rx = (cw - rw) / 2; ry = (ch - rh) / 2;
        } else {  // FIT_STRETCH
            rx = 0; ry = 0; rw = cw; rh = ch;
        }
        for (int gy = 0; gy < rows; ++gy)
            for (int gx = 0; gx < cols; ++gx) {
                std::uint8_t* d = db + static_cast<std::size_t>(gy) * dp + static_cast<std::size_t>(gx) * 3;
                area_mean(base, rp, rx + gx * rw / cols, ry + gy * rh / rows,
                          rx + (gx + 1) * rw / cols, ry + (gy + 1) * rh / rows, d);
            }
    }
    picture_Release(rgb);
    return out;
}

// Display-path call: downscale to a tiny RGB24 on the VLC thread (image_Convert
// is not safe off-thread), hand it to the worker for the MIDI blit, and return
// the original picture immediately. The display path never blocks on MIDI.
static picture_t* Filter(filter_t* f, picture_t* p_pic) {
    auto* sys = (filter_sys_t*)f->p_sys;
    if (p_pic && sys && sys->image && sys->dev && sys->connected.load()) {
        const int cols = sys->dev->cols(), rows = sys->dev->rows();
        // Read + clamp EVERY live setting here (VLC thread) and hand the worker
        // an immutable snapshot -- the worker never calls var_Inherit*, which is
        // what could crash VLC when saving settings during playback.
        Settings snap = snapshot_settings(f);
        const int mode = snap.pick;
        const int fit = snap.aspect;
        picture_t* small = (mode == CP_AVERAGE)
            ? downscale_area_average(sys->image, p_pic, cols, rows, fit)
            : downscale_custom(sys->image, p_pic, cols, rows, mode, fit);
        if (small) {
            std::lock_guard<std::mutex> lk(sys->mtx);
            if (sys->pending) picture_Release(sys->pending);  // drop stale frame
            sys->pending = small;
            sys->pending_snap = snap;   // paired with the frame; consumed atomically by the worker
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
static const int gridtv_colorpick_values[] = { 0, 1, 2, 3, 4, 5, 6 };
static const char* const gridtv_colorpick_texts[] = {
    "Average (smooth)", "Dominant colour (crisp)", "Brightest pixel", "Median", "Centre pixel", "Vibrant (saturated, punchy)", "Salient (detail-weighted)"
};

// Aspect/fit modes for the --gridtv-aspect dropdown.
static const int gridtv_aspect_values[] = { 0, 1, 2 };
static const char* const gridtv_aspect_texts[] = {
    "Stretch (distort to fill)", "Fill (crop to fill)", "Fit (letterbox / show all)"
};

// Temporal-smoothing presets for the --gridtv-smoothing dropdown.
static const int gridtv_smoothing_values[] = { 0, 1, 2, 3 };
static const char* const gridtv_smoothing_texts[] = {
    "Off", "Light", "Medium", "Strong"
};

// Dither modes for the --gridtv-dither dropdown.
static const int gridtv_dither_values[] = { 0, 1 };
static const char* const gridtv_dither_texts[] = {
    "Off", "Ordered (Bayer)"
};

// Local-contrast sharpen presets for the --gridtv-sharpen dropdown.
static const int gridtv_sharpen_values[] = { 0, 1, 2 };
static const char* const gridtv_sharpen_texts[] = {
    "Off", "Light", "Strong"
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
        "  Brightness, gamma, LED display curve, contrast, saturation, lift, "
        "bit depth, smoothing, dither, sharpen, max FPS, colour-pick and fit all "
        "update instantly while the video plays - no restart needed.\n\n"
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
    add_integer("gridtv-colorpick", CP_SALIENT,
                N_("Colour pick"), N_("How each pad's colour is chosen from the "
                "block of video behind it. Salient (default) weights the average "
                "toward detail / subject pixels so the subject reads through instead "
                "of being flattened by a pure mean. Dominant picks the most common "
                "colour (crisp). Vibrant favours saturated colours. Average is a "
                "smooth area-mean. Brightest emphasises highlights. Median is a "
                "robust middle. Centre uses the single centre pixel (sharpest, "
                "noisiest)."), false)
        change_integer_list(gridtv_colorpick_values, gridtv_colorpick_texts)
        change_safe()
    add_integer("gridtv-smoothing", SM_OFF,
                N_("Smoothing (temporal)"), N_("Blends each grid frame with the "
                "previous one to steady the colours and suppress per-frame "
                "shimmer/flicker - a big readability win on noisy or fast video. "
                "Off = every frame raw; Light/Medium/Strong = progressively more "
                "smoothing (Strong can trail briefly on fast motion). Resets "
                "instantly when you change any setting."), false)
        change_integer_list(gridtv_smoothing_values, gridtv_smoothing_texts)
        change_safe()
    add_integer("gridtv-dither", DT_OFF,
                N_("Dither"), N_("Spreads the device's coarse brightness steps "
                "(and the Bit-depth posterize) into a fine, stable checkerboard "
                "so gradients look smooth instead of banded. Ordered (Bayer) is "
                "flicker-free. Mainly helps Launchpads (64/128 levels); no effect "
                "on the monome (it already dithers) or where levels are effectively "
                "continuous."), false)
        change_integer_list(gridtv_dither_values, gridtv_dither_texts)
        change_safe()
    add_integer("gridtv-sharpen", SH_LIGHT,
                N_("Sharpen (local contrast)"), N_("A mild unsharp mask on the grid "
                "that crisps pad-to-pad edges the downscale softened, so the image "
                "reads a little sharper/more defined. Light is subtle; Strong is "
                "punchy (can emphasise noise on already-busy video). No effect on a "
                "1xN grid."), false)
        change_integer_list(gridtv_sharpen_values, gridtv_sharpen_texts)
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
    add_float_with_range("gridtv-ledgamma", 2.0f, 1.0f, 3.0f,
                         N_("LED display curve (gamma)"), N_("Compensates for the "
                         "controller's near-linear LED response so the colours and "
                         "brightness on the grid match your screen. The LED levels "
                         "are roughly linear in light output, but video is gamma-"
                         "encoded - so without correction (1.0) mid-tones look flat "
                         "and washed out and colours lose punch. 2.0-2.4 deepens them "
                         "to look vibrant and screen-accurate; raise it for a "
                         "punchier, contrasty look, lower toward 1.0 for a flatter "
                         "raw look. Applies to every device (Launchpad, monome, "
                         "classic). Tip: play a familiar scene, set it so the grid's "
                         "greys and skin tones look about as bright as on screen."), false)
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

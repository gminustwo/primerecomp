// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// Render targets the game sizes for its screen image (MPR: 1600x900, the Switch's docked resolution - the TV gets
// it scaled up to 1080p) are allocated at (sx, sy) times their size; the image is then drawn into (vx, vy, vw, vh)
// of the window. Video tab "Internal resolution" (g_settings.res_mode):
//   Match display  render at the window's own pixel size (default): a 4K window renders 3840 x 2160
//   900p .. 8K     that many lines (RES_FIXED, g_settings.res_lines; wide windows keep their shape); more than the
//                  window is drawn into it smaller (supersampling); capped to the GPU's 16384-pixel texture limit
//   .75x / .50x / .25x  of Match display (scaled up to the window - with MetalFX on macOS when chosen)
//   Dynamic        Match display, lowered in steps while the GPU can't hold 60 fps and raised again when it can
//                  (each change reallocates the render targets: a short hitch, so it changes rarely, with hysteresis)
// Test overrides: LARECOMP_SCALE=<f> (render height = the game's x f), LARECOMP_WIDE_TEST=<f> (forced shape).
#include <math.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "output_scale.h"

#define MAX_TEX 16384.0

// Dynamic resolution: a factor on the Match display scale, 0.5 .. 1 in 0.05 steps.
static double g_dyn = 1.0;
static double g_dyn_gpu_sum, g_dyn_dt_sum, g_last_t;
static int g_dyn_n, g_dyn_gpu_n, g_dyn_good;
static double g_dyn_cool_until, g_dyn_probe_wait = 5.0, g_dyn_probe_at;
static double g_dyn_last_raise = -100;

static void dyn_update(double now, double gpu_ms) {
    double dt = g_last_t > 0 ? (now - g_last_t) * 1000.0 : 16.7;
    g_last_t = now;
    if (dt > 250) return;  // (loading, menus, a stall: not the GPU's steady state)
    g_dyn_dt_sum += dt;
    g_dyn_n++;
    if (gpu_ms >= 0) { g_dyn_gpu_sum += gpu_ms; g_dyn_gpu_n++; }
    if (g_dyn_n < 30) return;  // decide every ~half second
    double avg_dt = g_dyn_dt_sum / g_dyn_n, avg_gpu = g_dyn_gpu_n ? g_dyn_gpu_sum / g_dyn_gpu_n : -1;
    g_dyn_dt_sum = g_dyn_gpu_sum = 0;
    g_dyn_n = g_dyn_gpu_n = 0;
    if (now < g_dyn_cool_until) return;
    double prev = g_dyn;
    bool slow = avg_dt > 17.4 || (avg_gpu >= 0 && avg_gpu > 14.5);
    if (slow) {
        g_dyn = fmax(0.5, g_dyn - (avg_dt > 22 ? 0.1 : 0.05));
        g_dyn_good = 0;
        if (now - g_dyn_last_raise < 3.0) g_dyn_probe_wait = fmin(g_dyn_probe_wait * 2, 60.0);  // raised too eagerly
    } else if (g_dyn < 1.0) {
        g_dyn_good++;
        // with GPU timing: raise when there is clear headroom; without (Vulkan): probe upwards now and then
        bool room = avg_gpu >= 0 ? avg_gpu < 10.5 && g_dyn_good >= 4 : now >= g_dyn_probe_at && g_dyn_good >= 2 * (int)g_dyn_probe_wait;
        if (room) { g_dyn = fmin(1.0, g_dyn + 0.05); g_dyn_good = 0; g_dyn_last_raise = now; g_dyn_probe_at = now + g_dyn_probe_wait; }
    }
    g_dyn = round(g_dyn * 20) / 20;
    if (g_dyn != prev) {
        g_dyn_cool_until = now + 1.5;
        LOG("[dynres] %.2f -> %.2f (frame %.1f ms, gpu %.1f ms)\n", prev, g_dyn, avg_dt, avg_gpu);
    }
}

double output_scale_dynamic_factor(void) { return g_dyn; }

static double g_render_w = 1920, g_render_h = 1080;
void output_scale_render_size(double *w, double *h) { *w = g_render_w; *h = g_render_h; }

void output_scale(double dw, double dh, double bw, double bh, bool reshape_allowed, double now, double gpu_ms, OutScale *o) {
    if (bw < 1 || bh < 1) bw = 1920, bh = 1080;
    double fit = fmin(dw / bw, dh / bh);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("LARECOMP_SCALE") ? atof(getenv("LARECOMP_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("LARECOMP_WIDE_TEST") ? atof(getenv("LARECOMP_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (bw / bh);
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005;
    if (!reshape) shape = 1.0;
    // render height in units of the game's own (bh lines)
    double h;
    int mode = g_settings.res_mode;
    if (forced > 0) h = forced;
    else if (mode == RES_FIXED) h = g_settings.res_lines / bh;
    else {
        h = reshape ? dh / bh : fit;  // Match display
        if (mode == RES_DYNAMIC) { dyn_update(now, gpu_ms); h *= g_dyn; }
        else if (mode == RES_Q75) h *= 0.75;
        else if (mode == RES_Q25) h *= 0.25;
        else if (mode == RES_Q50) h *= 0.5;
    }
    h = fmax(h, 0.1);  // (.25x of a small window)
    double sx = h * shape, sy = h;
    double cap = fmin(MAX_TEX / (bw * sx), MAX_TEX / (bh * sy));
    if (cap < 1.0) { sx *= cap; sy *= cap; }
    o->sx = sx; o->sy = sy; o->shape = shape;
    g_render_w = round(bw * sx); g_render_h = round(bh * sy);
    // where the image goes in the window: fills it when reshaped, else a fitted box of the game's shape
    if (reshape) { o->vw = dw; o->vh = dh; }
    else { o->vw = bw * fit; o->vh = bh * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (bw * wide_test), dh / bh);
        o->vw = bw * wide_test * f2; o->vh = bh * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
}

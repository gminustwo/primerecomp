// Output resolution (runtime/nvn/output_scale.c), shared by the Metal and Vulkan backends.
#pragma once
#include <stdbool.h>

typedef struct {
    double sx, sy;             // render-target scale (relative to the sizes the game gives its targets)
    double shape;              // window aspect / 16:9 when the camera is widened, else 1
    double vx, vy, vw, vh;     // where the image is drawn in the window (pixels)
} OutScale;

// dw x dh: the window's drawable size in pixels; bw x bh: the game's presented image as the game sizes it (MPR:
// 1600 x 900; 0: 1920 x 1080); now: seconds; gpu_ms: GPU time of the last frame (< 0: unknown)
void output_scale(double dw, double dh, double bw, double bh, bool reshape_allowed, double now, double gpu_ms, OutScale *o);
double output_scale_dynamic_factor(void);
// The game image's size at the current scale (the Video tab's "Rendering at ...")
void output_scale_render_size(double *w, double *h);

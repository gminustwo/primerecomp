// Motion controls: the controller's gyroscope and accelerometer (SDL) -> nn::hid's six-axis sensor, for Metroid Prime
// Remastered's own motion aiming (the motion control schemes in its Options -> Controls). The game reads all of it:
// CNXSixAxisAnalysis::UpdateMotion the acceleration and angular velocity, UpdateCursor the orientation matrix and the
// "connected" attribute.
//
// Frames: the Switch's (yuzu's SDL conversion) x right, y forward (away from the player), z up; SDL's, for a pad held
// in front of you, x right, y up, z toward the player. Angular velocity in turns per second (1 = 360 deg/s), positive
// counter-clockwise seen from the axis' end; the accelerometer in G, -1 on z lying level (it reads gravity, not the
// push against it); angle: the angular velocity summed up (turns); direction: the controller's axes in the world (rows
// x, y, z), the identity when it lies level pointing ahead - integrated from the gyro, its tilt pulled toward the
// measured gravity (a Mahony filter's proportional term), its heading free (the game recentres its cursor itself).
// The gyro's bias is learnt while the controller is still (SDL passes the raw sensor).
//
// Setting: Gameplay tab "Gyro aiming" (g_settings.gyro); off, or no gyro on the controller: the sensor reads
// disconnected. Tests (no controller needed): LARECOMP_GYRO_TEST="frame:x:y:z:frames,..." turns the controller at x,
// y, z degrees per second (Switch frame: x = pitch up, z = yaw left) for that many frames, straight into the
// integration; LARECOMP_GYRO_SDL_TEST (below) feeds raw SDL samples instead; LARECOMP_GYRO_REST=0/1 forces "at rest";
// LARECOMP_LOG_GYRO=1 logs the SDL path once a second.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "wiimote.h"

typedef struct {        // nn::hid::SixAxisSensorState
    int64_t delta_time;  // ns between the last two samples
    int64_t sampling;
    float accel[3];      // G
    float gyro[3];       // turns / s
    float angle[3];      // turns
    float dir[9];        // the controller's x, y, z axes in the world
    uint32_t attributes; // bit 0 connected, bit 1 interpolated
    uint32_t reserved;
} SixAxisState;
_Static_assert(sizeof(SixAxisState) == 0x60, "nn::hid::SixAxisSensorState is 0x60 bytes");

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    SDL_Gamepad *pad;     // (the main thread's; NULL: none with a gyro)
    bool have_accel;
    float accel[3];       // G, Switch frame
    float w[3];           // rad / s, bias removed
    float bias[3];        // rad / s
    float angle[3];       // turns
    float R[9];           // controller -> world, row-major
    uint64_t last_ns;     // the last gyro sample's sensor timestamp
    int64_t dt_ns, sampling;
    float still_s;        // how long the controller has been still (lying somewhere)
    float accel_jitter;   // G, the accelerometer's sample-to-sample change, smoothed
    float g_mean[3];      // rad / s, the raw gyro over the last quarter second
    float g_jitter;       // rad / s, how far it strays from that, smoothed
    bool test;            // LARECOMP_GYRO_TEST drives it
} m = {.R = {1, 0, 0, 0, 1, 0, 0, 0, 1}};

static void mat_mul3(const float *a, const float *b, float *out) {
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j];
    memcpy(out, r, sizeof r);
}

static void orthonormalize(float *R) {  // (rows; Gram-Schmidt, z = x cross y)
    float *x = R, *y = R + 3, *z = R + 6;
    float n = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int i = 0; i < 3; i++) x[i] /= n;
    float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (int i = 0; i < 3; i++) y[i] -= d * x[i];
    n = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    for (int i = 0; i < 3; i++) y[i] /= n;
    z[0] = x[1] * y[2] - x[2] * y[1], z[1] = x[2] * y[0] - x[0] * y[2], z[2] = x[0] * y[1] - x[1] * y[0];
}

// One gyro sample: w (rad / s, Switch frame, bias removed) for dt seconds. Under g_lock.
static void integrate(const float *w_in, float dt) {
    float w[3] = {w_in[0], w_in[1], w_in[2]};
    float an = sqrtf(m.accel[0] * m.accel[0] + m.accel[1] * m.accel[1] + m.accel[2] * m.accel[2]);
    if (m.have_accel && an > 0.8f && an < 1.2f) {
        // tilt: measured up (against gravity) x estimated up, both in the controller's frame (R's third row is the
        // world's up seen from the controller); a small turn toward it
        float a[3] = {-m.accel[0] / an, -m.accel[1] / an, -m.accel[2] / an}, *v = m.R + 6;
        const float kp = 0.5f;
        w[0] += kp * (a[1] * v[2] - a[2] * v[1]);
        w[1] += kp * (a[2] * v[0] - a[0] * v[2]);
        w[2] += kp * (a[0] * v[1] - a[1] * v[0]);
    }
    float th = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * dt;
    if (th > 1e-9f) {  // R = R * exp([w] dt) (Rodrigues)
        float k[3] = {w[0] * dt / th, w[1] * dt / th, w[2] * dt / th}, s = sinf(th), c = 1 - cosf(th);
        float Q[9] = {1 - c * (k[1] * k[1] + k[2] * k[2]), -s * k[2] + c * k[0] * k[1], s * k[1] + c * k[0] * k[2],
                      s * k[2] + c * k[0] * k[1], 1 - c * (k[0] * k[0] + k[2] * k[2]), -s * k[0] + c * k[1] * k[2],
                      -s * k[1] + c * k[0] * k[2], s * k[0] + c * k[1] * k[2], 1 - c * (k[0] * k[0] + k[1] * k[1])};
        mat_mul3(m.R, Q, m.R);
        orthonormalize(m.R);
    }
    for (int i = 0; i < 3; i++) m.angle[i] += w_in[i] * dt / (2 * (float)M_PI);
    memcpy(m.w, w_in, sizeof m.w);
    m.sampling++;
    m.dt_ns = (int64_t)(dt * 1e9f);
}

static void sensor_sample(int sensor, const float *d, uint64_t ts) {
    pthread_mutex_lock(&g_lock);
    if (sensor == SDL_SENSOR_ACCEL) {
        const float g = SDL_STANDARD_GRAVITY;
        float a[3] = {-d[0] / g, d[2] / g, -d[1] / g}, j = 0;
        for (int i = 0; i < 3; i++) j = fmaxf(j, fabsf(a[i] - m.accel[i]));
        m.accel_jitter = m.have_accel ? m.accel_jitter + (j - m.accel_jitter) * 0.05f : 1;  // (sample to sample, smoothed)
        memcpy(m.accel, a, sizeof a);
        m.have_accel = true;
    } else if (sensor == SDL_SENSOR_GYRO && !m.test) {
        float raw[3] = {d[0], -d[2], d[1]};
        float dt = m.last_ns && ts > m.last_ns ? (float)((ts - m.last_ns) * 1e-9) : 0;
        m.last_ns = ts;
        if (dt > 0.1f) dt = 0;  // (a pause: no jump)
        // still = lying on something, not held: the gyro's reading steady (within ~0.3 deg/s of its quarter-second
        // mean - a hand always trembles more), that mean small (a bias, not a turn) and the accelerometer steady at
        // 1 G. A second of it teaches the bias (the mean: slowly); a second and a half is "at rest" - the game's "put
        // down" (picked up again, it recentres the pointer; a gyro reading exactly 0 between moves made it ask every
        // few seconds). (Judged by steadiness, not by closeness to the bias: a bias larger than the threshold would
        // never have been learnt.)
        float k = fminf(1, dt / 0.25f), dev = 0, mean = 0;
        for (int i = 0; i < 3; i++) {
            m.g_mean[i] += (raw[i] - m.g_mean[i]) * k;
            dev = fmaxf(dev, fabsf(raw[i] - m.g_mean[i]));
            mean = fmaxf(mean, fabsf(m.g_mean[i]));
        }
        m.g_jitter += (dev - m.g_jitter) * k;
        float an = sqrtf(m.accel[0] * m.accel[0] + m.accel[1] * m.accel[1] + m.accel[2] * m.accel[2]);
        bool steady = !m.have_accel || (fabsf(an - 1) < 0.05f && m.accel_jitter < 0.02f);
        if (m.g_jitter < 0.006f && mean < 0.1f && steady) m.still_s += dt;
        else m.still_s = 0;
        if (m.still_s > 1.0f)
            for (int i = 0; i < 3; i++) m.bias[i] += (m.g_mean[i] - m.bias[i]) * fminf(1, dt * 0.5f);
        float w[3] = {raw[0] - m.bias[0], raw[1] - m.bias[1], raw[2] - m.bias[2]};
        if (dt > 0) integrate(w, dt);
        static int nlog = -1;
        if (nlog < 0) nlog = getenv("LARECOMP_LOG_GYRO") ? 0 : 1 << 30;
        if (nlog < 1 << 30 && m.sampling % 240 == 0)
            LOG("[gyro] w %.3f %.3f %.3f rad/s bias %.4f %.4f %.4f accel %.2f %.2f %.2f jitter %.3f G %.4f rad/s still "
                "%.1f s fwd %.2f %.2f %.2f\n", w[0], w[1], w[2], m.bias[0], m.bias[1], m.bias[2], m.accel[0], m.accel[1],
                m.accel[2], m.accel_jitter, m.g_jitter, m.still_s, m.R[1], m.R[4], m.R[7]);
    }
    pthread_mutex_unlock(&g_lock);
}

// From runtime/hle/input.c (main thread): the controller it opened / closed, and every event
void motion_pad(SDL_Gamepad *pad) {
    bool gyro = pad && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO), accel = pad && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    if (pad) LOG("[input] %s: gyro %s, accelerometer %s\n", SDL_GetGamepadName(pad), gyro ? "yes" : "no", accel ? "yes" : "no");
    if (gyro) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
    if (accel) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    pthread_mutex_lock(&g_lock);
    m.pad = gyro ? pad : NULL;
    m.have_accel = false;
    m.last_ns = 0;
    m.still_s = 0;
    m.accel_jitter = m.g_jitter = 1;
    memset(m.bias, 0, sizeof m.bias);
    pthread_mutex_unlock(&g_lock);
}

void motion_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE && m.pad && e->gsensor.which == SDL_GetGamepadID(m.pad))
        sensor_sample(e->gsensor.sensor, e->gsensor.data, e->gsensor.sensor_timestamp);
}

static bool g_sdl_test;  // LARECOMP_GYRO_SDL_TEST drives sensor_sample
// A Wii Remote that points (runtime/hle/wiimote.c), or its stand-in (LARECOMP_WIIMOTE_TEST)
static bool wiimote_drives(void) {
    static int test = -1;
    if (test < 0) test = getenv("LARECOMP_WIIMOTE_TEST") != NULL;
    if (test) return true;
    if (!g_settings.wiimote) return false;
    WiimoteState s;
    wiimote_state(&s);
    return s.connected;
}

bool motion_available(void) { return m.pad != NULL || m.test || g_sdl_test || wiimote_drives(); }

// LARECOMP_GYRO_SDL_TEST="frame:x:y:z:frames,...": raw SDL samples instead (rad / s in SDL's frame, 4 per frame at
// 240 Hz, the accelerometer at rest), with LARECOMP_GYRO_BIAS="x:y:z" (rad / s) added and LARECOMP_GYRO_NOISE=<rad / s>
// of random shake - the path a real controller takes (bias learning, "at rest").
static void sdl_test_frame(uint64_t frame, const char *spec) {
    static float bias[3], noise = -1;
    static uint64_t ts = 1000000000ull;
    if (noise < 0) {
        g_sdl_test = true;
        noise = getenv("LARECOMP_GYRO_NOISE") ? (float)atof(getenv("LARECOMP_GYRO_NOISE")) : 0;
        if (getenv("LARECOMP_GYRO_BIAS")) sscanf(getenv("LARECOMP_GYRO_BIAS"), "%f:%f:%f", &bias[0], &bias[1], &bias[2]);
        LOG("[gyro] LARECOMP_GYRO_SDL_TEST: bias %g %g %g, noise %g rad/s\n", bias[0], bias[1], bias[2], noise);
    }
    float w[3] = {0, 0, 0};
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 60;
        float x = 0, y = 0, z = 0;
        if (sscanf(tok, "%lu:%f:%f:%f:%lu", &start, &x, &y, &z, &len) >= 4 && frame >= start && frame < start + len)
            w[0] = x, w[1] = y, w[2] = z;
    }
    for (int k = 0; k < 4; k++) {
        ts += 1000000000ull / 240;
        float g[3], a[3] = {0, SDL_STANDARD_GRAVITY, 0};  // (held level: the push against gravity is up, SDL +y)
        for (int i = 0; i < 3; i++) {
            float r = noise * ((float)rand() / RAND_MAX * 2 - 1);
            g[i] = w[i] + bias[i] + r;
            a[i] += r * 0.5f;  // (a shaking hand shakes the accelerometer too: m/s^2)
        }
        sensor_sample(SDL_SENSOR_ACCEL, a, ts);
        sensor_sample(SDL_SENSOR_GYRO, g, ts);
    }
}

// LARECOMP_GYRO_TEST / LARECOMP_GYRO_SDL_TEST, once per presented frame (runtime/hle/input.c input_frame)
void motion_frame(uint64_t frame) {
    if (getenv("LARECOMP_GYRO_SDL_TEST")) { sdl_test_frame(frame, getenv("LARECOMP_GYRO_SDL_TEST")); return; }
    const char *spec = getenv("LARECOMP_GYRO_TEST");
    if (!spec) return;
    float w[3] = {0, 0, 0};
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok;) {  // frame:x:y:z:frames, separated by ','
        unsigned long start = 0, len = 60;
        float x = 0, y = 0, z = 0;
        if (sscanf(tok, "%lu:%f:%f:%f:%lu", &start, &x, &y, &z, &len) >= 4 && frame >= start && frame < start + len)
            w[0] = x * (float)M_PI / 180, w[1] = y * (float)M_PI / 180, w[2] = z * (float)M_PI / 180;
        tok = strtok(NULL, ",");
    }
    pthread_mutex_lock(&g_lock);
    if (!m.test) m.test = true, m.accel[2] = -1, m.have_accel = true, LOG("[gyro] LARECOMP_GYRO_TEST: synthetic motion\n");
    integrate(w, 1.0f / 60);
    // (the accelerometer follows: gravity seen from the turned controller)
    m.accel[0] = -m.R[6], m.accel[1] = -m.R[7], m.accel[2] = -m.R[8];
    pthread_mutex_unlock(&g_lock);
}


// ---- the Wii Remote as the controller's motion (runtime/hle/wiimote.c)
// Its pointer (where it points on the screen, -1..1) becomes the controller's orientation: turned WM_YAW_EDGE to the
// side at the screen's edge, tilted WM_PITCH_EDGE up or down - what the game's Pointer scheme turns into its cursor
// (CNXSixAxisAnalysis::UpdateCursor: the forward axis' heading and pitch from a reference, 2 / pi of a radian per
// unit). The reference is kept straight ahead (wiimote_pin_cursor) so the cursor goes where the remote points
// whenever the game recentres. Its accelerometer is the controller's (the game's shake / flick gestures); the turning
// rate comes from the pointer's motion. The sensor bar out of sight: the last orientation stays.
// (measured at the game's default aim sensitivity: 0.35 put the reticle 1.33 times as far out as the pointer, both ways)
#define WM_YAW_EDGE 0.26f    // radians at the screen's edge (LARECOMP_WIIMOTE_EDGE=<x>:<y> for measuring)
#define WM_PITCH_EDGE 0.26f
static struct { float p[2], yaw, pitch, still_s, last_accel[3]; uint64_t last_ns; int64_t sampling; bool have; } wm;

static void wiimote_motion(SixAxisState *s, uint64_t frame) {
    static float edge[2] = {-1, -1};
    if (edge[0] < 0) {
        edge[0] = WM_YAW_EDGE, edge[1] = WM_PITCH_EDGE;
        if (getenv("LARECOMP_WIIMOTE_EDGE")) sscanf(getenv("LARECOMP_WIIMOTE_EDGE"), "%f:%f", &edge[0], &edge[1]);
    }
    WiimoteState r;
    wiimote_state(&r);
    float px = 0, py = 0;
    bool seen = r.pointer_valid;
    if (seen) px = r.pointer[0], py = r.pointer[1];
    if (wiimote_test_pointer(frame, &px, &py)) seen = px > -9, r.accel[2] = 1, r.connected = true;  // (-9: not seen)
    uint64_t now = SDL_GetTicksNS();
    float dt = wm.last_ns ? (float)((now - wm.last_ns) * 1e-9) : 1.0f / 60;
    if (dt <= 0 || dt > 0.1f) dt = 1.0f / 60;
    wm.last_ns = now;
    if (seen) {  // smoothed (the setting: how much of the old position stays each 60th of a second)
        float k = wm.have ? 1 - powf(fminf(fmaxf(g_settings.wiimote_smooth, 0), 0.9f), dt * 60) : 1;
        px = fmaxf(-1.5f, fminf(1.5f, px)), py = fmaxf(-1.5f, fminf(1.5f, py));
        wm.p[0] += (px - wm.p[0]) * k, wm.p[1] += (py - wm.p[1]) * k;
        wm.have = true;
    }
    float yaw = wm.p[0] * edge[0], pitch = wm.p[1] * edge[1];
    float wz = -(yaw - wm.yaw) / dt, wx = (pitch - wm.pitch) / dt;  // (rad / s: turning right is clockwise about z)
    wm.yaw = yaw, wm.pitch = pitch;
    float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
    float rows[9] = {cy, -sy, 0, sy * cp, cy * cp, sp, -sy * sp, -cy * sp, cp};  // the controller's x, y, z in the world
    memcpy(s->dir, rows, sizeof rows);
    // the remote's accelerometer (its x left, y toward the camera, z up; it reads the push against gravity, +1 on z
    // lying flat) as the Switch's (x right, y forward, z up; gravity itself, -1 on z)
    s->accel[0] = r.accel[0], s->accel[1] = -r.accel[1], s->accel[2] = -r.accel[2];
    s->gyro[0] = wx / (2 * (float)M_PI), s->gyro[2] = wz / (2 * (float)M_PI);
    s->angle[0] = pitch / (2 * (float)M_PI), s->angle[2] = -yaw / (2 * (float)M_PI);
    s->delta_time = (int64_t)(dt * 1e9f);
    s->sampling = ++wm.sampling;
    s->attributes = 1;
    // at rest: lying still (the accelerometer steady at 1 G for 1.5 s) and not pointing at the screen - the game's
    // "put down" (its calibration waits for it; picked up again, it recentres, which the pinned reference makes
    // harmless). A remote that sees the sensor bar is in a hand.
    float an = sqrtf(r.accel[0] * r.accel[0] + r.accel[1] * r.accel[1] + r.accel[2] * r.accel[2]), j = 0;
    for (int i = 0; i < 3; i++) j = fmaxf(j, fabsf(r.accel[i] - wm.last_accel[i]));
    memcpy(wm.last_accel, r.accel, sizeof wm.last_accel);
    wm.still_s = !seen && fabsf(an - 1) < 0.06f && j < 0.03f ? wm.still_s + dt : 0;
}

// Before the game reads its controller each frame (CNXJoyCon_NPad::Poll, a virtual call: hle_override): player 1's
// six-axis analysis (pad + 0x2908 + slot * 0xE0, the slot at pad + 0x1C: CNXJoyCon_NPad::ResetRightCursor) gets its
// cursors' reference set straight ahead - heading (0, 0, 1) at +0xC0 / +0xCC in its x' = -x, z' = y frame, pitch
// acos(0) at +0xD8 / +0xDC - and its recentre requests (+0x9C / +0x9D) dropped, while the Wii Remote points.
static GuestFn g_npad_poll;
static void rt_npad_poll(Cpu *c) {
    uint64_t pad = c->x[0];
    if (wiimote_drives() && g_settings.gyro) {
        int32_t slot = (int32_t)LD32(pad + 0x1c);
        if (slot >= 0 && slot < 9) {
            uint8_t *a = (uint8_t *)(uintptr_t)(pad + 0x2908 + (uint64_t)slot * 0xE0);
            const float fwd[3] = {0, 0, 1}, pitch = (float)M_PI / 2;
            memcpy(a + 0xc0, fwd, 12), memcpy(a + 0xcc, fwd, 12);
            memcpy(a + 0xd8, &pitch, 4), memcpy(a + 0xdc, &pitch, 4);
            a[0x9c] = a[0x9d] = 0;
            static int logged;
            if (!logged++) LOG("[wiimote] pointer: the game's cursor reference kept ahead (pad %llx, slot %d)\n",
                               (unsigned long long)pad, slot);
        }
    }
    g_npad_poll(c);
}

void motion_hooks_init(void) {  // (each boot: runtime/hle/game.c game_hooks_init)
    uint64_t a = guest_sym("_ZN14CNXJoyCon_NPad4PollEf");
    static bool hooked;
    g_npad_poll = dispatch_lookup(a);
    if (g_npad_poll && !hooked) hle_override(a, rt_npad_poll, "CNXJoyCon_NPad::Poll"), hooked = true;
}

static bool sensor_on(void) { return g_settings.gyro && motion_available(); }

// GetSixAxisSensorHandles(handles*, count, const NpadId&, NpadStyleSet): FullKey / Handheld / JoyDual give one
// sensor per controller half; handle = style index, player, device (as the vibration handles: input.c)
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
         hid_sixaxis_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int cap = (int)c->x[1];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2], style = (uint32_t)c->x[3];
    int n = style & 0x6 ? 2 : 1;  // (Handheld / JoyDual: left and right; FullKey: one)
    if (n > cap) n = cap;
    uint32_t type = style & 1 ? 3 : style & 2 ? 4 : style & 4 ? 5 : style & 8 ? 6 : 7;
    for (int i = 0; i < n; i++) out[i] = type | (id & 0xFF) << 8 | (uint32_t)(n == 1 ? 2 : i) << 16;
    LOG("[gyro] six-axis handles: player %u, style %#x -> %d\n", id, style, n);
    c->x[0] = (uint64_t)n;
}

HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_sixaxis_start) {
    static int nlog;
    if (nlog++ < 4) LOG("[gyro] StartSixAxisSensor %#x (controller gyro: %s)\n", *(uint32_t *)(uintptr_t)c->x[0],
                        m.pad ? "yes" : "no");
}

HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",
         hid_sixaxis_drift) { (void)c; }

HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_sixaxis_state) {
    SixAxisState *s = (SixAxisState *)(uintptr_t)c->x[0];
    memset(s, 0, sizeof *s);
    s->dir[0] = s->dir[4] = s->dir[8] = 1;
    if (!sensor_on()) return;  // (disconnected: attributes 0)
    if (wiimote_drives()) {
        extern uint64_t vk_frame_counter(void);
        wiimote_motion(s, vk_frame_counter());
        return;
    }
    pthread_mutex_lock(&g_lock);
    s->delta_time = m.dt_ns ? m.dt_ns : 5000000;
    s->sampling = m.sampling;
    memcpy(s->accel, m.accel, sizeof s->accel);
    for (int i = 0; i < 3; i++) s->gyro[i] = m.w[i] / (2 * (float)M_PI);
    memcpy(s->angle, m.angle, sizeof s->angle);
    for (int r = 0; r < 3; r++)  // rows: the controller's axes in the world = R's columns
        for (int k = 0; k < 3; k++) s->dir[3 * r + k] = m.R[3 * k + r];
    s->attributes = 1;
    pthread_mutex_unlock(&g_lock);
}

HLE_FUNC("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", hid_sixaxis_at_rest) {
    static int env = -2;  // (LARECOMP_GYRO_REST=0/1: forced, tests; LARECOMP_GYRO_TEST's motion is never at rest)
    if (env == -2) env = getenv("LARECOMP_GYRO_REST") ? atoi(getenv("LARECOMP_GYRO_REST")) : -1;
    // The Wii Remote lying still counts only out of play (the pause screens, the map, the game's messages): the game
    // calibrates the Pointer scheme there (it waits for the controller to be put down), but put down in play and
    // picked up again it stops the game for "The pointer will now be recentered." - which an IR pointer, absolute,
    // has no use for (rt_npad_poll keeps its references pinned anyway). Seen with the real remote: four such stops in
    // three minutes, each time it was laid on the desk a moment.
    extern bool game_in_play(void);
    pthread_mutex_lock(&g_lock);
    bool rest = wiimote_drives() ? wm.still_s > 1.5f && !game_in_play() : m.still_s > 1.5f;
    pthread_mutex_unlock(&g_lock);
    c->x[0] = env >= 0 ? env : !sensor_on() || rest;
    if (getenv("LARECOMP_LOG_REST")) {  // (debug) who asks, and what it's told, on each change per caller
        extern uint64_t vk_frame_counter(void);
        static uint64_t lr[8]; static int ans[8], n;
        int k = 0;
        while (k < n && lr[k] != c->x[30]) k++;
        if (k == n && n < 8) lr[n] = c->x[30], ans[n++] = -1;
        if (k < 8 && ans[k] != (int)c->x[0]) {
            ans[k] = (int)c->x[0];
            LOG("[rest] frame %llu: caller %llx told %d\n", (unsigned long long)vk_frame_counter(), (unsigned long long)c->x[30], (int)c->x[0]);
        }
    }
}

RT_ON_RESET(motion_reset) {
    pthread_mutex_lock(&g_lock);
    memset(m.angle, 0, sizeof m.angle);
    float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(m.R, I, sizeof I);
    pthread_mutex_unlock(&g_lock);
}

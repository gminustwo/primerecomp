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
// Sources: the controller in use, through SDL; or, when it has no gyro (Steam Input's virtual pad), the real controller
// behind it - DualSense, DualShock 4, Switch Pro Controller, Joy-Con, Steam Controller, the Steam Deck's own - read
// directly (direct_thread below).
//
// Setting: Gameplay tab "Gyro aiming" (g_settings.gyro); off, or no gyro on the controller: the sensor reads
// disconnected. Tests (no controller needed): LARECOMP_GYRO_TEST="frame:x:y:z:frames,..." turns the controller at x,
// y, z degrees per second (Switch frame: x = pitch up, z = yaw left) for that many frames, straight into the
// integration; LARECOMP_GYRO_SDL_TEST (below) feeds raw SDL samples instead; LARECOMP_GYRO_REST=0/1 forces "at rest";
// LARECOMP_LOG_GYRO=1 logs the samples once a second.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && !defined(__ANDROID__)  // (hidraw: direct_open)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include "../hle.h"
#include "../platform.h"
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

static bool g_sdl_test;  // LARECOMP_GYRO_SDL_TEST drives sensor_sample

// One sample in SDL's frame and units (rad / s, m / s^2); direct: from a controller read directly (direct_thread),
// taken only while the controller in use has no gyro of its own
static void sensor_sample(int sensor, const float *d, uint64_t ts, bool direct) {
    pthread_mutex_lock(&g_lock);
    if (direct && (m.pad || m.test || g_sdl_test)) { pthread_mutex_unlock(&g_lock); return; }
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
        // never have been learnt.) "1 G" within 0.1: accelerometers are off by a few percent (a Steam Deck read 0.958
        // lying still).
        float k = fminf(1, dt / 0.25f), dev = 0, mean = 0;
        for (int i = 0; i < 3; i++) {
            m.g_mean[i] += (raw[i] - m.g_mean[i]) * k;
            dev = fmaxf(dev, fabsf(raw[i] - m.g_mean[i]));
            mean = fmaxf(mean, fabsf(m.g_mean[i]));
        }
        m.g_jitter += (dev - m.g_jitter) * k;
        float an = sqrtf(m.accel[0] * m.accel[0] + m.accel[1] * m.accel[1] + m.accel[2] * m.accel[2]);
        bool steady = !m.have_accel || (fabsf(an - 1) < 0.1f && m.accel_jitter < 0.02f);
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

static void direct_want(SDL_Gamepad *pad, bool own_gyro);

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
    direct_want(pad, gyro);
}

void motion_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE && m.pad && e->gsensor.which == SDL_GetGamepadID(m.pad))
        sensor_sample(e->gsensor.sensor, e->gsensor.data, e->gsensor.sensor_timestamp, false);
}

// ---- A controller's own gyro beside Steam Input (GitHub issue #73, after the Metroid Prime 2 port's way; any controller)
// Started from Steam with Steam Input on, the game sees only Steam's virtual pad, which has no sensors: Steam hides the
// real controller from SDL (SDL_GAMECONTROLLER_IGNORE_DEVICES). Its reports still reach every reader of its HID device,
// so the controller behind the virtual pad (SDL has its vendor / product from Steam) is read here, read-only: nothing
// is sent to it - SDL's own drivers would set its report mode, lights, trackpads and lizard mode, which are Steam's - so
// Steam Input and its layout stay in charge of the buttons and sticks. Only the accelerometer and gyro are taken,
// converted as SDL's drivers do (SDL 3.4: SDL_hidapi_steamdeck.c, _ps5.c, _ps4.c, _switch.c, _steam_triton.c), and only
// while the controller in use has no gyro of its own (sensor_sample's direct). Steam has the sensors stream for its own
// gyro features; reports without them (a controller Steam set to send none) give nothing, and the log says so. On a
// Steam Deck its own controller is read whenever the controller in use has no gyro, as an Xbox pad played through Steam.
// Linux reads the hidraw nodes (/sys/class/hidraw: Steam's udev rules give access; SteamOS has them), Windows the HID
// device through calls Steam's overlay doesn't hook to hide it (runtime/platform/windows.c plat_hid_*), macOS SDL's
// hid functions; all shared, as Steam's own. Looked for every 2 s while wanted and missing (a controller switched on, resume
// from sleep), every 10 s after the first minute. LARECOMP_DIRECT_GYRO=0 (or the older LARECOMP_DECK_GYRO=0): off.
enum { FAM_DECK, FAM_PS5, FAM_PS4, FAM_SWITCH, FAM_TRITON, FAM_ALL = 0x1F };
static const struct { uint16_t vid, pid; uint8_t fam; const char *name; } k_direct[] = {
    {0x28de, 0x1205, FAM_DECK, "Steam Deck"},        // (LCD and OLED alike)
    {0x054c, 0x0ce6, FAM_PS5, "DualSense"},
    {0x054c, 0x0df2, FAM_PS5, "DualSense Edge"},
    {0x054c, 0x05c4, FAM_PS4, "DualShock 4"},
    {0x054c, 0x09cc, FAM_PS4, "DualShock 4"},
    {0x054c, 0x0ba0, FAM_PS4, "DualShock 4"},        // (Sony's wireless adapter)
    {0x057e, 0x2009, FAM_SWITCH, "Switch Pro Controller"},
    {0x057e, 0x2007, FAM_SWITCH, "Joy-Con (R)"},
    {0x057e, 0x2006, FAM_SWITCH, "Joy-Con (L)"},
    {0x28de, 0x1302, FAM_TRITON, "Steam Controller"},  // (2026's: USB, Bluetooth, its two wireless receivers)
    {0x28de, 0x1303, FAM_TRITON, "Steam Controller"},
    {0x28de, 0x1304, FAM_TRITON, "Steam Controller"},
    {0x28de, 0x1305, FAM_TRITON, "Steam Controller"},
};
enum { N_DIRECT = sizeof k_direct / sizeof k_direct[0] };
static int direct_index(unsigned vid, unsigned pid) {
    for (int i = 0; i < N_DIRECT; i++)
        if (k_direct[i].vid == vid && k_direct[i].pid == pid) return i;
    return -1;
}

static _Atomic uint32_t g_direct_want;     // the families (1 << FAM_*) whose controller may be read now (motion_pad)
static _Atomic uint32_t g_direct_pref;     // vendor << 16 | product of the controller behind Steam's pad, 0: unknown
static _Atomic uint32_t g_direct_gen;      // bumped by each controller change: look again
static _Atomic int g_direct_idx = -1;      // k_direct index of the controller being read
static _Atomic uint64_t g_direct_live_ns;  // when its last motion sample came (SDL_GetTicksNS)
static bool direct_live(void) {
    uint64_t t = g_direct_live_ns;
    return t && SDL_GetTicksNS() - t < 2000000000ull && g_direct_idx >= 0;
}

typedef struct {
    int idx, fam;
    uint64_t ts;         // ns: this controller's sample clock
    uint32_t tick;       // its own timestamp / packet counter at the last sample
    bool have_tick;
    uint64_t last_host;  // ns: the last Switch report's arrival (they carry no timestamp)
    float interval;      // s between Switch motion samples, measured
    int empty;           // state reports in a row without motion
    bool told_empty;
} DirectDev;

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// The sample clock moves on by its own timestamp's step (ns); one out of range (a gap, a reset) counts as 4 ms
static void direct_step(DirectDev *d, uint64_t ns) { d->ts += ns >= 100000 && ns <= 100000000 ? ns : 4000000; }

static void direct_feed(DirectDev *d, const float *a, const float *g) {  // (SDL's frame and units)
    sensor_sample(SDL_SENSOR_ACCEL, a, d->ts, true);
    sensor_sample(SDL_SENSOR_GYRO, g, d->ts, true);
    g_direct_live_ns = SDL_GetTicksNS();
    if (d->told_empty) LOG("[gyro] %s: its motion is coming now\n", k_direct[d->idx].name), d->told_empty = false;
    d->empty = 0;
}

static void direct_empty(DirectDev *d) {  // a state report without motion
    if (++d->empty == 1000 && !d->told_empty)  // (4 s or so)
        LOG("[gyro] %s: its reports carry no motion (Steam has its gyro turned off?)\n", k_direct[d->idx].name),
            d->told_empty = true;
}

// One report from the controller: 0 not one of its state reports, 1 a state report (its motion fed when feed is set)
static int direct_report(DirectDev *d, const uint8_t *r, int n, bool feed) {
    const float D2R = (float)M_PI / 180, G = SDL_STANDARD_GRAVITY;
    if (n < 1) return 0;
    switch (d->fam) {
    case FAM_DECK: {
        // ValveInReportHeader_t (version 1 as u16, type 9 = ID_CONTROLLER_DECK_STATE, length), then the deck state:
        // its packet counter (u32) at 4, accelerometer and gyro (int16 x, y, z) at 24 and 30 - +-2 G and +-2000 deg/s
        // over the int16 range; one report each 4 ms, 250 a second. (The firmware zeroes the gyro within +-1 count,
        // 0.06 deg/s, of still: held, it's never 0, so "at rest" works as on other controllers.)
        if (n != 64 || r[0] != 1 || r[1] != 0 || r[2] != 9) return 0;
        if (!feed) return 1;
        uint32_t pkt = rd32(r + 4), steps = pkt - d->tick;
        d->ts += 4000000ull * (d->have_tick && steps >= 1 && steps <= 250 ? steps : 1);  // (a batch keeps its spacing)
        d->tick = pkt, d->have_tick = true;
        const float as = 2 * G / 32768, gs = 2000 * D2R / 32768;
        float a[3] = {rd16(r + 24) * as, rd16(r + 28) * as, -rd16(r + 26) * as};
        float g[3] = {rd16(r + 30) * gs, rd16(r + 34) * gs, -rd16(r + 32) * gs};
        direct_feed(d, a, g);
        return 1;
    }
    case FAM_PS5: {
        // USB: report 1, the state from byte 1; Bluetooth: report 0x31, from byte 2 (its report 1 is the short one
        // without motion - Steam has it send the long one). Gyro (int16 x, y, z) at 15, accelerometer at 21, a 32-bit
        // timestamp in 1/3 us at 27; 16 counts per deg/s, 8192 per G (SDL's figures without the factory calibration,
        // which takes a request to the controller); its axes are SDL's
        const uint8_t *p = r[0] == 0x01 ? r + 1 : r[0] == 0x31 && n > 2 ? r + 2 : NULL;
        if (!p) return 0;
        if (!feed) return 1;
        if (n < (p - r) + 31) return direct_empty(d), 1;
        uint32_t tick = rd32(p + 27);
        if (d->have_tick && tick == d->tick) return 1;  // (no new sample)
        if (d->have_tick) direct_step(d, (uint64_t)(tick - d->tick) * 1000 / 3);
        d->tick = tick, d->have_tick = true;
        const float gs = D2R / 16, as = G / 8192;
        float g[3] = {rd16(p + 15) * gs, rd16(p + 17) * gs, rd16(p + 19) * gs};
        float a[3] = {rd16(p + 21) * as, rd16(p + 23) * as, rd16(p + 25) * as};
        if (a[0] == 0 && a[1] == 0 && a[2] == 0) return direct_empty(d), 1;
        direct_feed(d, a, g);
        return 1;
    }
    case FAM_PS4: {
        // USB: report 1, the state from byte 1; Bluetooth: reports 0x11-0x19 with byte 1's top bit (HID data in it),
        // from byte 3. A 16-bit timestamp in 16/3 us at 9, gyro (int16 x, y, z) at 12, accelerometer at 18; 16
        // counts per deg/s, 8192 per G; SDL's axes. (The wireless adapter reports zeros with no controller.)
        const uint8_t *p = r[0] == 0x01 ? r + 1 : r[0] >= 0x11 && r[0] <= 0x19 && n >= 78 && (r[1] & 0x80) ? r + 3 : NULL;
        if (!p) return 0;
        if (!feed) return 1;
        if (n < (p - r) + 24) return direct_empty(d), 1;
        uint32_t tick = (uint32_t)(p[9] | p[10] << 8);
        if (d->have_tick && tick == d->tick) return 1;
        if (d->have_tick) direct_step(d, (uint64_t)(uint16_t)(tick - d->tick) * 16000 / 3);
        d->tick = tick, d->have_tick = true;
        const float gs = D2R / 16, as = G / 8192;
        float g[3] = {rd16(p + 12) * gs, rd16(p + 14) * gs, rd16(p + 16) * gs};
        float a[3] = {rd16(p + 18) * as, rd16(p + 20) * as, rd16(p + 22) * as};
        if (a[0] == 0 && a[1] == 0 && a[2] == 0) return direct_empty(d), 1;
        direct_feed(d, a, g);
        return 1;
    }
    case FAM_SWITCH: {
        // Report 0x30, the full mode Steam sets for its gyro features (0x3F is the simple one, without motion): 12
        // bytes of state from byte 1, then three motion samples, newest first, 12 bytes each - accelerometer then
        // gyro, int16 x, y, z in the controller's own axes (x forward, y left, z up): 4096 counts per G, 14.2842 per
        // deg/s (SDL's figures without the factory calibration). No timestamp: the samples are spaced by the reports'
        // measured rate (as SDL). A right Joy-Con's x and y point the other way.
        if (r[0] == 0x3F) {
            if (feed) direct_empty(d);
            return 1;
        }
        if (r[0] != 0x30) return 0;
        if (!feed) return 1;
        if (n < 49) return direct_empty(d), 1;
        uint64_t now = SDL_GetTicksNS();
        if (d->interval <= 0) d->interval = 0.005f;
        if (d->last_host) {
            float rep = (float)((now - d->last_host) * 1e-9);
            if (rep > 0.001f && rep < 0.05f) d->interval += (rep / 3 - d->interval) * 0.02f;
        }
        d->last_host = now;
        const float as = G / 4096, gs = D2R / 14.2842f, flip = k_direct[d->idx].pid == 0x2007 ? -1.0f : 1.0f;
        bool any = false;
        for (int k = 2; k >= 0; k--) {  // (oldest first)
            const uint8_t *s = r + 13 + 12 * k;
            float a[3] = {-rd16(s + 2) * as * flip, rd16(s + 4) * as * flip, -rd16(s + 0) * as};
            float g[3] = {-rd16(s + 8) * gs * flip, rd16(s + 10) * gs * flip, -rd16(s + 6) * gs};
            if (a[0] == 0 && a[1] == 0 && a[2] == 0) continue;
            d->ts += (uint64_t)(d->interval * 1e9f);
            direct_feed(d, a, g);
            any = true;
        }
        if (!any) direct_empty(d);
        return 1;
    }
    case FAM_TRITON: {
        // Reports 0x42 (USB, or through a receiver), 0x45 (Bluetooth), 0x47 (with the trackpads' timestamp): the state
        // from byte 1; accelerometer (int16 x, y, z) at 34, gyro at 40 - +-2 G and +-2000 deg/s over the int16 range;
        // the motion's timestamp in us (32-bit at 30), or for 0x47 in 32 us (16-bit at 32). A sample whose timestamp
        // hasn't moved is an old one (the sensors off: as SDL, nothing).
        if (r[0] != 0x42 && r[0] != 0x45 && r[0] != 0x47) return 0;
        if (!feed) return 1;
        if (n < 46) return direct_empty(d), 1;
        uint32_t tick = r[0] == 0x47 ? (uint32_t)(r[32] | r[33] << 8) : rd32(r + 30);
        if (d->have_tick && tick == d->tick) return direct_empty(d), 1;
        if (d->have_tick)
            direct_step(d, r[0] == 0x47 ? (uint64_t)(uint16_t)(tick - d->tick) * 32000 : (uint64_t)(tick - d->tick) * 1000);
        d->tick = tick, d->have_tick = true;
        const float as = 2 * G / 32768, gs = 2000 * D2R / 32768;
        float a[3] = {rd16(r + 34) * as, rd16(r + 38) * as, -rd16(r + 36) * as};
        float g[3] = {rd16(r + 40) * gs, rd16(r + 44) * gs, -rd16(r + 42) * gs};
        if (a[0] == 0 && a[1] == 0 && a[2] == 0) return direct_empty(d), 1;
        direct_feed(d, a, g);
        return 1;
    }
    }
    return 0;
}

// The controller's HID device: Linux hidraw, elsewhere SDL's hid functions. direct_open: the first wanted one that
// sends one of its state reports within 200 ms (of a controller's several interfaces, or a receiver's several slots,
// the one in use), the preferred one first.
#if defined(__linux__) && !defined(__ANDROID__)
typedef struct { int fd; } RawDev;
static bool direct_open(uint32_t want, uint32_t pref, RawDev *out, DirectDev *dev) {
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return false;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; pass++) {  // (the preferred one, then the rest)
        rewinddir(dir);
        for (struct dirent *de; !found && (de = readdir(dir));) {
            if (strncmp(de->d_name, "hidraw", 6)) continue;
            char p[300], line[256], path[64];
            snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", de->d_name);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            unsigned bus = 0, vid = 0, pid = 0;
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "HID_ID=", 7)) sscanf(line + 7, "%x:%x:%x", &bus, &vid, &pid);
            fclose(f);
            int idx = direct_index(vid, pid);
            if (idx < 0 || !(want & 1u << k_direct[idx].fam) || (pass == 0) != (pref == (vid << 16 | pid))) continue;
            snprintf(path, sizeof path, "/dev/%s", de->d_name);
            int c = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (c < 0) {
                static bool told;  // (once: it's looked for again every few seconds)
                if (!told) LOG("[gyro] %s %s: can't read it (%s)\n", k_direct[idx].name, path, strerror(errno)), told = true;
                continue;
            }
            DirectDev t = {.idx = idx, .fam = k_direct[idx].fam};
            uint8_t r[128];
            for (int i = 0; i < 10 && !found; i++) {  // (up to 200 ms)
                struct pollfd pf = {c, POLLIN, 0};
                if (poll(&pf, 1, 20) > 0) {
                    ssize_t n;
                    while (!found && (n = read(c, r, sizeof r)) > 0) found = direct_report(&t, r, (int)n, false);
                }
            }
            if (found) {
                out->fd = c, *dev = t;
                LOG("[gyro] %s %s: its motion is read directly, for when the controller in use has no gyro\n",
                    k_direct[idx].name, path);
            } else close(c);
        }
    }
    closedir(dir);
    return found;
}
// A report (bytes), 0 none within the time, -1 the device is gone
static int direct_read(RawDev *d, uint8_t *buf, int cap, int ms) {
    struct pollfd pf = {d->fd, POLLIN, 0};
    int pr = poll(&pf, 1, ms);
    if (pr < 0) return errno == EINTR ? 0 : -1;
    if (pr == 0) return 0;
    if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    ssize_t n = read(d->fd, buf, (size_t)cap);
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
    return (int)n;
}
static void direct_close(RawDev *d) { close(d->fd), d->fd = -1; }
#elif defined(_WIN32)
// (not SDL's hid functions: in a game Steam starts, its overlay hides the controllers Steam Input drives from the
// calls they make - runtime/platform/windows.c plat_hid_*)
typedef struct { PlatHid *h; } RawDev;
typedef struct { uint32_t want, pref; int pass; RawDev *out; DirectDev *dev; } DirectFind;
static bool direct_try(const char *path, unsigned vid, unsigned pid, void *ud) {
    DirectFind *f = ud;
    int idx = direct_index(vid, pid);
    if (idx < 0 || !(f->want & 1u << k_direct[idx].fam) || (f->pass == 0) != (f->pref == (vid << 16 | pid))) return false;
    PlatHid *h = plat_hid_open(path);
    if (!h) return false;  // (a keyboard or mouse interface: the system's own)
    DirectDev t = {.idx = idx, .fam = k_direct[idx].fam};
    uint8_t r[128];
    bool found = false;
    for (uint64_t until = SDL_GetTicks() + 200; !found && SDL_GetTicks() < until;) {
        int n = plat_hid_read(h, r, sizeof r, 20);
        if (n < 0) break;
        if (n > 0) found = direct_report(&t, r, n, false);
    }
    if (!found) return plat_hid_close(h), false;
    f->out->h = h, *f->dev = t;
    LOG("[gyro] %s (%04x:%04x): its motion is read directly, for when the controller in use has no gyro\n",
        k_direct[idx].name, vid, pid);
    return true;
}
static bool direct_open(uint32_t want, uint32_t pref, RawDev *out, DirectDev *dev) {
    DirectFind f = {want, pref, 0, out, dev};
    for (f.pass = 0; f.pass < 2; f.pass++)  // (the preferred one, then the rest)
        if (plat_hid_each(direct_try, &f)) return true;
    return false;
}
static int direct_read(RawDev *d, uint8_t *buf, int cap, int ms) { return plat_hid_read(d->h, buf, cap, ms); }
static void direct_close(RawDev *d) { plat_hid_close(d->h), d->h = NULL; }
#else
typedef struct { SDL_hid_device *h; } RawDev;
static bool direct_open(uint32_t want, uint32_t pref, RawDev *out, DirectDev *dev) {
    SDL_hid_device_info *list = SDL_hid_enumerate(0, 0);
    bool found = false;
    for (int pass = 0; pass < 2 && !found; pass++)
        for (SDL_hid_device_info *i = list; i && !found; i = i->next) {
            int idx = direct_index(i->vendor_id, i->product_id);
            if (idx < 0 || !(want & 1u << k_direct[idx].fam) ||
                (pass == 0) != (pref == ((uint32_t)i->vendor_id << 16 | i->product_id)))
                continue;
            SDL_hid_device *h = SDL_hid_open_path(i->path);
            if (!h) continue;  // (a keyboard or mouse interface: the system's own)
            DirectDev t = {.idx = idx, .fam = k_direct[idx].fam};
            uint8_t r[128];
            uint64_t until = SDL_GetTicks() + 200;
            while (!found && SDL_GetTicks() < until) {
                int n = SDL_hid_read_timeout(h, r, sizeof r, 20);
                if (n < 0) break;
                if (n > 0) found = direct_report(&t, r, n, false);
            }
            if (found) {
                out->h = h, *dev = t;
                LOG("[gyro] %s (%04x:%04x, interface %d): its motion is read directly, for when the controller in use "
                    "has no gyro\n", k_direct[idx].name, i->vendor_id, i->product_id, i->interface_number);
            } else SDL_hid_close(h);
        }
    SDL_hid_free_enumeration(list);
    return found;
}
static int direct_read(RawDev *d, uint8_t *buf, int cap, int ms) { return SDL_hid_read_timeout(d->h, buf, (size_t)cap, ms); }
static void direct_close(RawDev *d) { SDL_hid_close(d->h), d->h = NULL; }
#endif

static void *direct_thread(void *arg) {
    (void)arg;
    RawDev raw;
    DirectDev dev = {.idx = -1};
    bool open_ = false, told_missing = false;
    uint32_t gen = ~0u;
    uint64_t next_look = 0, wanted_since = 0;
    for (;;) {
        uint32_t want = g_direct_want, g = g_direct_gen, pref = g_direct_pref;
        // another controller in use: the one read is let go if it isn't wanted any more, or (once per change) if
        // another one is preferred - it may come back: a Steam Controller's receiver can stand for the controller
        uint32_t id = open_ ? (uint32_t)k_direct[dev.idx].vid << 16 | k_direct[dev.idx].pid : 0;
        if (open_ && (!(want & 1u << dev.fam) || (g != gen && pref && pref != id))) {
            direct_close(&raw), open_ = false, g_direct_idx = -1;
            LOG("[gyro] %s: no longer read (the controller in use changed)\n", k_direct[dev.idx].name);
        }
        if (g != gen) gen = g, next_look = 0, wanted_since = SDL_GetTicks(), told_missing = false;
        if (!open_) {
            uint64_t now = SDL_GetTicks();
            if (!want || now < next_look) { SDL_Delay(250); continue; }
            next_look = now + (now - wanted_since < 60000 ? 2000 : 10000);
            if (direct_open(want, pref, &raw, &dev)) {
                open_ = true, told_missing = false, g_direct_idx = dev.idx;
                dev.ts = SDL_GetTicksNS();
            } else if (!told_missing) {
                LOG("[gyro] no controller found to read the gyro of directly (looking again every few seconds)\n");
                told_missing = true;
            }
            continue;
        }
        uint8_t r[128];
        int n = direct_read(&raw, r, sizeof r, 100);
        if (n < 0) {
            LOG("[gyro] %s gone\n", k_direct[dev.idx].name);
            direct_close(&raw), open_ = false, g_direct_idx = -1, next_look = SDL_GetTicks() + 1000;
            wanted_since = SDL_GetTicks();
            continue;
        }
        if (n > 0) direct_report(&dev, r, n, true);
    }
    return NULL;
}

// From motion_pad (main thread): which controller's own motion may stand in for the one in use, when it has none
static void direct_want(SDL_Gamepad *pad, bool own_gyro) {
    uint32_t want = 0, pref = 0;
    if (pad && !own_gyro) {
        unsigned vid = SDL_GetGamepadVendor(pad), pid = SDL_GetGamepadProduct(pad);
        bool unknown = vid == 0x28de && pid == 0x11ff;  // (Steam's virtual pad, the controller behind it not said)
        if (SDL_GetGamepadSteamHandle(pad) || unknown) {
            int idx = direct_index(vid, pid);
            want = idx >= 0 ? 1u << k_direct[idx].fam : unknown ? FAM_ALL : 0;
            if (idx >= 0) pref = vid << 16 | pid;
            LOG("[input] %s is Steam Input's virtual pad for %04x:%04x%s\n", SDL_GetGamepadName(pad), vid, pid,
                idx >= 0 ? "" : unknown ? " (any controller with a gyro is looked for)" : " (no gyro there)");
        }
    }
    static int test = -1;  // LARECOMP_DIRECT_GYRO=<vendor>:<product> (hex; tests): that controller is read as if it
    static unsigned tv, tp;  // were behind the one in use, or no controller - SDL_GAMECONTROLLER_IGNORE_DEVICES hides
    if (test < 0) {          // it from SDL as Steam does
        const char *e = getenv("LARECOMP_DIRECT_GYRO");
        test = e && sscanf(e, "%x:%x", &tv, &tp) == 2 && direct_index(tv, tp) >= 0;
    }
    if (test && !own_gyro) want |= 1u << k_direct[direct_index(tv, tp)].fam, pref = tv << 16 | tp;
    if (!own_gyro && settings_on_steam_deck()) want |= 1u << FAM_DECK;  // (the Deck's own: no controller, or none
    g_direct_want = want, g_direct_pref = pref;                         // with a gyro)
    g_direct_gen++;
}

// From runtime/hle/input.c input_frame (each frame; it starts once): direct = a controller may be read directly (not
// in scripted test runs, as for SDL's sensor events there)
void motion_init(bool direct) {
    static bool started;
    if (started) return;
    started = true;
#ifndef __ANDROID__  // (no Steam Input there: SDL has the controllers themselves)
    const char *e = getenv("LARECOMP_DIRECT_GYRO"), *e2 = getenv("LARECOMP_DECK_GYRO");
    if (!direct || (e && atoi(e) == 0) || (e2 && atoi(e2) == 0)) return;
    if (!g_direct_gen) direct_want(NULL, false);  // (no controller opened yet: a Deck's own still counts)
    pthread_t t;
    if (pthread_create(&t, NULL, direct_thread, NULL) == 0) pthread_detach(t);
#else
    (void)direct;
#endif
}

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

bool motion_available(void) { return m.pad != NULL || m.test || g_sdl_test || wiimote_drives() || direct_live(); }
// The gyro in use is a controller's own, read directly (for the menu's text): its name ("DualSense", "Steam Deck"...),
// or NULL
const char *motion_direct(void) {
    int i = g_direct_idx;
    return !m.pad && !m.test && !g_sdl_test && !wiimote_drives() && direct_live() && i >= 0 ? k_direct[i].name : NULL;
}

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
        sensor_sample(SDL_SENSOR_ACCEL, a, ts, false);
        sensor_sample(SDL_SENSOR_GYRO, g, ts, false);
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
                        m.pad ? "yes" : motion_direct() ? motion_direct() : "no");
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

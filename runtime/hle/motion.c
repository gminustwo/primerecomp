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
bool motion_available(void) { return m.pad != NULL || m.test || g_sdl_test; }

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
    pthread_mutex_lock(&g_lock);
    bool rest = m.still_s > 1.5f;
    pthread_mutex_unlock(&g_lock);
    c->x[0] = env >= 0 ? env : !sensor_on() || rest;
}

RT_ON_RESET(motion_reset) {
    pthread_mutex_lock(&g_lock);
    memset(m.angle, 0, sizeof m.angle);
    float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(m.R, I, sizeof I);
    pthread_mutex_unlock(&g_lock);
}

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
// Sources: the controller in use, through SDL; or, when it has no gyro (Steam Input's virtual Xbox pad), the Steam
// Deck's own controller read directly (deck_thread below).
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
#if defined(__linux__) && !defined(__ANDROID__)  // (the Steam Deck)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

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

static bool g_sdl_test;  // LARECOMP_GYRO_SDL_TEST drives sensor_sample

// One sample in SDL's frame and units (rad / s, m / s^2); deck: from the Steam Deck's own controller (deck_thread),
// taken only while the controller in use has no gyro of its own
static void sensor_sample(int sensor, const float *d, uint64_t ts, bool deck) {
    pthread_mutex_lock(&g_lock);
    if (deck && (m.pad || m.test || g_sdl_test)) { pthread_mutex_unlock(&g_lock); return; }
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
        sensor_sample(e->gsensor.sensor, e->gsensor.data, e->gsensor.sensor_timestamp, false);
}

// ---- The Steam Deck's own gyro, beside Steam Input (Linux; GitHub issue #73, after the Metroid Prime 2 port's way)
// Started from Steam, the game sees only Steam Input's virtual Xbox pad, which has no sensors: Steam hides the Deck's
// real controller from SDL (SDL_GAMECONTROLLER_IGNORE_DEVICES). Its reports still reach every reader of its hidraw
// node (Valve 28de:1205; of its three interfaces the controller one, which streams 64-byte "deck state" reports, 250
// a second - keyboard and mouse send nothing while Steam runs). Read here, read-only: nothing is sent to the controller
// (SDL's own Steam Deck driver would set its trackpads and lizard mode, which are Steam's), so Steam Input and its
// layout stay as they are. Buttons and sticks still come from the virtual pad; this only feeds the motion filter, with
// SDL's driver's conversion (SDL_hidapi_steamdeck.c), while the controller in use has no gyro (sensor_sample's deck).
// The firmware zeroes the gyro within +-1 count (0.06 deg/s) of still: held, it is never 0, so "at rest" works as on
// other controllers. Access: Steam's udev rules (uaccess on Valve's hidraw nodes; SteamOS has them). It retries every
// 2 s while the controller is missing (resume from sleep). LARECOMP_DECK_GYRO=0: off.
static _Atomic bool g_deck_found;  // the Deck's controller is open and reporting

#if defined(__linux__) && !defined(__ANDROID__)  // (the Steam Deck)
enum { DECK_REPORT = 64, DECK_STATE = 9 };  // (ValveInReportHeader_t: version 1 (u16), type ID_CONTROLLER_DECK_STATE, length)
static bool deck_state_report(const uint8_t *r, ssize_t n) { return n == DECK_REPORT && r[0] == 1 && r[1] == 0 && r[2] == DECK_STATE; }

// The Deck controller's hidraw node, opened (non-blocking), or -1
static int deck_open(char *path, size_t cap) {
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return -1;
    int fd = -1;
    for (struct dirent *de; fd < 0 && (de = readdir(dir));) {
        if (strncmp(de->d_name, "hidraw", 6)) continue;
        char p[300], line[256];
        snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", de->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        unsigned bus = 0, vid = 0, pid = 0;
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "HID_ID=", 7)) sscanf(line + 7, "%x:%x:%x", &bus, &vid, &pid);
        fclose(f);
        if (vid != 0x28de || pid != 0x1205) continue;  // (USB_VENDOR_VALVE, USB_PRODUCT_STEAM_DECK: LCD and OLED alike)
        snprintf(path, cap, "/dev/%s", de->d_name);
        int c = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (c < 0) {
            static bool told;  // (once: it's looked for again every 2 s)
            if (!told) LOG("[gyro] Steam Deck controller %s: can't read it (%s)\n", path, strerror(errno)), told = true;
            continue;
        }
        uint8_t r[DECK_REPORT];
        for (int i = 0; i < 10 && fd < 0; i++) {  // (up to 200 ms: the controller interface reports every 4)
            struct pollfd pf = {c, POLLIN, 0};
            if (poll(&pf, 1, 20) > 0 && deck_state_report(r, read(c, r, sizeof r))) fd = c;
        }
        if (fd < 0) close(c);
    }
    closedir(dir);
    return fd;
}

// One report: accelerometer and gyro (int16 at 24 and 30: x, y, z) into SDL's frame, as SDL_hidapi_steamdeck.c -
// gyro +-2000 deg/s, accelerometer +-2 G over the int16 range. Timestamps from the packet counter (4 ms a report), so
// reports read in a batch keep their spacing.
static void deck_sample(const uint8_t *r, uint64_t *ts, uint32_t *last_pkt) {
    int16_t v[6];
    uint32_t pkt;
    memcpy(&pkt, r + 4, 4);
    memcpy(v, r + 24, sizeof v);
    uint32_t steps = pkt - *last_pkt;
    *ts += 4000000ull * (*last_pkt && steps >= 1 && steps <= 250 ? steps : 1);
    *last_pkt = pkt;
    const float gs = 2000.0f * (float)M_PI / 180 / 32768, as = 2.0f * SDL_STANDARD_GRAVITY / 32768;
    float a[3] = {v[0] * as, v[2] * as, -v[1] * as}, g[3] = {v[3] * gs, v[5] * gs, -v[4] * gs};
    sensor_sample(SDL_SENSOR_ACCEL, a, *ts, true);
    sensor_sample(SDL_SENSOR_GYRO, g, *ts, true);
}

static void *deck_thread(void *arg) {
    (void)arg;
    char path[64];
    uint64_t ts = 1000000000ull;
    bool told_missing = false;
    for (;;) {
        int fd = deck_open(path, sizeof path);
        if (fd < 0) {
            if (!told_missing) LOG("[gyro] Steam Deck controller not found (looking again every 2 s)\n");
            told_missing = true;
            sleep(2);
            continue;
        }
        LOG("[gyro] Steam Deck controller %s: its gyro is read directly, for when the controller in use has none\n", path);
        g_deck_found = true;
        told_missing = false;
        uint32_t last_pkt = 0;
        int err = 0;
        while (!err) {
            struct pollfd pf = {fd, POLLIN, 0};
            int pr = poll(&pf, 1, 1000);
            if (pr < 0) { if (errno != EINTR) err = errno; continue; }
            if (pr == 0) continue;  // (asleep: no reports)
            if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) { err = ENODEV; break; }
            uint8_t r[DECK_REPORT];
            ssize_t n;
            while ((n = read(fd, r, sizeof r)) > 0)
                if (deck_state_report(r, n)) deck_sample(r, &ts, &last_pkt);
            if (n < 0 && errno != EAGAIN && errno != EINTR) err = errno;
        }
        g_deck_found = false;
        close(fd);
        LOG("[gyro] Steam Deck controller %s gone (%s)\n", path, strerror(err));
        sleep(1);
    }
    return NULL;
}
#endif

// From runtime/hle/input.c input_frame (each frame; it starts once): deck = the Deck's own gyro may be read (not in
// scripted test runs, as for SDL's sensor events there)
void motion_init(bool deck) {
    static bool started;
    if (started) return;
    started = true;
#if defined(__linux__) && !defined(__ANDROID__)  // (the Steam Deck)
    const char *e = getenv("LARECOMP_DECK_GYRO");
    if (!deck || (e && atoi(e) == 0) || !settings_on_steam_deck()) return;
    pthread_t t;
    if (pthread_create(&t, NULL, deck_thread, NULL) == 0) pthread_detach(t);
#else
    (void)deck;
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

bool motion_available(void) { return m.pad != NULL || m.test || g_sdl_test || wiimote_drives() || g_deck_found; }
// The gyro in use is the Deck's own (for the menu's text)
bool motion_deck(void) { return !m.pad && !m.test && !g_sdl_test && !wiimote_drives() && g_deck_found; }

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
                        m.pad ? "yes" : motion_deck() ? "the Steam Deck's own" : "no");
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

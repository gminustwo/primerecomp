// Wii Remote (+ Nunchuk) through SDL's HID API: the IR camera's pointer (the sensor bar), the buttons, the
// accelerometer, the Nunchuk, rumble. A Mayflash DolphinBar in mode 4 does the Bluetooth itself and shows each of its
// four slots as a Wii Remote HID device (057e:0306); a remote paired with the PC's own Bluetooth looks the same (the
// Wii Remote Plus may say 0330). SDL's own Wii driver reads no IR (and is off by default): this one talks the remote's
// protocol (wiibrew.org/wiki/Wiimote) on a thread of its own.
//
// Into the game (runtime/hle/input.c, motion.c): the buttons and the Nunchuk as the Switch controller's (laid out as
// Metroid Prime: Trilogy's on the game's Pointer control scheme, which is modelled on it), the pointer
// as the controller's orientation - the game's own Pointer scheme turns it into its cursor (CNXSixAxisAnalysis::
// UpdateCursor), and wiimote_pin_cursor keeps the game's reference on the screen's middle so the cursor goes where the
// remote points - the accelerometer as the controller's.
//
// Settings: Controls tab "Wii Remote" (g_settings.wiimote*): on / off, the pointer's middle (calibrated by pointing at
// the screen's middle), its area, smoothing. Logs "[wiimote]". Tests: LARECOMP_WIIMOTE_TEST="frame:x:y:frames,..."
// stands in a remote pointing at x, y (-1..1, the screen's edges; y up; x = "off": the sensor bar out of sight, lying
// still) from that frame, LARECOMP_LOG_WIIMOTE=1 logs
// the remote's state twice a second.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "wiimote.h"

#define WM_VID 0x057E
#define WM_PID 0x0306
#define WM_PID_PLUS 0x0330

static SDL_Mutex *g_lock;
static WiimoteState g_st;            // under g_lock (the thread writes it, everyone else reads a copy)
static _Atomic int g_rumble;         // the game's rumble, as the remote's motor (on / off)
static SDL_Thread *g_thread;
static bool g_started;

// ---- protocol (on the thread)
static SDL_hid_device *g_dev;
static bool g_rumble_on;             // (what the last report said: every output report carries the rumble bit)

static bool out(const uint8_t *r, int n) {
    uint8_t b[22] = {0};
    if (n > 22) n = 22;
    memcpy(b, r, (size_t)n);
    if (n > 1) b[1] = (uint8_t)((b[1] & 0xFE) | (g_rumble_on ? 1 : 0));
    return SDL_hid_write(g_dev, b, (size_t)n) >= 0;
}
static bool write_reg(uint32_t addr, const uint8_t *data, int n) {
    uint8_t r[22] = {0x16, 0x04, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)n};
    memcpy(r + 6, data, (size_t)n);
    return out(r, 22);
}
static bool read_mem(uint32_t addr, int n, bool reg) {
    uint8_t r[7] = {0x17, reg ? 0x04 : 0x00, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(n >> 8), (uint8_t)n};
    return out(r, 7);
}
static bool report_mode(uint8_t mode) { const uint8_t r[3] = {0x12, 0x04, mode}; return out(r, 3); }
static bool status_request(void) { const uint8_t r[2] = {0x15, 0x00}; return out(r, 2); }

// the accelerometer's calibration (EEPROM 0x16: zero point, then 1 G, 10-bit), the Nunchuk's stick middle
static float g_acc_zero[3] = {512, 512, 512}, g_acc_g[3] = {104, 104, 104};
static int g_stick_mid[2] = {128, 128};
static uint8_t g_ext_id[6];
static int g_ext_state;  // 0 none, 1 initialising, 2 Nunchuk, 3 something else

static void parse_core(const uint8_t *d, bool has_accel) {
    uint16_t b = (uint16_t)(d[0] & 0x1F) | (uint16_t)((d[1] & 0x9F) << 8);
    float a[3] = {0, 0, 0};
    if (has_accel) {
        int raw[3] = {d[2] << 2 | (d[0] >> 5 & 3), d[3] << 2 | (d[1] >> 4 & 2), d[4] << 2 | (d[1] >> 5 & 2)};
        for (int i = 0; i < 3; i++) a[i] = (raw[i] - g_acc_zero[i]) / (g_acc_g[i] > 1 ? g_acc_g[i] : 104);
    }
    SDL_LockMutex(g_lock);
    g_st.buttons = b;
    if (has_accel) memcpy(g_st.accel, a, sizeof a);
    SDL_UnlockMutex(g_lock);
}

static void parse_ir_basic(const uint8_t *ir) {  // 10 bytes: two pairs of dots, 5 bytes each
    WiimoteDot d[4];
    for (int p = 0; p < 2; p++) {
        const uint8_t *q = ir + 5 * p;
        d[2 * p].x = q[0] | (q[2] >> 4 & 3) << 8, d[2 * p].y = q[1] | (q[2] >> 6 & 3) << 8;
        d[2 * p + 1].x = q[3] | (q[2] & 3) << 8, d[2 * p + 1].y = q[4] | (q[2] >> 2 & 3) << 8;
    }
    SDL_LockMutex(g_lock);
    for (int i = 0; i < 4; i++) {
        g_st.dots[i] = d[i];
        g_st.dots[i].valid = d[i].x < 1023 && d[i].y < 1023;
    }
    g_st.ir_ns = SDL_GetTicksNS();
    SDL_UnlockMutex(g_lock);
}

static void parse_nunchuk(const uint8_t *e) {
    if (g_ext_state != 2) return;
    float sx = (float)(e[0] - g_stick_mid[0]) / 100, sy = (float)(e[1] - g_stick_mid[1]) / 100;
    float a[3] = {(float)((e[2] << 2 | (e[5] >> 2 & 3)) - 512) / 200, (float)((e[3] << 2 | (e[5] >> 4 & 3)) - 512) / 200,
                  (float)((e[4] << 2 | (e[5] >> 6 & 3)) - 512) / 200};
    SDL_LockMutex(g_lock);
    g_st.stick[0] = fmaxf(-1, fminf(1, sx)), g_st.stick[1] = fmaxf(-1, fminf(1, sy));
    g_st.nunchuk_c = !(e[5] & 2), g_st.nunchuk_z = !(e[5] & 1);
    memcpy(g_st.nunchuk_accel, a, sizeof a);
    SDL_UnlockMutex(g_lock);
}

// One report from the remote. Returns its id (0: nothing within the timeout, -1: the device is gone).
static int pump(int timeout_ms, uint8_t *last, int *last_n) {
    uint8_t r[32];
    int n = SDL_hid_read_timeout(g_dev, r, sizeof r, timeout_ms);
    if (n < 0) return -1;
    if (n == 0) return 0;
    if (last) { memcpy(last, r, (size_t)(n < 32 ? n : 32)); *last_n = n; }
    switch (r[0]) {
    case 0x20:  // status: battery, extension, LEDs - and the remote stops reporting until the mode is set again
        parse_core(r + 1, false);
        SDL_LockMutex(g_lock);
        g_st.battery = r[6] / 2.0f > 100 ? 100 : r[6] / 2.0f;  // (0xC8 full)
        g_st.ext_plugged = (r[3] & 2) != 0;
        SDL_UnlockMutex(g_lock);
        break;
    case 0x21: case 0x22: parse_core(r + 1, false); break;
    case 0x30: parse_core(r + 1, false); break;
    case 0x31: parse_core(r + 1, true); break;
    case 0x33: parse_core(r + 1, true); break;
    case 0x37: parse_core(r + 1, true); parse_ir_basic(r + 6); parse_nunchuk(r + 16); break;
    default: break;
    }
    return r[0];
}

// Waits for the reply to a request (report `want`), handling the rest meanwhile
static bool wait_for(uint8_t want, int timeout_ms, uint8_t *reply) {
    uint64_t end = SDL_GetTicks() + (uint64_t)timeout_ms;
    int n = 0;
    while (SDL_GetTicks() < end) {
        int id = pump(20, reply, &n);
        if (id < 0) return false;
        if (id == want) return true;
    }
    return false;
}
static bool write_reg_wait(uint32_t addr, const uint8_t *data, int n) {
    uint8_t rep[32];
    for (int tries = 0; tries < 3; tries++)
        if (write_reg(addr, data, n) && wait_for(0x22, 200, rep)) return true;
    return false;
}
static bool read_mem_wait(uint32_t addr, int n, bool reg, uint8_t *out16) {
    uint8_t rep[32];
    for (int tries = 0; tries < 3; tries++)
        if (read_mem(addr, n, reg) && wait_for(0x21, 300, rep) && !(rep[3] & 0x0F)) { memcpy(out16, rep + 6, 16); return true; }
    return false;
}

// The IR camera (wiibrew's sequence, sensitivity "Wii level 3"), basic mode for report 0x37
static bool ir_on(void) {
    static const uint8_t blk1[9] = {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64}, blk2[2] = {0x63, 0x03};
    const uint8_t on13[2] = {0x13, 0x04}, on1a[2] = {0x1a, 0x04}, b08 = 0x08, mode = 0x01;
    uint8_t rep[32];
    out(on13, 2); wait_for(0x22, 200, rep);
    out(on1a, 2); wait_for(0x22, 200, rep);
    return write_reg_wait(0xB00030, &b08, 1) && write_reg_wait(0xB00000, blk1, 9) && write_reg_wait(0xB0001A, blk2, 2) &&
           write_reg_wait(0xB00033, &mode, 1) && write_reg_wait(0xB00030, &b08, 1);
}

// The extension (unencrypted: 0x55 -> A400F0, 0x00 -> A400FB), its id (A400FA), a Nunchuk's stick calibration
static void ext_init(void) {
    const uint8_t x55 = 0x55, x00 = 0x00;
    uint8_t d[16];
    g_ext_state = 1;
    if (!write_reg_wait(0xA400F0, &x55, 1) || !write_reg_wait(0xA400FB, &x00, 1) || !read_mem_wait(0xA400FA, 6, true, d)) {
        g_ext_state = 0;
        LOG("[wiimote] extension: no answer\n");
        return;
    }
    memcpy(g_ext_id, d, 6);
    bool nunchuk = d[2] == 0xA4 && d[3] == 0x20 && d[4] == 0x00 && d[5] == 0x00;
    g_ext_state = nunchuk ? 2 : 3;
    if (nunchuk && read_mem_wait(0xA40020, 16, true, d) && d[10] > 0x40 && d[10] < 0xC0 && d[13] > 0x40 && d[13] < 0xC0)
        g_stick_mid[0] = d[10], g_stick_mid[1] = d[13];  // (x: max min mid, y: max min mid)
    LOG("[wiimote] extension %02x%02x%02x%02x%02x%02x: %s\n", d[0], d[1], d[2], d[3], d[4], d[5],
        nunchuk ? "Nunchuk" : "not a Nunchuk (unused)");
    SDL_LockMutex(g_lock);
    g_st.nunchuk = nunchuk;
    SDL_UnlockMutex(g_lock);
}

static void setup(void) {
    const uint8_t led[2] = {0x11, 0x10};  // player 1's light
    out(led, 2);
    uint8_t d[16];
    if (read_mem_wait(0x0016, 10, false, d)) {  // the accelerometer's calibration
        int z[3] = {d[0] << 2 | (d[3] >> 4 & 3), d[1] << 2 | (d[3] >> 2 & 3), d[2] << 2 | (d[3] & 3)};
        int g[3] = {d[4] << 2 | (d[7] >> 4 & 3), d[5] << 2 | (d[7] >> 2 & 3), d[6] << 2 | (d[7] & 3)};
        for (int i = 0; i < 3; i++)
            if (g[i] > z[i]) g_acc_zero[i] = (float)z[i], g_acc_g[i] = (float)(g[i] - z[i]);
    }
    bool ir = ir_on();
    status_request();
    uint8_t rep[32];
    bool st = wait_for(0x20, 300, rep);
    SDL_LockMutex(g_lock);
    bool ext = g_st.ext_plugged;
    SDL_UnlockMutex(g_lock);
    if (st && ext) ext_init();
    report_mode(0x37);  // buttons, accelerometer, IR (basic: 4 dots), 6 extension bytes - 100 a second
    LOG("[wiimote] connected: IR camera %s, %s\n", ir ? "on" : "FAILED", ext ? "extension plugged" : "no extension");
}

// Opens the first HID path that answers a status request like a Wii Remote (a DolphinBar's empty slots don't)
static bool find_remote(void) {
    SDL_hid_device_info *list = SDL_hid_enumerate(WM_VID, 0), *best = NULL;
    for (SDL_hid_device_info *i = list; i && !g_dev; i = i->next) {
        if (i->product_id != WM_PID && i->product_id != WM_PID_PLUS) continue;
        SDL_hid_device *d = SDL_hid_open_path(i->path);
        if (!d) continue;
        g_dev = d;
        uint8_t rep[32];
        if (status_request() && wait_for(0x20, 250, rep)) { best = i; break; }
        SDL_hid_close(d);
        g_dev = NULL;
    }
    if (best) {
        char name[128] = "";
        if (best->product_string) SDL_snprintf(name, sizeof name, "%ls", best->product_string);
        LOG("[wiimote] found %04x:%04x %s (%s)\n", best->vendor_id, best->product_id, name, best->path);
        SDL_LockMutex(g_lock);
        g_st.plus = best->product_id == WM_PID_PLUS;
        SDL_snprintf(g_st.name, sizeof g_st.name, "%s", name[0] ? name : "Wii Remote");
        SDL_UnlockMutex(g_lock);
    }
    SDL_hid_free_enumeration(list);
    return g_dev != NULL;
}

static int SDLCALL wiimote_thread(void *arg) {
    (void)arg;
    uint64_t next_scan = 0;
    for (;;) {
        if (!g_settings.wiimote) { SDL_Delay(500); continue; }
        if (!g_dev) {
            if (SDL_GetTicks() < next_scan) { SDL_Delay(100); continue; }
            next_scan = SDL_GetTicks() + 2000;  // (rescans every 2 s: a remote synced later, a DolphinBar plugged in)
            if (!find_remote()) continue;
            setup();
            SDL_LockMutex(g_lock);
            g_st.connected = true;
            SDL_UnlockMutex(g_lock);
        }
        uint64_t next_status = SDL_GetTicks() + 10000;
        bool lost = false;
        while (!lost && g_settings.wiimote) {
            int id = pump(10, NULL, NULL);
            if (id < 0) { lost = true; break; }
            bool rumble = atomic_load(&g_rumble) != 0;
            if (rumble != g_rumble_on) {  // (a status request carries the rumble bit like any report)
                g_rumble_on = rumble;
                const uint8_t r[2] = {0x15, 0x00};
                if (!out(r, 2)) lost = true;
            }
            if (id == 0x20) {  // a status report (asked, or an extension plugged / unplugged): reporting again
                SDL_LockMutex(g_lock);
                bool ext = g_st.ext_plugged, had = g_st.nunchuk;
                SDL_UnlockMutex(g_lock);
                if (ext && g_ext_state == 0) ext_init();
                if (!ext && had) {
                    g_ext_state = 0;
                    SDL_LockMutex(g_lock);
                    g_st.nunchuk = false;
                    g_st.stick[0] = g_st.stick[1] = 0, g_st.nunchuk_c = g_st.nunchuk_z = false;
                    SDL_UnlockMutex(g_lock);
                    LOG("[wiimote] extension unplugged\n");
                }
                if (!report_mode(0x37)) lost = true;
            }
            if (SDL_GetTicks() > next_status) {  // battery, and a check the remote is still there
                next_status = SDL_GetTicks() + 10000;
                if (!status_request()) lost = true;
            }
            static uint64_t next_log;
            static int log = -1;
            if (log < 0) log = getenv("LARECOMP_LOG_WIIMOTE") != NULL;
            if (log && SDL_GetTicks() > next_log) {
                next_log = SDL_GetTicks() + 500;
                WiimoteState s;
                wiimote_state(&s);
                LOG("[wiimote] buttons %04x accel %.2f %.2f %.2f dots %d,%d %d,%d %d,%d %d,%d pointer %s %.3f %.3f "
                    "stick %.2f %.2f C%d Z%d battery %.0f%%\n", s.buttons, s.accel[0], s.accel[1], s.accel[2],
                    s.dots[0].valid ? s.dots[0].x : -1, s.dots[0].valid ? s.dots[0].y : -1, s.dots[1].valid ? s.dots[1].x : -1,
                    s.dots[1].valid ? s.dots[1].y : -1, s.dots[2].valid ? s.dots[2].x : -1, s.dots[2].valid ? s.dots[2].y : -1,
                    s.dots[3].valid ? s.dots[3].x : -1, s.dots[3].valid ? s.dots[3].y : -1, s.pointer_valid ? "on" : "off",
                    s.pointer[0], s.pointer[1], s.stick[0], s.stick[1], s.nunchuk_c, s.nunchuk_z, s.battery);
            }
        }
        if (g_dev) SDL_hid_close(g_dev), g_dev = NULL;
        g_ext_state = 0;
        SDL_LockMutex(g_lock);
        bool was = g_st.connected;
        memset(&g_st, 0, sizeof g_st);
        SDL_UnlockMutex(g_lock);
        if (was) LOG("[wiimote] disconnected\n");
        next_scan = SDL_GetTicks() + 1000;
    }
    return 0;
}

void wiimote_start(void) {  // (the main thread, after SDL_Init: runtime/hle/input.c)
    if (g_started) return;
    g_started = true;
    g_lock = SDL_CreateMutex();
    if (getenv("LARECOMP_NO_WIIMOTE")) return;
    g_thread = SDL_CreateThread(wiimote_thread, "wiimote", NULL);
    if (g_thread) SDL_DetachThread(g_thread);
}

// ---- the pointer: the sensor bar's two dots in the camera's 1024 x 768 image -> where on the screen the remote
// points, -1..1 across it (x right, y up). The middle of the two dots, turned back by the remote's roll (the dots'
// own slope: the bar is level), mirrored (pointing right moves the bar left in the image); one dot alone: the pair's
// last spacing gives the other. Then the calibrated middle and the area (how much of the camera's view is the screen),
// smoothed.
static float g_pair[2] = {200, 0};   // the last two-dot spacing (image pixels), left to right
static float g_raw_last[2];          // the last pointer before calibration / smoothing (for the calibration step)

static bool pointer_raw(const WiimoteDot *dots, float *px, float *py) {
    int v[4], n = 0;
    for (int i = 0; i < 4; i++) if (dots[i].valid) v[n++] = i;
    float mx, my;
    if (n >= 2) {
        int a = v[0], b = v[1];
        if (n > 2) {  // (a lamp or reflection too: the pair furthest apart sideways)
            float best = -1;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++) {
                    float dx = fabsf((float)dots[v[i]].x - dots[v[j]].x);
                    if (dx > best) best = dx, a = v[i], b = v[j];
                }
        }
        if (dots[a].x > dots[b].x) { int t = a; a = b; b = t; }
        g_pair[0] = (float)(dots[b].x - dots[a].x), g_pair[1] = (float)(dots[b].y - dots[a].y);
        mx = (dots[a].x + dots[b].x) / 2.0f, my = (dots[a].y + dots[b].y) / 2.0f;
    } else if (n == 1) {  // (the other dot off the camera's edge: whichever side it was nearer)
        const WiimoteDot *d = &dots[v[0]];
        float sgn = d->x < 512 ? 1.0f : -1.0f;  // (a dot on the left half: it is the left one)
        mx = d->x + sgn * g_pair[0] / 2, my = d->y + sgn * g_pair[1] / 2;
    } else return false;
    float roll = atan2f(g_pair[1], g_pair[0]), c = cosf(-roll), s = sinf(-roll);
    float dx = mx - 512, dy = my - 384;
    float rx = dx * c - dy * s, ry = dx * s + dy * c;
    *px = -rx / 512, *py = -ry / 384;  // (both ways mirrored: the user's real remote, 2026-10-29 - y was the wrong way)
    return true;
}

void wiimote_state(WiimoteState *out) {
    if (!g_lock) { memset(out, 0, sizeof *out); return; }
    SDL_LockMutex(g_lock);
    *out = g_st;
    SDL_UnlockMutex(g_lock);
    float px, py;
    bool fresh = out->connected && SDL_GetTicksNS() - out->ir_ns < 150000000ull;
    out->pointer_valid = fresh && pointer_raw(out->dots, &px, &py);
    if (out->pointer_valid) {
        g_raw_last[0] = px, g_raw_last[1] = py;
        float area = g_settings.wiimote_area > 0.1f ? g_settings.wiimote_area : 0.5f;
        out->pointer[0] = (px - g_settings.wiimote_mid[0]) / area;
        out->pointer[1] = (py - g_settings.wiimote_mid[1]) / area;
    }
}

// The menu's calibration: the raw pointer now becomes the screen's middle
bool wiimote_calibrate_middle(void) {
    WiimoteState s;
    wiimote_state(&s);
    if (!s.pointer_valid) return false;
    g_settings.wiimote_mid[0] = g_raw_last[0], g_settings.wiimote_mid[1] = g_raw_last[1];
    LOG("[wiimote] middle calibrated: %.3f %.3f\n", g_raw_last[0], g_raw_last[1]);
    return true;
}

void wiimote_rumble(bool on) { atomic_store(&g_rumble, on ? 1 : 0); }

// LARECOMP_WIIMOTE_TEST="frame:x:y:frames,...": a stand-in remote pointing at x, y (-1..1) - the game side without one
bool wiimote_test_pointer(uint64_t frame, float *x, float *y) {
    const char *spec = getenv("LARECOMP_WIIMOTE_TEST");
    if (!spec) return false;
    bool hit = false;
    for (const char *p = spec; p && *p;) {
        unsigned long start = 0, len = 60;
        float tx = 0, ty = 0;
        char off[4] = "";
        if (sscanf(p, "%lu:%f:%f:%lu", &start, &tx, &ty, &len) >= 3 && frame >= start && frame < start + len)
            *x = tx, *y = ty, hit = true;
        else if (sscanf(p, "%lu:%3[of]:%lu", &start, off, &len) == 3 && !strcmp(off, "off") && frame >= start && frame < start + len)
            *x = -10, *y = 0, hit = true;
        p = strchr(p, ',');
        if (p) p++;
    }
    return hit;
}

// Rumble: the game's vibration -> the controller (Audio tab: Rumble, Rumble strength, HD rumble).
//
// The game describes its vibration as the Switch's HD rumble plays it: per device (the left and the right Joy-Con, a
// Pro Controller's two grips) a low band and a high band, each an amplitude (0..1) and a frequency (Hz). The SDK's
// vibration players play the game's patterns and hand hid a new value every few milliseconds (SendVibrationValue(s):
// runtime/services/hid_set.c -> input_set_vibration). What the controller gets depends on what it has:
// - HD rumble, a Nintendo Switch Pro Controller or a single Joy-Con on SDL's own driver (USB or Bluetooth): the game's
//   values themselves, both bands at the game's frequencies, in the controller's own rumble packets
//   (SDL_SendGamepadEffect; the encoding of dekuNukem's Nintendo_Switch_Reverse_Engineering rumble_data_table, as
//   JoyShockLibrary-plus-HDRumble writes it) - what the game plays on a Switch.
// - HD rumble, a DualSense on USB: its two voice-coil actuators are the 3rd and 4th channels of its audio device, as
//   PS5 games drive them. The two bands are played there as sine waves at the game's frequencies (RenHDrum's way:
//   48 kHz, amplitudes through x^0.5, peaks at 0.8). (On Bluetooth those take a vendor HID stream: motors there.)
// - Anything else (Xbox, DualShock 4, DualSense on Bluetooth, Joy-Con pairs, Steam Input's virtual pad), or HD rumble
//   off: two motors, each device's stronger band driving its side's motor. A motor hardly turns at the amplitudes
//   a linear actuator plays well, which the game's are (seen: 0.1..0.25 - landing, the space jump; driven as they
//   were, 10..25%, rumble felt very weak: issue #13): through x^0.5 (0.1 -> 32%, 0.25 -> 50%).
// The game asks for 12..30 Hz in both bands, below either band's range: a Switch controller plays that at the lowest
// it has (~41 Hz low, ~82 Hz high), and so do these.
// Rumble strength (0..200%) scales the amplitudes everywhere. LARECOMP_NO_RUMBLE=1: none; scripted runs don't rumble;
// LARECOMP_LOG_RUMBLE=1 logs the game's values as they change and what was sent; LARECOMP_RUMBLE_DEVICE=<name part>
// picks the DualSense's audio device by name (another language's Windows calls it something else).
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "wiimote.h"

enum { AL, FL, AH, FH };  // a vibration value: amplitude low, frequency low, amplitude high, frequency high

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_now[2][2][4];   // [player 1, handheld][left, right device]: the game's latest value
static float g_peak[2][2][2];  // the strongest amplitudes (low, high) since the pump last took them
static bool g_log_values;

void input_set_vibration(uint32_t handle, const float v[4]) {
    uint32_t npad = (handle >> 8) & 0xFF;
    if (npad != 0 && npad != 0x20) return;  // (player 1 and the handheld one: the others have no controller here)
    int p = npad != 0, d = handle & 1;
    float c[4];
    for (int i = 0; i < 4; i++) c[i] = isfinite(v[i]) ? v[i] : 0;
    c[AL] = c[AL] < 0 ? 0 : c[AL] > 1 ? 1 : c[AL];
    c[AH] = c[AH] < 0 ? 0 : c[AH] > 1 ? 1 : c[AH];
    pthread_mutex_lock(&g_lock);
    bool changed = memcmp(g_now[p][d], c, sizeof c) != 0;
    memcpy(g_now[p][d], c, sizeof c);
    if (c[AL] > g_peak[p][d][0]) g_peak[p][d][0] = c[AL];
    if (c[AH] > g_peak[p][d][1]) g_peak[p][d][1] = c[AH];
    pthread_mutex_unlock(&g_lock);
    if (g_log_values && changed)
        LOG("[rumble] game: %s %s low %.3f @ %.0f Hz, high %.3f @ %.0f Hz\n", p ? "handheld" : "player 1",
            d ? "right" : "left", c[AL], c[FL], c[AH], c[FH]);
}

// Each side's value: player 1's or the handheld one's, whichever is stronger; with peaks, the strongest amplitudes
// since the last such take as well (the pump looks every ~10 ms; a 5 ms tap mustn't fall between two looks).
static void take(float out[2][4], bool peaks) {
    pthread_mutex_lock(&g_lock);
    for (int d = 0; d < 2; d++) {
        const float *a = g_now[0][d], *b = g_now[1][d];
        memcpy(out[d], fmaxf(b[AL], b[AH]) > fmaxf(a[AL], a[AH]) ? b : a, 4 * sizeof(float));
        if (!peaks) continue;
        out[d][AL] = fmaxf(out[d][AL], fmaxf(g_peak[0][d][0], g_peak[1][d][0]));
        out[d][AH] = fmaxf(out[d][AH], fmaxf(g_peak[0][d][1], g_peak[1][d][1]));
        g_peak[0][d][0] = g_peak[0][d][1] = g_peak[1][d][0] = g_peak[1][d][1] = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

// ---- Switch HD rumble packets
// An amplitude as the controller's step (0..100): 0.012 and below one step, then log steps (rumble_data_table)
static int amp_code(float a) {
    if (!(a > 0)) return 0;
    if (a >= 1) return 100;
    int e = a < 0.012f ? 1 : a < 0.112f ? (int)lroundf(4 * log2f(a * 110)) : a < 0.225f ? (int)lroundf(16 * log2f(a * 17))
                                                                                         : (int)lroundf(32 * log2f(a * 8.7f));
    return e < 1 ? 1 : e > 100 ? 100 : e;
}

// One side's 4 bytes: the high band's frequency (9 bits: 0x00..0x7f << 2, from 81.75 Hz), its amplitude (step * 2),
// the low band's frequency (7 bits: 0x01..0x7f, from 40.9 Hz), its amplitude (0x40 + step / 2, odd steps 0x8000) -
// frequencies 32 * log2(f / 10). Nothing playing: the controllers' usual neutral value, 00 01 40 40.
static void hd_encode(uint8_t out[4], float al, float fl, float ah, float fh) {
    int la = amp_code(al), ha = amp_code(ah);
    if (!la && !ha) { out[0] = 0x00, out[1] = 0x01, out[2] = 0x40, out[3] = 0x40; return; }
    fl = fminf(fmaxf(fl, 40.875885f), 626.286133f), fh = fminf(fmaxf(fh, 81.75177f), 1252.572266f);  // (the bands' ranges)
    int lf = (int)lroundf(32 * log2f(fl / 10)) - 0x40;
    int hf = (int)lroundf(32 * log2f(fh / 10)) - 0x60;
    lf = lf < 0x01 ? 0x01 : lf > 0x7f ? 0x7f : lf;
    hf = hf < 0x00 ? 0x00 : hf > 0x7f ? 0x7f : hf;
    unsigned hfe = (unsigned)hf << 2, lae = (0x40u + (unsigned)la / 2) | (la & 1 ? 0x8000u : 0);
    out[0] = (uint8_t)(hfe & 0xff);
    out[1] = (uint8_t)(ha * 2 | ((hfe >> 8) & 1));
    out[2] = (uint8_t)(lf | ((lae >> 8) & 0x80));
    out[3] = (uint8_t)(lae & 0xff);
}

// ---- DualSense haptics: its audio device (4 channels: speaker / headphones left, right, actuator left, right)
static SDL_AudioStream *g_ds;
static _Atomic float g_ds_gain;  // the strength; 0: silent (rumble off, a menu...)
static float g_ds_phase[2][2], g_ds_amp[2][2];  // (the audio thread's) [left, right][low, high band]

static void SDLCALL ds_feed(void *ud, SDL_AudioStream *st, int need, int total) {
    (void)ud, (void)total;
    enum { BLOCK = 48 };  // 1 ms: the game's value is looked at again each block
    float buf[BLOCK * 4];
    const float k = 1 - expf(-1.0f / (0.002f * 48000));  // (amplitude changes eased over ~2 ms: no clicks)
    for (int frames = (need + 15) / 16; frames > 0; frames -= BLOCK) {
        int n = frames < BLOCK ? frames : BLOCK;
        float v[2][4], s = g_ds_gain;
        take(v, false);
        for (int d = 0; d < 2; d++) {
            float t[2] = {s > 0 && v[d][AL] > 0 ? fminf(1, s * sqrtf(v[d][AL])) : 0,
                          s > 0 && v[d][AH] > 0 ? fminf(1, s * sqrtf(v[d][AH])) : 0};
            float sum = t[0] + t[1];
            if (sum > 1) t[0] /= sum, t[1] /= sum;  // (both bands at once never past full swing)
            // (the bands' ranges, as a Switch controller clamps them: the game asks for 12..30 Hz, which plays as
            // the lowest each band has - a voice coil wouldn't move at 13 Hz)
            float fl = fminf(fmaxf(v[d][FL], 40.875885f), 626.286133f), fh = fminf(fmaxf(v[d][FH], 81.75177f), 1252.572266f);
            float w[2] = {6.2831853f * fl / 48000, 6.2831853f * fh / 48000};
            for (int i = 0; i < n; i++) {
                float x = 0;
                for (int b = 0; b < 2; b++) {
                    g_ds_amp[d][b] += (t[b] - g_ds_amp[d][b]) * k;
                    x += g_ds_amp[d][b] * sinf(g_ds_phase[d][b]);
                    g_ds_phase[d][b] += w[b];
                    if (g_ds_phase[d][b] > 6.2831853f) g_ds_phase[d][b] -= 6.2831853f;
                }
                buf[i * 4 + 0] = buf[i * 4 + 1] = 0;
                buf[i * 4 + 2 + d] = 0.8f * x;
            }
        }
        SDL_PutAudioStreamData(st, buf, n * 16);
    }
}

static void ds_close(void) {
    if (!g_ds) return;
    SDL_DestroyAudioStream(g_ds);
    g_ds = NULL;
    LOG("[rumble] DualSense haptics closed\n");
}

// The DualSense's audio device (its name has "DualSense", or "Wireless Controller" as older Windows call it - a
// DualShock 4's is that too, but has 2 channels), opened for its haptics
static bool ds_open(void) {
    if (g_ds) return true;
    const char *na = getenv("LARECOMP_NO_AUDIO");
    if (na && strcmp(na, "0")) return false;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    const char *want = getenv("LARECOMP_RUMBLE_DEVICE");
    int n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n), dev = 0;
    for (int i = 0; ids && i < n && !dev; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (!name || !(want ? SDL_strcasestr(name, want) != NULL : SDL_strcasestr(name, "DualSense") || SDL_strcasestr(name, "Wireless Controller")))
            continue;
        SDL_AudioSpec spec;
        int frames;
        bool ok = SDL_GetAudioDeviceFormat(ids[i], &spec, &frames);
        LOG("[rumble] audio device \"%s\": %d channels%s\n", name, ok ? spec.channels : 0,
            ok && spec.channels == 4 ? "" : " (not 4: not its haptics)");
        if (ok && spec.channels == 4) dev = ids[i];
    }
    SDL_free(ids);
    if (!dev) { LOG("[rumble] no DualSense audio device (USB only): motors\n"); return false; }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 4, 48000};
    memset(g_ds_amp, 0, sizeof g_ds_amp);
    g_ds = SDL_OpenAudioDeviceStream(dev, &spec, ds_feed, NULL);
    if (!g_ds) { LOG("[rumble] DualSense haptics: can't open its audio device: %s\n", SDL_GetError()); return false; }
    SDL_ResumeAudioStreamDevice(g_ds);
    LOG("[rumble] DualSense haptics on its audio device\n");
    return true;
}

// ---- the pump (the event loop, which owns the controller)
enum { OUT_NONE, OUT_MOTORS, OUT_SWITCH_HD, OUT_DUALSENSE };
static const char *const g_out_names[] = {"none", "motors", "Switch HD rumble", "DualSense haptics"};
static _Atomic int g_out = OUT_NONE;
static uint8_t g_hd_sent[8];
static uint16_t g_motor_sent[2];
static uint64_t g_sent_ns;

static uint16_t motor(const float v[4], float s) {
    float a = fmaxf(v[AL], v[AH]);
    return a > 0 && s > 0 ? (uint16_t)(fminf(1, s * sqrtf(a)) * 65535.0f) : 0;
}

// What the controller gets (when it or the HD rumble setting changes); what played before stops
static void choose_output(SDL_Gamepad *pad, SDL_Gamepad *was_pad, bool hd) {
    int was = g_out, kind = OUT_NONE;
    if (pad) {
        SDL_GamepadType t = SDL_GetGamepadType(pad);
        bool sw = t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
                  t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
        bool wired = SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRED;
        kind = !hd ? OUT_MOTORS : sw ? OUT_SWITCH_HD : t == SDL_GAMEPAD_TYPE_PS5 && wired && ds_open() ? OUT_DUALSENSE : OUT_MOTORS;
    }
    if (was == OUT_DUALSENSE && kind != OUT_DUALSENSE) ds_close();
    if (pad && pad == was_pad) {  // (the same controller: the old way stops; another one was let go of already)
        if (was == OUT_MOTORS && kind != OUT_MOTORS) SDL_RumbleGamepad(pad, 0, 0, 0);
        static const uint8_t stop[10] = {0x10, 0, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
        if (was == OUT_SWITCH_HD && kind != OUT_SWITCH_HD) SDL_SendGamepadEffect(pad, stop, sizeof stop);
    }
    memset(g_hd_sent, 0, sizeof g_hd_sent);
    g_motor_sent[0] = g_motor_sent[1] = 0, g_sent_ns = 0;
    g_out = kind;
    if (pad) LOG("[rumble] %s: %s\n", SDL_GetGamepadName(pad), g_out_names[kind]);
}

void rumble_pump(SDL_Gamepad *pad, bool off) {
    static int verbose = -1;
    if (verbose < 0) verbose = getenv("LARECOMP_LOG_RUMBLE") ? 1 : 0;
    g_log_values = verbose;
    static SDL_Gamepad *pad_was;
    static SDL_JoystickID id_was;
    static bool hd_was;
    SDL_JoystickID id = pad ? SDL_GetGamepadID(pad) : 0;
    if (id != id_was || g_settings.hd_rumble != hd_was) {
        choose_output(pad, id == id_was ? pad_was : NULL, g_settings.hd_rumble);
        pad_was = pad, id_was = id, hd_was = g_settings.hd_rumble;
    }
    // (called for each event too - gyro streams come at hundreds a second: a look every 8 ms; the Switch controllers
    // get a packet at most every 15 ms, as the console sends them - more floods a Bluetooth link)
    uint64_t now = SDL_GetTicksNS();
    static uint64_t looked;
    if (now - looked < (g_out == OUT_SWITCH_HD ? 15000000ull : 8000000ull)) return;
    looked = now;

    float v[2][4];
    take(v, true);
    bool on = g_settings.rumble && !off;
    float s = on ? g_settings.rumble_strength : 0;
    uint16_t l = motor(v[0], s), r = motor(v[1], s);
    wiimote_rumble(on && (l > 16000 || r > 16000));  // (the remote's motor is on / off)
    g_ds_gain = g_out == OUT_DUALSENSE ? s : 0;
    if (!pad || off) return;

    if (g_out == OUT_SWITCH_HD) {
        uint8_t pkt[10] = {0x10, 0};  // (SDL numbers it)
        for (int d = 0; d < 2; d++)
            hd_encode(pkt + 2 + 4 * d, fminf(1, s * v[d][AL]), v[d][FL], fminf(1, s * v[d][AH]), v[d][FH]);
        static const uint8_t neutral[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
        bool same = !memcmp(pkt + 2, g_hd_sent, 8), idle = !memcmp(pkt + 2, neutral, 8);
        // a change goes out now; a vibration still playing again every 50 ms, as SDL keeps its own rumble up
        if (same && (idle || now - g_sent_ns < 50000000ull)) return;
        if (verbose && !same)
            LOG("[rumble] HD %02x %02x %02x %02x  %02x %02x %02x %02x\n", pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7], pkt[8], pkt[9]);
        if (SDL_SendGamepadEffect(pad, pkt, sizeof pkt)) {
            memcpy(g_hd_sent, pkt + 2, 8), g_sent_ns = now;
            return;
        }
        LOG("[rumble] %s refused an HD rumble packet (%s): motors\n", SDL_GetGamepadName(pad), SDL_GetError());
        g_out = OUT_MOTORS;
    }
    if (g_out != OUT_MOTORS) return;
    if (l == g_motor_sent[0] && r == g_motor_sent[1] && (!(l | r) || now - g_sent_ns < 100000000ull)) return;  // (refreshed every 100 ms)
    if (verbose && (l != g_motor_sent[0] || r != g_motor_sent[1])) LOG("[rumble] motors %u %u\n", l, r);
    SDL_RumbleGamepad(pad, l, r, 200);
    g_motor_sent[0] = l, g_motor_sent[1] = r, g_sent_ns = now;
}

// What the controller's rumble is played as (the Audio tab): "motors", "Switch HD rumble", "DualSense haptics",
// NULL with no controller
const char *rumble_output_name(void) {
    int o = g_out;
    return o == OUT_NONE ? NULL : g_out_names[o];
}

// Controller input: SDL keyboard / mouse / gamepad -> nn::hid Npad state.
//
// All go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard and mouse as a PC shooter,
// WASD, the mouse aiming (runtime/hle/game.c mouse aim), clicks to shoot and lock on; controller by position,
// Switch A = east).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../glyphs.h"
#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "wiimote.h"

// runtime/hle/motion.c
void motion_pad(SDL_Gamepad *pad);
void motion_event(const SDL_Event *e);
void motion_frame(uint64_t frame);

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[BIND_KEY_HOST_COUNT];  // keys, then mouse buttons and the wheel (BIND_MOUSE_*)
static SDL_Gamepad *pad;
static _Atomic int64_t sampling;

// Host keys / buttons held by a test script (LARECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[BIND_KEY_HOST_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in LARECOMP_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static int held_key(int sc) { return sc > 0 && sc < BIND_KEY_HOST_COUNT && (keys_down[sc] || test_keys[sc]); }

// Menu keys: a key pressed while the game isn't in play (its menus, the map, the title screen, cutscenes;
// game_in_play) means what it would in a PC game's menus - Enter / Space confirm (A), Esc / Backspace go back (B),
// Q / E switch the pause menu's pages (L / R), the arrows move (d-pad) - instead of its binding. A key keeps the
// meaning it was pressed with until it's let go (Esc opening the pause menu isn't also its Back).
extern bool game_in_play(void);  // runtime/hle/game.c
static uint8_t key_menu[BIND_KEY_HOST_COUNT];
static int menu_key(int sc) {
    switch (sc) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: return BTN_A;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: return BTN_B;
    case SDL_SCANCODE_Q: return BTN_L;
    case SDL_SCANCODE_E: return BTN_R;
    case SDL_SCANCODE_UP: return BTN_UP;
    case SDL_SCANCODE_DOWN: return BTN_DOWN;
    case SDL_SCANCODE_LEFT: return BTN_LEFT;
    case SDL_SCANCODE_RIGHT: return BTN_RIGHT;
    default: return -1;
    }
}
static int bound_held(int sc) { return held_key(sc) && !(key_menu[sc] && menu_key(sc) >= 0); }

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (bound_held(g_bind_key[i])) b |= 1ull << i;
    static const uint8_t beam_dir[4] = {BTN_UP, BTN_RIGHT, BTN_DOWN, BTN_LEFT};  // Power, Wave, Ice, Plasma
    for (int i = 0; i < 4; i++)
        if (bound_held(g_bind_key[BIND_BEAM_POWER + i])) b |= 1ull << BTN_X | 1ull << beam_dir[i];
    for (int sc = 1; sc < BIND_KEY_HOST_COUNT; sc++)
        if (key_menu[sc] && held_key(sc) && menu_key(sc) >= 0) b |= 1ull << menu_key(sc);
    key_buttons = b;
    key_lx = (bound_held(g_bind_key[BIND_LS_RIGHT]) - bound_held(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (bound_held(g_bind_key[BIND_LS_UP]) - bound_held(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (bound_held(g_bind_key[BIND_RS_RIGHT]) - bound_held(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (bound_held(g_bind_key[BIND_RS_UP]) - bound_held(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// ---- Mouse (runtime/hle/game.c mouse aim)
// Captured - relative mode: no cursor, raw motion - while the game is in play, its window has focus, no overlay menu is
// up and Mouse aim is on; released otherwise (in the game's menus the cursor comes back). Its motion is summed here
// and taken once per gameplay frame (input_mouse_take). Its buttons and wheel are bindings (BIND_MOUSE_*), taken
// while in play; a wheel notch holds its binding for a few frames.
static _Atomic int64_t mouse_dx, mouse_dy;  // counts, x256
static _Atomic int32_t mouse_rx, mouse_ry;  // the right stick, when the motion can't aim directly (game.c)
static bool mouse_captured;
static int wheel_frames[2];

bool input_mouse_take(float *dx, float *dy) {
    int64_t x = atomic_exchange(&mouse_dx, 0), y = atomic_exchange(&mouse_dy, 0);
    *dx = x / 256.0f, *dy = y / 256.0f;
    return x || y;
}
void input_mouse_stick(int32_t rx, int32_t ry) { mouse_rx = rx, mouse_ry = ry; }

static bool scripted_run(void);
void input_mouse_frame(SDL_Window *w) {  // (the event loop, each pass)
    extern bool menu_input_captured(void);
    bool want = g_settings.mouse_aim && game_in_play() && !menu_input_captured() &&
                (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) && !scripted_run();
    if (want == mouse_captured) return;
    SDL_SetWindowRelativeMouseMode(w, want);
    mouse_captured = want;
    mouse_dx = mouse_dy = 0;
    LOG("[input] mouse %s\n", want ? "captured (mouse aim)" : "released");
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad || host_test) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    pad_lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    pad_ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    pad_rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    pad_ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    memset(keys_down, 0, sizeof keys_down);
    mouse_dx = mouse_dy = 0;
    recompute_keyboard();
}

// Rumble: the game's vibration values (hid SendVibrationValue(s), per device: the low / high band amplitudes; MPR
// plays its rumble effects for missiles, damage and the like) drive the controller's motors, the left device the
// left (low frequency) one and the right device the right one. The game sends values for every player: player 1's
// (npad 0) and the handheld one's count, whichever is stronger. The game's IPC thread sets them; the event loop,
// which owns the pad, sends them (input_rumble_pump). LARECOMP_NO_RUMBLE=1: none; scripted runs don't rumble;
// LARECOMP_LOG_RUMBLE=1 logs the changes.
static _Atomic uint32_t rumble_amp[2][2];  // [npad 0, handheld][left, right], 0..65535
static bool scripted_run(void);
void input_set_vibration(uint32_t handle, float amp_low, float amp_high) {
    uint32_t npad = (handle >> 8) & 0xFF;
    if (npad != 0 && npad != 0x20) return;
    float a = fmaxf(amp_low, amp_high);
    rumble_amp[npad != 0][handle & 1] = a > 0 ? (uint32_t)(fminf(a, 1.0f) * 65535.0f) : 0;  // (NaN: 0)
}
void input_rumble_pump(void) {
    static int off = -1, verbose = -1;
    static uint32_t sent[2];
    static uint64_t sent_ns;
    if (off < 0) off = (getenv("LARECOMP_NO_RUMBLE") && atoi(getenv("LARECOMP_NO_RUMBLE"))) || scripted_run();
    if (verbose < 0) verbose = getenv("LARECOMP_LOG_RUMBLE") ? 1 : 0;
    uint32_t l = rumble_amp[0][0] > rumble_amp[1][0] ? rumble_amp[0][0] : rumble_amp[1][0];
    uint32_t r = rumble_amp[0][1] > rumble_amp[1][1] ? rumble_amp[0][1] : rumble_amp[1][1];
    if (!g_settings.rumble) l = r = 0;  // (switched off in the menu: a running rumble stops)
    uint64_t now = SDL_GetTicksNS();
    if (l == sent[0] && r == sent[1] && (!(l | r) || now - sent_ns < 100000000ull)) return;  // (refreshed every 100 ms)
    if (verbose && (l != sent[0] || r != sent[1])) LOG("[rumble] %u %u%s\n", l, r, pad && !off ? "" : " (not sent)");
    if (pad && !off) SDL_RumbleGamepad(pad, (Uint16)l, (Uint16)r, 200);
    wiimote_rumble(!off && (l > 16000 || r > 16000));  // (the remote's motor is on / off)
    sent[0] = l, sent[1] = r, sent_ns = now;
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log
volatile int g_pos_save_request;     // F8: note the player's position (runtime/hle/game.c, positions.txt)

// Events a test pushes (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Samus spoils it):
// LARECOMP_NO_USER_INPUT=1, on by default whenever LARECOMP_INPUT, LARECOMP_HOST_INPUT or LARECOMP_BIND_TEST is set
// (LARECOMP_NO_USER_INPUT=0 lets it through again).
static bool scripted_run(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    return on;
}

bool input_user_blocked(const SDL_Event *e) {
    if (!scripted_run()) return false;
    if (test_event(e)) return false;  // (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:  // (the player's pad on the desk moved the aim in tests; LARECOMP_REAL_GYRO=1:
        return !getenv("LARECOMP_REAL_GYRO");  // its gyro anyway, for looking at a real controller's data)
    default:
        return false;
    }
}

_Atomic uint64_t g_input_buttons;  // the Switch buttons the game was last given (runtime/hle/cheats.c: L3 for a hint)

// Controller family for the Controls tab's button names (bindings.c): 0 Nintendo / other / none, 1 Xbox, 2 PlayStation.
// LARECOMP_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
static _Atomic int pad_family;
static void note_family(void) {
    int f = 0;
    if (pad) switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: f = 1; break;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: f = 2; break;
        default: break;
    }
    if (f != pad_family) LOG("[input] controller family: %s\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo / other");
    pad_family = f;
}
static void log_pad(void) {
    if (!pad) return;
    SDL_PropertiesID p = SDL_GetGamepadProperties(pad);
    LOG("[input] gamepad: %s (rumble %s)\n", SDL_GetGamepadName(pad),
        SDL_GetBooleanProperty(p, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no");
}
// The open controller's SDL_GamepadType, -1 for none (runtime/hle/glyphs.c: which button icons Auto shows);
// LARECOMP_PAD_TYPE pretends as for input_pad_family
int input_pad_type(void) {
    const char *e = getenv("LARECOMP_PAD_TYPE");
    if (e) return !strcmp(e, "xbox") ? SDL_GAMEPAD_TYPE_XBOXONE : !strcmp(e, "playstation") || !strcmp(e, "ps") ? SDL_GAMEPAD_TYPE_PS5 :
                  !strcmp(e, "nintendo") ? SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO : SDL_GAMEPAD_TYPE_STANDARD;
    return pad ? (int)SDL_GetGamepadType(pad) : -1;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

void input_handle_event(const SDL_Event *e) {
    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (n > 0 && !pad) { pad = SDL_OpenGamepad(ids[0]); log_pad(); note_family(); motion_pad(pad); }
        SDL_free(ids);
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F8 && !e->key.repeat) g_pos_save_request = 1;
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && !(e->key.scancode >= SDL_SCANCODE_F1 && e->key.scancode <= SDL_SCANCODE_F12))
            glyphs_note_device(GLYPH_DEV_KEYS);  // (the button icons, on Auto: the runtime's own F keys don't count)
        if (e->key.scancode < SDL_SCANCODE_COUNT) {
            keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
            if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) key_menu[e->key.scancode] = !game_in_play();
        }
        recompute_keyboard();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_captured) {
            mouse_dx += (int64_t)lroundf(e->motion.xrel * 256);
            mouse_dy += (int64_t)lroundf(e->motion.yrel * 256);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) glyphs_note_device(GLYPH_DEV_KEYS);
        if (e->button.button >= 1 && e->button.button <= 5 && (e->type == SDL_EVENT_MOUSE_BUTTON_UP || game_in_play())) {
            keys_down[BIND_MOUSE_LEFT + e->button.button - 1] = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            recompute_keyboard();
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e->wheel.y != 0 && game_in_play()) {
            wheel_frames[e->wheel.y < 0] = 4;
            keys_down[e->wheel.y > 0 ? BIND_WHEEL_UP : BIND_WHEEL_DOWN] = 1;
            recompute_keyboard();
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad) { pad = SDL_OpenGamepad(e->gdevice.which); log_pad(); note_family(); motion_pad(pad); }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) { motion_pad(NULL); SDL_CloseGamepad(pad); pad = NULL; note_family(); }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(e->gaxis.value) > 20000))
            glyphs_note_device(GLYPH_DEV_PAD);
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        motion_event(e);  // (runtime/hle/motion.c: the gyro for the game's motion controls)
        break;
    }
    input_rumble_pump();  // (and from the event loop when it's idle)
}

// Scripted input for testing: LARECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;

// Tests of the bindings, through them like a real player:
// LARECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in primerecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// "frame:mouse:<dx>/<dy>:frames" moves the mouse that many counts each frame (mouse aim; "key:Mouse Left" clicks).
// LARECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
char g_window_shot_request[256];  // test hook ("shot" in LARECOMP_INPUT): the presenter saves the next frame here

static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "shot")) {
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("LARECOMP_HOST_INPUT"); bspec = getenv("LARECOMP_BIND_TEST"); host_test = hspec; }
    char buf[2048];
    if (hspec) {
        uint8_t keys[BIND_KEY_HOST_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { int sc = bind_key_from_name(name); if (sc > 0 && sc < BIND_KEY_HOST_COUNT) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else if (!strcmp(dev, "mouse")) {
                float mx = 0, my = 0;
                sscanf(name, "%f/%f", &mx, &my);
                mouse_dx += (int64_t)lroundf(mx * 256), mouse_dy += (int64_t)lroundf(my * 256);
            }
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        for (int sc = 0; sc < BIND_KEY_HOST_COUNT; sc++)
            if (keys[sc] && !test_keys[sc]) key_menu[sc] = !game_in_play();  // (as a real key press)
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
RT_ON_RESET(input_reboot_reset) { g_pad_obj = 0; }  // (reboot: the controller object was guest memory)
void input_frame(uint64_t frame) {
    wiimote_start();  // (once: the Wii Remote's reader thread, runtime/hle/wiimote.c)
    if (g_pad_obj && getenv("LARECOMP_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    for (int i = 0; i < 2; i++)  // a wheel notch's binding lets go
        if (wheel_frames[i] && !--wheel_frames[i]) { keys_down[i ? BIND_WHEEL_DOWN : BIND_WHEEL_UP] = 0; recompute_keyboard(); }
    host_test_frame(frame);
    motion_frame(frame);  // (LARECOMP_GYRO_TEST)
    const char *spec = getenv("LARECOMP_INPUT");
    if (!spec) return;
    {   // LARECOMP_INPUT_CLOCK=<hz>: the script's frames are 1/<hz> s since the first frame, not presented frames
        // (the same script at any frame rate: LARECOMP_INPUT_CLOCK=60 at 120 fps)
        static int hz = -1;
        static uint64_t t0;
        if (hz < 0) hz = getenv("LARECOMP_INPUT_CLOCK") ? atoi(getenv("LARECOMP_INPUT_CLOCK")) : 0;
        if (hz > 0) {
            if (!t0) t0 = plat_now_ns();
            frame = (plat_now_ns() - t0) * hz / 1000000000ull;
        }
    }
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)


static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// The Wii Remote + Nunchuk (runtime/hle/wiimote.c) as the Switch controller, laid out like Metroid Prime: Trilogy's
// controls on the game's Pointer scheme (its View Controls: A shoots, B jumps, ZL locks on, Y morphs, X fires a
// missile, the d-pad picks visors, the right stick beams, Plus the map, Minus the pause menu): A = A, B (the
// trigger) = B, Z = ZL, C = Y, 1 = X, the d-pad = the d-pad, - / + = Minus / Plus, the Nunchuk's stick = the left
// stick; 2 held turns the d-pad into the right stick (the beams). (ZR / R, the Pointer scheme's "reset gyro", aren't
// needed: the IR pointer doesn't drift. Home: unused - the overlay menu is F1 / the controller's own chord.)
static uint64_t wiimote_buttons(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    WiimoteState w;
    wiimote_state(&w);
    *lx = *ly = *rx = *ry = 0;
    if (!w.connected || !g_settings.wiimote) return 0;
    bool beams = (w.buttons & WM_TWO) != 0;
    static const struct { uint16_t wm; int bit; } map[] = {
        {WM_A, 0}, {WM_B, 1}, {WM_ONE, 2}, {WM_MINUS, 11}, {WM_PLUS, 10},
        {WM_LEFT, 12}, {WM_UP, 13}, {WM_RIGHT, 14}, {WM_DOWN, 15}};
    uint64_t b = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if ((w.buttons & map[i].wm) && !(beams && map[i].bit >= 12)) b |= 1ull << map[i].bit;
    if (beams) {  // (the d-pad as the right stick, full tilt)
        if (w.buttons & WM_LEFT) *rx = -32767;
        if (w.buttons & WM_RIGHT) *rx = 32767;
        if (w.buttons & WM_UP) *ry = 32767;
        if (w.buttons & WM_DOWN) *ry = -32767;
    }
    if (w.nunchuk_z) b |= 1ull << 8;  // ZL
    if (w.nunchuk_c) b |= 1ull << 3;  // Y
    float sx = w.stick[0], sy = w.stick[1];
    if (fabsf(sx) < 0.08f) sx = 0;  // (a dead zone: the Nunchuk's stick rests a little off)
    if (fabsf(sy) < 0.08f) sy = 0;
    *lx = (int32_t)(sx * 32767), *ly = (int32_t)(sy * 32767);
    return b;
}

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != 0) return;  // docked: only player 1 (FullKey); Handheld stays disconnected
    extern uint64_t g_watch_lo, g_watch_hi;
    if (id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (id == 0 && !g_watch_lo && getenv("LARECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("LARECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    int32_t wm_lx, wm_ly, wm_rx, wm_ry;
    uint64_t wm_b = wiimote_buttons(&wm_lx, &wm_ly, &wm_rx, &wm_ry);
    if (wm_b) glyphs_note_device(GLYPH_DEV_WII);
    glyphs_frame();  // (the button icons: what they should show now)
    uint64_t b = menu ? script_buttons : key_buttons | pad_buttons | wm_b | script_buttons;
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx + wm_lx) + script_lx);
    int32_t ly = clamp_axis((menu ? 0 : key_ly + pad_ly + wm_ly) + script_ly);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx + mouse_rx + wm_rx) + script_rx);
    int32_t ry = clamp_axis((menu ? 0 : key_ry + pad_ry + mouse_ry + wm_ry) + script_ry);
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    *(uint64_t *)(s + 8) = b;
    extern _Atomic uint64_t g_input_buttons;
    g_input_buttons = b;
    static uint64_t last_b;
    if (b != last_b && getenv("LARECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// FullKey for player 1 only (we report docked mode, where Handheld is unavailable)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == 0 ? 1 : 0;  // docked: no handheld controller
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// Vibration devices (the SDK's vibration node graph stays recompiled; its values reach hid's SendVibrationValue,
// runtime/services/hid_set.c, and the controller through input_set_vibration).
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = h & 1;    // position: left / right
}

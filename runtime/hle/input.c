// Controller input: SDL keyboard / mouse / gamepad -> nn::hid Npad state.
//
// All go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard and mouse as a PC shooter,
// WASD, the mouse aiming (runtime/hle/game.c mouse aim), clicks to shoot and lock on; controller by position,
// Switch A = east) - except in the game's menus, where Enter / Esc and the controller's face buttons by their labels
// confirm and go back (menu keys, menu face buttons below).
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
static _Atomic int32_t test_lx, test_ly, test_rx, test_ry;  // (a resting stick's drift: "lstick" / "rstick" in LARECOMP_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static int held_key(int sc) { return sc > 0 && sc < BIND_KEY_HOST_COUNT && (keys_down[sc] || test_keys[sc]); }

// Menu keys: a key pressed while the game isn't in play (its menus, the map, the title screen, cutscenes;
// game_in_play) means what it would in a PC game's menus - Enter / Space confirm (A), Esc / Backspace go back (B),
// Q / E switch the pause menu's pages (L / R), the arrows move (d-pad), Delete is Minus (the save slots' Erase, the
// pause screen's Exit: Esc, Minus in play, is Back there, so nothing pressed Minus in menus and its prompts kept the
// Switch's icon) - instead of its binding. A key keeps the meaning it was pressed with until it's let go (Esc opening
// the pause menu isn't also its Back).
extern bool game_in_play(void);  // runtime/hle/game.c
static uint8_t key_menu[BIND_KEY_HOST_COUNT];
static int menu_key(int sc) {
    switch (sc) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: return BTN_A;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: return BTN_B;
    case SDL_SCANCODE_DELETE: return BTN_MINUS;
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
int input_menu_key(int sc) { return sc > 0 && sc < BIND_KEY_HOST_COUNT ? menu_key(sc) : -1; }  // (glyphs.c)

// On the map (the in-game GUI's screen 2: runtime/hle/game.c game_gui_screen) a key bound to a stick is that stick,
// not a menu key: the map moves with the right stick and turns with the left, and its legend shows their keys - the
// arrows, by default the right stick's, were the d-pad there and moved nothing (issue #37)
static bool map_stick_key(int sc) {
    extern void game_gui_screen(int *now, int *before);
    int now, before;
    game_gui_screen(&now, &before);
    if (now != 2) return false;
    for (int i = BIND_LS_UP; i <= BIND_RS_RIGHT; i++)
        if (g_bind_key[i] == sc) return true;
    return false;
}

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

// The pointer, while the mouse isn't captured (the game's menus and cutscenes, Mouse aim off): hidden once a controller
// or the Wii Remote is what's played with, shown again as soon as the mouse moves a little or clicks - it sat in the
// middle of the screen while playing with a controller (issue #51). Motion right after the capture changes is left
// out (SDL may move the pointer back then).
static _Atomic bool pad_in_use;  // a controller / the Wii Remote used since the mouse last moved
static float pointer_moved;      // (pixels the mouse moved since then)
static uint64_t capture_changed_ns;

static bool scripted_run(void);
void input_mouse_frame(SDL_Window *w) {  // (the event loop, each pass)
    extern bool menu_input_captured(void);
    bool want = g_settings.mouse_aim && game_in_play() && !menu_input_captured() &&
                (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) && !scripted_run();
    bool hide = pad_in_use;
    if (hide == SDL_CursorVisible()) {  // (checked each pass: the start screen shows it again)
        if (hide) SDL_HideCursor(), pointer_moved = 0;
        else SDL_ShowCursor();
        static int n;
        if (n++ < 6) LOG("[input] pointer %s\n", hide ? "hidden: a controller is in use" : "shown");
    }
    if (want == mouse_captured) return;
    SDL_SetWindowRelativeMouseMode(w, want);
    mouse_captured = want;
    mouse_dx = mouse_dy = 0;
    capture_changed_ns = SDL_GetTicksNS();
    LOG("[input] mouse %s\n", want ? "captured (mouse aim)" : "released");
}

// The game's menus with the mouse (runtime/hle/menumouse.c): where the pointer is (fractions of the window), a serial
// bumped as it moves and one as the left button goes down - while the game isn't in play and the mouse isn't captured.
// A click over a menu item makes an A press: held for a few of the game's reads, a couple of reads on (the item
// selected first takes a frame).
static _Atomic uint32_t menu_px_bits, menu_py_bits, menu_moves, menu_clicks;
static _Atomic int64_t menu_a_from, menu_a_to;

static void menu_pointer_at(SDL_WindowID wid, float x, float y) {
    SDL_Window *w = SDL_GetWindowFromID(wid);
    int ww = 0, wh = 0;
    if (!w || !SDL_GetWindowSize(w, &ww, &wh) || ww <= 0 || wh <= 0) return;
    float fx = x / ww, fy = y / wh;
    uint32_t bx, by;
    memcpy(&bx, &fx, 4), memcpy(&by, &fy, 4);
    menu_px_bits = bx, menu_py_bits = by;
}

bool input_menu_pointer(float *x, float *y, uint32_t *moves, uint32_t *clicks) {
    uint32_t bx = menu_px_bits, by = menu_py_bits;
    memcpy(x, &bx, 4), memcpy(y, &by, 4);
    *moves = menu_moves, *clicks = menu_clicks;
    return !mouse_captured && !pad_in_use && !game_in_play();
}

void input_menu_press_a(void) {  // (the game's thread)
    menu_a_from = sampling + 1;
    menu_a_to = sampling + 5;
}

static void pointer_used(float dx, float dy, bool click) {
    if (!pad_in_use) return;
    if (!click && SDL_GetTicksNS() - capture_changed_ns < 200000000ull) return;
    pointer_moved += fabsf(dx) + fabsf(dy);
    if (click || pointer_moved > 12) pad_in_use = false;
}
static void pad_used(void) { pad_in_use = true; }  // (also from the game's thread: the Wii Remote)

// Every event, before the overlay menu takes its own (runtime/nvn/vulkan/device.c ui_run): what's being played with
void input_pointer_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: pointer_used(e->motion.xrel, e->motion.yrel, false); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: pointer_used(0, 0, true); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: pad_used(); break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: if (abs(e->gaxis.value) > 20000) pad_used(); break;
    default: break;
    }
}

// A stick's dead zone (Controls tab; issue #14): the game's own is small - about 5% of full tilt - and it acts hard
// just past it (measured: the right stick resting at 7.6% turned the view a full circle in 5 s; the left at 6.1% walked
// Samus slowly), so a stick resting a little off its middle turned the view or walked her. Radial: within dz of the
// middle it reads 0; past it the tilt is scaled back up from 0, so no step at its edge and full tilt stays full (with
// the game's own on top, 5% here ignores sticks resting up to ~10% off).
static void dead_zone(_Atomic int32_t *xa, _Atomic int32_t *ya, int32_t x, int32_t y, float dz) {
    float fx = x / 32767.0f, fy = y / 32767.0f, m = sqrtf(fx * fx + fy * fy);
    if (dz > 0 && m <= dz) x = y = 0;
    else if (dz > 0) {
        float k = (m - dz) / (1 - dz) / m;
        x = (int32_t)lroundf(fminf(fmaxf(fx * k, -1), 1) * 32767), y = (int32_t)lroundf(fminf(fmaxf(fy * k, -1), 1) * 32767);
    }
    *xa = x, *ya = y;
}

// The left stick's straight ahead (Controls tab; issue #59): within straight_deg of straight up or down it reads
// straight, its full tilt kept. In the Classic scheme the stick's sideways turns Samus, and a stick in a round gate is
// rarely pushed dead straight (a GameCube stick's gate has notches there), so she walked in curves.
static void straight_snap(_Atomic int32_t *xa, _Atomic int32_t *ya) {
    int32_t x = *xa, y = *ya;
    if (g_settings.straight_deg <= 0 || !x || !y) return;
    if (fabsf((float)x) > fabsf((float)y) * tanf((float)g_settings.straight_deg * 3.14159265f / 180)) return;
    float m = fminf(sqrtf((float)x * x + (float)y * y), 32767);
    *xa = 0, *ya = (int32_t)lroundf(y < 0 ? -m : m);
}

// The controller's sticks as they are (the Controls tab's dead zone picture): x, y of the left then the right, -1..1
// (y up); false with no controller
// Flick stick (runtime/hle/game.c flick_frame): the controller's right stick before its dead zone, and whether it's
// turning Samus now - the game then doesn't get it (its turning and looking would add to the flick's).
static _Atomic int32_t raw_rx, raw_ry;
static _Atomic bool flick_takes_stick;
void input_flick_take(bool on) { flick_takes_stick = on; }
bool input_flick_stick(float *x, float *y) {
    *x = raw_rx / 32767.0f, *y = raw_ry / 32767.0f;
    return pad || host_test;
}

bool input_raw_sticks(float out[4]) {
    SDL_Gamepad *p = pad;
    if (!p) return false;
    out[0] = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    out[1] = -SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    out[2] = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
    out[3] = -SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
    return true;
}

SDL_Gamepad *input_gamepad(void) { return host_test ? NULL : pad; }  // (bindings.c: its button labels)

// Menu face buttons (Face buttons: Labels, the default): in the game's menus - while it isn't in play (game_in_play:
// the title screen, the pause menu, the map, its messages, cutscenes) - the four face buttons go by the controller's
// labels (bindings.c bind_face_label), not their bindings: the button marked A (Cross) confirms and B (Circle) goes
// back on any controller - the bottom / right one on an Xbox or PlayStation controller, the right / bottom one on a
// Switch controller - and X / Y are the ones marked so. In play they're their bindings (by position by default: the
// bottom one jumps on every controller). As with the menu keys, a button keeps the meaning it was pressed with until
// it's let go (A starting the game isn't also a jump). Nintendo / Xbox: the bindings everywhere, as before.
// The game's paused messages (an item's "acquired" text: game.c game_message_screen) are play's: their icons show the
// buttons for play (glyphs.c; issue #52), so there the face buttons are their bindings - but the one labelled A
// confirms as in every menu, so both it and play's A (the right button) close a message.
static uint8_t pad_menu[4];  // SDL_GAMEPAD_BUTTON_SOUTH, EAST, WEST, NORTH: pressed in a menu
static uint32_t pad_was;

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint32_t pressed = host & ~pad_was, menu_held = 0;
    pad_was = host;
    extern bool game_message_screen(void);  // runtime/hle/game.c
    bool message = pressed & 15 ? game_message_screen() : false;
    for (int f = 0; f < 4; f++) {
        if (pressed >> f & 1)
            pad_menu[f] = g_settings.face_buttons == FACE_LABELS && !game_in_play() && (!message || bind_face_label(0, f) == BIND_A);
        if ((host >> f & 1) && pad_menu[f]) menu_held |= 1u << f;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && ((host & ~menu_held) >> g_bind_pad[i] & 1)) b |= 1ull << i;
    for (int f = 0; f < 4; f++)
        if (menu_held >> f & 1) b |= 1ull << bind_face_label(0, f);
    pad_buttons = b;
    if (!pad || host_test) {  // (a test's resting stick: through the dead zone as a real one)
        raw_rx = test_rx, raw_ry = test_ry;
        dead_zone(&pad_lx, &pad_ly, test_lx, test_ly, g_settings.deadzone[0]);
        straight_snap(&pad_lx, &pad_ly);
        dead_zone(&pad_rx, &pad_ry, test_rx, test_ry, g_settings.deadzone[1]);
        return;
    }
    dead_zone(&pad_lx, &pad_ly, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY),
              g_settings.deadzone[0]);
    straight_snap(&pad_lx, &pad_ly);
    raw_rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), raw_ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    dead_zone(&pad_rx, &pad_ry, raw_rx, raw_ry, g_settings.deadzone[1]);
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    memset(keys_down, 0, sizeof keys_down);
    mouse_dx = mouse_dy = 0;
    recompute_keyboard();
}

// Rumble: runtime/hle/rumble.c plays the game's vibration on the controller; the event loop, which owns it, pumps
// it (here, after each event, and when it's idle). LARECOMP_NO_RUMBLE=1: none; scripted runs don't rumble.
static bool scripted_run(void);
void rumble_pump(SDL_Gamepad *pad, bool off);
void input_rumble_pump(void) {
    static int off = -1;
    if (off < 0) off = (getenv("LARECOMP_NO_RUMBLE") && atoi(getenv("LARECOMP_NO_RUMBLE"))) || scripted_run();
    rumble_pump(pad, off);
    static float dz_was[2] = {-1, -1};  // (a dead zone changed in the menu: the sticks again, resting or not)
    if (g_settings.deadzone[0] != dz_was[0] || g_settings.deadzone[1] != dz_was[1]) {
        dz_was[0] = g_settings.deadzone[0], dz_was[1] = g_settings.deadzone[1];
        recompute_pad();
    }
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
    case SDL_EVENT_MOUSE_MOTION: return e->motion.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return e->button.which == TEST_DEVICE;
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
#ifdef __ANDROID__
// SDL's own mapping for a controller it has no entry for (SDL_CreateMappingForAndroidGamepad) gives it trigger axes only
// when it reports six axes or more. One with digital triggers - KEYCODE_BUTTON_L2 / R2, SDL's buttons 15 and 16 there
// (a Switch Pro Controller, many Bluetooth pads) - got no LT / RT at all: they did nothing. They join its mapping as the
// triggers (fully pressed or not), for the game, the menu and the controls screen alike.
static bool mapping_uses_button(const char *m, int b) {
    char key[8];
    snprintf(key, sizeof key, ":b%d", b);
    for (const char *s = strstr(m, key); s; s = strstr(s + 1, key))
        if (s[strlen(key)] == ',' || !s[strlen(key)]) return true;
    return false;
}
static void android_digital_triggers(SDL_Gamepad *p) {
    SDL_Joystick *j = SDL_GetGamepadJoystick(p);
    if (!j || SDL_GamepadHasAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) || SDL_GamepadHasAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) ||
        SDL_GetNumJoystickButtons(j) < 17)
        return;
    char *m = SDL_GetGamepadMapping(p);
    if (!m) return;
    if (!mapping_uses_button(m, 15) && !mapping_uses_button(m, 16)) {
        size_t n = strlen(m) + 40;
        char *with = SDL_malloc(n);
        if (with) {
            snprintf(with, n, "%s%slefttrigger:b15,righttrigger:b16,", m, *m && m[strlen(m) - 1] != ',' ? "," : "");
            if (SDL_AddGamepadMapping(with) >= 0) LOG("[input] digital triggers: L2 / R2 (buttons 15 / 16) as LT / RT\n");
            else LOG("[input] digital triggers not mapped: %s\n", SDL_GetError());
            SDL_free(with);
        }
    }
    SDL_free(m);
}
#endif
static void log_pad(void) {
    if (!pad) return;
#ifdef __ANDROID__
    android_digital_triggers(pad);
    char *m = SDL_GetGamepadMapping(pad);  // (what SDL made of the controller: bug reports)
    LOG("[input] mapping: %s\n", m ? m : "none");
    SDL_free(m);
#endif
    SDL_PropertiesID p = SDL_GetGamepadProperties(pad);
    LOG("[input] gamepad: %s (rumble %s)\n", SDL_GetGamepadName(pad),
        SDL_GetBooleanProperty(p, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no");
    static const char *const side[4] = {"bottom", "right", "left", "top"};  // (SDL_GAMEPAD_BUTTON_SOUTH ...)
    LOG("[input] menus: A (confirm) is its %s face button, B (back) its %s one; in play the bindings\n",
        side[bind_label_host(BIND_A) & 3], side[bind_label_host(BIND_B) & 3]);
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
            if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat)
                key_menu[e->key.scancode] = !game_in_play() && !map_stick_key(e->key.scancode);
        }
        recompute_keyboard();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_captured) {
            mouse_dx += (int64_t)lroundf(e->motion.xrel * 256);
            mouse_dy += (int64_t)lroundf(e->motion.yrel * 256);
        } else if (e->motion.xrel != 0 || e->motion.yrel != 0) {
            menu_pointer_at(e->motion.windowID, e->motion.x, e->motion.y);
            menu_moves++;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) glyphs_note_device(GLYPH_DEV_KEYS);
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT && !mouse_captured &&
            !game_in_play()) {
            menu_pointer_at(e->button.windowID, e->button.x, e->button.y);
            menu_clicks++;
        }
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
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick);
//   "rstick" the right one.
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// "frame:mouse:<dx>/<dy>:frames" moves the mouse that many counts each frame (mouse aim; "key:Mouse Left" clicks).
// "frame:point:<x>/<y>" / "frame:click:<x>/<y>" put the pointer there (fractions of the window) / click there, as
//   real events (the game's menus with the mouse: runtime/hle/menumouse.c).
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
    if (!strcmp(dev, "point") || !strcmp(dev, "click")) {  // the pointer to x/y (fractions of the window), a left click
        float fx = 0, fy = 0;
        int n = 0, ww = 0, wh = 0;
        SDL_Window **ws = SDL_GetWindows(&n);
        SDL_Window *w = ws && n > 0 ? ws[0] : NULL;
        SDL_free(ws);
        if (frame != start || !w || sscanf(arg, "%f/%f", &fx, &fy) != 2 || !SDL_GetWindowSize(w, &ww, &wh)) {
            if (frame == start) LOG("[input] test: no pointer event (window %p, \"%s\")\n", (void *)w, arg);
            return;
        }
        LOG("[input] test: pointer %s at %.0f %.0f of %dx%d\n", dev, fx * ww, fy * wh, ww, wh);
        e.type = SDL_EVENT_MOUSE_MOTION, e.motion.windowID = SDL_GetWindowID(w), e.motion.which = TEST_DEVICE;
        e.motion.x = fx * ww, e.motion.y = fy * wh, e.motion.xrel = 20, e.motion.yrel = 0;
        SDL_PushEvent(&e);
        if (!strcmp(dev, "click")) {
            memset(&e, 0, sizeof e);
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN, e.button.windowID = SDL_GetWindowID(w), e.button.which = TEST_DEVICE;
            e.button.button = SDL_BUTTON_LEFT, e.button.down = true, e.button.clicks = 1, e.button.x = fx * ww, e.button.y = fy * wh;
            SDL_PushEvent(&e);
            e.type = SDL_EVENT_MOUSE_BUTTON_UP, e.button.down = false;
            SDL_PushEvent(&e);
        }
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
        int32_t sx = 0, sy = 0, rx = 0, ry = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot") ||
                !strcmp(dev, "point") || !strcmp(dev, "click")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { int sc = bind_key_from_name(name); if (sc > 0 && sc < BIND_KEY_HOST_COUNT) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else if (!strcmp(dev, "rstick")) sscanf(name, "%d/%d", &rx, &ry);
            else if (!strcmp(dev, "mouse")) {
                float mx = 0, my = 0;
                sscanf(name, "%f/%f", &mx, &my);
                mouse_dx += (int64_t)lroundf(mx * 256), mouse_dy += (int64_t)lroundf(my * 256);
            }
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly ||
                                                            rx != test_rx || ry != test_ry;
        for (int sc = 0; sc < BIND_KEY_HOST_COUNT; sc++)
            if (keys[sc] && !test_keys[sc]) key_menu[sc] = !game_in_play();  // (as a real key press)
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy, test_rx = rx, test_ry = ry;
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
// trigger) = B, Z = ZL, C = Y, + = X (missile), 2 = Plus (the map), 1 = Minus (the pause menu), the d-pad = the d-pad, the
// Nunchuk's stick = the left stick; - held turns the stick into the right stick (the beams: a push snaps to its
// direction, Samus stands still meanwhile; the user's layout, 2026-10-29). (ZR / R, the Pointer scheme's "reset
// gyro", aren't needed: the IR pointer doesn't drift.) Home: the pointer centred where the remote points now (the Controls tab's
// "Centre the pointer" from the couch: with the sensor bar above or below the screen, an uncentred pointer reads
// off the middle - seen with the real remote, the view kept turning down).
static uint64_t wiimote_buttons(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    WiimoteState w;
    wiimote_state(&w);
    *lx = *ly = *rx = *ry = 0;
    if (!w.connected || !g_settings.wiimote) return 0;
    static bool home_was;
    bool home = (w.buttons & WM_HOME) != 0;
    if (home && !home_was && wiimote_calibrate_middle()) settings_save();
    home_was = home;
    static const struct { uint16_t wm; int bit; } map[] = {
        {WM_A, 0}, {WM_B, 1}, {WM_PLUS, 2}, {WM_ONE, 11}, {WM_TWO, 10},
        {WM_LEFT, 12}, {WM_UP, 13}, {WM_RIGHT, 14}, {WM_DOWN, 15}};
    uint64_t b = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (w.buttons & map[i].wm) b |= 1ull << map[i].bit;
    if (w.nunchuk_z) b |= 1ull << 8;  // ZL
    if (w.nunchuk_c) b |= 1ull << 3;  // Y
    float sx = w.stick[0], sy = w.stick[1];
    if (w.buttons & WM_MINUS) {  // - held: the stick picks a beam - its direction, once pushed well over, at full tilt
        if (fabsf(sx) > 0.6f || fabsf(sy) > 0.6f) {
            if (fabsf(sx) > fabsf(sy)) *rx = sx > 0 ? 32767 : -32767;
            else *ry = sy > 0 ? 32767 : -32767;
        }
        return b;
    }
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
    if (wm_b) glyphs_note_device(GLYPH_DEV_WII), pad_used();
    glyphs_frame();  // (the button icons: what they should show now)
    uint64_t b = menu ? script_buttons : key_buttons | pad_buttons | wm_b | script_buttons;
    if (!menu && sampling >= menu_a_from && sampling < menu_a_to) b |= 1ull << BTN_A;  // (a click on a menu item)
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx + wm_lx) + script_lx);
    int32_t ly = clamp_axis((menu ? 0 : key_ly + pad_ly + wm_ly) + script_ly);
    int32_t prx = flick_takes_stick ? 0 : pad_rx, pry = flick_takes_stick ? 0 : pad_ry;
    int32_t rx = clamp_axis((menu ? 0 : key_rx + prx + mouse_rx + wm_rx) + script_rx);
    int32_t ry = clamp_axis((menu ? 0 : key_ry + pry + mouse_ry + wm_ry) + script_ry);
    static long stick_log = -2;  // (debug) LARECOMP_LOG_STICKS=<n>: the sticks the game gets, every n-th read
    if (stick_log == -2) stick_log = getenv("LARECOMP_LOG_STICKS") ? atol(getenv("LARECOMP_LOG_STICKS")) : 0;
    if (stick_log > 0 && sampling % stick_log == 0) LOG("[input] sticks %d %d  %d %d\n", lx, ly, rx, ry);
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

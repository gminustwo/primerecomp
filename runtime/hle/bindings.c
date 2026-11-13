// Control remapping: tables runtime/hle/input.c reads on every input event (so a change applies at once), the
// capture used by the "click a binding, then press" UI, and their primerecomp.ini lines. See runtime/input_bind.h.
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../settings.h"

_Static_assert(PADB_LTRIGGER == SDL_GAMEPAD_BUTTON_COUNT, "trigger pseudo-buttons follow SDL's buttons");
_Static_assert(BIND_MOUSE_LEFT == SDL_SCANCODE_COUNT, "mouse buttons follow SDL's scancodes");

// Defaults: by position, as on a Switch controller (east = A, south = B, north = X, west = Y: the bottom button
// jumps on every controller; the menus go by the labels, bind_face_label; Face buttons: Xbox swaps them), triggers =
// ZL / ZR;
// keyboard and mouse as a PC shooter, for the game's Dual Stick scheme: WASD move, the mouse aims (runtime/hle/game.c
// mouse aim; the arrows turn), left click shoots (ZR), right click locks on / scans (ZL), Space jumps (B), F fires a
// missile (R), Left Ctrl morphs (Y), Q / E / R / T the Combat / Scan / Thermal / X-Ray visors (d-pad up / right /
// down / left), 1-4 the beams, Tab the map (Plus), Esc the pause menu (Minus), Enter = A, X = X (beam select),
// Left Shift = L (jump).
#define DEF_PAD { \
    SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST, \
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, \
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PADB_LTRIGGER, PADB_RTRIGGER, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, \
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN}
#define DEF_KEY { \
    SDL_SCANCODE_RETURN, SDL_SCANCODE_SPACE, SDL_SCANCODE_X, SDL_SCANCODE_LCTRL, SDL_SCANCODE_H, 0, \
    SDL_SCANCODE_LSHIFT, SDL_SCANCODE_F, BIND_MOUSE_RIGHT, BIND_MOUSE_LEFT, SDL_SCANCODE_TAB, SDL_SCANCODE_ESCAPE, \
    SDL_SCANCODE_T, SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_R, \
    SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, \
    SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4}
static const int16_t k_def_pad[BIND_PAD_COUNT] = DEF_PAD, k_def_key[BIND_KEY_COUNT] = DEF_KEY;
int16_t g_bind_pad[BIND_PAD_COUNT] = DEF_PAD, g_bind_key[BIND_KEY_COUNT] = DEF_KEY;
int16_t g_bind_key_mod[BIND_KEY_COUNT][BIND_MODS];  // (the defaults: single keys)

const char *const g_face_names[FACE_COUNT] = {"labels", "nintendo", "xbox"};

// The game's controller (runtime/hle/input.c: none in a scripted run) and its SDL_GamepadType (LARECOMP_PAD_TYPE's
// pretend one too)
extern SDL_Gamepad *input_gamepad(void);
extern int input_pad_type(void);

static int xbox_place(int host) {  // the Xbox controller's: A at the bottom, B on the right, X on the left, Y on top
    return host == SDL_GAMEPAD_BUTTON_SOUTH ? BIND_A : host == SDL_GAMEPAD_BUTTON_EAST ? BIND_B :
           host == SDL_GAMEPAD_BUTTON_WEST ? BIND_X : BIND_Y;
}

int bind_face_label(uint32_t which, int host) {
    if (host != SDL_GAMEPAD_BUTTON_SOUTH && host != SDL_GAMEPAD_BUTTON_EAST && host != SDL_GAMEPAD_BUTTON_WEST &&
        host != SDL_GAMEPAD_BUTTON_NORTH)
        return -1;
    if (g_settings.face_buttons == FACE_NINTENDO)  // by position on every controller, as on a Switch
        return host == SDL_GAMEPAD_BUTTON_EAST ? BIND_A : host == SDL_GAMEPAD_BUTTON_SOUTH ? BIND_B :
               host == SDL_GAMEPAD_BUTTON_NORTH ? BIND_X : BIND_Y;
    if (g_settings.face_buttons == FACE_XBOX) return xbox_place(host);
    // the controller's own labels (SDL knows them by its type, or its mapping's "face:" for look-alikes); a test's
    // events (no controller of their own) and LARECOMP_PAD_TYPE: the type's
    static int pretend = -1;
    if (pretend < 0) pretend = getenv("LARECOMP_PAD_TYPE") != NULL;
    SDL_Gamepad *p = pretend ? NULL : which ? SDL_GetGamepadFromID((SDL_JoystickID)which) : input_gamepad();
    int t = input_pad_type();
    SDL_GamepadButtonLabel l = p ? SDL_GetGamepadButtonLabel(p, (SDL_GamepadButton)host)
                                 : SDL_GetGamepadButtonLabelForType(t < 0 ? SDL_GAMEPAD_TYPE_UNKNOWN : (SDL_GamepadType)t,
                                                                    (SDL_GamepadButton)host);
    switch (l) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return BIND_A;
    case SDL_GAMEPAD_BUTTON_LABEL_B: case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return BIND_B;
    case SDL_GAMEPAD_BUTTON_LABEL_X: case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return BIND_X;
    case SDL_GAMEPAD_BUTTON_LABEL_Y: case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return BIND_Y;
    default: return xbox_place(host);  // (unlabelled: as an Xbox controller, as most PC controllers are)
    }
}

int bind_label_host(int slot) {
    static const int face[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                SDL_GAMEPAD_BUTTON_NORTH};
    for (int i = 0; i < 4; i++)
        if (bind_face_label(0, face[i]) == slot) return face[i];
    return k_def_pad[slot];
}

// Face buttons: Xbox swaps south <-> east and west <-> north, in the defaults and in whatever the player has bound, so
// A is the bottom button in play too (it shoots) and the menus confirm with it.
static int face_swap(int host) {
    switch (host) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return SDL_GAMEPAD_BUTTON_EAST;
    case SDL_GAMEPAD_BUTTON_EAST: return SDL_GAMEPAD_BUTTON_SOUTH;
    case SDL_GAMEPAD_BUTTON_WEST: return SDL_GAMEPAD_BUTTON_NORTH;
    case SDL_GAMEPAD_BUTTON_NORTH: return SDL_GAMEPAD_BUTTON_WEST;
    default: return host;
    }
}

static int16_t def_pad(int slot) {
    return (int16_t)(g_settings.face_buttons == FACE_XBOX ? face_swap(k_def_pad[slot]) : k_def_pad[slot]);
}

void bind_set_face_buttons(int mode) {
    if (mode < 0 || mode >= FACE_COUNT || mode == g_settings.face_buttons) return;
    if ((mode == FACE_XBOX) != (g_settings.face_buttons == FACE_XBOX))
        for (int i = 0; i < BIND_PAD_COUNT; i++) g_bind_pad[i] = (int16_t)face_swap(g_bind_pad[i]);
    g_settings.face_buttons = mode;
    LOG("[input] face buttons: %s\n", g_face_names[mode]);
}

void bind_load_face_buttons(int mode) {
    g_settings.face_buttons = mode >= 0 && mode < FACE_COUNT ? mode : FACE_LABELS;
    for (int i = 0; i < BIND_PAD_COUNT; i++) g_bind_pad[i] = def_pad(i);
}

static const char *const k_ini[BIND_KEY_COUNT] = {
    "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down",
    "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right",
    "beam_power", "beam_wave", "beam_ice", "beam_plasma",
};
static const char *const k_names[BIND_KEY_COUNT] = {
    "A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down", "Left stick Up", "Left stick Down", "Left stick Left",
    "Left stick Right", "Right stick Up", "Right stick Down", "Right stick Left", "Right stick Right",
    "X + D-pad Up", "X + D-pad Right", "X + D-pad Down", "X + D-pad Left",
};
// What the game's Dual Stick scheme does with each (its Options -> Controls -> View Controls page; the game's "Swap
// Beam and Visor" option trades the d-pad's visors for the beams)
static const char *const k_actions[BIND_KEY_COUNT] = {
    "Shoot beam", "Jump", "Beam select (hold)", "Morph Ball", "Show a hint (Cheats tab)", "", "Jump", "Missile", "Lock on / scan (hold)",
    "Shoot beam (hold: charge)", "Map", "Pause menu", "X-Ray Visor", "Combat Visor", "Scan Visor", "Thermal Visor",
    "Move forward", "Move back", "Strafe left", "Strafe right", "Look up", "Look down", "Turn left", "Turn right",
    "Power Beam", "Wave Beam", "Ice Beam", "Plasma Beam",
};
// controller buttons by position (primerecomp.ini)
static const char *const k_pad_ini[PADB_HOST_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2",
    "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "ltrigger", "rtrigger",
};

int bind_slot_from_name(const char *name) {
    for (int i = 0; i < BIND_KEY_COUNT; i++) if (!strcmp(name, k_ini[i])) return i;
    return -1;
}
int bind_pad_from_name(const char *name) {
    for (int i = 0; i < PADB_HOST_COUNT; i++) if (!strcmp(name, k_pad_ini[i])) return i;
    return -1;
}

const char *bind_input_name(int slot) { return slot >= 0 && slot < BIND_KEY_COUNT ? k_names[slot] : "?"; }
const char *bind_input_action(int slot) { return slot >= 0 && slot < BIND_KEY_COUNT ? k_actions[slot] : ""; }

// fam: 0 Nintendo (and pads SDL doesn't name), 1 Xbox, 2 PlayStation
const char *bind_pad_label(int host, int fam) {
    static const char *const nin[PADB_HOST_COUNT] = {
        "B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick press", "Right stick press", "L", "R",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Capture", "Right paddle 1", "Left paddle 1",
        "Right paddle 2", "Left paddle 2", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "ZL", "ZR",
    };
    static const char *const xb[PADB_HOST_COUNT] = {
        "A", "B", "X", "Y", "View", "Xbox", "Menu", "LS (press)", "RS (press)", "LB", "RB",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Share", "Paddle P1", "Paddle P3",
        "Paddle P2", "Paddle P4", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT", "RT",
    };
    static const char *const ps[PADB_HOST_COUNT] = {
        "Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Mute", "Right back button", "Left back button",
        "Right Fn", "Left Fn", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "L2", "R2",
    };
    if (host < 0 || host >= PADB_HOST_COUNT) return "(none)";
    return fam == 1 ? xb[host] : fam == 2 ? ps[host] : nin[host];
}

static const char *const k_mouse_names[BIND_KEY_HOST_COUNT - BIND_MOUSE_LEFT] = {
    "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse 4", "Mouse 5", "Wheel Up", "Wheel Down",
};

const char *bind_key_label(int sc) {
    if (sc >= BIND_MOUSE_LEFT && sc < BIND_KEY_HOST_COUNT) return k_mouse_names[sc - BIND_MOUSE_LEFT];
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return "(none)";
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    return n && *n ? n : "(unnamed key)";
}

int bind_key_combo(int slot, int keys[BIND_COMBO]) {
    if (slot < 0 || slot >= BIND_KEY_COUNT || g_bind_key[slot] <= 0) return 0;
    int n = 0;
    for (int m = 0; m < BIND_MODS; m++)
        if (g_bind_key_mod[slot][m] > 0) keys[n++] = g_bind_key_mod[slot][m];
    keys[n++] = g_bind_key[slot];
    return n;
}

static void combo_name(const int *keys, int n, char *buf, size_t size) {
    size_t at = 0;
    buf[0] = 0;
    for (int i = 0; i < n && at < size; i++)
        at += (size_t)snprintf(buf + at, size - at, "%s%s", i ? " + " : "", bind_key_label(keys[i]));
    if (!n) snprintf(buf, size, "(none)");
}

const char *bind_key_combo_name(int slot, char *buf, size_t size) {
    int k[BIND_COMBO], n = bind_key_combo(slot, k);
    combo_name(k, n, buf, size);
    return buf;
}

// the same keys, in any order
static bool same_keys(const int *a, int na, const int *b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++) {
        bool found = false;
        for (int j = 0; j < nb && !found; j++) found = a[i] == b[j];
        if (!found) return false;
    }
    return true;
}

int bind_key_from_name(const char *name) {
    for (int i = 0; i < BIND_KEY_HOST_COUNT - BIND_MOUSE_LEFT; i++)
        if (!SDL_strcasecmp(name, k_mouse_names[i])) return BIND_MOUSE_LEFT + i;
    return SDL_GetScancodeFromName(name);
}

int bind_label_family(void) {
    extern int input_pad_family(void);  // runtime/hle/input.c (the game's open pad, or LARECOMP_PAD_TYPE)
    int f = input_pad_family();
    if (f) return f;
    int n = 0, t = 0;  // start screen: input.c hasn't opened a pad yet
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) t = SDL_GetGamepadTypeForID(ids[0]);
    SDL_free(ids);
    if (n <= 0 && g_settings.face_buttons == FACE_XBOX) return 1;  // (no controller: the names the chosen layout uses)
    return t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE ? 1 :
           t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? 2 : 0;
}

static void result(int kind, int dev, int slot, int other);

void bind_reset(int dev) {  // (the controller's: those of the face buttons setting)
    if (dev == BIND_DEV_PAD) for (int i = 0; i < BIND_PAD_COUNT; i++) g_bind_pad[i] = def_pad(i);
    else memcpy(g_bind_key, k_def_key, sizeof g_bind_key), memset(g_bind_key_mod, 0, sizeof g_bind_key_mod);
    LOG("[input] %s bindings reset to the defaults\n", dev == BIND_DEV_PAD ? "controller" : "keyboard");
    result(5, dev, -1, -1);
}

bool bind_is_default_slot(int dev, int slot) {
    if (dev == BIND_DEV_PAD) return g_bind_pad[slot] == def_pad(slot);
    for (int m = 0; m < BIND_MODS; m++)
        if (g_bind_key_mod[slot][m]) return false;
    return g_bind_key[slot] == k_def_key[slot];
}

bool bind_is_default(int dev) {
    for (int i = 0; i < (dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT); i++)
        if (!bind_is_default_slot(dev, i)) return false;
    return true;
}


// ------------------------------------------------------------------ capture
static _Atomic int cap_dev = -1, cap_slot = -1;
static _Atomic uint64_t cap_deadline;
static _Atomic int res_kind, res_dev, res_slot, res_other, res_serial;
// A keyboard capture's keys (cap_lock: the events' thread adds them, the UI's ends the countdown): held now, in the
// order pressed, and the most held at once since they were last all up
static SDL_SpinLock cap_lock;
static int cap_held[BIND_COMBO], cap_nheld, cap_best[BIND_COMBO], cap_nbest;

static void result(int kind, int dev, int slot, int other) {
    res_kind = kind, res_dev = dev, res_slot = slot, res_other = other;
    res_serial++;
}

void bind_capture_begin(int dev, int slot) {
    if (slot < 0 || slot >= (dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT)) return;
    SDL_LockSpinlock(&cap_lock);
    cap_nheld = cap_nbest = 0;
    SDL_UnlockSpinlock(&cap_lock);
    cap_deadline = SDL_GetTicks() + BIND_CAPTURE_MS;
    cap_slot = slot;
    cap_dev = dev;
}

void bind_capture_cancel(void) {
    int d = cap_dev;
    if (d < 0 || !atomic_compare_exchange_strong(&cap_dev, &d, -1)) return;
    result(3, d, cap_slot, -1);
}

static void cap_press(int key) {  // (cap_lock held)
    for (int i = 0; i < cap_nheld; i++)
        if (cap_held[i] == key) return;
    if (!cap_nheld) cap_nbest = 0;  // (a new press: the last one's keys are forgotten)
    if (cap_nheld == BIND_COMBO) return;  // (more than a combo takes: the first ones)
    cap_held[cap_nheld++] = key;
    if (cap_nheld >= cap_nbest) memcpy(cap_best, cap_held, sizeof cap_best), cap_nbest = cap_nheld;
}

static void cap_release(int key) {  // (cap_lock held)
    for (int i = 0; i < cap_nheld; i++)
        if (cap_held[i] == key) {
            memmove(cap_held + i, cap_held + i + 1, (size_t)(cap_nheld - i - 1) * sizeof *cap_held);
            cap_nheld--;
            return;
        }
}

int bind_capture_keys(int keys[BIND_COMBO]) {
    SDL_LockSpinlock(&cap_lock);
    int n = cap_nheld ? cap_nheld : cap_nbest;
    memcpy(keys, cap_nheld ? cap_held : cap_best, sizeof(int) * BIND_COMBO);
    SDL_UnlockSpinlock(&cap_lock);
    return n;
}

static void assign_keys(int slot, const int *keys, int n);

// A keyboard capture's end (its countdown, or a wheel notch): the keys held, or the last held together, become the
// binding; none pressed - nothing changes
static void key_capture_finish(void) {
    int d = BIND_DEV_KEY;
    if (!atomic_compare_exchange_strong(&cap_dev, &d, -1)) return;  // (the other thread ended it)
    int k[BIND_COMBO], n = bind_capture_keys(k);
    if (n) assign_keys(cap_slot, k, n);
    else result(4, BIND_DEV_KEY, cap_slot, -1);
}

bool bind_capture_active(int *dev, int *slot, int *ms_left) {
    int d = cap_dev;
    if (d >= 0 && SDL_GetTicks() >= cap_deadline) {
        if (d == BIND_DEV_KEY) key_capture_finish();
        else if (atomic_compare_exchange_strong(&cap_dev, &d, -1)) result(4, d, cap_slot, -1);
        d = cap_dev;
    }
    if (dev) *dev = d;
    if (slot) *slot = d >= 0 ? cap_slot : -1;
    if (ms_left) *ms_left = d >= 0 ? (int)(cap_deadline - SDL_GetTicks()) : 0;
    return d >= 0;
}

int bind_last_result(int *dev, int *slot, int *other, int *serial) {
    if (dev) *dev = res_dev;
    if (slot) *slot = res_slot;
    if (other) *other = res_other;
    if (serial) *serial = res_serial;
    return res_kind;
}

// Bind `host` to `slot`; an input already on `host` takes the slot's old binding (swap).
static void assign(int dev, int slot, int host) {
    int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, other = -1;
    for (int i = 0; i < n; i++)
        if (i != slot && t[i] == host) { other = i; t[i] = t[slot]; }
    t[slot] = (int16_t)host;
    LOG("[input] %s %s -> %s%s%s%s\n", dev == BIND_DEV_PAD ? "controller" : "keyboard", k_names[slot],
        dev == BIND_DEV_PAD ? k_pad_ini[host] : bind_key_label(host), other >= 0 ? " (swapped with " : "",
        other >= 0 ? k_names[other] : "", other >= 0 ? ")" : "");
    if (other >= 0) LOG("[input]   %s -> %s\n", k_names[other], dev == BIND_DEV_PAD ? k_pad_ini[t[other]] : bind_key_label(t[other]));
    result(other >= 0 ? 2 : 1, dev, slot, other);
}

static void set_keys(int slot, const int *keys, int n) {  // (the held ones, then the one that completes the combo)
    memset(g_bind_key_mod[slot], 0, sizeof g_bind_key_mod[slot]);
    for (int i = 0; i + 1 < n && i < BIND_MODS; i++) g_bind_key_mod[slot][i] = (int16_t)keys[i];
    g_bind_key[slot] = (int16_t)(n ? keys[n - 1] : 0);
}

// Bind keys to keyboard `slot`; an input already on the same keys (in any order) takes the slot's old ones (swap).
// A combo sharing some of them (1, and E + 1) is no conflict: that's what combos are for.
static void assign_keys(int slot, const int *keys, int n) {
    int old[BIND_COMBO], nold = bind_key_combo(slot, old), other = -1;
    for (int i = 0; i < BIND_KEY_COUNT && other < 0 && n; i++) {
        int k[BIND_COMBO], nk = bind_key_combo(i, k);
        if (i != slot && same_keys(k, nk, keys, n)) other = i;
    }
    if (other >= 0) set_keys(other, old, nold);
    set_keys(slot, keys, n);
    char a[96], b[96];
    LOG("[input] keyboard %s -> %s%s%s%s\n", k_names[slot], bind_key_combo_name(slot, a, sizeof a),
        other >= 0 ? " (swapped with " : "", other >= 0 ? k_names[other] : "", other >= 0 ? ")" : "");
    if (other >= 0) LOG("[input]   %s -> %s\n", k_names[other], bind_key_combo_name(other, b, sizeof b));
    result(other >= 0 ? 2 : 1, BIND_DEV_KEY, slot, other);
}

void bind_reset_slot(int dev, int slot) {
    if (slot < 0 || slot >= (dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT) || bind_is_default_slot(dev, slot)) return;
    if (dev == BIND_DEV_PAD) { assign(dev, slot, def_pad(slot)); return; }
    int k = k_def_key[slot];
    if (k) { assign_keys(slot, &k, 1); return; }
    set_keys(slot, NULL, 0);  // (no key by default)
    LOG("[input] keyboard %s -> (none)\n", k_names[slot]);
    result(1, BIND_DEV_KEY, slot, -1);
}

bool bind_capture_event(const SDL_Event *e) {
    int dev = cap_dev;
    if (dev < 0) return false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) return true;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { bind_capture_cancel(); return true; }
        // F1 (menu), F8 (position note), F9 (frame dump), F11 (fullscreen) stay the overlay's; a controller capture
        // ignores keys
        if (dev != BIND_DEV_KEY || e->key.scancode == SDL_SCANCODE_F1 || e->key.scancode == SDL_SCANCODE_F8 ||
            e->key.scancode == SDL_SCANCODE_F9 ||
            e->key.scancode == SDL_SCANCODE_F11 || e->key.scancode >= SDL_SCANCODE_COUNT) return true;
        SDL_LockSpinlock(&cap_lock);
        cap_press(e->key.scancode);  // (the countdown's end takes them: bind_capture_active)
        SDL_UnlockSpinlock(&cap_lock);
        return true;
    case SDL_EVENT_KEY_UP:
        if (dev == BIND_DEV_KEY && e->key.scancode < SDL_SCANCODE_COUNT) {
            SDL_LockSpinlock(&cap_lock);
            cap_release(e->key.scancode);
            SDL_UnlockSpinlock(&cap_lock);
        }
        return true;
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Home / Guide opens the menu; a keyboard capture ignores the controller (it times out, or Esc)
        if (dev != BIND_DEV_PAD || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e->gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT) return true;
        cap_dev = -1;
        assign(dev, cap_slot, e->gbutton.button);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        // the keyboard column takes mouse buttons as keys (the click that began the capture came before)
        if (dev != BIND_DEV_KEY || e->button.button < 1 || e->button.button > 5) return true;
        SDL_LockSpinlock(&cap_lock);
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) cap_press(BIND_MOUSE_LEFT + e->button.button - 1);
        else cap_release(BIND_MOUSE_LEFT + e->button.button - 1);
        SDL_UnlockSpinlock(&cap_lock);
        return true;
    case SDL_EVENT_MOUSE_WHEEL:  // and the wheel: a notch is pressed and let go at once, so it ends the capture
        if (dev != BIND_DEV_KEY || e->wheel.y == 0) return true;
        SDL_LockSpinlock(&cap_lock);
        cap_press(e->wheel.y > 0 ? BIND_WHEEL_UP : BIND_WHEEL_DOWN);
        SDL_UnlockSpinlock(&cap_lock);
        key_capture_finish();
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (dev == BIND_DEV_PAD && e->gaxis.value > 16000 &&
            (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            cap_dev = -1;
            assign(dev, cap_slot, e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LTRIGGER : PADB_RTRIGGER);
        }
        return true;  // (sticks: no menu movement while waiting)
    default:
        return false;
    }
}

// ------------------------------------------------------------------ primerecomp.ini
// "bind_keys = fps" comes before the bind_key_* lines: keyboard bindings saved before the shooter layout (Link's
// Awakening's K/J/L/I era) aren't loaded - those players start on the new defaults.
static bool keys_fps;

bool bind_settings_load(const char *key, const char *val) {
    if (!strcmp(key, "bind_keys")) {  // (the keyboard's lines follow: the defaults, then those)
        keys_fps = !strcmp(val, "fps");
        if (keys_fps) memcpy(g_bind_key, k_def_key, sizeof g_bind_key), memset(g_bind_key_mod, 0, sizeof g_bind_key_mod);
        return true;
    }
    int mod = !strncmp(key, "bind_keymod1_", 13) ? 1 : !strncmp(key, "bind_keymod2_", 13) ? 2 : 0;
    if (mod) {  // a combo's other keys (BIND_MODS)
        int i = keys_fps ? bind_slot_from_name(key + 13) : -1;
        if (i >= 0) g_bind_key_mod[i][mod - 1] = (int16_t)(strcmp(val, "none") ? bind_key_from_name(val) : 0);
        return true;
    }
    bool pad = !strncmp(key, "bind_pad_", 9), kb = !strncmp(key, "bind_key_", 9);
    if (!pad && !kb) return false;
    if (kb && !keys_fps) {
        static int logged;
        if (!logged++) LOG("[input] keyboard bindings from before the shooter layout: the new defaults instead\n");
        return true;
    }
    for (int i = 0; i < (pad ? BIND_PAD_COUNT : BIND_KEY_COUNT); i++) {
        if (strcmp(key + 9, k_ini[i])) continue;
        if (pad) {
            for (int h = 0; h < PADB_HOST_COUNT; h++)
                if (!strcmp(val, k_pad_ini[h])) g_bind_pad[i] = (int16_t)h;
        } else {
            int sc = strcmp(val, "none") ? bind_key_from_name(val) : 0;
            if (sc || !strcmp(val, "none")) g_bind_key[i] = (int16_t)sc;
        }
    }
    return true;
}

void bind_settings_save(void (*put)(void *ctx, const char *fmt, ...), void *ctx) {
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] != def_pad(i) && g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT)
            put(ctx, "bind_pad_%s = %s\n", k_ini[i], k_pad_ini[g_bind_pad[i]]);
    put(ctx, "bind_keys = fps\n");
    for (int i = 0; i < BIND_KEY_COUNT; i++) {
        if (bind_is_default_slot(BIND_DEV_KEY, i)) continue;
        put(ctx, "bind_key_%s = %s\n", k_ini[i], g_bind_key[i] ? bind_key_label(g_bind_key[i]) : "none");
        for (int m = 0; m < BIND_MODS; m++)
            if (g_bind_key_mod[i][m]) put(ctx, "bind_keymod%d_%s = %s\n", m + 1, k_ini[i], bind_key_label(g_bind_key_mod[i][m]));
    }
}

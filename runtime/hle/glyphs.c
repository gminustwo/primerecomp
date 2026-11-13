// Button prompt icons for the controller in use (the Controls tab's "Button icons"). The game draws its prompts with
// the Switch's buttons: 36 x 36 BC7 images (a blue set in play - the pause screens, the map, the HUD -, a gold one on
// the start screen and for highlighted prompts, a red one in the front end's menus), and 128 x 36 ones with two
// buttons ("X + d-pad right", "ZR / A": the HUD's hints that name two - k_wide), placed in its text and UI. As
// one is uploaded, runtime/nvn/vulkan/textures.c asks glyph_match whether it's one of them (by its contents: the
// table below, from the game's own files), and glyph_redraw has it drawn again (runtime/nvn/glyph_art.c) with what
// the player presses for that Switch button - through their bindings (runtime/hle/bindings.c): an Xbox controller's
// button, a PlayStation controller's, a Switch controller's (the game's own icon unless rebound), the key or mouse
// button, or the Wii Remote's (runtime/hle/input.c's layout for it). In the game's menus (not in play) A / B / X / Y
// are the face buttons labelled so (Face buttons: Labels), and Esc is B - as input.c takes them there. Auto follows whatever was used last.
// Nothing of the game's is changed: only the images it uploads.
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../glyphs.h"
#include "../hle.h"
#include "../input_bind.h"
#include "../nvn/glyph_art.h"
#include "../paths.h"
#include "../settings.h"

// The game's prompt icons: FNV-1a (64-bit) of level 0 as linear BC7 blocks (1296 bytes), the icon's shape, the
// Switch input it shows (BIND_*: a stick's own entry for its stick), and for the four dots - the d-pad, drawn as a
// Joy-Con's four buttons (the visors: "Use [up] or ZR / A to return to the Combat Visor"; issue #32, they were taken
// for the face buttons, so keys showed X for d-pad up and Enter for right) - the one lit (-1: none, the d-pad as a
// whole), for a stick whether it's the press (1). From the game's paks (GuiSysMP1, PreloadFrontEndMPT, UI_FrontEnd,
// DuplicateData). Its other 36 x 36 images (the map's legend, arrows, the stick-rotate icon, a Joy-Con's motion)
// aren't buttons: every 36 x 36 BC7 image in all 340 paks was checked (2026-11-12).
typedef struct { uint64_t hash; uint8_t shape; int8_t input; int8_t extra; } GameIcon;
static const GameIcon k_icons[] = {
    {0x00cd94c75c5e7bc1ull, GA_TRIGGER, BIND_ZR, 0},  // red
    {0x07611242e899b58full, GA_DOTS, BIND_DOWN, GD_DOWN},  // red
    {0x0831a6decb6409dbull, GA_STICK, BIND_RSTICK, 0},  // blue
    {0x09f2ee2416abe8bcull, GA_STICK, BIND_RSTICK, 0},  // blue
    {0x100ee24bbaac7ba7ull, GA_CIRCLE, BIND_PLUS, 0},  // gold
    {0x11c8332ce0e431b5ull, GA_STICK, BIND_LSTICK, 0},  // red
    {0x173a17950258f8d2ull, GA_CIRCLE, BIND_A, 0},  // red
    {0x17dd7cdb95a11d6eull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0x19b57a235fc2c475ull, GA_RECT, BIND_R, 0},  // gold
    {0x26c45825c650650dull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0x28f5f058152df467ull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0x294dc8d0022185caull, GA_DOTS, BIND_UP, -1},  // red (the d-pad: no dot lit)
    {0x347bb64c1419fbb5ull, GA_RECT, BIND_L, 0},  // red
    {0x34a416909a9fd66aull, GA_STICK, BIND_LSTICK, 0},  // red
    {0x36d78d439e9374c3ull, GA_CIRCLE, BIND_MINUS, 0},  // gold
    {0x3d3c04f4c075e30full, GA_CIRCLE, BIND_B, 0},  // gold
    {0x3d92c915b782b3ecull, GA_TRIGGER, BIND_ZL, 0},  // gold
    {0x3e2dae0dbde0bf64ull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0x417a8036c1355aabull, GA_DOTS, BIND_LEFT, GD_LEFT},  // red
    {0x44026fdd80a4796dull, GA_STICK, BIND_LSTICK, 0},  // red
    {0x443f50443fc20f3bull, GA_CIRCLE, BIND_A, 0},  // blue
    {0x46367daae070346cull, GA_STICK, BIND_RSTICK, 0},  // red
    {0x46e1027d9e278107ull, GA_STICK, BIND_RSTICK, 0},  // blue
    {0x4a097e2e28720a99ull, GA_CIRCLE, BIND_X, 0},  // red
    {0x4c78e77035db04faull, GA_STICK, BIND_LSTICK, 0},  // red
    {0x4d3a8ac05ec5c7d0ull, GA_CIRCLE, BIND_MINUS, 0},  // gold
    {0x4fae82d2c180214full, GA_DOTS, BIND_RIGHT, GD_RIGHT},  // blue
    {0x529423f3dcb45d36ull, GA_DOTS, BIND_LEFT, GD_LEFT},  // blue
    {0x55050f284c260bd8ull, GA_DOTS, BIND_DOWN, GD_DOWN},  // blue
    {0x5b696a7b1b6a66c4ull, GA_STICK, BIND_RSTICK, 0},  // red
    {0x5d805ef9d2841e9eull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0x5e97df6473cf1114ull, GA_STICK, BIND_LSTICK, 0},  // red
    {0x62564d472a9d68d2ull, GA_DOTS, BIND_UP, GD_UP},  // red
    {0x63b80b88962e7a7dull, GA_DOTS, BIND_RIGHT, GD_RIGHT},  // red
    {0x683aa4b551a67980ull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0x689d4a34210cf50bull, GA_STICK, BIND_RSTICK, 1},  // blue
    {0x68d0ddc5e4f8981cull, GA_STICK, BIND_RSTICK, 0},  // blue
    {0x68f455f982de82d0ull, GA_CIRCLE, BIND_B, 0},  // blue
    {0x739ae500ebc55838ull, GA_CIRCLE, BIND_A, 0},  // gold
    {0x75ccd177da93c4a0ull, GA_RECT, BIND_L, 0},  // gold
    {0x76a0559ad91850a6ull, GA_CIRCLE, BIND_X, 0},  // gold
    {0x78294360a3ee0bd3ull, GA_CIRCLE, BIND_PLUS, 0},  // red
    {0x81920650deeb2439ull, GA_RECT, BIND_R, 0},  // red
    {0x8413773693587dc1ull, GA_CIRCLE, BIND_MINUS, 0},  // blue
    {0x8535febb320db893ull, GA_STICK, BIND_RSTICK, 0},  // red
    {0x8a28bf4573d37d9cull, GA_CIRCLE, BIND_PLUS, 0},  // gold
    {0x8c60df67fc2c169full, GA_TRIGGER, BIND_ZL, 0},  // blue
    {0x8ce314f5e4f51fe5ull, GA_STICK, BIND_RSTICK, 0},  // red
    {0x8dd431a904d7ad72ull, GA_STICK, BIND_RSTICK, 0},  // red
    {0x9011bcc20657eecbull, GA_RECT, BIND_R, 0},  // gold
    {0x978c812108e288bfull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0x97af6ae8086e374dull, GA_STICK, BIND_RSTICK, 0},  // red
    {0x9988890bb9153d4aull, GA_STICK, BIND_RSTICK, 0},  // red
    {0x99a1ec5be55aac6dull, GA_RECT, BIND_L, 0},  // gold
    {0x9a25d45a80ae11a3ull, GA_STICK, BIND_RSTICK, 0},  // blue
    {0x9a59c3ae509603efull, GA_CIRCLE, BIND_B, 0},  // gold
    {0x9cc9567a62f6a00eull, GA_TRIGGER, BIND_ZR, 0},  // gold
    {0x9e7b3a90e3226745ull, GA_CIRCLE, BIND_X, 0},  // blue
    {0xa20390fea6f59570ull, GA_STICK, BIND_LSTICK, 0},  // red
    {0xa4ea4c612e42af6full, GA_RECT, BIND_L, 0},  // blue
    {0xa8f2ca0f58598752ull, GA_DOTS, BIND_UP, -1},  // blue (the d-pad: no dot lit)
    {0xabfae820dbd8d1e9ull, GA_STICK, BIND_RSTICK, 0},  // blue
    {0xac9b77fafd718b08ull, GA_RECT, BIND_R, 0},  // blue
    {0xaf602390ccc62f29ull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0xb5c778ad809ad069ull, GA_STICK, BIND_LSTICK, 0},  // red
    {0xbc7861f7881f45a3ull, GA_STICK, BIND_RSTICK, 0},  // blue
    {0xbf9092afded3cb79ull, GA_TRIGGER, BIND_ZL, 0},  // red
    {0xc122468b5b65b56dull, GA_TRIGGER, BIND_ZR, 0},  // blue
    {0xc811ed98cd0b73c0ull, GA_STICK, BIND_RSTICK, 0},  // red
    {0xc9165ffc054d694dull, GA_STICK, BIND_RSTICK, 1},  // red
    {0xd6b65b5d19f10756ull, GA_STICK, BIND_LSTICK, 0},  // blue
    {0xde979200a0830396ull, GA_TRIGGER, BIND_ZL, 0},  // gold
    {0xe3781f074067f067ull, GA_CIRCLE, BIND_A, 0},  // gold
    {0xe7feb0e2aaf0c2d0ull, GA_CIRCLE, BIND_PLUS, 0},  // blue
    {0xe9cc0f5563dfe35dull, GA_CIRCLE, BIND_B, 0},  // red
    {0xea261c1e13dfd631ull, GA_CIRCLE, BIND_Y, 0},  // blue
    {0xefeb938a6eae4566ull, GA_DOTS, BIND_UP, GD_UP},  // blue
    {0xf22c90a21a406f15ull, GA_STICK, BIND_RSTICK, 0},  // blue
    {0xf3c490cc45ca69d4ull, GA_STICK, BIND_LSTICK, 0},  // red
    {0xfa88849fde76f4b1ull, GA_STICK, BIND_LSTICK, 0},  // red
    {0xfa94a30572a242bbull, GA_STICK, BIND_RSTICK, 0},  // red
    {0xfc3867fe048b55c0ull, GA_CIRCLE, BIND_Y, 0},  // red
    {0xfc96cae11b55cb3cull, GA_TRIGGER, BIND_ZR, 0},  // gold
    {0xffcef528274f1511ull, GA_CIRCLE, BIND_MINUS, 0},  // red
    {0xffee2ce306eae5e1ull, GA_STICK, BIND_RSTICK, 0},  // blue
};
#define N_ICONS ((int)(sizeof k_icons / sizeof *k_icons))

// Two buttons in one image, 128 x 36 BC7 (4608 bytes as linear blocks): the HUD hints that name two - "Use [X +
// d-pad right] to switch to the Scan Visor", "Use the Beam [ZR / A] to open doors" kept the Switch's buttons on keys
// and every controller (user 2026-11-12: only the 36 x 36 icons were known). The left button lies in the 36 x 36 at
// x 8 (block columns 2-10: its pixels 10-43), the right one in the 36 x 36 at x 84 (columns 21-29: pixels 84-117), a
// '+' or '/' between them (51-75). All 18 such
// images in the game's paks (UIMP1, GuiSysMP1; input -1: a Joy-Con's motion, no button - left as it is). Ids after
// the icons' (N_ICONS + i).
typedef struct { uint64_t hash; GameIcon l, r; } WideIcon;
static const WideIcon k_wide[] = {
    {0x2b0564224dc0a9afull, {0, GA_RECT, BIND_R, 0}, {0, GA_DOTS, BIND_UP, -1}},              // R + d-pad
    {0x0dcc7b4c7a07aa39ull, {0, GA_RECT, BIND_R, 0}, {0, GA_TRIGGER, BIND_ZR, 0}},            // R / ZR
    {0x67d2ae987d72b9e1ull, {0, GA_CIRCLE, BIND_X, 0}, {0, GA_DOTS, BIND_RIGHT, GD_RIGHT}},   // X + right
    {0xea18cf4c3a38435eull, {0, GA_CIRCLE, BIND_X, 0}, {0, GA_DOTS, BIND_LEFT, GD_LEFT}},     // X + left
    {0xe3031c5c564d6560ull, {0, GA_CIRCLE, BIND_X, 0}, {0, GA_DOTS, BIND_DOWN, GD_DOWN}},     // X + down
    {0x476c89033a9cec3bull, {0, GA_RECT, BIND_R, 0}, {0, GA_DOTS, BIND_RIGHT, GD_RIGHT}},     // R + right
    {0x1f75531e44023390ull, {0, GA_RECT, BIND_R, 0}, {0, GA_DOTS, BIND_UP, GD_UP}},           // R + up
    {0x1df27279bf4ad0cdull, {0, GA_CIRCLE, BIND_X, 0}, {0, GA_DOTS, BIND_UP, -1}},            // X + d-pad
    {0x5d34bc4238162cf7ull, {0, GA_CIRCLE, BIND_PLUS, 0}, {0, GA_RECT, BIND_R, 0}},           // Plus / R
    {0x5e108d030dea71deull, {0, GA_RECT, BIND_R, 0}, {0, GA_DOTS, BIND_DOWN, GD_DOWN}},       // R + down
    {0x8d843583d7c92c9aull, {0, GA_CIRCLE, BIND_X, 0}, {0, GA_DOTS, BIND_UP, GD_UP}},         // X + up
    {0x50d9e53d1cb406b0ull, {0, GA_RECT, BIND_R, 0}, {0, GA_DOTS, BIND_LEFT, GD_LEFT}},       // R + left
    {0xcdccb6e98703d822ull, {0, GA_CIRCLE, BIND_PLUS, 0}, {0, GA_TRIGGER, BIND_ZR, 0}},       // Plus / ZR
    {0xfbe329e555528a72ull, {0, GA_RECT, BIND_L, 0}, {0, GA_RECT, BIND_R, 0}},                // L + R
    {0x3c80068583bfadc1ull, {0, GA_CIRCLE, -1, 0}, {0, GA_CIRCLE, BIND_X, 0}},                // motion / X
    {0xe8da0744379a5ca4ull, {0, GA_TRIGGER, BIND_ZR, 0}, {0, GA_CIRCLE, BIND_A, 0}},          // ZR / A
    {0xfea00ff7e0008bfaull, {0, GA_RECT, BIND_L, 0}, {0, GA_CIRCLE, BIND_B, 0}},              // L / B
    {0x25a2fb7ae4114298ull, {0, GA_RECT, BIND_L, 0}, {0, GA_TRIGGER, BIND_ZL, 0}},            // L / ZL
};
#define N_WIDE ((int)(sizeof k_wide / sizeof *k_wide))

const char *const g_icons_names[ICONS_COUNT] = {"auto", "switch", "xbox", "playstation", "keyboard", "wii"};
static const char *const k_set_names[ICONS_COUNT] = {"", "Switch", "Xbox", "PlayStation", "keyboard", "Wii Remote"};

static int log_on(void) {  // LARECOMP_LOG_GLYPHS=1: the icons found and what they became
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_LOG_GLYPHS") ? 1 : 0;
    return on;
}

// ------------------------------------------------------------------ which set, by what was used last
static _Atomic int g_dev = -1;            // GLYPH_DEV_*, -1: nothing used yet
static _Atomic uint32_t g_gen = 1;
static _Atomic int g_set = ICONS_SWITCH;  // the set shown (Auto resolved)

void glyphs_note_device(int dev) { g_dev = dev; }
int glyphs_last_device(void) { return g_dev; }

// A controller's set: its family's (bindings.c: Xbox, PlayStation); of the rest, a Switch controller keeps the
// game's own, and an unnamed one (most PC pads) is shown as an Xbox controller
static int pad_set(void) {
    extern int input_pad_family(void), input_pad_type(void);  // runtime/hle/input.c
    int f = input_pad_family(), t = input_pad_type();
    if (f == 1) return ICONS_XBOX;
    if (f == 2) return ICONS_PLAYSTATION;
    if (t < 0) return ICONS_SWITCH;  // (LARECOMP_PAD_TYPE=nintendo, or none)
    return t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
           t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
               ? ICONS_SWITCH : ICONS_XBOX;
}

static int resolve_set(void) {
    int s = g_settings.button_icons;
    if (s > ICONS_AUTO && s < ICONS_COUNT) return s;
    extern int input_pad_type(void);
    int d = g_dev;
    if (d < 0) d = input_pad_type() >= 0 ? GLYPH_DEV_PAD : GLYPH_DEV_KEYS;  // (nothing pressed yet: a controller if one is there)
    return d == GLYPH_DEV_KEYS ? ICONS_KEYBOARD : d == GLYPH_DEV_WII ? ICONS_WII : pad_set();
}

int glyphs_current(void) { return g_set; }
uint32_t glyph_generation(void) { return g_gen; }

// What each Switch button is pressed with now (glyphs_frame), on a controller and with keys: its binding - but in
// the game's menus (game_in_play false: runtime/hle/input.c's menu face buttons and menu keys) A / B / X / Y are the
// controller's face buttons labelled so (bind_label_host; Face buttons: Labels), and a button a menu key means shows
// that key (Enter A, Esc B, Delete Minus, Q / E the pause screen's pages - L / R -, the arrows the d-pad: issue #32, Shift / F were
// shown for the pages); of the rest, one whose key means another button there (Space: A) shows none (the game's own).
// The game's paused messages (an item's "acquired" text: game_message_screen) count as play: their text says what
// to press in play ("Press [Y] to activate Morph Ball mode" showed the button labelled Y - a PlayStation controller's
// triangle - where morphing is the left button, square; issue #52). input.c lets the A-labelled button confirm there too.
static int16_t g_pad_now[BIND_PAD_COUNT], g_key_now[BIND_PAD_COUNT], g_mod_now[BIND_PAD_COUNT][BIND_MODS];
static _Atomic bool g_now_set;  // (before the first glyphs_frame an icon stays the game's; that frame redraws it)

void glyphs_frame(void) {  // what the icons depend on: the set, the bindings, menu or play; changed -> redraw
    extern bool game_in_play(void), game_message_screen(void);  // runtime/hle/game.c
    extern int input_menu_key(int sc);  // runtime/hle/input.c
    static uint64_t sig;
    int set = resolve_set();
    bool menu = !game_in_play() && !game_message_screen();
    static int menu_for[BIND_PAD_COUNT];  // the first menu key meaning each button (Enter before Space: their scancodes)
    static bool menu_for_set;
    if (!menu_for_set) {
        menu_for_set = true;
        for (int sc = SDL_SCANCODE_COUNT - 1; sc > 0; sc--)
            if (input_menu_key(sc) >= 0 && input_menu_key(sc) < BIND_PAD_COUNT) menu_for[input_menu_key(sc)] = sc;
    }
    int16_t pad_now[BIND_PAD_COUNT], key_now[BIND_PAD_COUNT], mod_now[BIND_PAD_COUNT][BIND_MODS] = {{0}};
    for (int i = 0; i < BIND_PAD_COUNT; i++) {
        pad_now[i] = menu && g_settings.face_buttons == FACE_LABELS && i <= BIND_Y ? (int16_t)bind_label_host(i)
                                                                                    : g_bind_pad[i];
        int k = g_bind_key[i];
        if (menu && menu_for[i]) k = menu_for[i];
        else if (menu && input_menu_key(k) >= 0) k = 0;  // (none: the game's own icon)
        else memcpy(mod_now[i], g_bind_key_mod[i], sizeof mod_now[i]);  // (a combo: E + 1)
        key_now[i] = (int16_t)k;
    }
    uint64_t h = 0xcbf29ce484222325ull;
#define MIX(v) (h = (h ^ (uint64_t)(v)) * 0x100000001b3ull)
    MIX(set);
    for (int i = 0; i < BIND_PAD_COUNT; i++) MIX(pad_now[i]), MIX(key_now[i]), MIX(mod_now[i][0]), MIX(mod_now[i][1]);
    for (int i = BIND_PAD_COUNT; i < BIND_KEY_COUNT; i++) MIX(g_bind_key[i]), MIX(g_bind_key_mod[i][0]), MIX(g_bind_key_mod[i][1]);
#undef MIX
    static int logged = -1;  // (the play log says which set is shown, as it changes: bug reports)
    if (set != logged) {
        LOG("[glyphs] button icons: %s (setting %s, last used %s)\n", k_set_names[set],
            g_icons_names[g_settings.button_icons >= 0 && g_settings.button_icons < ICONS_COUNT ? g_settings.button_icons : 0],
            g_dev == GLYPH_DEV_KEYS ? "keys" : g_dev == GLYPH_DEV_WII ? "Wii Remote" : g_dev == GLYPH_DEV_PAD ? "controller" : "nothing");
        logged = set;
    }
    if (h != sig) {
        if (sig && log_on()) LOG("[glyphs] button icons: %s%s\n", k_set_names[set], menu ? " (menus)" : "");
        sig = h;
        g_set = set;
        memcpy(g_pad_now, pad_now, sizeof pad_now);
        memcpy(g_key_now, key_now, sizeof key_now);
        memcpy(g_mod_now, mod_now, sizeof mod_now);
        g_now_set = true;
        g_gen++;
    }
}

// ------------------------------------------------------------------ what to draw
static void text(GaSpec *s, const char *t) { s->sym = GS_TEXT; snprintf(s->text, sizeof s->text, "%s", t); }

// a controller button (an SDL_GamepadButton or a trigger) in a set's own marks
static void pad_symbol(GaSpec *s, int host, int set) {
    switch (host) {
    case SDL_GAMEPAD_BUTTON_SOUTH: if (set == ICONS_PLAYSTATION) s->sym = GS_CROSS; else text(s, set == ICONS_XBOX ? "A" : "B"); return;
    case SDL_GAMEPAD_BUTTON_EAST: if (set == ICONS_PLAYSTATION) s->sym = GS_RING; else text(s, set == ICONS_XBOX ? "B" : "A"); return;
    case SDL_GAMEPAD_BUTTON_WEST: if (set == ICONS_PLAYSTATION) s->sym = GS_SQUARE; else text(s, set == ICONS_XBOX ? "X" : "Y"); return;
    case SDL_GAMEPAD_BUTTON_NORTH: if (set == ICONS_PLAYSTATION) s->sym = GS_TRIANGLE; else text(s, set == ICONS_XBOX ? "Y" : "X"); return;
    case SDL_GAMEPAD_BUTTON_BACK: if (set == ICONS_SWITCH) text(s, "-"); else s->sym = set == ICONS_XBOX ? GS_VIEW : GS_SHARE; return;
    case SDL_GAMEPAD_BUTTON_START: if (set == ICONS_SWITCH) text(s, "+"); else s->sym = GS_MENU; return;
    case SDL_GAMEPAD_BUTTON_GUIDE: text(s, set == ICONS_PLAYSTATION ? "PS" : "Home"); return;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: text(s, set == ICONS_PLAYSTATION ? "L3" : "LS"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: text(s, set == ICONS_PLAYSTATION ? "R3" : "RS"); return;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: text(s, set == ICONS_PLAYSTATION ? "L1" : set == ICONS_XBOX ? "LB" : "L"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: text(s, set == ICONS_PLAYSTATION ? "R1" : set == ICONS_XBOX ? "RB" : "R"); return;
    case PADB_LTRIGGER: text(s, set == ICONS_PLAYSTATION ? "L2" : set == ICONS_XBOX ? "LT" : "ZL"); return;
    case PADB_RTRIGGER: text(s, set == ICONS_PLAYSTATION ? "R2" : set == ICONS_XBOX ? "RT" : "ZR"); return;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: s->sym = GS_DPAD, s->dir = GD_UP; return;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: s->sym = GS_DPAD, s->dir = GD_DOWN; return;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: s->sym = GS_DPAD, s->dir = GD_LEFT; return;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: s->sym = GS_DPAD, s->dir = GD_RIGHT; return;
    case SDL_GAMEPAD_BUTTON_MISC1: text(s, set == ICONS_PLAYSTATION ? "Mic" : set == ICONS_XBOX ? "Shr" : "Cap"); return;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: text(s, "Pad"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: text(s, "P1"); return;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: text(s, "P3"); return;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: text(s, "P2"); return;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: text(s, "P4"); return;
    default: text(s, "?"); return;
    }
}

// a key's label as short as a key's top has room for
static void key_label(char *out, size_t n, int sc) {
    static const struct { int sc; const char *t; } names[] = {
        {SDL_SCANCODE_RETURN, "Enter"}, {SDL_SCANCODE_SPACE, "Space"}, {SDL_SCANCODE_ESCAPE, "Esc"},
        {SDL_SCANCODE_TAB, "Tab"}, {SDL_SCANCODE_BACKSPACE, "Bksp"}, {SDL_SCANCODE_LSHIFT, "Shift"},
        {SDL_SCANCODE_RSHIFT, "Shift"}, {SDL_SCANCODE_LCTRL, "Ctrl"}, {SDL_SCANCODE_RCTRL, "Ctrl"},
        {SDL_SCANCODE_LALT, "Alt"}, {SDL_SCANCODE_RALT, "Alt"}, {SDL_SCANCODE_LGUI, "Win"}, {SDL_SCANCODE_RGUI, "Win"},
        {SDL_SCANCODE_CAPSLOCK, "Caps"}, {SDL_SCANCODE_DELETE, "Del"}, {SDL_SCANCODE_INSERT, "Ins"},
        {SDL_SCANCODE_HOME, "Home"}, {SDL_SCANCODE_END, "End"}, {SDL_SCANCODE_PAGEUP, "PgUp"},
        {SDL_SCANCODE_PAGEDOWN, "PgDn"}, {SDL_SCANCODE_KP_ENTER, "Ent"}, {SDL_SCANCODE_GRAVE, "`"},
        {SDL_SCANCODE_PRINTSCREEN, "PrtSc"}, {SDL_SCANCODE_SCROLLLOCK, "ScrLk"}, {SDL_SCANCODE_PAUSE, "Pause"},
        {SDL_SCANCODE_NUMLOCKCLEAR, "Num"}, {SDL_SCANCODE_MENU, "Menu"},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (names[i].sc == sc) { snprintf(out, n, "%s", names[i].t); return; }
    const char *nm = SDL_GetScancodeName((SDL_Scancode)sc);
    if (!nm || !*nm) { snprintf(out, n, "?"); return; }
    if (!strncmp(nm, "Keypad ", 7)) { snprintf(out, n, "KP%s", nm + 7); return; }  // "Keypad 1" -> "KP1"
    snprintf(out, n, "%s", nm);
}

// a key in a combo's label: at most `max` characters
static void short_label(char *out, size_t n, int key, int max) {
    static const struct { int k; const char *t; } names[] = {
        {BIND_MOUSE_LEFT, "LMB"}, {BIND_MOUSE_RIGHT, "RMB"}, {BIND_MOUSE_MIDDLE, "MMB"}, {BIND_MOUSE_X1, "M4"},
        {BIND_MOUSE_X2, "M5"}, {BIND_WHEEL_UP, "WhU"}, {BIND_WHEEL_DOWN, "WhD"}, {SDL_SCANCODE_UP, "^"},
        {SDL_SCANCODE_DOWN, "v"}, {SDL_SCANCODE_LEFT, "<"}, {SDL_SCANCODE_RIGHT, ">"}, {SDL_SCANCODE_LSHIFT, "Sh"},
        {SDL_SCANCODE_RSHIFT, "Sh"}, {SDL_SCANCODE_LCTRL, "Ctl"}, {SDL_SCANCODE_RCTRL, "Ctl"}, {SDL_SCANCODE_SPACE, "Spc"},
        {SDL_SCANCODE_RETURN, "Ent"},
    };
    char l[16] = "?";
    bool named = false;
    for (size_t i = 0; i < sizeof names / sizeof *names && !named; i++)
        if (names[i].k == key) snprintf(l, sizeof l, "%s", names[i].t), named = true;
    if (!named && key > 0 && key < SDL_SCANCODE_COUNT) key_label(l, sizeof l, key);
    snprintf(out, n, "%.*s", max, l);
}

// a combo (the keys held, then `key`) on one key's top: "E+1", "Sh+F" - the held keys cut to one character if the
// whole is too long ("C+S+1")
static void combo_label(char *out, size_t n, int key, const int16_t *mods) {
    for (int pass = 0; pass < 2; pass++) {
        size_t at = 0;
        char l[16];
        for (int m = 0; m < BIND_MODS; m++)
            if (mods[m]) short_label(l, sizeof l, mods[m], pass ? 1 : 3), at += (size_t)snprintf(out + at, at < n ? n - at : 0, "%s+", l);
        short_label(l, sizeof l, key, 3);
        at += (size_t)snprintf(out + at, at < n ? n - at : 0, "%s", l);
        if (at < n) return;
    }
}

// a key or mouse button (a keyboard binding value), with the keys held with it (mods: a combo, or NULL): false if
// none is bound
static bool key_symbol(GaSpec *s, int key, const int16_t *mods) {
    if (key > 0 && mods && mods[0]) {
        s->sym = GS_KEY;
        combo_label(s->text, sizeof s->text, key, mods);
        return true;
    }
    switch (key) {
    case 0: return false;
    case BIND_MOUSE_LEFT: s->sym = GS_MOUSE, s->dir = GM_LEFT; return true;
    case BIND_MOUSE_RIGHT: s->sym = GS_MOUSE, s->dir = GM_RIGHT; return true;
    case BIND_MOUSE_MIDDLE: s->sym = GS_MOUSE, s->dir = GM_MIDDLE; return true;
    case BIND_WHEEL_UP: s->sym = GS_MOUSE, s->dir = GM_WHEEL_UP; return true;
    case BIND_WHEEL_DOWN: s->sym = GS_MOUSE, s->dir = GM_WHEEL_DOWN; return true;
    case BIND_MOUSE_X1: s->sym = GS_KEY, snprintf(s->text, sizeof s->text, "M4"); return true;
    case BIND_MOUSE_X2: s->sym = GS_KEY, snprintf(s->text, sizeof s->text, "M5"); return true;
    case SDL_SCANCODE_UP: s->sym = GS_ARROW_KEY, s->dir = GD_UP; return true;
    case SDL_SCANCODE_DOWN: s->sym = GS_ARROW_KEY, s->dir = GD_DOWN; return true;
    case SDL_SCANCODE_LEFT: s->sym = GS_ARROW_KEY, s->dir = GD_LEFT; return true;
    case SDL_SCANCODE_RIGHT: s->sym = GS_ARROW_KEY, s->dir = GD_RIGHT; return true;
    default:
        if (key < 0 || key >= SDL_SCANCODE_COUNT) return false;
        s->sym = GS_KEY;
        key_label(s->text, sizeof s->text, key);
        return true;
    }
}

// four keys standing in for a stick or the d-pad (labels: one or two chars; the arrow keys "^" "v" "<" ">", drawn as
// arrows): up, left, down, right
// (a combo's key: the first held key's first character before it, "E1" for E + 1)
static void keys4_of(GaSpec *s, const int k[4], const int16_t *const mods[4]) {
    s->sym = GS_KEYS4;
    for (int i = 0; i < 4; i++) {
        char l[16];
        if (k[i] > 0 && mods[i] && mods[i][0]) {
            char m[4];
            short_label(m, sizeof m, mods[i][0], 1);
            short_label(l + 1, sizeof l - 1, k[i], 1);
            l[0] = m[0];
        } else if (k[i] == SDL_SCANCODE_UP || k[i] == SDL_SCANCODE_DOWN || k[i] == SDL_SCANCODE_LEFT || k[i] == SDL_SCANCODE_RIGHT)
            snprintf(l, sizeof l, "%s", k[i] == SDL_SCANCODE_UP ? "^" : k[i] == SDL_SCANCODE_DOWN ? "v" : k[i] == SDL_SCANCODE_LEFT ? "<" : ">");
        else if (k[i] > 0 && k[i] < SDL_SCANCODE_COUNT) key_label(l, sizeof l, k[i]);
        else snprintf(l, sizeof l, " ");
        snprintf(s->keys[i], sizeof s->keys[i], "%.2s", l);
    }
}
static void keys4(GaSpec *s, int up, int left, int down, int right) {  // (a stick's four bindings)
    const int k[4] = {g_bind_key[up], g_bind_key[left], g_bind_key[down], g_bind_key[right]};
    const int16_t *const m[4] = {g_bind_key_mod[up], g_bind_key_mod[left], g_bind_key_mod[down], g_bind_key_mod[right]};
    keys4_of(s, k, m);
}

// The icon's own label in the Switch set, to tell whether a Switch controller's icon would change at all
static const char *const k_switch_label[BIND_PAD_COUNT] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "+", "-"};

// What icon `ic` becomes in `set`; false: the game's own stays
static bool icon_spec(const GameIcon *ic, int set, GaSpec *s) {
    memset(s, 0, sizeof *s);
    // the sticks are the sticks on every controller; with keys, the four keys that stand in for one (the right
    // stick's, the arrows by default, rather than the mouse: its prompts are in menus - the map -, where the mouse
    // is let go)
    if (ic->shape == GA_STICK) {
        if (set != ICONS_KEYBOARD) return false;
        if (ic->extra == 1) return key_symbol(s, g_key_now[ic->input], g_mod_now[ic->input]);  // (pressing it in)
        if (ic->input == BIND_LSTICK) keys4(s, BIND_LS_UP, BIND_LS_LEFT, BIND_LS_DOWN, BIND_LS_RIGHT);
        else keys4(s, BIND_RS_UP, BIND_RS_LEFT, BIND_RS_DOWN, BIND_RS_RIGHT);
        return true;
    }
    if (ic->shape == GA_DOTS && ic->extra < 0) {  // the d-pad as a whole: its four keys, or a d-pad
        if (set == ICONS_SWITCH) return false;
        if (set == ICONS_KEYBOARD) {
            const int k[4] = {g_key_now[BIND_UP], g_key_now[BIND_LEFT], g_key_now[BIND_DOWN], g_key_now[BIND_RIGHT]};
            const int16_t *const m[4] = {g_mod_now[BIND_UP], g_mod_now[BIND_LEFT], g_mod_now[BIND_DOWN], g_mod_now[BIND_RIGHT]};
            keys4_of(s, k, m);
            return true;
        }
        s->sym = GS_DPAD, s->dir = -1;  // (no arm lit)
        return true;
    }
    if (set == ICONS_KEYBOARD) return key_symbol(s, g_key_now[ic->input], g_mod_now[ic->input]);
    if (set == ICONS_WII) {  // (input.c wiimote_buttons: + = X, 2 = Plus, 1 = Minus, C = Y, Z = ZL; A, B and the
                             // d-pad are themselves; L, R and ZR have nothing)
        if (ic->shape == GA_DOTS) { s->sym = GS_DPAD, s->dir = ic->extra; return true; }  // (the remote's cross)
        const char *w = ic->input == BIND_X ? "+" : ic->input == BIND_Y ? "C" : ic->input == BIND_ZL ? "Z" :
                        ic->input == BIND_A ? "A" : ic->input == BIND_B ? "B" : ic->input == BIND_PLUS ? "2" :
                        ic->input == BIND_MINUS ? "1" : NULL;
        if (!w || !strcmp(w, k_switch_label[ic->input])) return false;
        text(s, w);
        return true;
    }
    int host = g_pad_now[ic->input];
    if (ic->shape == GA_DOTS) {  // the d-pad as a Joy-Con's four buttons, the one meant lit
        int dir = host == SDL_GAMEPAD_BUTTON_DPAD_UP ? GD_UP : host == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? GD_DOWN :
                  host == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? GD_LEFT : host == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? GD_RIGHT : -1;
        if (set == ICONS_SWITCH && dir >= 0) {  // (rebound to another direction: that one lit)
            if (dir == ic->extra) return false;
            s->sym = GS_DOT, s->dir = dir;
            return true;
        }
        pad_symbol(s, host, set);  // (an Xbox / PlayStation d-pad, or the button it's bound to)
        return true;
    }
    pad_symbol(s, host, set);
    if (set == ICONS_SWITCH && s->sym == GS_TEXT && !strcmp(s->text, k_switch_label[ic->input])) return false;
    return true;
}

// ------------------------------------------------------------------ the textures
static uint64_t fnv(const uint8_t *p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

int glyph_match(uint32_t w, uint32_t h, uint32_t format, uint32_t levels, const uint8_t *lin, uint64_t size) {
    if (format == 0x4E && w == 128 && h == 36 && levels == 1 && size == 4608) {  // (two buttons: k_wide)
        uint64_t f = fnv(lin, size);
        for (int i = 0; i < N_WIDE; i++)
            if (k_wide[i].hash == f) return N_ICONS + i;
        if (log_on()) LOG("[glyphs] a 128 x 36 BC7 image that isn't a known two-button icon: %016llx\n", (unsigned long long)f);
        return -1;
    }
    if (w == 36 && h == 36 && (format != 0x4E || levels != 1 || size != 1296) && log_on())
        LOG("[glyphs] a 36 x 36 image not checked: format %#x, %u levels, %llu bytes\n", format, levels,
            (unsigned long long)size);
    if (format != 0x4E || w != 36 || h != 36 || levels != 1 || size != 1296) return -1;  // (BC7 sRGB, 36 x 36)
    uint64_t f = fnv(lin, size);
    int lo = 0, hi = N_ICONS - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (k_icons[m].hash == f) return m;
        if (k_icons[m].hash < f) lo = m + 1;
        else hi = m - 1;
    }
    if (log_on()) LOG("[glyphs] a 36 x 36 BC7 image that isn't a known button icon: %016llx\n", (unsigned long long)f);
    return -1;
}

static const uint8_t *font(void) {  // Baloo 2 SemiBold (the menus' font, shipped with the game)
    static uint8_t *data;
    static bool tried;
    if (tried) return data;
    tried = true;
    char path[1024];
    if (!rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", path, sizeof path)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n > 0 ? (size_t)n : 1);
    if (n <= 0 || fread(data, 1, (size_t)n, f) != (size_t)n) free(data), data = NULL;
    fclose(f);
    if (!data) LOG("[glyphs] can't read %s: button icons drawn without labels\n", path);
    return data;
}

// A 36 x 36 part of a 128 x 36 image (block column bx0 .. bx0 + 8), out of / into its linear BC7 blocks
static void wide_part(uint8_t *lin, uint8_t part[1296], int bx0, bool put) {
    for (int by = 0; by < 9; by++) {
        uint8_t *row = lin + by * 512 + bx0 * 16;
        if (put) memcpy(row, part + by * 144, 144);
        else memcpy(part + by * 144, row, 144);
    }
}

// a two-button image (k_wide) drawn again: each button as the icon it is; on keys, X + a d-pad direction is the one key
// bound to it (the beam / visor shortcuts - X held and that direction: BIND_BEAM_*, Power up, Wave right, Ice down,
// Plasma left), and X + the whole d-pad their four, in the middle of the image with the rest cleared
static bool wide_redraw(const WideIcon *wi, uint8_t *lin, int set) {
    static const int beam_of[4] = {BIND_BEAM_POWER, BIND_BEAM_ICE, BIND_BEAM_PLASMA, BIND_BEAM_WAVE};  // (GD_ order)
    uint8_t part[1296];
    GaSpec s;
    if (set == ICONS_KEYBOARD && wi->l.input == BIND_X && wi->r.shape == GA_DOTS) {
        bool one = false;
        memset(&s, 0, sizeof s);
        if (wi->r.extra >= 0 && wi->r.extra < 4) {
            int slot = beam_of[wi->r.extra];
            one = key_symbol(&s, g_bind_key[slot], g_bind_key_mod[slot]);
        } else {
            const int k[4] = {g_bind_key[BIND_BEAM_POWER], g_bind_key[BIND_BEAM_PLASMA], g_bind_key[BIND_BEAM_ICE],
                              g_bind_key[BIND_BEAM_WAVE]};
            const int16_t *const m[4] = {g_bind_key_mod[BIND_BEAM_POWER], g_bind_key_mod[BIND_BEAM_PLASMA],
                                         g_bind_key_mod[BIND_BEAM_ICE], g_bind_key_mod[BIND_BEAM_WAVE]};
            one = k[0] && k[1] && k[2] && k[3];
            if (one) keys4_of(&s, k, m);
        }
        if (one) {
            wide_part(lin, part, 2, false);  // (drawn on the X button: its colors)
            glyph_art_render(part, 36, 36, wi->l.shape, 0, &s, font());
            static const uint8_t clear[16] = {0x40};  // (BC7 mode 6, all zero: transparent)
            for (int i = 0; i < 4608; i += 16) memcpy(lin + i, clear, 16);
            wide_part(lin, part, 12, true);  // (pixels 48-83: the middle)
            return true;
        }
    }
    bool any = false;
    for (int side = 0; side < 2; side++) {
        const GameIcon *ic = side ? &wi->r : &wi->l;
        if (ic->input < 0 || !icon_spec(ic, set, &s)) continue;
        wide_part(lin, part, side ? 21 : 2, false);
        if (glyph_art_render(part, 36, 36, ic->shape, ic->extra, &s, font())) wide_part(lin, part, side ? 21 : 2, true), any = true;
    }
    return any;
}

bool glyph_redraw(int id, uint8_t *lin, uint32_t w, uint32_t h) {
    if (id >= N_ICONS && id < N_ICONS + N_WIDE && w == 128 && h == 36 && g_now_set) {
        const WideIcon *wi = &k_wide[id - N_ICONS];
        int set = g_set;
        bool done = wide_redraw(wi, lin, set);
        if (log_on()) LOG("[glyphs] two-button icon %016llx (%s / %s) %s for %s\n", (unsigned long long)wi->hash,
                          wi->l.input >= 0 ? bind_input_name(wi->l.input) : "motion", bind_input_name(wi->r.input),
                          done ? "drawn" : "left", k_set_names[set]);
        return done;
    }
    if (id < 0 || id >= N_ICONS || !g_now_set) return false;
    const GameIcon *ic = &k_icons[id];
    GaSpec s;
    int set = g_set;
    if (!icon_spec(ic, set, &s)) return false;
    bool done = glyph_art_render(lin, (int)w, (int)h, ic->shape, ic->extra, &s, font());
    if (log_on()) LOG("[glyphs] icon %016llx (%s) drawn for %s: symbol %d %s\n", (unsigned long long)ic->hash,
                      bind_input_name(ic->input), k_set_names[set],
                      s.sym, s.sym == GS_TEXT || s.sym == GS_KEY ? s.text : "");
    return done;
}

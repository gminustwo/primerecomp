// Control remapping (runtime/hle/bindings.c): which controller button / key drives each Switch input. Edited from
// the in-game menu's Controls tab and the start screen (runtime/ui/controls_ui.mm), saved in primerecomp.ini.
// Menus (the overlay, the start screen, the game's own while it isn't in play) never use the face buttons' bindings:
// those go by the controller's own labels there (bind_face_label: A confirms, B goes back, on any controller), so no
// binding can lock the player out.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Switch inputs, in nn::hid button-bit order (runtime/hle/input.c BTN_*), then the keyboard's stick directions and
// beam shortcuts.
enum {
    BIND_A, BIND_B, BIND_X, BIND_Y, BIND_LSTICK, BIND_RSTICK, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
    BIND_LEFT, BIND_UP, BIND_RIGHT, BIND_DOWN,
    BIND_PAD_COUNT,                                      // controller: the 16 buttons (its sticks stay the sticks)
    BIND_LS_UP = BIND_PAD_COUNT, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT,
    BIND_RS_UP, BIND_RS_DOWN, BIND_RS_LEFT, BIND_RS_RIGHT,
    // one key for a beam: X held (the game's beam select) and that beam's d-pad direction
    BIND_BEAM_POWER, BIND_BEAM_WAVE, BIND_BEAM_ICE, BIND_BEAM_PLASMA,
    BIND_KEY_COUNT                                       // keyboard: buttons + both sticks + beams
};
enum { BIND_DEV_PAD, BIND_DEV_KEY };
// Controller binding values: an SDL_GamepadButton, or the analog triggers as buttons
enum { PADB_LTRIGGER = 26 /* SDL_GAMEPAD_BUTTON_COUNT */, PADB_RTRIGGER, PADB_HOST_COUNT };
// Keyboard binding values: an SDL_Scancode, or a mouse button / the wheel past them
enum { BIND_MOUSE_LEFT = 512 /* SDL_SCANCODE_COUNT */, BIND_MOUSE_MIDDLE, BIND_MOUSE_RIGHT, BIND_MOUSE_X1, BIND_MOUSE_X2,
       BIND_WHEEL_UP, BIND_WHEEL_DOWN, BIND_KEY_HOST_COUNT };

extern int16_t g_bind_pad[BIND_PAD_COUNT];  // PADB value per Switch button
extern int16_t g_bind_key[BIND_KEY_COUNT];  // SDL_Scancode / BIND_MOUSE_* per input (0 = none)

const char *bind_input_name(int slot);         // "A", "D-pad Up", "Left stick Up"
const char *bind_input_action(int slot);       // what Metroid Prime does with it on foot ("Shoot beam"), or ""
const char *bind_pad_label(int host, int fam); // controller button in the pad family's own words (fam: input_pad_family)
const char *bind_key_label(int host);          // key or mouse button name, as primerecomp.ini stores it
int bind_key_from_name(const char *name);      // bind_key_label's name -> keyboard binding value (0: unknown)
int bind_label_family(void);                   // the connected controller's family (also on the start screen)
void bind_reset(int dev);
bool bind_is_default(int dev);
bool bind_is_default_slot(int dev, int slot);

// "Click a binding, then press": begun from the UI; the next press (any thread's SDL event) is taken. Escape cancels,
// as does BIND_CAPTURE_MS without a press. On a conflict the two inputs swap.
#define BIND_CAPTURE_MS 5000
void bind_capture_begin(int dev, int slot);
void bind_capture_cancel(void);
// capturing: dev / slot (else -1), ms left
bool bind_capture_active(int *dev, int *slot, int *ms_left);
// Main thread, before anything else sees the event: true = consumed by a capture
bool bind_capture_event(const SDL_Event *e);
// Last change for the UI: 0 none, 1 set, 2 swapped (with *other), 3 cancelled, 4 timed out, 5 reset; *serial bumps
int bind_last_result(int *dev, int *slot, int *other, int *serial);

// primerecomp.ini: bind_pad_<input> / bind_key_<input> (only the ones that differ from the defaults are written; an
// older file's "face_layout = xbox" comes first: then the bind_pad_* lines are read with the face buttons swapped back)
bool bind_settings_load(const char *key, const char *val);
void bind_settings_save(FILE *f);

// bind_* names (primerecomp.ini): Switch input "a", "zl", "ls_up"... -> slot; controller button "south"... -> PADB value
int bind_slot_from_name(const char *name);
int bind_pad_from_name(const char *name);


// The face buttons in menus: by what they're labelled on the controller, not where they sit - the button marked A
// (Cross) is A, B (Circle) is B, X (Square) is X, Y (Triangle) is Y. On an Xbox or PlayStation controller A is the
// bottom one, on a Switch controller the right one. In play they go through the bindings above instead (by position
// by default, as on a Switch: the bottom one jumps). host: a controller button; which: the controller it came from
// (0: the game's - runtime/hle/input.c). BIND_A .. BIND_Y, or -1 for a button that isn't a face button.
int bind_face_label(uint32_t which, int host);
// The face button labelled as Switch input `slot` (BIND_A .. BIND_Y) on the game's controller
int bind_label_host(int slot);

#ifdef __cplusplus
}
#endif

// Control remapping (runtime/hle/bindings.c): which controller button / key drives each Switch input. Edited from
// the in-game menu's Controls tab and the start screen (runtime/ui/controls_ui.mm), saved in primerecomp.ini.
// Menus (the overlay, the start screen; the game's own while it isn't in play, with Face buttons: Labels) never use
// the face buttons' bindings: those go by bind_face_label there (by default the controller's own labels: A confirms, B
// goes back, on any controller), so no binding can lock the player out.
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
// A keyboard binding can be a combo (issue #30: E + 1 for a visor, 1 for a beam): g_bind_key is the key that completes
// it, these the ones held with it, in the order they were pressed (0 = none). In play a binding is on while all its
// keys are held, and a combo's keys are its own until each is let go: a binding of fewer of them (1) stays off -
// runtime/hle/input.c.
#define BIND_MODS 2
#define BIND_COMBO (1 + BIND_MODS)
extern int16_t g_bind_key_mod[BIND_KEY_COUNT][BIND_MODS];

const char *bind_input_name(int slot);         // "A", "D-pad Up", "Left stick Up"
const char *bind_input_action(int slot);       // what Metroid Prime does with it on foot ("Shoot beam"), or ""
const char *bind_pad_label(int host, int fam); // controller button in the pad family's own words (fam: input_pad_family)
const char *bind_key_label(int host);          // key or mouse button name, as primerecomp.ini stores it
// a keyboard binding's keys as the UI shows them, "E + 1" (n: buf's size); buf
const char *bind_key_combo_name(int slot, char *buf, size_t n);
// its keys (the held ones first, the key that completes it last): how many (0: none)
int bind_key_combo(int slot, int keys[BIND_COMBO]);
int bind_key_from_name(const char *name);      // bind_key_label's name -> keyboard binding value (0: unknown)
int bind_label_family(void);                   // the connected controller's family (also on the start screen)
void bind_reset(int dev);
void bind_reset_slot(int dev, int slot);       // one binding back to its default (one already on that swaps)
bool bind_is_default(int dev);
bool bind_is_default_slot(int dev, int slot);

// "Click a binding, then press": begun from the UI. A controller's: the next button (any thread's SDL event) is taken,
// BIND_CAPTURE_MS without one cancels. The keyboard's: a BIND_CAPTURE_MS countdown, and the keys held when it ends
// are the binding - up to BIND_COMBO, a combo - or, with none held then, the most held together in the last press; a
// wheel notch ends it at once (with the keys held with it). Escape cancels either. On a conflict (the same button, or
// the same keys) the two inputs swap.
#define BIND_CAPTURE_MS 5000
void bind_capture_begin(int dev, int slot);
void bind_capture_cancel(void);
// capturing: dev / slot (else -1), ms left. (It also ends a keyboard capture whose time is up: poll it.)
bool bind_capture_active(int *dev, int *slot, int *ms_left);
// a keyboard capture's keys so far (held now, or the last ones held together), in the order pressed: how many
int bind_capture_keys(int keys[BIND_COMBO]);
// Main thread, before anything else sees the event: true = consumed by a capture
bool bind_capture_event(const SDL_Event *e);
// Last change for the UI: 0 none, 1 set, 2 swapped (with *other), 3 cancelled, 4 timed out, 5 reset; *serial bumps
int bind_last_result(int *dev, int *slot, int *other, int *serial);

// primerecomp.ini: bind_pad_<input> / bind_key_<input>, a combo's other keys bind_keymod1_<input> / bind_keymod2_<input>
// (only the ones that differ from the defaults are written)
bool bind_settings_load(const char *key, const char *val);
// (put: settings.c's writer - the file, or the log's copy of the settings)
void bind_settings_save(void (*put)(void *ctx, const char *fmt, ...), void *ctx);

// bind_* names (primerecomp.ini): Switch input "a", "zl", "ls_up"... -> slot; controller button "south"... -> PADB value
int bind_slot_from_name(const char *name);
int bind_pad_from_name(const char *name);


// Face buttons (Controls tab, g_settings.face_buttons, FACE_*):
// - Labels (the default): in menus by what they're labelled on the controller, not where they sit - the button marked
//   A (Cross) is A, B (Circle) is B, X (Square) is X, Y (Triangle) is Y: on an Xbox or PlayStation controller A is the
//   bottom one, on a Switch controller the right one. In play they go through the bindings above (by position by
//   default, as on a Switch: the bottom one jumps).
// - Nintendo: by position everywhere, as on a Switch (the right button is A in menus too); the game gets the bindings.
// - Xbox: A is the bottom button everywhere - the bindings' face buttons swapped (so A shoots in play) and the menus'.
// bind_face_label: the Switch input (BIND_A .. BIND_Y) a face button means in menus, -1 for another button. which: the
// controller it came from (0: the game's - runtime/hle/input.c).
int bind_face_label(uint32_t which, int host);
// The face button labelled as Switch input `slot` (BIND_A .. BIND_Y) on the game's controller
int bind_label_host(int slot);
extern const char *const g_face_names[];  // primerecomp.ini's face_buttons: "labels", "nintendo", "xbox"
// set swaps the face buttons in the current bindings (and the defaults they're compared with) going to or from Xbox;
// load (primerecomp.ini, before the bind_pad_* lines) sets the setting's defaults
void bind_set_face_buttons(int mode);
void bind_load_face_buttons(int mode);

#ifdef __cplusplus
}
#endif

// Button prompt icons redrawn for the controller in use (runtime/hle/glyphs.c decides what, this draws it): the
// game's own icon decoded, its symbol taken out and another drawn in its colours, then encoded back to BC7.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The game icon's shape: a round button, a shoulder button (a rounded box), a trigger (a box rounded at one top
// corner), the four face buttons as dots with one lit, a stick
enum { GA_CIRCLE, GA_RECT, GA_TRIGGER, GA_DOTS, GA_STICK };
// What goes in it. Controller: a label, PlayStation's four shapes, the Menu / View / Create buttons' marks, a d-pad
// direction, the dot of a face position. Keyboard and mouse (the icon redrawn as a key): a key's label, an arrow
// key, a mouse button, four movement keys.
enum { GS_TEXT, GS_CROSS, GS_RING, GS_SQUARE, GS_TRIANGLE, GS_MENU, GS_VIEW, GS_SHARE, GS_DPAD, GS_DOT,
       GS_KEY, GS_ARROW_KEY, GS_MOUSE, GS_KEYS4 };
enum { GD_UP, GD_DOWN, GD_LEFT, GD_RIGHT };
enum { GM_LEFT, GM_RIGHT, GM_MIDDLE, GM_BODY, GM_WHEEL_UP, GM_WHEEL_DOWN };

typedef struct {
    int sym;           // GS_*
    char text[8];      // GS_TEXT / GS_KEY: the label (ASCII)
    int dir;           // GS_DPAD / GS_DOT / GS_ARROW_KEY: GD_*; GS_MOUSE: GM_*
    char keys[4][4];   // GS_KEYS4: the up, left, down and right keys' labels ("^" "v" "<" ">": arrows)
} GaSpec;

// bc7: the texture's level 0 as linear BC7 blocks, w x h texels (multiples of 4), its rows bottom first as the game
// stores its UI images; redrawn in place. shape: GA_*; dot_lit: GA_DOTS, the dot the game's icon lights (GD_*).
// ttf: a TrueType font for the labels (NULL: labels are left out). False: nothing changed.
bool glyph_art_render(uint8_t *bc7, int w, int h, int shape, int dot_lit, const GaSpec *s, const uint8_t *ttf);

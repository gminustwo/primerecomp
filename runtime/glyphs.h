// Button prompt icons for the controller in use (runtime/hle/glyphs.c; Controls tab "Button icons").
#pragma once
#include <stdbool.h>
#include <stdint.h>

// g_settings.button_icons
enum { ICONS_AUTO, ICONS_SWITCH, ICONS_XBOX, ICONS_PLAYSTATION, ICONS_KEYBOARD, ICONS_WII, ICONS_COUNT };
extern const char *const g_icons_names[ICONS_COUNT];  // primerecomp.ini's: "auto", "switch", ... "wii"
enum { GLYPH_DEV_PAD, GLYPH_DEV_KEYS, GLYPH_DEV_WII };

// runtime/hle/input.c: what the player used last (Auto follows it)
void glyphs_note_device(int dev);
// Once a frame (input.c): the icons to show worked out again; a change bumps the generation
void glyphs_frame(void);
// The set shown now (Auto resolved: ICONS_SWITCH .. ICONS_WII), for the menu
int glyphs_current(void);

// runtime/nvn/vulkan/textures.c, as a texture's level 0 is uploaded (lin: its texels as linear blocks):
// one of the game's prompt icons (an id), or -1
int glyph_match(uint32_t w, uint32_t h, uint32_t format, uint32_t levels, const uint8_t *lin, uint64_t size);
// that icon redrawn in place for the controller in use (false: the game's own stays)
bool glyph_redraw(int id, uint8_t *lin, uint32_t w, uint32_t h);
// changes whenever the icons to show do: an icon uploaded at an older one is uploaded again
uint32_t glyph_generation(void);

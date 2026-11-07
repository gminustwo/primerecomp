// Texture mods: the game's textures replaced by image files in mods/, and the dump that names them (texmods.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

enum { MOD_RGBA8, MOD_BC1, MOD_BC2, MOD_BC3, MOD_BC4, MOD_BC5, MOD_BC7 };

typedef struct ModImage {
    uint64_t key;
    uint32_t w, h, levels;
    int kind;                 // MOD_*: the pixels' encoding (RGBA8: R, G, B, A bytes, top row first)
    uint8_t *data;
    uint64_t off[16], len[16];  // each level's bytes in data
    _Atomic int state;        // 0 loading, 1 ready, 2 failed, 3 let go while loading (the loader frees it)
} ModImage;

void texmods_init(void);       // at start: the mods folder's files (Video tab "Texture mods" on)
bool texmods_active(void);     // replacements found (or dumping): the upload path asks about textures
int texmods_count(void);       // replacement files found
const char *texmods_folder(void);
// The name of a texture: its full-size level as the game stores it, its size and format
uint64_t texmods_key(const uint8_t *level0, uint64_t n, uint32_t w, uint32_t h, uint32_t format);
bool texmods_has(uint64_t key);
ModImage *texmods_request(uint64_t key);  // loading starts (NULL: no such file); poll ->state, then texmods_release
void texmods_release(ModImage *m);
// RGBA8 pixels (R, G, B, A bytes, top row first) as m's levels, the smaller ones made here (false: out of memory)
bool texmods_set_rgba(ModImage *m, const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t pitch);
// Dump textures: written at the start of the next presented frame (texmods_dump_frame), a few a frame
void texmods_dump_request(TexBackend *b, uint64_t key, uint32_t w, uint32_t h);
void texmods_dump_frame(void);
void texmods_forget(TexBackend *b);  // (a backend going away: its queued dump with it)

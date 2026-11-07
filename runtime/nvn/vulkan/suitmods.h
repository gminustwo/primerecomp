// Suit colors (Gameplay tab): Samus' suits recolored from the player's own textures (suitmods.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "texmods.h"

// The player's own colors are chosen (Custom)
bool suitmods_on(void);
// Changes whenever the colors to draw do: a suit texture made for an older one is made again
uint32_t suitmods_generation(void);
// Could this texture be one of the suits (size and format; then its key decides): worth naming at upload
bool suitmods_candidate(uint32_t w, uint32_t h, uint32_t format);
// Is the texture of this key one of the suits, to be recolored now
bool suitmods_wants(uint64_t key);
// Its recolored image starts being made from level 0 as the game stores it (block linear ASTC): poll ->state, then
// texmods_release, as for a file
typedef struct {
    const uint8_t *level0;   // the level's bytes (copied here)
    uint64_t size;
    uint32_t w, h, wb, hb;   // texels, blocks
    uint32_t bw, bh;         // the ASTC block's size
    uint32_t block_height_log2, block_depth_log2;
    bool srgb;
} SuitSource;
ModImage *suitmods_request(uint64_t key, const SuitSource *src);

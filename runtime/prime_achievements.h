#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Called from the player update with the amount/capacity pairs and the current room name, if known.
void prime_achievements_inventory(uint64_t game_state, uint64_t player_state, const int32_t *items, const char *room);
void prime_achievements_save_read(void);
#ifdef __cplusplus
}
#endif

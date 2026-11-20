// Discord status ("Rich Presence", issue #96): runtime/discord.c
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void discord_start(void);  // once the settings are loaded (the window's first start): the thread that keeps it
// What the status says now, for the menu: "Shown on Discord", "Discord isn't running on this PC"...
const char *discord_status_text(void);

#ifdef __cplusplus
}
#endif

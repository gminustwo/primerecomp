// What the runtime asks about the running game: the renderers' frame logs and the window's framing. Link's
// Awakening's answers (its level name from PlayerStatus, widescreen framing per room) went with the rest of its game
// code (git history: runtime/hle/widescreen.c); Metroid Prime Remastered's belong here.
#include "../hle.h"

int g_start_screen;  // runtime/ui/launcher.mm: the start screen is up - before the first boot nothing is loaded

// Current area, for the logs ("-": not known)
const char *game_level_name(void) { return "-"; }

// The window may show more than 16:9 (Video tab, aspect ratio unlocked). That needs the game's cameras widened,
// which isn't done for this game yet: always the original framing.
bool widescreen_allowed(void) { return false; }

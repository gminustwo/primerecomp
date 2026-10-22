// What the runtime asks about the running game: the renderers' frame logs and the window's framing. Link's
// Awakening's answers (its level name from PlayerStatus, widescreen framing per room) went with the rest of its game
// code (git history: runtime/hle/widescreen.c); Metroid Prime Remastered's belong here.
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../settings.h"

int g_start_screen;  // runtime/ui/launcher.mm: the start screen is up - before the first boot nothing is loaded

// Current area, for the logs ("-": not known)
const char *game_level_name(void) { return "-"; }

// Frame rate (Video tab "Frame rate", LARECOMP_FPS): the frames presented per second and the game's simulation rate.
// The game steps a fixed 1/60 s per simulation tick, at most one tick per frame (CGameArchitectureSupport::Update);
// at 120 every place that builds that 1/60 builds 1/120 instead (tools/patches.py STEP_SITES), so the game runs at
// the same speed with twice the steps. Uncapped (0): frames as fast as they come, the simulation at 60.
int game_frame_rate(void) {
    static int env = -2;
    if (env == -2) env = getenv("LARECOMP_FPS") ? atoi(getenv("LARECOMP_FPS")) : -1;
    return env >= 0 ? env : g_settings.frame_rate;
}

uint32_t rt_sim_step_bits(int negative) {
    float step = 1.0f / (game_frame_rate() == 120 ? 120 : 60);
    uint32_t b;
    memcpy(&b, &step, 4);
    return negative ? b | 0x80000000u : b;
}

// The Switch presents at the display's vsync times the window's present interval (1 here) and blocks until then;
// the host waits for the frame's deadline the same way (runtime/nvn/objects.c nvnQueuePresentTexture). A frame
// later than a few periods starts the schedule over instead of rushing to catch up.
void game_pace_present(int interval) {
    int fps = game_frame_rate();
    if (fps <= 0) return;
    static uint64_t next;
    uint64_t period = (uint64_t)(1e9 * (interval > 0 ? interval : 1) / fps), now = plat_now_ns();
    if (!next || now > next + 4 * period) next = now;
    else plat_sleep_until_ns(next);
    next += period;
}

// The window may show more than 16:9 (Video tab, aspect ratio unlocked). That needs the game's cameras widened,
// which isn't done for this game yet: always the original framing.
bool widescreen_allowed(void) { return false; }

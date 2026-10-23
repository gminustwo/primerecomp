// What the runtime asks about the running game: the renderers' frame logs and the window's framing. Link's
// Awakening's answers (its level name from PlayerStatus, widescreen framing per room) went with the rest of its game
// code (git history: runtime/hle/widescreen.c); Metroid Prime Remastered's belong here.
#include <stdio.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"
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

// CGameArchitectureSupport::Update: each frame adds the elapsed time it measured (at most one tick) to an
// accumulator and runs a simulation tick per full tick in it. (debug) LARECOMP_TICK_LOG=<from frame>:<frames>: per
// frame, that elapsed time and the passes of its tick loop the frame before ("[tick]").
static int g_tick_passes, g_ticks;
static volatile uint64_t g_present_ns;  // when the latest present's deadline wait ended
void rt_tick_body(void) { g_ticks++; }
unsigned rt_tick_pass(void) {
    g_tick_passes++;
    return rt_sim_step_bits(1);
}

unsigned rt_frame_step(Cpu *c) {
    static long from = -2, left;
    if (from == -2) {
        const char *e = getenv("LARECOMP_TICK_LOG");
        if (!e || sscanf(e, "%ld:%ld", &from, &left) != 2) from = -1;
    }
    extern uint64_t renderer_present_count(void);
    if (from >= 0 && left > 0 && (long)renderer_present_count() >= from) {
        left--;
        float el;
        memcpy(&el, &c->v[8], 4);
        LOG("[tick] frame %llu elapsed %.3f ms, %.3f ms after the present, tick loop passes before %d, ticks %d\n", (unsigned long long)renderer_present_count(),
            el * 1000, (double)(plat_now_ns() - g_present_ns) / 1e6, g_tick_passes, g_ticks);
    }
    g_tick_passes = g_ticks = 0;
    static int once;
    if (!once++) {
        extern KThread *kernel_current_thread(void);
        KThread *t = kernel_current_thread();
        LOG("[pace] the game's update runs on thread %x\n", t ? t->handle : 0);
    }
    return rt_sim_step_bits(0);
}

void rt_movie_dt(Cpu *c) {  // (debug, LARECOMP_TICK_LOG) the time step the movies advance by
    static long from = -2, left;
    if (from == -2) {
        const char *e = getenv("LARECOMP_TICK_LOG");
        if (!e || sscanf(e, "%ld:%ld", &from, &left) != 2) from = -1;
    }
    extern uint64_t renderer_present_count(void);
    if (from < 0 || left <= 0 || (long)renderer_present_count() < from) return;
    left--;
    float dt;
    memcpy(&dt, &c->v[0], 4);
    LOG("[tick] frame %llu movie dt %.3f ms\n", (unsigned long long)renderer_present_count(), dt * 1000);
}

// The Switch presents at the display's vsync times the window's present interval (1 here) and blocks until then;
// the host waits for the frame's deadline the same way (runtime/nvn/objects.c nvnQueuePresentTexture). A frame
// later than a few periods starts the schedule over instead of rushing to catch up.
void game_pace_present(int interval) {
    static int once;
    if (!once++) {
        extern KThread *kernel_current_thread(void);
        KThread *t = kernel_current_thread();
        LOG("[pace] frames presented by thread %x\n", t ? t->handle : 0);
    }
    int fps = game_frame_rate();
    if (fps <= 0) return;
    static uint64_t next;
    uint64_t period = (uint64_t)(1e9 * (interval > 0 ? interval : 1) / fps), now = plat_now_ns();
    if (!next || now > next + 4 * period) next = now;
    else plat_sleep_until_ns(next);
    g_present_ns = plat_now_ns();
    next += period;
}

// (debug) LARECOMP_AUTOSAVE_AT=<frame>: from that frame on, the next gameplay frame starts the game's autosave
// (CStateManager::StartAutoSave, as the settings menu's "save game" action does), which writes the current slot -
// for testing saves without playing to a save station. Called from CStateManager::BeginFrame (tools/patches.py),
// state manager in x0; every register is put back afterwards.
static void debug_teleport(Cpu *c);

int rt_skip_cinematics(void) {  // (also CCameraManager::CheckForCinematicSkip: tools/patches.py)
    static int skip = -1;
    if (skip < 0) skip = getenv("LARECOMP_SKIP_CINEMATICS") && atoi(getenv("LARECOMP_SKIP_CINEMATICS")) ? 1 : 0;
    return skip;
}

void rt_debug_autosave(Cpu *c) {
    debug_teleport(c);
    // (debug) LARECOMP_SKIP_CINEMATICS=1: every cutscene skippable, as on a second playthrough
    // (CGameState::mCinematicForceSkippableOverride, which IsCinematicSkippable checks first), and skipped
    // (rt_skip_cinematics) - the intro replay reaches the landing site at frame ~5000 instead of ~12000
    if (rt_skip_cinematics()) *(uint8_t *)(uintptr_t)guest_sym("_ZN10CGameState32mCinematicForceSkippableOverrideE") = 1;
    static long at = -2;
    if (at == -2) at = getenv("LARECOMP_AUTOSAVE_AT") ? atol(getenv("LARECOMP_AUTOSAVE_AT")) : -1;
    extern uint64_t vk_frame_counter(void);
    if (at < 0 || (long)vk_frame_counter() < at) return;
    at = -1;
    LOG("[save] debug autosave (state manager %llx)\n", (unsigned long long)c->x[0]);
    Cpu *saved = malloc(sizeof *saved);
    *saved = *c;
    // LARECOMP_SAVE_AREA=<area id, 32 hex digits> or "current": the save's desired area, where loading it starts (the
    // world state's +0x40, CWorldStateMP1::SetDesiredAreaAssetId; +0x10 is the current area) - by default the
    // frigate's landing site
    const char *area = getenv("LARECOMP_SAVE_AREA");
    extern uint64_t g_world_state;
    if (area && g_world_state) {
        uint8_t *ws = (uint8_t *)(uintptr_t)g_world_state;
        if (!strcmp(area, "current")) memcpy(ws + 0x40, ws + 0x10, 16);
        else if (strlen(area) == 32)
            for (int i = 0; i < 16; i++) { unsigned v = 0; sscanf(area + 2 * i, "%2x", &v); ws[0x40 + i] = (uint8_t)v; }
        char h[33];
        for (int i = 0; i < 16; i++) snprintf(h + 2 * i, 3, "%02x", ws[0x40 + i]);
        LOG("[save] desired area %s (world state %llx)\n", h, (unsigned long long)g_world_state);
    }
    guest_call(c, guest_sym("_ZN13CStateManager13StartAutoSaveEv"));
    *c = *saved;
    free(saved);
}

uint64_t g_world_state;  // (the running world's CWorldStateMP1)
void rt_world_state(Cpu *c) { g_world_state = c->x[0]; }

// ---- (debug) areas
// An area (a room) is a 16-byte CObjectId (GUID); each one the game loads is logged ("[area] load ..."). Called from
// CGameAreaManager::BeginAreaLoad (tools/patches.py), the ID at [x1], the load type in w2.
static uint64_t g_area_mgr;  // (the CGameAreaManager of the latest load: the running game's)

void rt_area_load(Cpu *c) {
    g_area_mgr = c->x[0];
    const uint8_t *id = (const uint8_t *)(uintptr_t)c->x[1];
    char h[33];
    for (int i = 0; i < 16; i++) snprintf(h + 2 * i, 3, "%02x", id[i]);
    extern uint64_t vk_frame_counter(void);
    LOG("[area] load %s type %u (frame %llu)\n", h, (unsigned)c->x[2], (unsigned long long)vk_frame_counter());
}

// ---- (debug) the player: position notes, position log, teleport
// CPlayerMP1::UpdateCameraBob (each gameplay frame, the player in x0 and the state manager in x1; tools/patches.py)
// keeps both for these:
// F8: the player's position and facing, appended to positions.txt (and the log) as a LARECOMP_TELEPORT value - the
// user walks somewhere, presses F8, and a test run starts there.
// LARECOMP_POS_LOG=<n>: the player's position every n frames ("[pos] frame F x y z").
// LARECOMP_TELEPORT=<frame>:<x>,<y>,<z>[,<9 rotation values, by rows>]: from that frame on (0: the first gameplay
// frame), the next gameplay frame teleports the player there (CPlayerMP1::Teleport; without a rotation, facing kept).
// Rooms away from the landing site need LARECOMP_LOAD_AREAS (below) and a later frame.
// With LARECOMP_SKIP_CINEMATICS, a new game's first gameplay frame is ~5600.
static uint64_t g_player, g_player_mgr;

// The actor's world transform: 3x4 rows, the translation in column 3 (CActorMP1::SetTranslation's layout)
static float *player_transform(uint64_t player) {
    uint64_t e = LD64(player + 8), t = e ? LD64(e + 0x20) : 0, m = t ? LD64(t + 0x40) : 0;
    return m ? (float *)(uintptr_t)m : NULL;
}

void rt_player_frame(Cpu *c) {
    g_player = c->x[0];
    g_player_mgr = c->x[1];
    static long every = -2;
    if (every == -2) every = getenv("LARECOMP_POS_LOG") ? atol(getenv("LARECOMP_POS_LOG")) : 0;
    extern uint64_t vk_frame_counter(void);
    uint64_t frame = vk_frame_counter();
    static uint64_t logged = ~0ull;
    float *m = player_transform(g_player);
    if (every > 0 && m && frame % every == 0 && frame != logged) {
        logged = frame;
        LOG("[pos] frame %llu %.2f %.2f %.2f\n", (unsigned long long)frame, m[3], m[7], m[11]);
    }
    extern volatile int g_pos_save_request;
    if (g_pos_save_request && m) {
        g_pos_save_request = 0;
        char line[512];
        snprintf(line, sizeof line, "LARECOMP_TELEPORT=0:%.3f,%.3f,%.3f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f",
                 m[3], m[7], m[11], m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
        FILE *f = fopen("positions.txt", "a");
        if (f) { fprintf(f, "%s   (frame %llu)\n", line, (unsigned long long)frame); fclose(f); }
        LOG("[pos] F8: %s (positions.txt%s)\n", line, f ? "" : ": not writable");
    }
}

// LARECOMP_LOAD_AREAS=<id>,<id>,... (32 hex digits each, as "[area] load" logs them): at the first gameplay frame,
// CGameAreaManager::BeginAreaLoad for each (as the room controllers do when a door opens); the teleport (a later
// LARECOMP_TELEPORT frame) then makes the last one the current area (SetCurrentArea, as walking through its door
// does). A room away from the landing site is neither drawn nor solid otherwise.
static uint32_t g_load_vid;  // (the last requested area's runtime id, BeginAreaLoad's result)

static void debug_load_areas(Cpu *c) {
    static int done;
    const char *e = getenv("LARECOMP_LOAD_AREAS");
    if (done || !e || !g_area_mgr || !g_player) return;
    done = 1;
    static uint8_t *ids;
    if (!ids) ids = hle_alloc(16 * 32);
    int n = 0;
    for (const char *p = e; n < 32 && strlen(p) >= 32; n++) {
        for (int i = 0; i < 16; i++) { unsigned v = 0; sscanf(p + 2 * i, "%2x", &v); ids[16 * n + i] = (uint8_t)v; }
        p += 32;
        if (*p == ',') p++;
    }
    Cpu *saved = malloc(sizeof *saved);
    for (int k = 0; k < n; k++) {
        *saved = *c;
        c->x[0] = g_area_mgr;
        c->x[1] = (uint64_t)(uintptr_t)(ids + 16 * k);
        c->x[2] = 1;  // (the load type the doors use)
        guest_call(c, guest_sym("_ZN16CGameAreaManager13BeginAreaLoadERK9CObjectId13EAreaLoadType"));
        LOG("[area] LARECOMP_LOAD_AREAS: area %d of %d requested (result %llx)\n", k + 1, n, (unsigned long long)c->x[0]);
        g_load_vid = (uint32_t)c->x[0];
        *c = *saved;
    }
    free(saved);
}

// From CStateManager::BeginFrame (rt_debug_autosave): the teleport runs between frames, not inside the player's update
static void debug_teleport(Cpu *c) {
    debug_load_areas(c);
    static long at = -2;
    static float to[3], rot[9];
    static int nrot;
    if (at == -2) {
        const char *e = getenv("LARECOMP_TELEPORT");
        int n = e ? sscanf(e, "%ld:%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &at, &to[0], &to[1], &to[2], &rot[0], &rot[1],
                           &rot[2], &rot[3], &rot[4], &rot[5], &rot[6], &rot[7], &rot[8]) : 0;
        if (n != 4 && n != 13) at = -1;
        nrot = n == 13 ? 9 : 0;
    }
    extern uint64_t vk_frame_counter(void);
    if (at < 0 || (long)vk_frame_counter() < at || !g_player) return;
    float *m = player_transform(g_player);
    if (!m) return;
    Cpu *saved = malloc(sizeof *saved);
    static int made_current;
    if (g_load_vid && g_area_mgr && !made_current) {
        // first the room becomes current (its neighbours load, its collision comes up), the teleport 2 s later: in
        // the same frame the player fell through the floor
        made_current = 1;
        static uint32_t *vid;
        if (!vid) vid = hle_alloc(16);
        *vid = g_load_vid;
        *saved = *c;
        c->x[0] = g_area_mgr;
        c->x[1] = g_player_mgr;
        c->x[2] = (uint64_t)(uintptr_t)vid;
        guest_call(c, guest_sym("_ZN16CGameAreaManager14SetCurrentAreaER13CStateManagerRK15CValueVersionIdIjttLj16ELj16EE"));
        *c = *saved;
        free(saved);
        LOG("[area] current area set to %x, teleport in 120 frames\n", g_load_vid);
        at = (long)vk_frame_counter() + 120;
        return;
    }
    at = -1;
    static float *xf;
    if (!xf) xf = hle_alloc(64);
    memcpy(xf, m, 48);
    LOG("[pos] teleport from %.2f %.2f %.2f to %.2f %.2f %.2f\n", m[3], m[7], m[11], to[0], to[1], to[2]);
    xf[3] = to[0], xf[7] = to[1], xf[11] = to[2];
    for (int r = 0; r < 3 && nrot; r++) for (int k = 0; k < 3; k++) xf[4 * r + k] = rot[3 * r + k];
    *saved = *c;
    c->x[0] = g_player;
    c->x[1] = (uint64_t)(uintptr_t)xf;
    c->x[2] = g_player_mgr;
    c->x[3] = 0;
    guest_call(c, guest_sym("_ZN10CPlayerMP18TeleportERK12CTransform4fR13CStateManagerb"));
    *c = *saved;
    free(saved);
}

// The window may show more than 16:9 (Video tab, aspect ratio unlocked). That needs the game's cameras widened,
// which isn't done for this game yet: always the original framing.
bool widescreen_allowed(void) { return false; }

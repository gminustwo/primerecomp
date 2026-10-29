// What the runtime asks about the running game: the renderers' frame logs and the window's framing. Link's
// Awakening's answers (its level name from PlayerStatus, widescreen framing per room) went with the rest of its game
// code (git history: runtime/hle/widescreen.c); Metroid Prime Remastered's belong here.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"
#include "../platform.h"
#include "../settings.h"

int g_start_screen;  // runtime/ui/launcher.mm: the start screen is up - before the first boot nothing is loaded

// Current area, for the logs ("-": not known)
const char *game_level_name(void) { return "-"; }

// Frame rate (Video tab "Frame rate", LARECOMP_FPS): the frames presented per second and the game's simulation rate.
// The game steps a fixed 1/60 s per simulation tick (CGameArchitectureSupport::Update; rt_frame_step below says how
// much time a frame adds); at 120 every place that builds that 1/60 builds 1/120 instead (tools/patches.py
// STEP_SITES), so the game runs at the same speed with twice the steps. 30: a frame every 1/30 s, two ticks each
// (slower machines: a steady 30 at full speed). (No uncapped rate since 2026-10-27, at the user's request: the game
// runs a tick every frame however short the frame was, so one needed a tick of each frame's own time.)
int game_frame_rate(void) {
    static int env = -2;
    if (env == -2) {
        int e = getenv("LARECOMP_FPS") ? atoi(getenv("LARECOMP_FPS")) : -1;
        env = e == 30 || e == 60 || e == 120 ? e : -1;
    }
    return env > 0 ? env : g_settings.frame_rate;
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
static _Atomic uint64_t g_ticks_total, g_sim_ns;  // (the stats line's simulation rate and speed: exec.c renderer_stats)
void rt_tick_body(void) {
    g_ticks++, g_ticks_total++;
    uint32_t b = rt_sim_step_bits(0);
    float step;
    memcpy(&step, &b, 4);
    g_sim_ns += (uint64_t)(step * 1e9f);
}
uint64_t game_ticks_total(void) { return g_ticks_total; }
uint64_t game_sim_ns(void) { return g_sim_ns; }  // the game time the simulation has advanced
unsigned rt_tick_pass(void) {
    g_tick_passes++;
    return rt_sim_step_bits(1);
}

// CGameArchitectureSupport::Update adds the time its frame took, but at most what this returns, then runs one tick
// per whole tick of time it has (0x70003c8234). The game's own limit is one tick: a frame slower than 1/60 s put the
// game in slow motion (25 fps: 42% speed). Now up to 3 ticks (full speed down to 20 fps), and the time is snapped to
// whole ticks when it's within a quarter tick of them: a steady 60 (or 30) keeps exactly one (two) ticks a frame
// through the usual jitter instead of now and then none or two. (s8 is the time measured; set to the result too, as
// the game takes the smaller of the two.) LARECOMP_MAX_TICKS=1: the game's own limit.
static int g_frame_ticks_max = -1;
static unsigned frame_time(Cpu *c) {
    if (g_frame_ticks_max < 0) {
        const char *e = getenv("LARECOMP_MAX_TICKS");
        g_frame_ticks_max = e && atoi(e) >= 1 ? atoi(e) : 3;
    }
    float tick, el, t;
    memcpy(&el, &c->v[8], 4);
    uint32_t tb = rt_sim_step_bits(0);
    memcpy(&tick, &tb, 4);
    if (!(el >= 0)) el = tick;  // (NaN, a clock going back)
    float n = roundf(el / tick);
    t = n >= 1 && fabsf(el - n * tick) < 0.25f * tick ? n * tick : el;
    if (t > g_frame_ticks_max * tick) t = g_frame_ticks_max * tick;
    uint32_t b;
    memcpy(&b, &t, 4);
    c->v[8] = (__uint128_t)b;
    return b;
}

// The update thread's work per frame (the FPS overlay: runtime/nvn/vulkan/exec.c vk_pace_frame): its time between
// updates, less its time blocked in waits and sleeps (kernel.c g_blocked_ns)
static _Atomic uint64_t g_update_busy_ns;
uint64_t game_update_busy_ns(void) { return g_update_busy_ns; }

unsigned rt_frame_step(Cpu *c) {
    {
        extern __thread uint64_t g_blocked_ns;
        static uint64_t last, last_blocked;
        uint64_t now = plat_now_ns(), p = now - last, b = g_blocked_ns - last_blocked;
        if (last && p < 250000000ull) g_update_busy_ns += p > b ? p - b : 0;
        last = now, last_blocked = g_blocked_ns;
    }
    static long from = -2, left;
    if (from == -2) {
        const char *e = getenv("LARECOMP_TICK_LOG");
        if (!e || sscanf(e, "%ld:%ld", &from, &left) != 2) from = -1;
    }
    static int once;
    if (!once++) {
        extern KThread *kernel_current_thread(void);
        KThread *t = kernel_current_thread();
        LOG("[pace] the game's update runs on thread %x\n", t ? t->handle : 0);
    }
    float el;
    memcpy(&el, &c->v[8], 4);
    unsigned r = frame_time(c);
    extern uint64_t renderer_present_count(void);
    if (from >= 0 && left > 0 && (long)renderer_present_count() >= from) {
        left--;
        uint32_t sb = rt_sim_step_bits(0);
        float step;
        memcpy(&step, &sb, 4);
        LOG("[tick] frame %llu elapsed %.3f ms, step %.3f ms, %.3f ms after the present, tick loop passes before %d, "
            "ticks %d\n", (unsigned long long)renderer_present_count(), el * 1000, step * 1000,
            (double)(plat_now_ns() - g_present_ns) / 1e6, g_tick_passes, g_ticks);
    }
    g_tick_passes = g_ticks = 0;
    return r;
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
    static uint64_t next;
    uint64_t period = (uint64_t)(1e9 * (interval > 0 ? interval : 1) / fps), now = plat_now_ns();
    if (!next || now > next + 4 * period) next = now;
    else plat_sleep_until_ns(next);
    g_present_ns = plat_now_ns();
    // (idle, not work: the frame pacing's busy time - runtime/nvn/vulkan/exec.c vk_pace_frame - leaves it out)
    extern __thread uint64_t g_blocked_ns;
    g_blocked_ns += g_present_ns - now;
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

// Field of view (Video tab, g_settings.fov): the first-person camera's vertical FOV, 55 degrees in the game (86
// horizontal at 16:9). The game takes its default from one float (CCameraManagerMP1::GetDefaultFirstPersonVerticalFOV,
// unnamed data): the first-person camera when it's made, the morph ball camera on its way back to it, animated cameras
// handing back. The camera already made keeps its own (CGameCameraMP1: +0x25c the FOV now, +0x280 / +0x284 an
// interpolation's start / target, +0x278 its time left, +0x270 bit 0 "changed"): that one is set as well while it's
// the current camera and not interpolating, unless the game aimed it somewhere of its own.
#define FP_FOV_DEFAULT 0x70020e7a70ull
static uint64_t g_player_seen;  // the frame the player last updated (rt_player_frame; 0: not yet)

int rt_fov(void) {  // (LARECOMP_FOV: test override)
    static int env = -2;
    if (env == -2) env = getenv("LARECOMP_FOV") ? atoi(getenv("LARECOMP_FOV")) : -1;
    int f = env > 0 ? env : g_settings.fov;
    return f < FOV_MIN ? FOV_MIN : f > FOV_MAX ? FOV_MAX : f;
}

// The game is in play: the player updated just now (rt_player_frame). Not in its pause menu, map, title screen or
// loading (runtime/hle/input.c: keys mean menu keys then, and the mouse is let go).
bool game_in_play(void) {
    extern uint64_t vk_frame_counter(void);
    return g_player_seen && vk_frame_counter() <= g_player_seen + 2;
}

#define GUEST_PTR_OK(p) ((p) >= 0x7000000000ull && (p) < 0x7f00000000ull && !((p) & 7))

// The in-game GUI's screen (NInGameGuiStatesMP1::EInGameGuiState; 0: no game running). CInGameGuiManagerMP1 keeps the
// screen it's on or going to at +0x2e4 and the one before at +0x2e0 (BeginStateTransition; IsGamePaused: either
// above 1, in play). The manager is CGameIOWinMP1's +0x48, noted as CGameIOWinMP1::FrameBegin runs each frame: a
// virtual call, so the runtime steps in at dispatch (hle_override) - no patch, the game code players built stays.
static uint64_t g_gui_mgr, g_gui_seen;
static GuestFn g_frame_begin;

RT_ON_RESET(gui_reset) { g_gui_mgr = g_gui_seen = 0; }

static void rt_frame_begin(Cpu *c) {
    extern uint64_t vk_frame_counter(void);
    uint64_t m = LD64(c->x[0] + 0x48);
    g_gui_mgr = GUEST_PTR_OK(m) ? m : 0;
    g_gui_seen = vk_frame_counter();
    if (g_gui_mgr) {
        static uint32_t was[2] = {~0u, ~0u};
        uint32_t s[2] = {LD32(g_gui_mgr + 0x2e0), LD32(g_gui_mgr + 0x2e4)};
        if (s[0] != was[0] || s[1] != was[1])
            LOG("[gui] frame %llu: screen %u (from %u)\n", (unsigned long long)g_gui_seen, s[1], s[0]);
        was[0] = s[0], was[1] = s[1];
    }
    g_frame_begin(c);
}

void game_hooks_init(void) {  // (each boot, after dispatch_init)
    uint64_t a = guest_sym("_ZN13CGameIOWinMP110FrameBeginER18CArchitectureQueue");
    static bool hooked;
    g_frame_begin = dispatch_lookup(a);
    if (g_frame_begin && !hooked) hle_override(a, rt_frame_begin, "CGameIOWinMP1::FrameBegin"), hooked = true;
}

// The screen now and the one before (0 0: no game)
void game_gui_screen(int *now, int *before) {
    extern uint64_t vk_frame_counter(void);
    *now = *before = 0;
    if (!g_gui_mgr || vk_frame_counter() > g_gui_seen + 2) return;
    *now = (int)LD32(g_gui_mgr + 0x2e4), *before = (int)LD32(g_gui_mgr + 0x2e0);
}

// The first-person camera when it's the current camera (not a cutscene's, the morph ball's...), else 0. Only in play
// (at boot the state manager has no cameras yet, and looking one up crashed), found as the game does: the camera
// manager, its current camera's id (the top of its two override stacks, else its own),
// CCameraManagerMP1::GetCurrentCamera. The camera manager hangs off the state manager's game logic (+0x4e0150) - when
// that is MP1's: between worlds it can be CStateManagerGameLogicDefault, whose +0x30 isn't a pointer (a crash as
// gameplay began after a skipped cutscene). Every register is put back.
static uint64_t first_person_camera(Cpu *c, uint64_t sm) {
    if (!game_in_play()) return 0;
    uint64_t logic = GUEST_PTR_OK(sm) ? LD64(sm + 0x4e0150) : 0;
    if (!GUEST_PTR_OK(logic) || LD64(logic) != guest_sym("_ZTV25CStateManagerGameLogicMP1") + 0x10) return 0;
    uint64_t mgr = LD64(logic + 0x30);
    if (!GUEST_PTR_OK(mgr)) return 0;
    uint32_t n2 = LD32(mgr + 0x18), n1 = LD32(mgr + 0x8);
    uint64_t stack = n2 ? LD64(mgr + 0x20) : n1 ? LD64(mgr + 0x10) : 0;
    if (n2 > 64 || n1 > 64 || ((n2 || n1) && !GUEST_PTR_OK(stack & ~7ull))) return 0;
    uint32_t id = n2 ? LD32(stack + 4ull * (n2 - 1)) : n1 ? LD32(stack + 4ull * (n1 - 1)) : LD32(mgr);
    if (id == 0xffffffffu) return 0;
    Cpu *saved = malloc(sizeof *saved);
    *saved = *c;
    c->x[0] = mgr;
    c->x[1] = sm;
    guest_call(c, guest_sym("_ZNK17CCameraManagerMP116GetCurrentCameraERK13CStateManager"));
    uint64_t cam = c->x[0];
    *c = *saved;
    free(saved);
    return GUEST_PTR_OK(cam) && LD64(cam) == guest_sym("_ZTV21CFirstPersonCameraMP1") + 0x10 ? cam : 0;
}

static void fov_frame(Cpu *c) {
    static int ok = -1;
    float *def = (float *)(uintptr_t)FP_FOV_DEFAULT;
    if (ok < 0 && !(ok = *def == 55.0f)) LOG("[fov] the default FOV isn't 55 (%g): another game build? FOV left alone\n", *def);
    if (!ok) return;
    float want = (float)rt_fov();
    static float applied = 55.0f;
    *def = want;
    uint64_t cam = first_person_camera(c, c->x[0]);
    if (!cam) return;
    float *now = (float *)(uintptr_t)(cam + 0x25c), *from = (float *)(uintptr_t)(cam + 0x280);
    float *target = (float *)(uintptr_t)(cam + 0x284), *left = (float *)(uintptr_t)(cam + 0x278);
    if (*left <= 0 && *target != want && (*target == 55.0f || *target == applied)) {
        LOG("[fov] first-person camera %llx: %g -> %g\n", (unsigned long long)cam, *target, want);
        *now = *from = *target = want;
        *(uint8_t *)(uintptr_t)(cam + 0x270) |= 1;
    }
    applied = want;
}

static void wide_frame(Cpu *c);

void rt_debug_autosave(Cpu *c) {
    debug_teleport(c);
    fov_frame(c);
    wide_frame(c);
    // Every cutscene skippable, as on a second playthrough (CGameState::mCinematicForceSkippableOverride, which
    // IsCinematicSkippable checks first): the Gameplay tab's "Skip any cutscene", or (debug) LARECOMP_SKIP_CINEMATICS=1,
    // which also skips them (rt_skip_cinematics) - the intro replay reaches the landing site at frame ~5000 instead of
    // ~12000. Written each frame while on; turning it off puts the game's 0 back once.
    {
        static int was;
        int on = rt_skip_cinematics() || g_settings.skippable_cutscenes;
        if (on || was) *(uint8_t *)(uintptr_t)guest_sym("_ZN10CGameState32mCinematicForceSkippableOverrideE") = (uint8_t)on;
        was = on;
    }
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
// LARECOMP_TELEPORT=<frame>:<x>,<y>,<z>[,<9 rotation values, by rows> | @<yaw degrees>]: from that frame on (0: the
// first gameplay frame), the next gameplay frame teleports the player there (CPlayerMP1::Teleport; without a
// rotation, facing kept). Several, separated by ';', run in turn (viewpoints in one run: walk into a room through its
// door - it is then lit as in play - and look around it). Rooms away from the landing site need LARECOMP_LOAD_AREAS
// (below) and a later frame.
// With LARECOMP_SKIP_CINEMATICS, a new game's first gameplay frame is ~5600.
static uint64_t g_player, g_player_mgr;

// The actor's world transform: 3x4 rows, the translation in column 3 (CActorMP1::SetTranslation's layout)
static float *player_transform(uint64_t player) {
    uint64_t e = LD64(player + 8), t = e ? LD64(e + 0x20) : 0, m = t ? LD64(t + 0x40) : 0;
    return m ? (float *)(uintptr_t)m : NULL;
}

static void mouse_look(Cpu *c);

void rt_player_frame(Cpu *c) {
    g_player = c->x[0];
    g_player_mgr = c->x[1];
    static long every = -2;
    if (every == -2) every = getenv("LARECOMP_POS_LOG") ? atol(getenv("LARECOMP_POS_LOG")) : 0;
    extern uint64_t vk_frame_counter(void);
    uint64_t frame = vk_frame_counter();
    g_player_seen = frame;
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
    mouse_look(c);
}

// ---- Mouse aim (Controls tab "Mouse aim"; runtime/hle/input.c sums the mouse's motion while it's captured)
// As in a PC shooter the mouse turns Samus and tilts the view itself, instead of steering the right stick (whose
// turning eases in and tops out: a flick would lag behind the hand). Yaw turns the player's transform about the
// world's up axis (y) through CActorMP1::SetTransform, which also tells whoever follows it; pitch is the first-person
// look angle, the player's current (+0x5cc) and desired (+0x5d0) pitch in radians (up positive), both set
// (CPlayerMP1::UpdateFreeLook eases the first toward the second) along with the filter the game smooths the desired
// pitch through ([+0x1628], a CFIRFilter or CIIRFilter: NSharedPlayerControls::UpdateFreeLookDesiredUp; without its
// ResetTo the filter averaged a mouse move down to a fourteenth). Only on foot (+0x4d4, the morph ball state, 0 = unmorphed:
// CPlayerMP1::GetMorphBallTransitionState), not locked on (+0x4e0, the orbit state: CPlayerMP1::SetOrbitState), while
// the game takes turn input at all (+0x1610 bit 0 clear: CPlayerMP1::TurnInput) and with the first-person camera
// current (not in cutscenes). Otherwise - the morph ball's camera, lock-on - the motion steers the right stick.
// Called from CPlayerMP1::UpdateCameraBob (rt_player_frame): the player in x0, the state manager in x1.
// LARECOMP_LOOK_LOG=<n>: the look state every n frames.
#define MOUSE_DEG_PER_COUNT 0.044f  // (at sensitivity 1)
#define PITCH_LIMIT 1.2217f         // 70 degrees up or down: the game's own free-look range (the stick stops there)

static void mouse_look(Cpu *c) {
    extern bool input_mouse_take(float *dx, float *dy);
    extern void input_mouse_stick(int32_t rx, int32_t ry);
    uint64_t player = c->x[0], sm = c->x[1];
    float *pitch = (float *)(uintptr_t)(player + 0x5cc), *pitch_to = (float *)(uintptr_t)(player + 0x5d0);
    uint32_t morph = LD32(player + 0x4d4), orbit = LD32(player + 0x4e0);
    uint8_t no_turn = *(uint8_t *)(uintptr_t)(player + 0x1610) & 1;
    static long log_every = -2;
    if (log_every == -2) log_every = getenv("LARECOMP_LOOK_LOG") ? atol(getenv("LARECOMP_LOOK_LOG")) : 0;
    extern uint64_t vk_frame_counter(void);
    if (log_every > 0 && vk_frame_counter() % log_every == 0) {
        float *m = player_transform(player);
        LOG("[look] frame %llu pitch %.4f -> %.4f morph %u orbit %u no_turn %u row0 %.3f %.3f %.3f row2 %.3f %.3f %.3f\n",
            (unsigned long long)vk_frame_counter(), *pitch, *pitch_to, morph, orbit, no_turn, m ? m[0] : 0, m ? m[1] : 0,
            m ? m[2] : 0, m ? m[8] : 0, m ? m[9] : 0, m ? m[10] : 0);
    }
    float dx, dy;
    if (!input_mouse_take(&dx, &dy) || !g_settings.mouse_aim) { input_mouse_stick(0, 0); return; }
    float deg = MOUSE_DEG_PER_COUNT * g_settings.mouse_sens;
    if (g_settings.mouse_invert) dy = -dy;
    if (morph != 0 || orbit != 0 || no_turn || !first_person_camera(c, sm)) {
        // the right stick instead, in proportion to the motion this frame (~25 counts a frame: full tilt)
        float k = 32767.0f / 25 * g_settings.mouse_sens;
        float rx = dx * k, ry = -dy * k;
        input_mouse_stick((int32_t)fmaxf(-32767, fminf(32767, rx)), (int32_t)fmaxf(-32767, fminf(32767, ry)));
        return;
    }
    input_mouse_stick(0, 0);
    if (dy) {
        float p = *pitch - dy * deg * (float)M_PI / 180;
        p = p > PITCH_LIMIT ? PITCH_LIMIT : p < -PITCH_LIMIT ? -PITCH_LIMIT : p;
        *pitch = *pitch_to = p;
        uint64_t filt = LD64(player + 0x1628), vt = GUEST_PTR_OK(filt) ? LD64(filt) : 0;
        const char *reset = vt == guest_sym("_ZTV10CFIRFilter") + 0x10 ? "_ZN10CFIRFilter7ResetToEf" :
                            vt == guest_sym("_ZTV10CIIRFilter") + 0x10 ? "_ZN10CIIRFilter7ResetToEf" : NULL;
        static int logged;
        if (!logged++) LOG("[look] the desired pitch's filter: %s\n", reset ? reset : "unknown (pitch may lag)");
        if (reset) {
            Cpu *saved = malloc(sizeof *saved);
            *saved = *c;
            uint32_t bits;
            memcpy(&bits, &p, 4);
            c->x[0] = filt;
            c->v[0] = bits;
            guest_call(c, guest_sym(reset));
            *c = *saved;
            free(saved);
        }
    }
    float *m = player_transform(player);
    if (!dx || !m) return;
    float a = -dx * deg * (float)M_PI / 180, co = cosf(a), s = sinf(a);
    static float *xf;
    if (!xf) xf = hle_alloc(64);
    memcpy(xf, m, 48);
    for (int k = 0; k < 3; k++) {  // turned about the world's y: rows 0 and 2 of the rotation
        float r0 = m[k], r2 = m[8 + k];
        xf[k] = co * r0 + s * r2;
        xf[8 + k] = -s * r0 + co * r2;
    }
    Cpu *saved = malloc(sizeof *saved);
    *saved = *c;
    c->x[0] = player;
    c->x[1] = (uint64_t)(uintptr_t)xf;
    guest_call(c, guest_sym("_ZN9CActorMP112SetTransformERK12CTransform4f"));
    *c = *saved;
    free(saved);
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
    enum { MAXTP = 32 };
    static struct { long at; float to[3], rot[9]; int nrot; } tp[MAXTP];
    static int ntp = -1, cur;
    if (ntp < 0) {
        ntp = 0;
        for (const char *e = getenv("LARECOMP_TELEPORT"); e && *e && ntp < MAXTP;) {
            float *t = tp[ntp].to, *r = tp[ntp].rot, yaw;
            int n = sscanf(e, "%ld:%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &tp[ntp].at, &t[0], &t[1], &t[2], &r[0], &r[1],
                           &r[2], &r[3], &r[4], &r[5], &r[6], &r[7], &r[8]);
            const char *at_sign = strchr(e, '@'), *semi = strchr(e, ';');
            if (n == 4 && at_sign && (!semi || at_sign < semi) && sscanf(at_sign + 1, "%f", &yaw) == 1) {
                float s = sinf(yaw * (float)M_PI / 180), co = cosf(yaw * (float)M_PI / 180);
                float m[9] = {co, 0, s, 0, 1, 0, -s, 0, co};  // (rows, as F8 writes them)
                memcpy(r, m, sizeof m);
                n = 13;
            }
            if (n == 4 || n == 13) tp[ntp].nrot = n == 13 ? 9 : 0, ntp++;
            e = semi ? semi + 1 : NULL;
        }
    }
    extern uint64_t vk_frame_counter(void);
    if (cur >= ntp || (long)vk_frame_counter() < tp[cur].at || !g_player) return;
    long *at_p = &tp[cur].at;
    const float *to = tp[cur].to, *rot = tp[cur].rot;
    int nrot = tp[cur].nrot;
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
        *at_p = (long)vk_frame_counter() + 120;
        return;
    }
    cur++;
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

// Ultrawide (Video tab "Aspect ratio": Unlocked): a window wider than 16:9 widens the render targets (the renderers'
// output_scale reshape: 16:9 targets drawn wider) and the game's projection. CDisplayManager::UpdateCameraNodes
// builds the camera's projection every frame from an aspect ratio at [[display manager + 0x10] + 0x98] (16:9), which
// GetAspectRatioForVisibility (the culling frustum) and CTweakPlayerMP1::GetAspectRatioScalarForAimAndOrbit read too:
// wide_frame sets it to the window's each gameplay frame. LARECOMP_NO_WIDE=1: always 16:9.
bool widescreen_allowed(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_WIDE") != NULL;
    return g_settings.aspect == ASPECT_UNLOCKED && !off;
}

static void wide_frame(Cpu *c) {
    extern double output_scale_shape(void);  // runtime/nvn/output_scale.c: the window's shape / 16:9 (1: not reshaped)
    uint64_t sm = c->x[0], dm = GUEST_PTR_OK(sm) ? LD64(sm + 0x4e0168) : 0;
    if (!GUEST_PTR_OK(dm) || LD64(dm) != guest_sym("_ZTV15CDisplayManager") + 0x10) return;
    uint64_t ps = LD64(dm + 0x10);
    if (!GUEST_PTR_OK(ps)) return;
    float *aspect = (float *)(uintptr_t)(ps + 0x98);
    static float base;  // (the game's own: 16:9)
    if (!base) {
        if (*aspect < 1.0f || *aspect > 3.0f) return;
        base = *aspect;
        LOG("[wide] the game's aspect ratio: %g\n", base);
    }
    float want = (float)(base * (widescreen_allowed() ? output_scale_shape() : 1.0));
    if (*aspect != want) {
        static float logged;
        if (want != logged) LOG("[wide] projection aspect %g -> %g\n", *aspect, want), logged = want;
        *aspect = want;
    }
}

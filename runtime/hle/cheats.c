// Cheats (the menu's Cheats tab): the hint timer, infinite energy, missiles and power bombs. All from the runtime, on
// the game's own data: the game code players built stays as it is (no patch, no new game code id).
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

#define GUEST_PTR_OK(p) ((p) >= 0x7000000000ull && (p) < 0x7f00000000ull && !((p) & 7))

// The player's state (CPlayerStateMP1): the game state (gpGameState, CGameStateMP1) holds it at +0x20 as an rc_ptr
// (CGameStateMP1::GetPlayerState). In it: +0x00 bit 0 alive (CPlayerStateMP1::Kill clears it), +0x0c the energy
// (CHealthInfoMP1: the energy, then the knockback resistance), +0x2c + 8 * item an item's amount and +0x30 + 8 * item
// its capacity (GetItemAmount / GetItemCapacity). Items (CPlayerStateMP1::EItemType): 4 missiles, 7 power bombs, 24
// energy tanks; the full energy is 99 + 100 per tank (GetMaxHealth).
enum { ITEM_MISSILES = 4, ITEM_POWER_BOMBS = 7, ITEM_ENERGY_TANKS = 24 };

static uint64_t game_state(void) {
    uint64_t gs = LD64(guest_sym("gpGameState"));
    return GUEST_PTR_OK(gs) ? gs : 0;
}

static uint64_t player_state(void) {
    uint64_t gs = game_state(), ps = gs ? LD64(gs + 0x20) : 0;
    return GUEST_PTR_OK(ps) ? ps : 0;
}

static float full_energy(uint64_t ps) { return (float)(int32_t)LD32(ps + 0x2c + 8 * ITEM_ENERGY_TANKS) * 100 + 99; }

static void refill(uint64_t ps, int item) {
    int32_t *amount = (int32_t *)(uintptr_t)(ps + 0x2c + 8 * item), cap = amount[1];
    if (cap > 0 && *amount < cap) *amount = cap;
}

// ---- Infinite energy
// Damage takes the energy off in CStateManagerGameLogicMP1::ApplyLocalDamage: the damagee's CHealthInfo from its
// virtual HealthInfo (for Samus CPlayerMP1::HealthInfo: the player state's +0x0c), less the damage; Samus dies when it
// reaches 0. A virtual call goes through dispatch, so the runtime steps in there (hle_override): called from
// ApplyLocalDamage (its return address) with the cheat on, HealthInfo hands over a stand-in with plenty of energy
// instead of Samus' own. The hit still lands - the flash, the sound, the knockback - and her energy stays where it was,
// even for a hit bigger than it. Everything else that asks (the HUD, ApplyDamage before and after) gets her own.
// cheats_frame also keeps the energy full, for anything that takes it off some other way.
#define APPLY_LOCAL_DAMAGE_RET 0x7000c62dd4ull  // (ApplyLocalDamage, just after its call to the damagee's HealthInfo)
static GuestFn g_health_info;
static float *g_stand_in;  // (guest memory: the game writes the reduced energy there)

RT_ON_RESET(cheats_reset) { g_stand_in = NULL; }

static void rt_player_health_info(Cpu *c) {
    uint64_t from = c->x[30];
    g_health_info(c);
    if (!g_settings.inf_energy || from != APPLY_LOCAL_DAMAGE_RET || !GUEST_PTR_OK(c->x[0] & ~7ull)) return;  // (+0xc)
    if (!g_stand_in) g_stand_in = hle_alloc(16);
    const float *own = (const float *)(uintptr_t)c->x[0];
    g_stand_in[0] = 10000.0f;  // (small enough that a 0.01 tick of heat damage still counts as a hit: float steps)
    g_stand_in[1] = own[1];
    c->x[0] = (uint64_t)(uintptr_t)g_stand_in;
}

void cheats_hooks_init(void) {  // (each boot, from game_hooks_init)
    uint64_t a = guest_sym("_ZN10CPlayerMP110HealthInfoER13CStateManager");
    static bool hooked;
    g_health_info = dispatch_lookup(a);
    if (g_health_info && !hooked) hle_override(a, rt_player_health_info, "CPlayerMP1::HealthInfo"), hooked = true;
}

// ---- Hint timer
// The hint system (the game's Options -> Hint System: the game state's +0x40 + 0x10e, CGameOptionsMP1::
// SetIsRedundantHintSystem) keeps a state per hint in CRedundantHintManagerMP1, the game state's +0x2a0: +0 their
// count, +8 the states (12 bytes each: +0 0 not yet / 1 counting down / 2 shown / 3 done, +4 the time left, +8 a
// second timer), +0x10 the hint shown now. The hints themselves are the universe's CRedundantHintMP1s
// (CUniverseInfoMP1::GetRedundantHintSet: +0 their count, +8 the hints, 0x38 bytes each: +0x00 the name, a
// rstl::string - its characters at +0, its length at +0x10 -, +0x18 the time it counts down from, +0x1c the time
// before it's shown again, +0x20 its text, +0x24 the time its text shows, 3 s a page, +0x28 the rooms it points to).
// CRedundantHintManagerMP1::Update, each frame:
//  1 (IncrementHint: a script found the player needs it): the time counts down from the hint's +0x18; at 0 it's shown
//    (state 2, the time = its text's time).
//  2: the time counts down to 0, the text shows (CStateManagerGameLogicMP1::UpdateRedundantHintSystem: a HUD memo, its
//    room marked on the map) while it's under the text's time. The player reaching that room, or the text done (the
//    second timer, 20 s), sets the time to the hint's +0x1c: it counts down again and the hint shows again.
// The game's waits are long: 15 to 40 minutes before most hints first show (a few right away: 0 or 9 s), 30 minutes
// before one shows again (the artifacts' 60). The hint timer (g_settings.hint_seconds, 0 = the game's own) cuts both
// waits down to it, those already under way too; shorter ones stay as they are.
// LARECOMP_LOG_HINTS=1: the hints and their times, once, and each hint's changes ("[hint]").
static uint64_t hint_set(void) {
    uint64_t ui = LD64(guest_sym("gpUniverseInfo")), tok = GUEST_PTR_OK(ui) ? LD64(ui) : 0;
    uint64_t set = GUEST_PTR_OK(tok) ? LD64(tok + 0x10) : 0;  // (GetRedundantHintSet's cached pointer: loaded at boot)
    return GUEST_PTR_OK(set) ? set : 0;
}

static void hint_name(uint64_t hint, char *out, size_t cap) {
    uint64_t s = LD64(hint), n = LD64(hint + 0x10);
    if (!GUEST_PTR_OK(s & ~7ull) || n >= cap) n = 0;
    if (n) memcpy(out, (const void *)(uintptr_t)s, n);
    out[n] = 0;
}

enum { MAX_HINTS = 64 };

static void hint_frame(uint64_t gs) {
    static int log = -1;
    if (log < 0) log = getenv("LARECOMP_LOG_HINTS") && atoi(getenv("LARECOMP_LOG_HINTS"));
    uint64_t mgr = gs + 0x2a0, set = hint_set(), states = LD64(mgr + 8), hints = set ? LD64(set + 8) : 0;
    int32_t n = (int32_t)LD32(mgr);
    if (!set || !GUEST_PTR_OK(states) || !GUEST_PTR_OK(hints) || n <= 0 || n > MAX_HINTS || n != (int32_t)LD32(set))
        return;
    static bool listed;
    if (log && !listed) {
        listed = true;
        LOG("[hint] %d hints (hint system %s)\n", n, LD8(gs + 0x14e) ? "on" : "off");
        for (int i = 0; i < n; i++) {
            uint64_t h = hints + 0x38ull * i;
            char name[64];
            hint_name(h, name, sizeof name);
            float *t = (float *)(uintptr_t)(h + 0x18);
            LOG("[hint] %2d %-40s first %6.0f s, again %6.0f s, text %4.1f s, %d rooms\n", i, name, t[0], t[1], t[3],
                (int32_t)LD32(h + 0x28));
        }
    }
    static struct { uint32_t state; float t; } was[MAX_HINTS];  // ((log) each hint a frame ago)
    static uint64_t was_states;
    bool fresh = was_states != states;  // (another game state or a loaded save)
    was_states = states;
    float wait = (float)g_settings.hint_seconds;
    for (int i = 0; i < n; i++) {
        uint32_t st = LD32(states + 12ull * i);
        float *t = (float *)(uintptr_t)(states + 12ull * i + 4);
        float text = *(const float *)(uintptr_t)(hints + 0x38ull * i + 0x24);
        if (log && fresh && (st == 1 || st == 2)) LOG("[hint] %d: state %u, time %.1f (as loaded)\n", i, st, *t);
        else if (log && !fresh && (st != was[i].state || *t > was[i].t + 0.5f))
            LOG("[hint] %d: state %u -> %u, time %.1f -> %.1f\n", i, was[i].state, st, was[i].t, *t);
        if (wait > 0 && st == 1 && *t > wait) *t = wait;
        else if (wait > 0 && st == 2 && *t > text + wait) *t = text + wait;
        was[i].state = st, was[i].t = *t;
    }
}

// (debug) LARECOMP_DAMAGE=<frame>:<amount>[,<frame>:<amount>...]: from that frame on, the next gameplay frame hits
// Samus for <amount> through ApplyLocalDamage, as an enemy's shot would (a power beam hit without knockback) - Infinite
// energy tested without an enemy around. Her energy before and after goes to the log ("[cheat]").
#define GAME_LOGIC_OFF 0x4e0150ull  // (the state manager's CStateManagerGameLogicMP1)
static float *g_hit;  // (guest memory: the hit's position, direction and CDamageInfoMP1)

RT_ON_RESET(cheats_debug_reset) { g_hit = NULL; }

static void debug_damage(Cpu *c, uint64_t ps) {
    enum { MAXHITS = 16 };
    static struct { long at; float amount; } hit[MAXHITS];
    static int nhit = -1, cur;
    if (nhit < 0) {
        nhit = 0;
        for (const char *e = getenv("LARECOMP_DAMAGE"); e && *e && nhit < MAXHITS;) {
            if (sscanf(e, "%ld:%f", &hit[nhit].at, &hit[nhit].amount) == 2) nhit++;
            e = strchr(e, ',');
            e = e ? e + 1 : NULL;
        }
    }
    extern uint64_t vk_frame_counter(void);
    extern uint64_t game_player(void);
    uint64_t sm = c->x[0], logic = GUEST_PTR_OK(sm) ? LD64(sm + GAME_LOGIC_OFF) : 0, player = game_player();
    if (cur >= nhit || (long)vk_frame_counter() < hit[cur].at || !GUEST_PTR_OK(player) || !GUEST_PTR_OK(logic) ||
        LD64(logic) != guest_sym("_ZTV25CStateManagerGameLogicMP1") + 0x10)
        return;
    if (!g_hit) g_hit = hle_alloc(64);
    memset(g_hit, 0, 64);
    g_hit[4] = 1;  // (the direction: x)
    g_hit[9] = g_hit[10] = hit[cur].amount;  // (CDamageInfoMP1: +0 the weapon, 0 the power beam; +4 / +8 the damage)
    float before = *(const float *)(uintptr_t)(ps + 0xc);
    Cpu *saved = malloc(sizeof *saved);
    *saved = *c;
    c->x[0] = logic;
    c->x[1] = (uint64_t)(uintptr_t)g_hit;
    c->x[2] = (uint64_t)(uintptr_t)(g_hit + 4);
    c->x[3] = player;
    uint32_t bits;
    memcpy(&bits, &hit[cur].amount, 4);
    c->v[0] = bits;
    c->x[4] = (uint64_t)(uintptr_t)(g_hit + 8);
    c->x[5] = 0xffffffffu;  // (no damager)
    c->x[6] = 0;
    guest_call(c, guest_sym("_ZN25CStateManagerGameLogicMP116ApplyLocalDamageERK9CVector3fS2_R9CActorMP1fRK14CDamageInfoMP115CValueVersionIdIjjtLj16ELj16EEb"));
    *c = *saved;
    free(saved);
    LOG("[cheat] LARECOMP_DAMAGE: hit for %g at frame %llu: energy %.1f -> %.1f (alive %d)\n", hit[cur].amount,
        (unsigned long long)vk_frame_counter(), before, *(const float *)(uintptr_t)(ps + 0xc), LD8(ps) & 1);
    cur++;
}

// Each frame, from CStateManager::BeginFrame (runtime/hle/game.c rt_debug_autosave: the state manager in x0)
void cheats_frame(Cpu *c) {
    extern bool game_in_play(void);
    uint64_t gs = game_state(), ps = player_state();
    if (!gs || !ps || !game_in_play()) return;
    debug_damage(c, ps);
    {   // (the log: the cheats in play, as they change)
        static int was = -1;
        int now = g_settings.inf_energy | g_settings.inf_missiles << 1 | g_settings.inf_power_bombs << 2 |
                  g_settings.hint_seconds << 3;
        if (now != was) {
            const int32_t *it = (const int32_t *)(uintptr_t)(ps + 0x2c);
            LOG("[cheat] energy %s, missiles %s, power bombs %s, hint timer %d s (energy %.0f of %.0f, missiles %d "
                "of %d, power bombs %d of %d)\n", g_settings.inf_energy ? "on" : "off",
                g_settings.inf_missiles ? "on" : "off", g_settings.inf_power_bombs ? "on" : "off",
                g_settings.hint_seconds, *(const float *)(uintptr_t)(ps + 0xc), full_energy(ps),
                it[2 * ITEM_MISSILES], it[2 * ITEM_MISSILES + 1], it[2 * ITEM_POWER_BOMBS], it[2 * ITEM_POWER_BOMBS + 1]);
            was = now;
        }
    }
    if (g_settings.inf_energy && (LD8(ps) & 1)) {
        float *energy = (float *)(uintptr_t)(ps + 0xc), full = full_energy(ps);
        if (*energy < full) *energy = full;
    }
    if (g_settings.inf_missiles) refill(ps, ITEM_MISSILES);
    if (g_settings.inf_power_bombs) refill(ps, ITEM_POWER_BOMBS);
    hint_frame(gs);
}

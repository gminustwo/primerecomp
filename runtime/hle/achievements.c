// Achievements (runtime/achievements.h): each one is a moment in play - an upgrade picked up, a boss beaten, a world
// left, a count reached - seen as it happens, from one frame in play to the next. What a save already holds when it's
// loaded isn't earned again: a save being read (runtime/services/fs.c) or the game booting again makes the next frame
// in play the new starting point (as RetroAchievements counts them). Cheats don't stop them: they're the player's own.
//
// All read from the game's own data as it runs, nothing changed:
// - the player state (CPlayerStateMP1; runtime/hle/playlog.c): +0x18 the visor (0 Combat, 1 X-Ray, 2 Scan, 3 Thermal),
//   +0x2c + 8 * item an item's amount, +0x30 + 8 * item its capacity (CPlayerStateMP1::EItemType), and its scans:
//   +0x178 how many, +0x180 the list (20 bytes each: the scan's id, then how much of it is scanned, 1 when done)
// - the game state (gpGameState): +0x10 / +0x18 the worlds' states (CWorldStateMP1, 0x50 bytes each: +0 the world's id,
//   the ROOM entry of its own pak, Worlds/MP1/<world>/<world>.pak; +0x30 its CMapWorldInfoMP1: +0 / +8 / +0x18 the
//   rooms' visited flags - a key_value_vector<CGuid, bool>: how many, the room ids sorted, a byte each, as
//   IsAreaVisited reads it; the world's rooms are all in it once it's been loaded - and +0x78 the map station used,
//   IsWorldOrAreaMapped), +0x2b8 the difficulty (0 Casual, 1 Normal, 2 Hard)
// - the play time, in seconds, as the game shows it: CGameState +0xffb08 (SetTotalPlayTime), the state manager's
//   game state (CStateManager::GetGameState: +8, then +8) - another object than gpGameState
// - every scan the Log Book counts (gpUniverseInfo +0x490 how many, +0x498 the list, 20 bytes each: the id, then its
//   Log Book category - 1 Pirate Data, 2 Chozo Lore, 3 Creatures, 4 Research; 5, the artifacts, and 0 don't count),
//   as CPlayerStateMP1::CalculateScanPair counts the scan percentage
// - the frigate's self-destruct countdown: the state manager's game logic (+0x4e0150, CStateManagerGameLogicMP1)
//   +0xb38, seconds left (UpdateEscapeSequenceTimer counts it down; 0 when none is running)
// - the room Samus is in and its name (runtime/hle/playlog.c: its pak in the player's game files)
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../achievements.h"
#include "../hle.h"
#include "../paths.h"

#define GUEST_PTR_OK(p) ((p) >= GUEST_LO && (p) < GUEST_HI && !((p) & 7))
#define GUEST_PTR4_OK(p) ((p) >= GUEST_LO && (p) < GUEST_HI && !((p) & 3))

// (runtime/hle/playlog.c)
int playlog_room_count(void);
const char *playlog_room_at(int i, const uint8_t **id);
int playlog_item_percent(const int32_t *caps);

// ------------------------------------------------------------------ the worlds
enum { W_FRIGATE, W_TALLON, W_CHOZO, W_MAGMOOR, W_PHENDRANA, W_MINES, W_CRATER, W_ENDING, W_COUNT, W_NONE = -1 };
static const char *const k_worlds[W_COUNT] = {"Frigate Orpheon", "Tallon Overworld", "Chozo Ruins", "Magmoor Caverns",
                                              "Phendrana Drifts", "Phazon Mines", "Impact Crater", "Ending"};

// The world a room's name is in ("Phendrana Drifts / 19_Ice_Thardus")
static int world_of(const char *name) {
    if (!name) return W_NONE;
    const char *slash = strstr(name, " / ");
    if (!slash) return W_NONE;
    for (int w = 0; w < W_COUNT; w++)
        if ((size_t)(slash - name) == strlen(k_worlds[w]) && !strncmp(name, k_worlds[w], slash - name)) return w;
    return W_NONE;
}

// Each world's rooms (from the room names: playlog.c reads them from the game files once): its own id (the world's
// pak) and its rooms' ids - all of its paks but the world's own and a leftover copy of a room (Phendrana Drifts'
// 08_Ice_Ridley_Broken_Tower_Copy, which no door leads to): Chozo Ruins 64, the frigate 28, as RetroAchievements counts
// them
#define MAX_ROOMS 80
static struct {
    uint8_t id[16];
    bool have_id;
    int n;
    uint8_t rooms[MAX_ROOMS][16];
} g_world[W_COUNT];
static bool g_worlds_read;

static void read_worlds(void) {
    if (g_worlds_read) return;
    int n = playlog_room_count();
    if (n < 0) return;
    memset(g_world, 0, sizeof g_world);
    for (int i = 0; i < n; i++) {
        const uint8_t *id;
        const char *name = playlog_room_at(i, &id);
        int w = world_of(name);
        if (w < 0) continue;
        const char *room = strstr(name, " / ") + 3;
        if (room[0] == '!') memcpy(g_world[w].id, id, 16), g_world[w].have_id = true;
        else if (!strstr(room, "_Copy") && g_world[w].n < MAX_ROOMS) memcpy(g_world[w].rooms[g_world[w].n++], id, 16);
    }
    g_worlds_read = true;
    LOG("[achievements] rooms: frigate %d, Tallon %d, Chozo %d, Magmoor %d, Phendrana %d, Mines %d, Crater %d\n",
        g_world[W_FRIGATE].n, g_world[W_TALLON].n, g_world[W_CHOZO].n, g_world[W_MAGMOOR].n, g_world[W_PHENDRANA].n,
        g_world[W_MINES].n, g_world[W_CRATER].n);
}

// ------------------------------------------------------------------ the list
enum Kind {
    K_ITEM,       // a: the item picked up (its capacity from 0)
    K_ITEM_HARD,  // ... on Hard
    K_ORDER,      // a picked up while b is had (c 1) / not yet had (c 0)
    K_EVENT,      // a: the event (below)
    K_EVENT_HARD, // ... on Hard
    K_ARTIFACTS,  // all 12 Chozo Artifacts
    K_ITEMS_PCT,  // the item percentage reaches a
    K_SCANS_PCT,  // the scan percentage reaches a
    K_RESEARCH,   // every Research entry of the Log Book scanned
    K_MAP,        // world a's map station used
    K_ROOMS,      // every room of world a visited
};
enum Event { E_QUEEN, E_ESCAPE, E_ESCAPE_FAST, E_HIVE, E_THARDUS_COLD, E_RIDLEY, E_PRIME, E_FAST_FINISH, E_COUNT };

// items (CPlayerStateMP1::EItemType)
enum { I_ICE = 1, I_WAVE = 2, I_PLASMA = 3, I_MISSILES = 4, I_BOMBS = 6, I_POWER_BOMBS = 7, I_FLAMETHROWER = 8,
       I_THERMAL = 9, I_CHARGE = 10, I_SUPER = 11, I_GRAPPLE = 12, I_XRAY = 13, I_ICE_SPREADER = 14,
       I_SPACE_JUMP = 15, I_MORPH = 16, I_BOOST = 18, I_SPIDER = 19, I_GRAVITY = 21, I_VARIA = 22, I_PHAZON = 23,
       I_WAVEBUSTER = 28, I_ARTIFACT_FIRST = 29, I_NATURE = 36 };

typedef struct {
    AchInfo info;
    uint8_t kind;
    int8_t a, b, c;
} Ach;

#define A(id, title, desc, group, badge, kind, a, b, c) {{id, title, desc, ACH_##group, ACH_BADGE_##badge}, kind, a, b, c}
static const Ach k_ach[] = {
    // the story, in the order it's played
    A("queen", "Reactor Shutdown", "Defeat the Parasite Queen.", STORY, BOSS, K_EVENT, E_QUEEN, 0, 0),
    A("escape", "Touchdown", "Escape the Frigate Orpheon and land on Tallon IV.", STORY, ESCAPE, K_EVENT, E_ESCAPE, 0, 0),
    A("hive_totem", "Totem Toppled", "Defeat the Hive Totem and take back the Missile Launcher.", STORY, MISSILE,
      K_EVENT, E_HIVE, 0, 0),
    A("ridley", "Old Nemesis", "Defeat Meta Ridley at the Artifact Temple.", STORY, BOSS, K_EVENT, E_RIDLEY, 0, 0),
    A("prime", "The Source", "Defeat Metroid Prime.", STORY, BOSS, K_EVENT, E_PRIME, 0, 0),
    // upgrades
    A("morph_ball", "Roll Out", "Get the Morph Ball.", UPGRADES, GEAR, K_ITEM, I_MORPH, 0, 0),
    A("charge_beam", "Fully Charged", "Get the Charge Beam.", UPGRADES, BEAM, K_ITEM, I_CHARGE, 0, 0),
    A("bombs", "Bombs Away", "Get the Morph Ball Bombs.", UPGRADES, BOMB, K_ITEM, I_BOMBS, 0, 0),
    A("varia", "Heatproof", "Get the Varia Suit.", UPGRADES, SUIT, K_ITEM, I_VARIA, 0, 0),
    A("boost_ball", "Boosted", "Get the Boost Ball.", UPGRADES, GEAR, K_ITEM, I_BOOST, 0, 0),
    A("space_jump", "Sky High", "Get the Space Jump Boots.", UPGRADES, GEAR, K_ITEM, I_SPACE_JUMP, 0, 0),
    A("wave_beam", "Riding the Wave", "Get the Wave Beam.", UPGRADES, BEAM, K_ITEM, I_WAVE, 0, 0),
    A("super_missile", "Super Charged", "Get the Super Missile.", UPGRADES, MISSILE, K_ITEM, I_SUPER, 0, 0),
    A("thermal", "Heat Seeker", "Get the Thermal Visor.", UPGRADES, GEAR, K_ITEM, I_THERMAL, 0, 0),
    A("spider_ball", "Wall Crawler", "Get the Spider Ball.", UPGRADES, GEAR, K_ITEM, I_SPIDER, 0, 0),
    A("ice_beam", "Cold Snap", "Get the Ice Beam.", UPGRADES, BEAM, K_ITEM, I_ICE, 0, 0),
    A("gravity", "Deep Diver", "Get the Gravity Suit.", UPGRADES, SUIT, K_ITEM, I_GRAVITY, 0, 0),
    A("power_bombs", "Big Bang", "Get the Power Bombs.", UPGRADES, BOMB, K_ITEM, I_POWER_BOMBS, 0, 0),
    A("grapple", "Swing Line", "Get the Grapple Beam.", UPGRADES, BEAM, K_ITEM, I_GRAPPLE, 0, 0),
    A("xray", "Through the Walls", "Get the X-Ray Visor.", UPGRADES, GEAR, K_ITEM, I_XRAY, 0, 0),
    A("plasma_beam", "White Hot", "Get the Plasma Beam.", UPGRADES, BEAM, K_ITEM, I_PLASMA, 0, 0),
    A("phazon_suit", "Phazon Proof", "Get the Phazon Suit.", UPGRADES, SUIT, K_ITEM, I_PHAZON, 0, 0),
    A("wavebuster", "Chain Lightning", "Get the Wavebuster.", UPGRADES, BEAM, K_ITEM, I_WAVEBUSTER, 0, 0),
    A("ice_spreader", "Deep Freeze", "Get the Ice Spreader.", UPGRADES, MISSILE, K_ITEM, I_ICE_SPREADER, 0, 0),
    A("flamethrower", "Flame On", "Get the Flamethrower.", UPGRADES, BEAM, K_ITEM, I_FLAMETHROWER, 0, 0),
    A("artifacts", "The Twelve", "Collect all 12 Chozo Artifacts.", UPGRADES, ARTIFACT, K_ARTIFACTS, 0, 0, 0),
    // challenges
    A("escape_fast", "With Time to Spare", "Escape the Frigate Orpheon with 4:00 or more left on the self-destruct "
      "countdown.", CHALLENGES, ESCAPE, K_EVENT, E_ESCAPE_FAST, 0, 0),
    A("charge_late", "Charge Later", "Get the Charge Beam after the Thermal Visor.", CHALLENGES, BEAM, K_ORDER,
      I_CHARGE, I_THERMAL, 1),
    A("space_jump_first", "Skip the Boost", "Get the Space Jump Boots before the Boost Ball.", CHALLENGES, GEAR,
      K_ORDER, I_SPACE_JUMP, I_BOOST, 0),
    A("nature_first", "Nature First", "Get the Artifact of Nature before the Super Missile.", CHALLENGES, ARTIFACT,
      K_ORDER, I_NATURE, I_SUPER, 0),
    A("thardus_cold", "Snow Blind", "Defeat Thardus without using the Thermal Visor.", CHALLENGES, BOSS, K_EVENT,
      E_THARDUS_COLD, 0, 0),
    A("fast_finish", "Speed Hunter", "Defeat Metroid Prime with 3:30 or less on the game's clock.", CHALLENGES, TIME,
      K_EVENT, E_FAST_FINISH, 0, 0),
    // Hard
    A("hard_queen", "Hard: Parasite Queen", "Defeat the Parasite Queen on Hard.", HARD, BOSS, K_EVENT_HARD, E_QUEEN,
      0, 0),
    A("hard_hive_totem", "Hard: Hive Totem", "Defeat the Hive Totem and take back the Missile Launcher on Hard.", HARD,
      BOSS, K_EVENT_HARD, E_HIVE, 0, 0),
    A("hard_incinerator", "Hard: Incinerator Drone", "Destroy the Incinerator Drone and take back the Morph Ball Bombs "
      "on Hard.", HARD, BOSS, K_ITEM_HARD, I_BOMBS, 0, 0),
    A("hard_flaahgra", "Hard: Flaahgra", "Defeat Flaahgra and get the Varia Suit on Hard.", HARD, BOSS, K_ITEM_HARD,
      I_VARIA, 0, 0),
    A("hard_sheegoth", "Hard: Sheegoth", "Defeat Sheegoth and take back the Wave Beam on Hard.", HARD, BOSS,
      K_ITEM_HARD, I_WAVE, 0, 0),
    A("hard_thardus", "Hard: Thardus", "Defeat Thardus and get the Spider Ball on Hard.", HARD, BOSS, K_ITEM_HARD,
      I_SPIDER, 0, 0),
    A("hard_omega_pirate", "Hard: Omega Pirate", "Defeat the Omega Pirate and get the Phazon Suit on Hard.", HARD, BOSS,
      K_ITEM_HARD, I_PHAZON, 0, 0),
    A("hard_ridley", "Hard: Meta Ridley", "Defeat Meta Ridley on Hard.", HARD, BOSS, K_EVENT_HARD, E_RIDLEY, 0, 0),
    A("hard_prime", "Hard: Metroid Prime", "Defeat Metroid Prime on Hard.", HARD, BOSS, K_EVENT_HARD, E_PRIME, 0, 0),
    // maps and rooms
    A("map_frigate", "Frigate Charted", "Download the map of the Frigate Orpheon.", EXPLORE, MAP, K_MAP, W_FRIGATE, 0,
      0),
    A("rooms_tallon", "Tallon Explorer", "Visit every room of Tallon Overworld.", EXPLORE, MAP, K_ROOMS, W_TALLON, 0, 0),
    A("map_chozo", "Ruins Charted", "Download the map of the Chozo Ruins.", EXPLORE, MAP, K_MAP, W_CHOZO, 0, 0),
    A("rooms_chozo", "Ruins Explorer", "Visit every room of the Chozo Ruins.", EXPLORE, MAP, K_ROOMS, W_CHOZO, 0, 0),
    A("rooms_magmoor", "Magmoor Explorer", "Visit every room of Magmoor Caverns.", EXPLORE, MAP, K_ROOMS, W_MAGMOOR, 0,
      0),
    A("map_phendrana", "Drifts Charted", "Download the map of Phendrana Drifts.", EXPLORE, MAP, K_MAP, W_PHENDRANA, 0,
      0),
    A("rooms_phendrana", "Drifts Explorer", "Visit every room of Phendrana Drifts.", EXPLORE, MAP, K_ROOMS, W_PHENDRANA,
      0, 0),
    A("map_mines", "Mines Charted", "Download the map of the Phazon Mines.", EXPLORE, MAP, K_MAP, W_MINES, 0, 0),
    A("rooms_mines", "Mines Explorer", "Visit every room of the Phazon Mines.", EXPLORE, MAP, K_ROOMS, W_MINES, 0, 0),
    // completion
    A("items_25", "Quarter Full", "Reach 25% of the items.", COMPLETE, ARTIFACT, K_ITEMS_PCT, 25, 0, 0),
    A("items_50", "Halfway There", "Reach 50% of the items.", COMPLETE, ARTIFACT, K_ITEMS_PCT, 50, 0, 0),
    A("items_100", "Nothing Left Behind", "Collect every item: 100%.", COMPLETE, ARTIFACT, K_ITEMS_PCT, 100, 0, 0),
    A("scans_25", "Taking Notes", "Reach a 25% scan rate in the Log Book.", COMPLETE, SCAN, K_SCANS_PCT, 25, 0, 0),
    A("scans_50", "Field Researcher", "Reach a 50% scan rate in the Log Book.", COMPLETE, SCAN, K_SCANS_PCT, 50, 0, 0),
    A("scans_100", "Complete Record", "Scan everything: a 100% scan rate in the Log Book.", COMPLETE, SCAN,
      K_SCANS_PCT, 100, 0, 0),
    A("research", "Research Complete", "Fill in the Log Book's whole Research section.", COMPLETE, SCAN, K_RESEARCH, 0,
      0, 0),
};
#undef A
#define N_ACH ((int)(sizeof k_ach / sizeof *k_ach))

static const char *const k_groups[ACH_GROUPS] = {"Story", "Upgrades", "Challenges", "Hard", "Maps and rooms",
                                                 "Completion"};

// ------------------------------------------------------------------ earned: achievements.txt
// One line per achievement earned: its id and when (Unix time). Read once; each new one appended.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_earned[N_ACH];
static bool g_loaded, g_file_bad;
static int g_popup[16], g_npopup;  // earned, waiting for the popup

static void file_path(char *out, size_t n) {
    const char *e = getenv("LARECOMP_ACHIEVEMENTS_FILE");  // (tests)
    if (e && *e) snprintf(out, n, "%s", e);
    else snprintf(out, n, "%s/achievements.txt", rt_data_root());
}

static void load_locked(void) {
    if (g_loaded) return;
    g_loaded = true;
    char path[1100], line[256], id[64];
    file_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;  // (none yet)
    while (fgets(line, sizeof line, f)) {
        long long when;
        if (sscanf(line, "%63s %lld", id, &when) != 2 || when <= 0) continue;
        for (int i = 0; i < N_ACH; i++)
            if (!strcmp(id, k_ach[i].info.id) && !g_earned[i]) g_earned[i] = when;
    }
    if (ferror(f)) g_file_bad = true;
    fclose(f);
}

static void award(int i, const char *why) {
    pthread_mutex_lock(&g_lock);
    load_locked();
    if (g_earned[i]) { pthread_mutex_unlock(&g_lock); return; }
    int64_t now = (int64_t)time(NULL);
    g_earned[i] = now;
    char path[1100];
    file_path(path, sizeof path);
    FILE *f = fopen(path, "ab");
    bool ok = f && fprintf(f, "%s %lld\n", k_ach[i].info.id, (long long)now) > 0;
    if (f && fclose(f)) ok = false;
    if (!ok) g_file_bad = true;
    if (g_npopup < (int)(sizeof g_popup / sizeof *g_popup)) g_popup[g_npopup++] = i;
    pthread_mutex_unlock(&g_lock);
    LOG("[achievements] earned \"%s\" (%s)%s\n", k_ach[i].info.title, why, ok ? "" : " - achievements.txt not written");
}

// ------------------------------------------------------------------ what's read each frame
typedef struct {
    bool ok;
    int world;
    int32_t caps[41];
    int diff, visor;
    float escape;              // the frigate's countdown, seconds left (0: none running; -1: not known)
    int items_pct, artifacts;
    bool scans_ok;
    int scans_done, scans_total, research_done, research_total;
    bool worlds_ok;
    uint8_t mapped[W_COUNT];
    int visited[W_COUNT];      // the world's rooms visited (of g_world[w].n)
} Snap;

static Snap g_prev;            // the last frame's (ok false: none since the start, a load or a boot)
static _Atomic bool g_rebase;  // a save was read (the file service's thread): the next frame starts counting again
static uint8_t g_room[16];     // the room she's in, and its name and world
static char g_room_name[96];
static int g_room_world = W_NONE;
static float g_escape_last;    // the countdown's last value seen while it ran (seconds left)
static bool g_thardus_armed, g_thardus_thermal;  // (Snow Blind: in Quarantine Cave before the Spider Ball)
static unsigned g_frame;

// (the UI's progress counts: a copy of the latest)
static struct {
    bool items, scans, worlds;
    int items_pct, artifacts, scans_pct, research_done, research_total;
    int visited[W_COUNT], rooms[W_COUNT];
} g_shown;

static int scan_pct(const Snap *s) { return s->scans_total ? s->scans_done * 100 / s->scans_total : 0; }

// The Log Book's scans: done (1.0) of the counted ones, and of the Research category's
static bool read_scans(Snap *s, uint64_t ps) {
    uint64_t uni = LD64(guest_sym("gpUniverseInfo"));
    if (!GUEST_PTR_OK(uni)) return false;
    int32_t n = (int32_t)LD32(uni + 0x490), m = (int32_t)LD32(ps + 0x178);
    uint64_t list = LD64(uni + 0x498), mine = LD64(ps + 0x180);
    if (n <= 0 || n > 4096 || !GUEST_PTR4_OK(list) || m < 0 || m > 4096 || (m && !GUEST_PTR4_OK(mine))) return false;
    s->scans_done = s->scans_total = s->research_done = s->research_total = 0;
    static bool said;  // (debug: each category's count, once)
    if (!said && ENV("LARECOMP_ACH_LOG")) {
        said = true;
        int per[8] = {0};
        for (int32_t i = 0; i < n; i++) {
            uint32_t cat = LD32(list + 20ull * i + 16);
            per[cat < 7 ? cat : 7]++;
        }
        LOG("[achievements] Log Book scans by category: %d %d %d %d %d %d %d %d\n", per[0], per[1], per[2], per[3],
            per[4], per[5], per[6], per[7]);
    }
    for (int32_t i = 0; i < n; i++) {
        const uint8_t *e = (const uint8_t *)(uintptr_t)(list + 20ull * i);
        uint32_t cat;
        memcpy(&cat, e + 16, 4);
        if (cat < 1 || cat > 4) continue;
        // (the player's list is sorted by id; few enough to look through)
        bool done = false;
        for (int32_t j = 0; j < m && !done; j++) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)(mine + 20ull * j);
            float f;
            memcpy(&f, p + 16, 4);
            done = !memcmp(p, e, 16) && f >= 1.0f;
        }
        s->scans_total++, s->scans_done += done;
        if (cat == 4) s->research_total++, s->research_done += done;
    }
    return s->scans_total > 0;
}

// Each world's map station and rooms visited, from its CWorldStateMP1
static bool read_worlds_state(Snap *s, uint64_t gs) {
    if (!g_worlds_read) return false;
    int32_t n = (int32_t)LD32(gs + 0x10);
    uint64_t arr = LD64(gs + 0x18);
    if (n <= 0 || n > 64 || !GUEST_PTR_OK(arr)) return false;
    memset(s->mapped, 0, sizeof s->mapped), memset(s->visited, 0, sizeof s->visited);
    for (int32_t k = 0; k < n; k++) {
        uint64_t ws = arr + 0x50ull * k, info = LD64(ws + 0x30);
        int w = -1;
        for (int i = 0; i < W_COUNT && w < 0; i++)
            if (g_world[i].have_id && !memcmp((const void *)(uintptr_t)ws, g_world[i].id, 16)) w = i;
        if (w < 0 || !GUEST_PTR_OK(info)) continue;
        s->mapped[w] = LD8(info + 0x78) != 0;
        int32_t nv = (int32_t)LD32(info);
        uint64_t v = LD64(info + 8), flag = LD64(info + 0x18);
        if (nv < 0 || nv > 512 || (nv && (!GUEST_PTR4_OK(v) || flag < GUEST_LO || flag >= GUEST_HI))) continue;
        for (int r = 0; r < g_world[w].n; r++)
            for (int32_t j = 0; j < nv; j++)
                if (!memcmp((const void *)(uintptr_t)(v + 16ull * j), g_world[w].rooms[r], 16)) {
                    s->visited[w] += LD8(flag + j) != 0;
                    break;
                }
    }
    return true;
}

static bool sample(Snap *s, uint64_t gs, uint64_t ps, uint64_t sm) {
    memset(s, 0, sizeof *s);
    if (!GUEST_PTR_OK(gs) || !GUEST_PTR_OK(ps) || g_room_world < 0) return false;
    s->world = g_room_world;
    for (int i = 0; i < 41; i++) s->caps[i] = (int32_t)LD32(ps + 0x30 + 8 * i);
    s->diff = (int)LD32(gs + 0x2b8);
    s->visor = (int)LD32(ps + 0x18);
    s->escape = -1;
    uint64_t logic = GUEST_PTR_OK(sm) ? LD64(sm + 0x4e0150) : 0;
    if (GUEST_PTR_OK(logic) && LD64(logic) == guest_sym("_ZTV25CStateManagerGameLogicMP1") + 0x10) {
        // (debug) LARECOMP_ESCAPE_TIMER=<frame>:<seconds>: the self-destruct countdown started then, as
        // ResetEscapeSequenceTimer does (a test of what follows without the Parasite Queen's fight)
        static long at = -2;
        static float secs;
        if (at == -2) at = ENV("LARECOMP_ESCAPE_TIMER") && sscanf(ENV("LARECOMP_ESCAPE_TIMER"), "%ld:%f", &at, &secs) == 2 ? at : -1;
        extern uint64_t vk_frame_counter(void);
        if (at >= 0 && (long)vk_frame_counter() >= at) {
            at = -1;
            memcpy((void *)(uintptr_t)(logic + 0xb38), &secs, 4), memcpy((void *)(uintptr_t)(logic + 0xb3c), &secs, 4);
            LOG("[achievements] LARECOMP_ESCAPE_TIMER: the countdown set to %.0f s\n", secs);
        }
        memcpy(&s->escape, (const void *)(uintptr_t)(logic + 0xb38), 4);
    }
    s->items_pct = playlog_item_percent((const int32_t *)(uintptr_t)(ps + 0x2c));  // (amount, capacity pairs)
    for (int i = 0; i < 12; i++) s->artifacts += s->caps[I_ARTIFACT_FIRST + i] > 0;
    // (the scans and the worlds' rooms: a few times a second - copied from the frame before in between)
    bool slow = g_frame % 8 == 0 || !g_prev.ok;
    if (slow) {
        s->scans_ok = read_scans(s, ps);
        s->worlds_ok = read_worlds_state(s, gs);
    }
    if (!slow || !s->scans_ok) {
        s->scans_ok = g_prev.scans_ok, s->scans_done = g_prev.scans_done, s->scans_total = g_prev.scans_total;
        s->research_done = g_prev.research_done, s->research_total = g_prev.research_total;
    }
    if (!slow || !s->worlds_ok) {
        s->worlds_ok = g_prev.worlds_ok;
        memcpy(s->mapped, g_prev.mapped, sizeof s->mapped), memcpy(s->visited, g_prev.visited, sizeof s->visited);
    }
    s->ok = true;
    return true;
}

static void event(int e, const Snap *s, const char *why) {
    for (int i = 0; i < N_ACH; i++)
        if (k_ach[i].a == e && (k_ach[i].kind == K_EVENT || (k_ach[i].kind == K_EVENT_HARD && s->diff == 2)))
            award(i, why);
}

static bool in_room(const char *name) { return !strcmp(g_room_name, name); }

// What happened from one frame in play to the next
static void compare(const Snap *p, const Snap *s) {
    char why[160];
    bool frigate = p->world == W_FRIGATE || s->world == W_FRIGATE;  // (its upgrades are lent: lost in the crash)
    if (!frigate) {
        for (int i = 0; i < N_ACH; i++) {
            const Ach *a = &k_ach[i];
            if ((a->kind != K_ITEM && a->kind != K_ITEM_HARD && a->kind != K_ORDER) || p->caps[a->a] > 0 ||
                s->caps[a->a] <= 0)
                continue;
            if (a->kind == K_ITEM_HARD && s->diff != 2) continue;
            if (a->kind == K_ORDER && (s->caps[a->b] > 0) != (a->c != 0)) continue;
            snprintf(why, sizeof why, "item %d picked up in %s", a->a, g_room_name);
            award(i, why);
        }
        // the Missile Launcher: Hive Totem's (a missile expansion found before it adds to 0 as well)
        if (s->caps[I_MISSILES] > p->caps[I_MISSILES] && in_room("Chozo Ruins / 19_hivetotem"))
            event(E_HIVE, s, "missiles up in the Hive Totem's room");
        if (p->artifacts < 12 && s->artifacts == 12)
            for (int i = 0; i < N_ACH; i++)
                if (k_ach[i].kind == K_ARTIFACTS) award(i, "the 12th artifact");
        for (int i = 0; i < N_ACH; i++)
            if (k_ach[i].kind == K_ITEMS_PCT && p->items_pct < k_ach[i].a && s->items_pct >= k_ach[i].a) {
                snprintf(why, sizeof why, "items %d%% -> %d%%", p->items_pct, s->items_pct);
                award(i, why);
            }
    }
    if (p->scans_ok && s->scans_ok) {
        for (int i = 0; i < N_ACH; i++)
            if (k_ach[i].kind == K_SCANS_PCT && scan_pct(p) < k_ach[i].a && scan_pct(s) >= k_ach[i].a) {
                snprintf(why, sizeof why, "scans %d / %d", s->scans_done, s->scans_total);
                award(i, why);
            }
        if (s->research_total && p->research_done < s->research_total && s->research_done == s->research_total)
            for (int i = 0; i < N_ACH; i++)
                if (k_ach[i].kind == K_RESEARCH) award(i, "the last Research scan");
    }
    if (p->worlds_ok && s->worlds_ok)
        for (int i = 0; i < N_ACH; i++) {
            int w = k_ach[i].a;
            if (k_ach[i].kind == K_MAP && !p->mapped[w] && s->mapped[w]) award(i, "map station");
            if (k_ach[i].kind == K_ROOMS && g_world[w].n && p->visited[w] < g_world[w].n && s->visited[w] >= g_world[w].n) {
                snprintf(why, sizeof why, "the last of %d rooms: %s", g_world[w].n, g_room_name);
                award(i, why);
            }
        }
    // the frigate: its countdown starts as the Parasite Queen falls into the reactor
    if (s->world == W_FRIGATE && p->escape == 0 && s->escape > 0) {
        snprintf(why, sizeof why, "the self-destruct countdown started: %.0f s", s->escape);
        event(E_QUEEN, s, why);
    }
    if (s->world == W_FRIGATE && s->escape > 0) g_escape_last = s->escape;
    // Snow Blind: from entering Quarantine Cave without the Spider Ball to picking it up, never the Thermal Visor there
    bool cave = in_room("Phendrana Drifts / 19_Ice_Thardus");
    if (cave && s->caps[I_SPIDER] == 0 && !g_thardus_armed) g_thardus_armed = true, g_thardus_thermal = false;
    if (cave && g_thardus_armed && s->visor == 3) g_thardus_thermal = true;
    if (g_thardus_armed && p->caps[I_SPIDER] == 0 && s->caps[I_SPIDER] > 0) {
        if (!g_thardus_thermal) event(E_THARDUS_COLD, s, "the Spider Ball, no Thermal Visor in Quarantine Cave");
        g_thardus_armed = false;
    }
}

// The counts the tab shows, from this frame
static void show(const Snap *s) {
    pthread_mutex_lock(&g_lock);
    g_shown.items = true;
    g_shown.items_pct = s->items_pct, g_shown.artifacts = s->artifacts;
    if (s->scans_ok) {
        g_shown.scans = true;
        g_shown.scans_pct = scan_pct(s), g_shown.research_done = s->research_done, g_shown.research_total = s->research_total;
    }
    if (s->worlds_ok) {
        g_shown.worlds = true;
        for (int w = 0; w < W_COUNT; w++) g_shown.visited[w] = s->visited[w], g_shown.rooms[w] = g_world[w].n;
    }
    pthread_mutex_unlock(&g_lock);
}

static uint64_t g_sm;  // (the latest frame's state manager)

// The play time the game shows, in seconds (0: not known)
static double play_time(void) {
    uint64_t a = GUEST_PTR_OK(g_sm) ? LD64(g_sm + 8) : 0, gs = GUEST_PTR_OK(a) ? LD64(a + 8) : 0;
    return GUEST_PTR_OK(gs) ? *(const double *)(uintptr_t)(gs + 0xffb08) : 0;
}

void achievements_frame(uint64_t gs, uint64_t ps, uint64_t sm) {
    read_worlds();
    g_frame++;
    g_sm = sm;
    Snap s;
    if (!sample(&s, gs, ps, sm)) { g_prev.ok = false; return; }
    if (g_rebase || !g_prev.ok) {
        if (ENV("LARECOMP_ACH_LOG")) {
            char rooms[200];
            int k = 0;
            for (int w = 0; w < W_COUNT; w++)
                k += snprintf(rooms + k, sizeof rooms - k, " %d/%d%s", s.visited[w], g_world[w].n, s.mapped[w] ? "m" : "");
            LOG("[achievements] counting from here: %s, %s, items %d%%, scans %d / %d (research %d / %d), rooms%s, "
                "escape %.1f, play time %.0f s\n", g_room_name, s.diff == 2 ? "Hard" : s.diff == 1 ? "Normal" : "Casual",
                s.items_pct, s.scans_done, s.scans_total, s.research_done, s.research_total, rooms, s.escape, play_time());
        }
        g_rebase = false;
        g_escape_last = 0;
        g_thardus_armed = g_thardus_thermal = false;
    } else {
        compare(&g_prev, &s);
    }
    g_prev = s;
    if (g_frame % 8 == 0) show(&s);
    static long every = -2;  // (debug) LARECOMP_ACH_LOG=<n>: what's read, every n frames
    if (every == -2) every = ENV("LARECOMP_ACH_LOG") ? atol(ENV("LARECOMP_ACH_LOG")) : 0;
    if (every > 0 && g_frame % every == 0) {
        char rooms[200];
        int k = 0;
        for (int w = 0; w < W_COUNT; w++)
            k += snprintf(rooms + k, sizeof rooms - k, " %d/%d%s", s.visited[w], g_world[w].n, s.mapped[w] ? "m" : "");
        LOG("[achievements] %s | diff %d visor %d escape %.1f | items %d%% artifacts %d | scans %d/%d research %d/%d |"
            " rooms%s | play time %.1f\n", g_room_name, s.diff, s.visor, s.escape, s.items_pct, s.artifacts,
            s.scans_done, s.scans_total, s.research_done, s.research_total, rooms, play_time());
    }
}

void achievements_room(const uint8_t id[16], const char *name) {
    read_worlds();
    int world = world_of(name);
    memcpy(g_room, id, 16);
    snprintf(g_room_name, sizeof g_room_name, "%s", name ? name : "");
    int before = g_room_world;
    g_room_world = world;
    if (world == before || world < 0 || before < 0 || !g_prev.ok || g_rebase) return;
    // from one world to another in play (not a load: a save read in between cleared g_prev)
    uint64_t gs = LD64(guest_sym("gpGameState"));
    Snap s = g_prev;
    if (GUEST_PTR_OK(gs)) s.diff = (int)LD32(gs + 0x2b8);
    if (before == W_FRIGATE && world == W_TALLON) {
        event(E_ESCAPE, &s, "landed on Tallon IV");
        char why[96];
        snprintf(why, sizeof why, "escaped with %.1f s left", g_escape_last);
        if (g_escape_last >= 240) event(E_ESCAPE_FAST, &s, why);
        g_escape_last = 0;
    }
    if (before != W_CRATER && world == W_CRATER) event(E_RIDLEY, &s, "into the Impact Crater");
    if (before == W_CRATER && world == W_ENDING) {
        event(E_PRIME, &s, "the ending");
        double t = play_time();
        char why[96];
        snprintf(why, sizeof why, "play time %.0f s", t);
        if (t > 0 && t <= 3.5 * 3600) event(E_FAST_FINISH, &s, why);
        else LOG("[achievements] the ending, play time %.0f s\n", t);
    }
}

void achievements_save_read(void) { g_rebase = true; }

RT_ON_RESET(achievements_reset) {
    memset(&g_prev, 0, sizeof g_prev);
    g_rebase = false, g_sm = 0;
    memset(g_room, 0, sizeof g_room), g_room_name[0] = 0, g_room_world = W_NONE;
    g_escape_last = 0, g_thardus_armed = g_thardus_thermal = false;
}

// ------------------------------------------------------------------ for the UI
int ach_count(void) { return N_ACH; }
const AchInfo *ach_info(int i) { return i >= 0 && i < N_ACH ? &k_ach[i].info : NULL; }
const char *ach_group_name(int g) { return g >= 0 && g < ACH_GROUPS ? k_groups[g] : ""; }

int64_t ach_earned(int i) {
    if (i < 0 || i >= N_ACH) return 0;
    pthread_mutex_lock(&g_lock);
    load_locked();
    int64_t t = g_earned[i];
    pthread_mutex_unlock(&g_lock);
    return t;
}

bool ach_file_error(void) {
    pthread_mutex_lock(&g_lock);
    load_locked();
    bool bad = g_file_bad;
    pthread_mutex_unlock(&g_lock);
    return bad;
}

bool ach_progress(int i, int *have, int *need, bool *percent) {
    if (i < 0 || i >= N_ACH) return false;
    const Ach *a = &k_ach[i];
    bool ok = false;
    *percent = a->kind == K_ITEMS_PCT || a->kind == K_SCANS_PCT;
    pthread_mutex_lock(&g_lock);
    switch (a->kind) {
    case K_ARTIFACTS: ok = g_shown.items, *have = g_shown.artifacts, *need = 12; break;
    case K_ITEMS_PCT: ok = g_shown.items, *have = g_shown.items_pct, *need = a->a; break;
    case K_SCANS_PCT: ok = g_shown.scans, *have = g_shown.scans_pct, *need = a->a; break;
    case K_RESEARCH: ok = g_shown.scans, *have = g_shown.research_done, *need = g_shown.research_total; break;
    case K_ROOMS: ok = g_shown.worlds && g_shown.rooms[a->a], *have = g_shown.visited[a->a], *need = g_shown.rooms[a->a];
        break;
    }
    pthread_mutex_unlock(&g_lock);
    if (ok && *have > *need) *have = *need;
    return ok;
}

int ach_next_popup(void) {
    pthread_mutex_lock(&g_lock);
    int i = -1;
    if (g_npopup) {
        i = g_popup[0];
        memmove(g_popup, g_popup + 1, sizeof *g_popup * --g_npopup);
    }
    pthread_mutex_unlock(&g_lock);
    return i;
}

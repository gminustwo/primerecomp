// The play log: [state] lines in primerecomp.log saying where Samus is and with what, so a bug report's log tells
// what the game was doing when it went wrong (asked 2026-11-05: "have a log ... when gamers are playing ... the state
// things are in when they're reporting issues"). Written:
// - as she enters a room: the room by name (its pak in the player's own files: Phendrana Drifts / 01_Ice_Plaza), her
//   position, energy, missiles and power bombs, suit, visor and beam - the "state line";
// - every 5 minutes in play, the state line again;
// - as an item is picked up or changes (a missile expansion, a suit, an energy tank), and when a save is loaded, all
//   of them;
// - when she dies;
// - in a crash report, the last state line (rt_fatal_trace).
// All read from the game's own data as it runs (game.c: CWorldStateMP1::SetCurrentArea's hook, the player's frame):
// the game code stays as it was built.
//
// Rooms: the world state's current area (+0x10, a CObjectId) is the room's own asset id - the ROOM entry in the
// table of contents (RFRM PACK -> TOCC -> ADIR: 52-byte entries, type, id, ...) of its pak, Worlds/MP1/<world>/
// <room>.pak. Those are read once, on a thread of their own, the first time a room is entered.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../romfs_build.h"
#include "../settings.h"

#define GUEST_PTR_OK(p) ((p) >= GUEST_LO && (p) < GUEST_HI && !((p) & 7))

typedef struct GameFiles GameFiles;  // (runtime/services/fs.c)
GameFiles *game_files_open(void);
bool game_files_read(GameFiles *g, const char *path, uint64_t off, void *dst, uint64_t n);
int64_t game_files_size(GameFiles *g, const char *path);
void game_files_close(GameFiles *g);

// ------------------------------------------------------------------ room names
typedef struct { uint8_t id[16]; char name[96]; } Room;
static Room *g_rooms;
static int g_nrooms;
static _Atomic int g_rooms_state;  // 0 not read yet, 1 reading, 2 read

static const char *world_name(const char *folder) {
    static const struct { const char *folder, *name; } w[] = {
        {"!Intro_Master", "Frigate Orpheon"}, {"!Over_Master", "Tallon Overworld"}, {"!RuinsWorld", "Chozo Ruins"},
        {"!IceWorld", "Phendrana Drifts"}, {"!Mines_Master", "Phazon Mines"}, {"!Lava_Master", "Magmoor Caverns"},
        {"!Crater_Master", "Impact Crater"}, {"end_cinema", "Ending"}, {"CharacterGallery", "Gallery"},
    };
    for (size_t i = 0; i < sizeof w / sizeof *w; i++)
        if (!strcmp(folder, w[i].folder)) return w[i].name;
    return folder;
}

static void *read_rooms(void *arg) {
    (void)arg;
    GameFiles *gf = game_files_open();
    int cap = 0;
    uint8_t *buf = NULL;
    size_t bufcap = 0;
    for (int i = 0; gf && i < romfs_game_file_count(); i++) {
        const char *p = romfs_game_file_path(i);
        size_t n = strlen(p);
        if (strncmp(p, "Worlds/", 7) || n < 5 || strcmp(p + n - 4, ".pak")) continue;
        int64_t size = game_files_size(gf, p);
        uint8_t head[0x60];
        if (size < (int64_t)sizeof head || !game_files_read(gf, p, 0, head, sizeof head)) continue;
        if (memcmp(head, "RFRM", 4) || memcmp(head + 0x14, "PACK", 4) || memcmp(head + 0x34, "TOCC", 4) ||
            memcmp(head + 0x40, "ADIR", 4))
            continue;
        uint32_t entries;
        memcpy(&entries, head + 0x58, 4);
        size_t need = (size_t)entries * 52;
        if (!entries || entries > 100000 || 0x5c + need > (uint64_t)size) continue;
        if (need > bufcap) buf = realloc(buf, bufcap = need);
        if (!game_files_read(gf, p, 0x5c, buf, need)) continue;
        for (uint32_t e = 0; e < entries; e++) {
            if (memcmp(buf + 52ull * e, "ROOM", 4)) continue;
            if (g_nrooms == cap) g_rooms = realloc(g_rooms, sizeof *g_rooms * (cap = cap ? cap * 2 : 512));
            Room *r = &g_rooms[g_nrooms++];
            memcpy(r->id, buf + 52ull * e + 4, 16);
            // "Worlds/MP1/!IceWorld/01_Ice_Plaza.pak" -> "Phendrana Drifts / 01_Ice_Plaza"
            const char *file = strrchr(p, '/') + 1, *dir_end = file - 1, *dir = dir_end;
            while (dir > p && dir[-1] != '/') dir--;
            char folder[64];
            snprintf(folder, sizeof folder, "%.*s", (int)(dir_end - dir), dir);
            if (!strcmp(folder, "MP1")) snprintf(r->name, sizeof r->name, "%.*s", (int)strlen(file) - 4, file);
            else snprintf(r->name, sizeof r->name, "%s / %.*s", world_name(folder), (int)strlen(file) - 4, file);
        }
    }
    free(buf);
    if (gf) game_files_close(gf);
    LOG("[state] %d room names read from the game files\n", g_nrooms);
    g_rooms_state = 2;
    return NULL;
}

static const char *room_name(const uint8_t id[16]) {
    if (g_rooms_state != 2) return NULL;
    for (int i = 0; i < g_nrooms; i++)
        if (!memcmp(g_rooms[i].id, id, 16)) return g_rooms[i].name;
    return NULL;
}

// ------------------------------------------------------------------ the state line
static uint8_t g_room[16];         // the room Samus is in (zero: none yet)
static bool g_room_named;          // (its name was in the line: once the names are read, the next frame says it)
static char g_last[640];           // the latest state line (rt_fatal_trace: in a crash report)
static uint64_t g_last_ns;         // when a state line was written
static int32_t g_caps[41];         // the items' capacities in the latest frame
static bool g_caps_set;
static bool g_alive = true;
static float g_pos[3];             // her latest position
static bool g_pos_set;

static const char *const k_items[41] = {
    "Power Beam", "Ice Beam", "Wave Beam", "Plasma Beam", "Missiles", "Scan Visor", "Morph Ball Bomb", "Power Bombs",
    "Flamethrower", "Thermal Visor", "Charge Beam", "Super Missile", "Grapple Beam", "X-Ray Visor", "Ice Spreader",
    "Space Jump Boots", "Morph Ball", "Combat Visor", "Boost Ball", "Spider Ball", "Power Suit", "Gravity Suit",
    "Varia Suit", "Phazon Suit", "Energy Tanks", NULL, NULL, NULL, "Wavebuster", "Artifact of Truth",
    "Artifact of Strength", "Artifact of Elder", "Artifact of Wild", "Artifact of Lifegiver", "Artifact of Warrior",
    "Artifact of Chozo", "Artifact of Nature", "Artifact of Sun", "Artifact of World", "Artifact of Spirit",
    "Artifact of Newborn"};  // (CPlayerStateMP1::EItemType; 25 - 27 aren't kept: one is the energy refill)

// The player's state (CPlayerStateMP1, runtime/hle/cheats.c): +0x00 bit 0 alive, +0x08 the beam, +0x0c the energy,
// +0x18 the visor, +0x24 the suit worn, +0x2c + 8 * item an item's amount, +0x30 + 8 * item its capacity
static uint64_t player_state(void) {
    uint64_t gs = LD64(guest_sym("gpGameState")), ps = GUEST_PTR_OK(gs) ? LD64(gs + 0x20) : 0;
    return GUEST_PTR_OK(ps) ? ps : 0;
}

static void room_text(char *out, size_t cap) {
    const char *n = room_name(g_room);
    if (n) snprintf(out, cap, "%s", n);
    else {
        int k = snprintf(out, cap, "room ");
        for (int i = 0; i < 16 && k + 2 < (int)cap; i++) k += snprintf(out + k, cap - k, "%02x", g_room[i]);
    }
}

static void state_line(const float *pos, const char *why) {
    uint64_t ps = player_state();
    if (!ps) return;
    static const char *const beams[] = {"Power Beam", "Ice Beam", "Wave Beam", "Plasma Beam"};
    static const char *const visors[] = {"Combat Visor", "X-Ray Visor", "Scan Visor", "Thermal Visor"};
    static const char *const suits[] = {"Power Suit", "Gravity Suit", "Varia Suit", "Phazon Suit"};
    uint32_t beam = LD32(ps + 0x08), visor = LD32(ps + 0x18), suit = LD32(ps + 0x24);
    float energy;
    memcpy(&energy, (const void *)(uintptr_t)(ps + 0x0c), 4);
    const int32_t *it = (const int32_t *)(uintptr_t)(ps + 0x2c);  // (amount, capacity) per item
    char room[128], where[64] = "";
    room_text(room, sizeof room);
    if (!pos && g_pos_set) pos = g_pos;  // (entering a room: where she was last frame, by its door)
    if (pos) snprintf(where, sizeof where, " | at %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
    extern uint64_t vk_frame_counter(void);
    snprintf(g_last, sizeof g_last,
             "frame %llu | %s%s | energy %.0f/%d | missiles %d/%d | power bombs %d/%d | %s | %s | %s%s%s%s",
             (unsigned long long)vk_frame_counter(), room, where, energy, it[2 * 24 + 1] * 100 + 99, it[2 * 4],
             it[2 * 4 + 1], it[2 * 7], it[2 * 7 + 1], suit < 4 ? suits[suit] : "suit ?", visor < 4 ? visors[visor] : "visor ?",
             beam < 4 ? beams[beam] : "beam ?", g_settings.inf_energy ? " | cheat: infinite energy" : "",
             g_settings.inf_missiles ? " | cheat: infinite missiles" : "",
             g_settings.inf_power_bombs ? " | cheat: infinite power bombs" : "");
    LOG("[state] %s%s\n", g_last, why);
    g_last_ns = plat_now_ns();
}

// Items: picked up (a capacity up), or all of them when a save comes in (many at once): that's a save loaded (or a new
// game begun) - counted for runtime/hle/suitsounds.c
static _Atomic int g_save_loads;
int playlog_save_loads(void) { return g_save_loads; }
static void items_frame(uint64_t ps) {
    const int32_t *it = (const int32_t *)(uintptr_t)(ps + 0x2c);
    int changed = 0;
    for (int i = 0; i < 41; i++)
        if (k_items[i] && g_caps_set && it[2 * i + 1] != g_caps[i]) changed++;
    if (!g_caps_set || changed > 4) {
        g_save_loads++;
        char line[1024];
        int k = 0;
        for (int i = 0; i < 41 && k < (int)sizeof line - 40; i++)
            if (k_items[i] && it[2 * i + 1] > 0)
                k += snprintf(line + k, sizeof line - k, i == 4 || i == 7 || i == 24 ? "%s%s %d" : "%s%s", k ? ", " : "",
                              k_items[i], it[2 * i + 1]);
        LOG("[state] items: %s\n", k ? line : "none");
    } else if (changed) {
        for (int i = 0; i < 41; i++)
            if (k_items[i] && it[2 * i + 1] != g_caps[i])
                LOG("[state] item: %s %d -> %d\n", k_items[i], g_caps[i], it[2 * i + 1]);
    }
    for (int i = 0; i < 41; i++) g_caps[i] = it[2 * i + 1];
    g_caps_set = true;
}

// The game's controls (its Options -> Controls; CGameOptionsMP1, the game state's +0x40, kept in the save file):
// +0x11c the control scheme (EControlScheme: SetCurrentControllerScheme), +0x130 a bit set, a bit per scheme: "Swap
// Beam and Visor" (GetIsSwitchVisorBeamControls) - on, the d-pad picks the beams and the HUD shows the beams at its
// lower left, the visors at its lower right. Said in play when they change (and after each boot), so a report of the
// HUD's selectors "flipped" shows the game's own option: 2026-11-06, a save brought in from the Eden emulator had it
// on (scheme 0), the test save off.
static int32_t g_ctl_scheme = -2;
static int g_ctl_swap = -2;

static void controls_frame(void) {
    uint64_t gs = LD64(guest_sym("gpGameState"));
    if (!GUEST_PTR_OK(gs)) return;
    int32_t scheme = (int32_t)LD32(gs + 0x40 + 0x11c);
    uint64_t bits = LD64(gs + 0x40 + 0x130);
    int sw = scheme >= 0 && scheme < 64 && bits >= GUEST_LO && bits < GUEST_HI && !(bits & 3)
                 ? (int)((LD32(bits + 4 * (scheme / 32)) >> (scheme % 32)) & 1) : -1;
    if (scheme == g_ctl_scheme && sw == g_ctl_swap) return;
    LOG("[state] controls: scheme %d, Swap Beam and Visor %s\n", scheme,
        sw < 0 ? "?" : sw ? "on (the d-pad picks beams; the HUD's beams at the lower left, visors at the right)" : "off");
    g_ctl_scheme = scheme, g_ctl_swap = sw;
}

// ------------------------------------------------------------------ from the game (runtime/hle/game.c)
// Each gameplay frame, CWorldStateMP1::SetCurrentArea's id (x1, x2: its two halves)
void playlog_area(uint64_t lo, uint64_t hi) {
    uint8_t id[16];
    memcpy(id, &lo, 8), memcpy(id + 8, &hi, 8);
    static const uint8_t zero[16];
    if (!memcmp(id, zero, 16)) return;
    if (!memcmp(id, g_room, 16)) {
        if (!g_room_named && room_name(g_room)) {  // (the names came in since: said once)
            g_room_named = true;
            char room[128];
            room_text(room, sizeof room);
            LOG("[state] (that room: %s)\n", room);
        }
        return;
    }
    memcpy(g_room, id, 16);
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_rooms_state, &expected, 1)) {
        pthread_t t;
        if (pthread_create(&t, NULL, read_rooms, NULL) == 0) pthread_detach(t);
        else g_rooms_state = 2;
    }
    g_room_named = room_name(g_room) != NULL;
    state_line(NULL, "  (entered)");
}

// Each frame Samus is in play (rt_player_frame), her position (x, y, z; y up)
void playlog_frame(const float pos[3]) {
    uint64_t ps = player_state();
    if (!ps) return;
    memcpy(g_pos, pos, sizeof g_pos), g_pos_set = true;
    items_frame(ps);
    controls_frame();
    bool alive = (LD8(ps) & 1) != 0;
    if (g_alive && !alive) {
        char room[128];
        room_text(room, sizeof room);
        LOG("[state] Samus died: %s\n", room);
    }
    g_alive = alive;
    if (g_last_ns && plat_now_ns() - g_last_ns >= 300000000000ull) state_line(pos, "  (every 5 minutes in play)");
}

// The game booted again (Quit to Start Screen): taken in again from the start
RT_ON_RESET(playlog_reset) {
    g_caps_set = false, g_alive = true, g_pos_set = false, g_last_ns = 0;
    g_ctl_scheme = -2, g_ctl_swap = -2;
    memset(g_room, 0, sizeof g_room);
}

// The crash report's (runtime/dispatch.c rt_fatal_trace)
void playlog_crash_note(void) {
    if (g_last[0]) fprintf(stderr, "[state] the last state line: %s\n", g_last);
}

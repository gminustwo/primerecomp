// The game's menus with the mouse: the item under the pointer is selected, a left click on it chooses it.
//
// The new menus (the title screen, the save slots, the difficulty, Settings - CUIMenuGOC, its items CUIWidgetGOC
// with a CUIButtonGOC each) still carry the Switch's touch screen code (CUIWidgetGOC::ProcessWidgetTouchScreenInput),
// but the game never runs it - its gate asks for an input that never reads as pressed - and what it would go by isn't
// kept up: the screen-to-UI transform (CGameInstanceState +0x70) stays the identity, the buttons' touch boxes hold
// stale numbers. Their touch centres are kept (where each item sits: pointer_ui / item_at below), so the pointer is
// matched to the items here, as each menu thinks (CUIMenuGOC::BeforeThink, a virtual call: the runtime steps in at
// dispatch, no game-code change):
// - the pointer moved onto an item it can select (CanBeSelected, ShouldProcessInput): selected the way the game moves
//   its own selection (the item's +0x150 set to itself, CUIWidgetGOC::ProcessDeferredSelection from the one selected
//   now, CUIMenuGOC::FindSelectedWidget) - the highlight and its sound, as with the d-pad;
// - a left click on an item: selected first if it isn't, then an A press (runtime/hle/input.c) once it is - the game's
//   own confirm, so what A does there happens (a click elsewhere does nothing).
// Only while the game isn't in play, the mouse isn't captured and the pointer is shown (input_menu_pointer). The
// older screens (the pause screen's pages, the map) go by the keys and the controller as before.
// (debug) LARECOMP_LOG_MENU_MOUSE=1: what was selected and chosen ("[menumouse]"); 2: also each pointer move and the
// item under it; 3: also where each item sits.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

extern bool input_menu_pointer(float *x, float *y, uint32_t *moves, uint32_t *clicks);  // runtime/hle/input.c
extern void input_menu_press_a(void);
extern void vk_game_rect(float r[4]);  // runtime/nvn/vulkan/device.c

#define GUEST_PTR_OK(p) ((p) >= GUEST_LO && (p) < GUEST_HI && !((p) & 7))

static GuestFn g_before_think;
static uint64_t fn_component, fn_widget_cast, fn_button_cast, fn_can_select, fn_should_input, fn_find_selected,
    fn_deferred;
static int g_log = -1;

static uint64_t call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2) {
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2;
    guest_call(c, fn);
    return c->x[0];
}

// The pointer in the menus' own space: the UI camera's 16 x 9 view of the screen, y up (measured on the title menu:
// its items' touch centres 0.75 apart where they're 0.0845 of the screen apart; the game's own screen-to-UI transform,
// CGameInstanceState +0x70, is left as it is - the camera never fills it in, the touch code being unused). False if
// the pointer is off the game's image.
static bool pointer_ui(float px, float py, float *ux, float *uy) {
    float r[4];
    vk_game_rect(r);
    float u = (px - r[0]) / r[2], v = (py - r[1]) / r[3];
    if (u < 0 || u > 1 || v < 0 || v > 1) return false;
    *ux = (u * 2 - 1) * 8.0f, *uy = -(v * 2 - 1) * 4.5f;
    return true;
}

// The menu's item under the pointer (0: none), of those the player could move to now. Each item's touch centre
// (CUIButtonGOC::IsTouching's circle: its entity's +0x40 -> +0x3c, +0x40) is where it sits - the left of a title menu
// row, the middle of a save slot -, and the buttons' own touch boxes aren't kept up to date, so an item's area is made
// from where the items are: up and down halfway to the nearest item above or below (0.5 if none), sideways up to 7.5
// (nearly half the screen). Of the items the pointer is in the areas of, the nearest (the row first).
typedef struct { uint64_t w; float x, y; } Item;
static uint64_t item_at(Cpu *c, uint64_t menu, uint64_t mgr, float ux, float uy) {
    int n = (int)LD32(menu + 0xc0);
    uint64_t ids = LD64(menu + 0xc8);
    if (n <= 0 || n > 256 || !GUEST_PTR_OK(ids & ~7ull)) return 0;
    Item it[256];
    int m = 0;
    for (int i = 0; i < n; i++) {
        uint64_t w = call(c, fn_widget_cast, call(c, fn_component, mgr + 0x30, LD32(ids + 4 * i), 0), 0, 0);
        if (!GUEST_PTR_OK(w)) continue;
        bool selected = LD32(w + 0x80) == 4;
        if (!selected && !(call(c, fn_can_select, w, 0, 0) & 1)) continue;
        if (!(call(c, fn_should_input, w, 0, 0) & 1)) continue;
        uint64_t btn = call(c, fn_button_cast, call(c, fn_component, mgr + 0x30, LD32(w + 0x90), 0), 0, 0);
        if (!GUEST_PTR_OK(btn)) continue;
        uint64_t ent = LD64(btn + 0x20), tf = GUEST_PTR_OK(ent) ? LD64(ent + 0x40) : 0;
        if (!GUEST_PTR_OK(tf)) continue;
        float x, y;
        memcpy(&x, (const void *)(uintptr_t)(tf + 0x3c), 4), memcpy(&y, (const void *)(uintptr_t)(tf + 0x40), 4);
        if (!isfinite(x) || !isfinite(y) || fabsf(x) > 100 || fabsf(y) > 100) continue;
        it[m++] = (Item){w, x, y};
        if (g_log > 2) LOG("[menumouse] item %llx at %.3f %.3f\n", (unsigned long long)w, x, y);
    }
    uint64_t best = 0;
    float best_d = 1e9f;
    for (int i = 0; i < m; i++) {
        float half = 1e9f;
        for (int j = 0; j < m; j++) {  // (halfway to the nearest row above or below)
            float dy = fabsf(it[j].y - it[i].y);
            if (j != i && fabsf(it[j].x - it[i].x) < 1.0f && dy > 0.05f && dy / 2 < half) half = dy / 2;
        }
        if (half > 1e8f) half = 0.5f;  // (a single row)
        float dy = fabsf(uy - it[i].y), dx = fabsf(ux - it[i].x);
        float d = dy / half + dx / 7.5f;
        if (dy <= half && dx <= 7.5f && d < best_d) best = it[i].w, best_d = d;
    }
    return best;
}

static void select_item(Cpu *c, uint64_t menu, uint64_t mgr, uint64_t w) {
    uint32_t now = (uint32_t)call(c, fn_find_selected, menu, mgr, 0);
    ST32(w + 0x150, LD32(w + 0x18));
    call(c, fn_deferred, w, mgr, now);
}

static void rt_before_think(Cpu *c) {
    static uint32_t seen_moves, seen_clicks;
    static uint64_t pending;  // an item clicked before it was selected: chosen once it is
    static int pending_frames;
    uint64_t menu = c->x[0], mgr = c->x[1];
    static int first;
    if (g_log > 1 && !first++) LOG("[menumouse] BeforeThink reached (menu %llx)\n", (unsigned long long)menu);
    float px, py;
    uint32_t moves, clicks;
    bool on = input_menu_pointer(&px, &py, &moves, &clicks);
    bool moved = moves != seen_moves, clicked = clicks != seen_clicks;
    if (g_log > 1 && (moved || clicked))
        LOG("[menumouse] think: menu %llx focused %d, pointer %s at %.3f %.3f (moves %u clicks %u)\n",
            (unsigned long long)menu, GUEST_PTR_OK(menu) ? LD8(menu + 0xd5) : -1, on ? "on" : "off", px, py, moves, clicks);
    if (on && GUEST_PTR_OK(menu) && GUEST_PTR_OK(mgr) && LD8(menu + 0xd5) && (moved || clicked || pending)) {
        Cpu *saved = malloc(sizeof *saved);
        *saved = *c;
        float ux, uy;
        uint64_t w = pointer_ui(px, py, &ux, &uy) ? item_at(c, menu, mgr, ux, uy) : 0;
        if (g_log > 1) LOG("[menumouse] at %.3f %.3f -> ui %.3f %.3f: item %llx\n", px, py, ux, uy, (unsigned long long)w);
        if (pending && --pending_frames <= 0) pending = 0;
        if (pending && w == pending && LD32(w + 0x80) == 4) {
            if (g_log) LOG("[menumouse] %llx chosen\n", (unsigned long long)w);
            input_menu_press_a(), pending = 0;
        }
        if (w && (moved || clicked) && LD32(w + 0x80) != 4) {
            if (g_log) LOG("[menumouse] menu %llx: %llx selected (pointer %.3f %.3f)\n", (unsigned long long)menu,
                           (unsigned long long)w, ux, uy);
            select_item(c, menu, mgr, w);
        }
        if (clicked && w) {
            if (LD32(w + 0x80) == 4) {
                if (g_log) LOG("[menumouse] %llx clicked: chosen\n", (unsigned long long)w);
                input_menu_press_a(), pending = 0;
            } else {
                pending = w, pending_frames = 30;
            }
        }
        if (g_log && clicked && !w) LOG("[menumouse] click on nothing (pointer %.3f %.3f)\n", px, py);
        *c = *saved;
        free(saved);
        seen_moves = moves, seen_clicks = clicks;
    } else if (!on) {
        seen_moves = moves, seen_clicks = clicks, pending = 0;
    }
    g_before_think(c);
}

void menumouse_hooks_init(void) {  // (game.c game_hooks_init, each boot)
    if (g_log < 0) g_log = getenv("LARECOMP_LOG_MENU_MOUSE") ? atoi(getenv("LARECOMP_LOG_MENU_MOUSE")) : 0;
    fn_component = guest_sym("_ZN19CStateManagerObject13ComponentByIdE15CValueVersionIdIjjtLj16ELj16EE");
    fn_widget_cast = guest_sym("_ZN12CUIWidgetGOC9CastToPtrEP20CGameObjectComponent");
    fn_button_cast = guest_sym("_ZN12CUIButtonGOC9CastToPtrEP20CGameObjectComponent");
    fn_can_select = guest_sym("_ZNK12CUIWidgetGOC13CanBeSelectedEv");
    fn_should_input = guest_sym("_ZNK12CUIWidgetGOC18ShouldProcessInputEv");
    fn_find_selected = guest_sym("_ZN10CUIMenuGOC18FindSelectedWidgetER13CStateManager");
    fn_deferred = guest_sym("_ZN12CUIWidgetGOC24ProcessDeferredSelectionER13CStateManager15CValueVersionIdIjjtLj16ELj16EE");
    uint64_t bt = guest_sym("_ZN10CUIMenuGOC11BeforeThinkER13CStateManagerf");
    static bool hooked;
    g_before_think = dispatch_lookup(bt);
    if (!g_before_think) {
        LOG("[menumouse] the menus' think wasn't found: no mouse in the game's menus\n");
        return;
    }
    if (!hooked) hle_override(bt, rt_before_think, "CUIMenuGOC::BeforeThink"), hooked = true;
}

#include "settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glyphs.h"
#include "hle.h"
#include "input_bind.h"

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's primerecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    return e && *e ? e : "primerecomp.ini";
}

Settings g_settings = {
    .preload_shaders = true,  // (with the shipped shader list: a first visit anywhere it covers draws at once)
    .aspect = ASPECT_UNLOCKED,
    .hud = HUD_EDGES,
    .pause = PAUSE_FILL,
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .rumble = true,
    .rumble_strength = 1.0f,
    .hd_rumble = true,
    .check_updates = true,
    .deadzone = {0.05f, 0.05f},
    .movie_audio = true,
    .show_fps = false,
    .frame_rate = 60,
    .vsync = true,
    .hint_on_l3 = true,
    .texture_mods = true,
    .hdr_peak = 1000,
    .hdr_paper = 200,
    .upscaler = UPSCALE_FSR1,
    .res_lines = 2160,
    .fov = FOV_GAME,
    .weapon_fov = FOV_GAME,
    .gyro = true,
    .aniso = 16,
    .mouse_aim = true,
    .mouse_sens = 1.0f,
    .wiimote = true,
    .wiimote_area = 0.5f,
    .wiimote_smooth = 0.3f,
};

const int g_hint_waits[N_HINT_WAITS] = {0, 1, 30, 60, 120, 180, 240, 300, 360, 420, 480, 540, 600, 660, 720, 780, 840, 900,
                                        960, 1020, 1080, 1140, 1200, 1260, 1320, 1380, 1440, 1500, 1560, 1620, 1680, 1740, 1800};
int settings_hint_wait_index(int seconds) {
    int best = 0;
    for (int i = 1; i < N_HINT_WAITS; i++)
        if (abs(g_hint_waits[i] - seconds) < abs(g_hint_waits[best] - seconds)) best = i;
    return best;
}

const int g_res_lines[N_RES_LINES] = {900, 1080, 1440, 2160, 4320};
const char *const g_res_names[N_RES_LINES] = {"900p (Switch)", "1080p", "1440p", "4K", "8K"};

// the offered height nearest to `lines` (a hand-edited larecomp.ini, LAS's "2x")
static int res_snap(int lines) {
    int best = g_res_lines[0];
    for (int i = 1; i < N_RES_LINES; i++)
        if (abs(g_res_lines[i] - lines) < abs(best - lines)) best = g_res_lines[i];
    return best;
}

// The game's own text languages, as the Switch codes them: the game picks its text and region from the language it
// reads back (runtime/services/am.c; its table: COsContext::COsContext). That table also knows Portuguese, Russian
// and "zh-TW" (the Switch's older Taiwanese, a language of its own there), but the game has no text for them: told
// either of the last two it crashed building the HUD (CHudVisorBeamMenuMP1, its visor menu's text pane not there;
// issue #21). Its Traditional Chinese is "zh-Hant". A console only ever reports one of the game's own languages
// (GetDesiredLanguage chooses among them), so these are the ones offered: each loads a save.
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ko",
                                                   "zh-CN", "zh-Hant"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Korean", "Chinese (Simplified)",
                                                   "Chinese (Traditional)"};

// LANG_AUTO: the host's own language (LANG=es_ES.UTF-8 -> es), en-US when it is none of the game's. Linux only:
// macOS / Windows apps get no locale variables from the system, so Auto is English there (pick one in settings).
static const char *host_language(void) {
#ifndef __linux__
    return "en-US";
#endif
    const char *e = getenv("LC_ALL");
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
    if (!e || !*e || !strcmp(e, "C") || !strcmp(e, "POSIX")) return "en-US";
    char l[8] = {0};
    size_t n = 0;
    for (const char *p = e; *p && *p != '.' && *p != '@' && *p != '_' && n < sizeof l - 1; p++) l[n++] = *p;
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"nl", "nl"}, {"de", "de"},
                                                             {"it", "it"}, {"fr", "fr"}, {"es", "es"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(l, map[i].loc)) return map[i].code;
    if (!strcmp(l, "zh")) return e[n] == '_' && (e[n + 1] == 'T' || e[n + 1] == 'H') ? "zh-Hant" : "zh-CN";  // (TW / HK)
    return "en-US";
}

const char *settings_language(void) {
    int i = g_settings.language;
    return i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
}

// Steam sets SteamDeck=1 for what it starts on a Deck (Game Mode and Desktop Mode); started otherwise, the board says
// it (/sys/class/dmi/id: Valve's Jupiter - the LCD model - or Galileo - the OLED).
bool settings_on_steam_deck(void) {
#ifdef __linux__
    static int deck = -1;
    if (deck < 0) {
        const char *e = getenv("SteamDeck");
        deck = e && atoi(e) == 1;
        char vendor[64] = "", product[64] = "";
        FILE *f = fopen("/sys/class/dmi/id/board_vendor", "r");
        if (f) { if (!fgets(vendor, sizeof vendor, f)) vendor[0] = 0; fclose(f); }
        f = fopen("/sys/class/dmi/id/product_name", "r");
        if (f) { if (!fgets(product, sizeof product, f)) product[0] = 0; fclose(f); }
        if (!strncmp(vendor, "Valve", 5) && (!strncmp(product, "Jupiter", 7) || !strncmp(product, "Galileo", 7))) deck = 1;
    }
    return deck;
#else
    return false;
#endif
}

// The Deck's screen is 1280 x 800: the game's 16:9 image is 1280 x 720 at Match display. Its GPU shares the memory
// bandwidth with the CPU, so the texture filtering comes down too, and SMAA stays off.
void settings_steam_deck_preset(int fps) {
    g_settings.frame_rate = fps == 30 ? 30 : 60;
    g_settings.res_mode = fps == 30 ? RES_MATCH : RES_Q75;
    g_settings.upscaler = UPSCALE_FSR1;
    g_settings.antialias = AA_OFF;
    g_settings.aniso = fps == 30 ? 4 : 2;
    LOG("[settings] Steam Deck preset: %d fps\n", g_settings.frame_rate);
}

const char *settings_language_name(void) {
    const char *c = settings_language();
    for (int i = 0; i < N_LANGUAGES; i++)
        if (!strcmp(c, g_language_codes[i])) return g_language_names[i];
    return "English";
}

static void log_settings(bool all);

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    // larecomp.ini until 2026-10-25: renamed, then saved again below (without Link's Awakening's keys)
    bool migrated = !f && !getenv("LARECOMP_SETTINGS") && rename("larecomp.ini", SETTINGS_FILE) == 0;
    if (migrated) {
        LOG("[settings] larecomp.ini renamed to %s\n", SETTINGS_FILE);
        f = fopen(SETTINGS_FILE, "r");
    }
    if (!f) {  // (a first start: on a Steam Deck its steady 30 fps preset)
        if (settings_on_steam_deck()) settings_steam_deck_preset(30);
        log_settings(true);
        return;
    }
    char line[4400];  // (mods_order / mods_disabled: lists of folder names)
    bool tip_key = false;
    for (bool first = true; fgets(line, sizeof line, f); first = false) {
        char key[64];
        char val[4200] = "";
        // (a file saved by an editor as "UTF-8 with BOM": the mark hid the first line's key)
        const char *p = first && !memcmp(line, "\xEF\xBB\xBF", 3) ? line + 3 : line;
        if (sscanf(p, " %63[^= ] = %4199[^\n]", key, val) < 1) continue;
        // (a hand-edited file's Windows line ends: the file is read as bytes, "original\r" didn't match "original")
        for (size_t n = strnlen(val, sizeof val); n && (val[n - 1] == '\r' || val[n - 1] == ' '); n--) val[n - 1] = 0;
        // (keys only Link's Awakening had - cheats, randomizer, cosmetics... - fall through to the bindings, unread)
        if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;
        else if (!strcmp(key, "hud")) g_settings.hud = !strcmp(val, "middle") ? HUD_MIDDLE : !strcmp(val, "21:9") ? HUD_21_9 : HUD_EDGES;
        else if (!strcmp(key, "pause")) g_settings.pause = !strcmp(val, "middle") ? PAUSE_MIDDLE : PAUSE_FILL;
        else if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "rumble")) g_settings.rumble = atoi(val) != 0;
        else if (!strcmp(key, "rumble_strength")) g_settings.rumble_strength = (float)atof(val);
        else if (!strcmp(key, "hd_rumble")) g_settings.hd_rumble = atoi(val) != 0;
        else if (!strcmp(key, "deadzone_left")) g_settings.deadzone[0] = (float)atof(val);
        else if (!strcmp(key, "deadzone_right")) g_settings.deadzone[1] = (float)atof(val);
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "frame_rate")) {
            int r = atoi(val);
            g_settings.frame_rate = r == 120 || r == 30 ? r : r == 0 ? 120 : 60;  // (0: Uncapped, gone since 2026-10-27)
        }
        else if (!strcmp(key, "vsync")) g_settings.vsync = atoi(val) != 0;
        else if (!strcmp(key, "preload_shaders")) g_settings.preload_shaders = atoi(val) != 0;
        else if (!strcmp(key, "texture_mods")) g_settings.texture_mods = atoi(val) != 0;
        else if (!strcmp(key, "dump_textures")) g_settings.dump_textures = atoi(val) != 0;
        else if (!strcmp(key, "mods_order")) snprintf(g_settings.mods_order, sizeof g_settings.mods_order, "%s", val);
        else if (!strcmp(key, "mods_disabled")) snprintf(g_settings.mods_disabled, sizeof g_settings.mods_disabled, "%s", val);
        else if (!strcmp(key, "cheats_on")) snprintf(g_settings.cheats_on, sizeof g_settings.cheats_on, "%s", val);
        else if (!strcmp(key, "hdr")) g_settings.hdr = atoi(val) != 0;
        else if (!strcmp(key, "hdr_peak_nits")) g_settings.hdr_peak = atoi(val);
        else if (!strcmp(key, "hdr_paper_white_nits")) g_settings.hdr_paper = atoi(val);
        else if (!strcmp(key, "fov")) g_settings.fov = atoi(val) < FOV_MIN ? FOV_MIN : atoi(val) > FOV_MAX ? FOV_MAX : atoi(val);
        else if (!strcmp(key, "weapon_fov"))
            g_settings.weapon_fov = atoi(val) < FOV_MIN ? FOV_MIN : atoi(val) > FOV_MAX ? FOV_MAX : atoi(val);
        else if (!strcmp(key, "skippable_cutscenes")) g_settings.skippable_cutscenes = atoi(val) != 0;
        else if (!strcmp(key, "hint_on_l3")) g_settings.hint_on_l3 = atoi(val) != 0;
        else if (!strcmp(key, "hint_seconds")) g_settings.hint_seconds = g_hint_waits[settings_hint_wait_index(atoi(val))];
        else if (!strcmp(key, "hint_minutes"))  // (before 2026-10-29: whole minutes)
            g_settings.hint_seconds = g_hint_waits[settings_hint_wait_index(atoi(val) * 60)];
        else if (!strcmp(key, "infinite_energy")) g_settings.inf_energy = atoi(val) != 0;
        else if (!strcmp(key, "infinite_missiles")) g_settings.inf_missiles = atoi(val) != 0;
        else if (!strcmp(key, "infinite_power_bombs")) g_settings.inf_power_bombs = atoi(val) != 0;
        else if (!strcmp(key, "wiimote")) g_settings.wiimote = atoi(val) != 0;
        else if (!strcmp(key, "wiimote_mid")) sscanf(val, "%f %f", &g_settings.wiimote_mid[0], &g_settings.wiimote_mid[1]);
        else if (!strcmp(key, "wiimote_area")) g_settings.wiimote_area = (float)atof(val);
        else if (!strcmp(key, "wiimote_smooth")) g_settings.wiimote_smooth = (float)atof(val);
        else if (!strcmp(key, "gyro")) g_settings.gyro = atoi(val) != 0;
        else if (!strcmp(key, "mouse_aim")) g_settings.mouse_aim = atoi(val) != 0;
        else if (!strcmp(key, "mouse_sens")) g_settings.mouse_sens = (float)atof(val);
        else if (!strcmp(key, "mouse_invert")) g_settings.mouse_invert = atoi(val) != 0;
        else if (!strcmp(key, "flick_stick")) g_settings.flick_stick = atoi(val) != 0;
        else if (!strcmp(key, "steam_controller_direct")) g_settings.steam_controller_direct = atoi(val) != 0;
        else if (!strcmp(key, "resolution")) {  // match / dynamic / 75% / 50% / 25% / <lines>p / 4k / 8k (LAS: 1x-8x)
            int n = atoi(val);
            size_t l = strlen(val);
            char unit = l ? (char)(val[l - 1] | 0x20) : 0;
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                                  n > 0 && (unit == 'p' || unit == 'k' || unit == 'x') ? RES_FIXED : RES_MATCH;
            if (g_settings.res_mode == RES_FIXED)
                g_settings.res_lines = res_snap(unit == 'p' ? n : unit == 'k' ? n * 540 : n * 1080);
        }
        else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_FSR1;  // (fsr1, LAS's metalfx)
        else if (!strcmp(key, "aniso")) { int a = atoi(val); g_settings.aniso = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 0; }
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : AA_SMAA;  // ("fxaa": SMAA now)
        // (before the bind_pad_* lines, which settings_save writes last: they're the differences from its defaults)
        else if (!strcmp(key, "face_buttons")) {  // (anything else: labels)
            int m = FACE_LABELS;
            for (int i = 0; i < FACE_COUNT; i++)
                if (!strcmp(val, g_face_names[i])) m = i;
            bind_load_face_buttons(m);
        }
        // (v0.0.6 and older: "face_layout = nintendo" was its default, written by every save - the labels now; its
        // "xbox", chosen, stays)
        else if (!strcmp(key, "face_layout")) { if (!strcmp(val, "xbox")) bind_load_face_buttons(FACE_XBOX); }
        else if (!strcmp(key, "button_icons")) {  // (anything else: auto)
            g_settings.button_icons = ICONS_AUTO;
            for (int i = 0; i < ICONS_COUNT; i++)
                if (!strcmp(val, g_icons_names[i])) g_settings.button_icons = i;
        }
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            if (!strcmp(val, "zh-TW")) strcpy(val, "zh-Hant");  // (an older version's Traditional Chinese)
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(val, g_language_codes[i])) g_settings.language = i;
        }
        else if (!strcmp(key, "menu_tip_seen")) g_settings.menu_tip_seen = atoi(val) != 0, tip_key = true;
        else if (!strcmp(key, "check_updates")) g_settings.check_updates = atoi(val) != 0;
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
    }
    fclose(f);
    if (!tip_key) g_settings.menu_tip_seen = true;  // (a settings file from before the note: not a first launch)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    if (!(g_settings.rumble_strength >= 0)) g_settings.rumble_strength = 0;  // (NaN too)
    if (g_settings.rumble_strength > RUMBLE_STRENGTH_MAX) g_settings.rumble_strength = RUMBLE_STRENGTH_MAX;
    for (int i = 0; i < 2; i++) {
        if (!(g_settings.deadzone[i] >= 0)) g_settings.deadzone[i] = 0;
        if (g_settings.deadzone[i] > DEADZONE_MAX) g_settings.deadzone[i] = DEADZONE_MAX;
    }
    if (!(g_settings.mouse_sens >= MOUSE_SENS_MIN)) g_settings.mouse_sens = MOUSE_SENS_MIN;  // (NaN too)
    if (g_settings.mouse_sens > MOUSE_SENS_MAX) g_settings.mouse_sens = MOUSE_SENS_MAX;
    if (!(g_settings.wiimote_area >= 0.2f)) g_settings.wiimote_area = 0.2f;  // (NaN too)
    if (g_settings.wiimote_area > 1) g_settings.wiimote_area = 1;
    if (!(g_settings.wiimote_smooth >= 0)) g_settings.wiimote_smooth = 0;
    if (g_settings.wiimote_smooth > 0.9f) g_settings.wiimote_smooth = 0.9f;
    if (g_settings.hdr_peak < HDR_PEAK_MIN) g_settings.hdr_peak = HDR_PEAK_MIN;
    if (g_settings.hdr_peak > HDR_PEAK_MAX) g_settings.hdr_peak = HDR_PEAK_MAX;
    if (g_settings.hdr_paper < HDR_PAPER_MIN) g_settings.hdr_paper = HDR_PAPER_MIN;
    if (g_settings.hdr_paper > HDR_PAPER_MAX) g_settings.hdr_paper = HDR_PAPER_MAX;
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
    log_settings(true);
    if (migrated) settings_save();
}

// The settings as primerecomp.ini has them, written through put: to the file, or to a buffer (Out.s) for the log
typedef struct { FILE *f; char *s; size_t n, cap; } Out;
static void put(void *ctx, const char *fmt, ...) {
    Out *o = ctx;
    va_list ap;
    va_start(ap, fmt);
    if (o->f) vfprintf(o->f, fmt, ap);
    else if (o->n + 1 < o->cap) {
        int k = vsnprintf(o->s + o->n, o->cap - o->n, fmt, ap);
        if (k > 0) o->n = o->n + (size_t)k < o->cap ? o->n + (size_t)k : o->cap - 1;
    }
    va_end(ap);
}

static void write_settings(void *o) {
    put(o, "# PrimeRecomp settings (edited from the in-game menu: F1, or L3+R3 on a controller)\n");
    put(o, "aspect = %s\n", g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked");
    put(o, "hud = %s\n", g_settings.hud == HUD_MIDDLE ? "middle" : g_settings.hud == HUD_21_9 ? "21:9" : "edges");
    put(o, "pause = %s\n", g_settings.pause == PAUSE_MIDDLE ? "middle" : "fill");
    put(o, "fullscreen = %d\n", g_settings.fullscreen);
    put(o, "volume = %.2f\n", g_settings.volume);
    put(o, "reverb = %d\n", g_settings.reverb);
    put(o, "rumble = %d\nrumble_strength = %.2f\nhd_rumble = %d\n", g_settings.rumble, g_settings.rumble_strength,
            g_settings.hd_rumble);
    put(o, "deadzone_left = %.2f\ndeadzone_right = %.2f\n", g_settings.deadzone[0], g_settings.deadzone[1]);
    put(o, "movie_audio = %d\n", g_settings.movie_audio);
    put(o, "show_fps = %d\n", g_settings.show_fps);
    put(o, "frame_rate = %d\n", g_settings.frame_rate);
    put(o, "vsync = %d\n", g_settings.vsync);
    put(o, "preload_shaders = %d\n", g_settings.preload_shaders);
    put(o, "texture_mods = %d\ndump_textures = %d\n", g_settings.texture_mods, g_settings.dump_textures);
    if (g_settings.mods_order[0]) put(o, "mods_order = %s\n", g_settings.mods_order);
    if (g_settings.mods_disabled[0]) put(o, "mods_disabled = %s\n", g_settings.mods_disabled);
    if (g_settings.cheats_on[0]) put(o, "cheats_on = %s\n", g_settings.cheats_on);
    put(o, "hdr = %d\nhdr_peak_nits = %d\nhdr_paper_white_nits = %d\n", g_settings.hdr, g_settings.hdr_peak,
            g_settings.hdr_paper);
    put(o, "fov = %d\nweapon_fov = %d\nskippable_cutscenes = %d\ngyro = %d\n", g_settings.fov, g_settings.weapon_fov,
            g_settings.skippable_cutscenes, g_settings.gyro);
    put(o, "mouse_aim = %d\nmouse_sens = %.2f\nmouse_invert = %d\n", g_settings.mouse_aim, g_settings.mouse_sens,
            g_settings.mouse_invert);
    put(o, "flick_stick = %d\n", g_settings.flick_stick);
    put(o, "hint_seconds = %d\nhint_on_l3 = %d\ninfinite_energy = %d\ninfinite_missiles = %d\ninfinite_power_bombs = %d\n",
            g_settings.hint_seconds, g_settings.hint_on_l3, g_settings.inf_energy, g_settings.inf_missiles,
            g_settings.inf_power_bombs);
    put(o, "wiimote = %d\nwiimote_mid = %.4f %.4f\nwiimote_area = %.2f\nwiimote_smooth = %.2f\n", g_settings.wiimote,
            g_settings.wiimote_mid[0], g_settings.wiimote_mid[1], g_settings.wiimote_area, g_settings.wiimote_smooth);
    if (g_settings.steam_controller_direct) put(o, "steam_controller_direct = 1\n");  // (written only when set)
    put(o, "upscaler = %s\n", g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
    put(o, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    put(o, "aniso = %d\n", g_settings.aniso);
    if (g_settings.res_mode == RES_DYNAMIC) put(o, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) put(o, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) put(o, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) put(o, "resolution = 50%%\n");
    else if (g_settings.res_mode == RES_FIXED) put(o, "resolution = %dp\n", g_settings.res_lines);
    else put(o, "resolution = match\n");
    put(o, "face_buttons = %s\n", g_face_names[g_settings.face_buttons >= 0 && g_settings.face_buttons < FACE_COUNT
                                                         ? g_settings.face_buttons : FACE_LABELS]);
    put(o, "button_icons = %s\n", g_icons_names[g_settings.button_icons >= 0 && g_settings.button_icons < ICONS_COUNT
                                                         ? g_settings.button_icons : ICONS_AUTO]);
    put(o, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    put(o, "menu_tip_seen = %d\n", g_settings.menu_tip_seen);
    put(o, "check_updates = %d\n", g_settings.check_updates);
    bind_settings_save(put, o);
}

// The settings in the log (bug reports: what the player had set): all of them once, as "key value" pairs, then each
// change as it's saved ("[settings] changed: fov 55 -> 90"). Not the window's own bookkeeping (the Wii Remote's
// calibrated middle, the one-time tip).
static char g_logged[16384];  // the ini text last logged
static bool logged_key(const char *key, size_t klen) {
    static const char *const skip[] = {"wiimote_mid", "menu_tip_seen", "bind_keys"};
    for (size_t i = 0; i < sizeof skip / sizeof *skip; i++)
        if (strlen(skip[i]) == klen && !strncmp(key, skip[i], klen)) return false;
    return true;
}
static void settings_text(char *buf, size_t cap) {
    Out o = {NULL, buf, 0, cap};
    buf[0] = 0;
    write_settings(&o);
}
// An ini text's next "key = value" line from *at (comments and others skipped): false at the end
static bool ini_next(const char **at, const char **key, size_t *klen, const char **val, size_t *vlen) {
    while (**at) {
        const char *l = *at, *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        *at = l + len + (e ? 1 : 0);
        const char *eq = memchr(l, '=', len);
        if (l[0] == '#' || !eq || eq - l < 2 || eq[-1] != ' ') continue;
        *key = l, *klen = (size_t)(eq - l) - 1;
        *val = eq + 1 < l + len && eq[1] == ' ' ? eq + 2 : eq + 1, *vlen = (size_t)(l + len - *val);
        return true;
    }
    return false;
}
// key's value in an ini text, or NULL
static const char *ini_value(const char *text, const char *key, size_t klen, size_t *vlen) {
    const char *k, *v;
    size_t kl;
    for (const char *at = text; ini_next(&at, &k, &kl, &v, vlen);)
        if (kl == klen && !strncmp(k, key, klen)) return v;
    return NULL;
}
static void log_settings(bool all) {
    static char now[sizeof g_logged], out[8192];
    settings_text(now, sizeof now);
    size_t n = 0;
    // all: each setting; else what changed: the keys now (new or another value), then the ones gone (back to their
    // defaults, as a binding is when it's set back)
    for (int pass = 0; pass < (all ? 1 : 2); pass++) {
        const char *text = pass ? g_logged : now, *other = pass ? now : g_logged, *k, *v;
        size_t kl, vl;
        for (const char *at = text; ini_next(&at, &k, &kl, &v, &vl) && n + 80 < sizeof out;) {
            if (!logged_key(k, kl)) continue;
            size_t ol = 0;
            const char *ov = all ? NULL : ini_value(other, k, kl, &ol);
            int w = 0;
            const char *sep = n ? ", " : "";
            if (all) w = snprintf(out + n, sizeof out - n, "%s%.*s %.*s", sep, (int)kl, k, (int)vl, v);
            else if (pass == 0 && (!ov || ol != vl || memcmp(ov, v, vl)))
                w = snprintf(out + n, sizeof out - n, "%s%.*s %.*s -> %.*s", sep, (int)kl, k, ov ? (int)ol : 7,
                             ov ? ov : "default", (int)vl, v);
            else if (pass == 1 && !ov)
                w = snprintf(out + n, sizeof out - n, "%s%.*s %.*s -> default", sep, (int)kl, k, (int)vl, v);
            if (w > 0) n = n + (size_t)w < sizeof out ? n + (size_t)w : sizeof out - 1;
        }
    }
    if (n) LOG(all ? "[settings] %s\n" : "[settings] changed: %s\n", out);
    memcpy(g_logged, now, sizeof g_logged);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    Out o = {f};
    write_settings(&o);
    fclose(f);
    if (g_logged[0]) log_settings(false);
}

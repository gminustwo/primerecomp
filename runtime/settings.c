#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's primerecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    return e && *e ? e : "primerecomp.ini";
}

Settings g_settings = {
    .aspect = ASPECT_UNLOCKED,
    .hud = HUD_EDGES,
    .pause = PAUSE_FILL,
    .fullscreen = false,
    .volume = 1.0f,
    .reverb = true,
    .rumble = true,
    .movie_audio = true,
    .show_fps = false,
    .frame_rate = 60,
    .vsync = true,
    .upscaler = UPSCALE_FSR1,
    .res_lines = 2160,
    .fov = FOV_GAME,
    .gyro = true,
    .aniso = 16,
    .mouse_aim = true,
    .mouse_sens = 1.0f,
};

const int g_res_lines[N_RES_LINES] = {900, 1080, 1440, 2160, 4320};
const char *const g_res_names[N_RES_LINES] = {"900p (Switch)", "1080p", "1440p", "4K", "8K"};

// the offered height nearest to `lines` (a hand-edited larecomp.ini, LAS's "2x")
static int res_snap(int lines) {
    int best = g_res_lines[0];
    for (int i = 1; i < N_RES_LINES; i++)
        if (abs(g_res_lines[i] - lines) < abs(best - lines)) best = g_res_lines[i];
    return best;
}

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr, regionEU/EUen|EUes|...,
// regionJP/JPja, regionKR/KRkr, regionTW/TWch, regionCN/CNch), and the game picks one from the system language it
// reads back (runtime/services/am.c), exactly as it does on the console. Codes as the Switch reports them.
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                   "zh-CN", "zh-TW"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
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
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"ru", "ru"}, {"nl", "nl"},
                                                             {"de", "de"}, {"it", "it"}, {"fr", "fr"}, {"es", "es"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(l, map[i].loc)) return map[i].code;
    if (!strcmp(l, "zh")) return e[n] == '_' && e[n + 1] == 'T' ? "zh-TW" : "zh-CN";  // (TW / HK: traditional)
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
        return;
    }
    char line[256];
    bool tip_key = false;
    while (fgets(line, sizeof line, f)) {
        char key[64];
        char val[128] = "";
        if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) < 1) continue;
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
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "frame_rate")) {
            int r = atoi(val);
            g_settings.frame_rate = r == 120 || r == 30 ? r : r == 0 ? 120 : 60;  // (0: Uncapped, gone since 2026-10-27)
        }
        else if (!strcmp(key, "vsync")) g_settings.vsync = atoi(val) != 0;
        else if (!strcmp(key, "fov")) g_settings.fov = atoi(val) < FOV_MIN ? FOV_MIN : atoi(val) > FOV_MAX ? FOV_MAX : atoi(val);
        else if (!strcmp(key, "skippable_cutscenes")) g_settings.skippable_cutscenes = atoi(val) != 0;
        else if (!strcmp(key, "hint_minutes"))
            g_settings.hint_minutes = atoi(val) < 0 ? 0 : atoi(val) > HINT_MINUTES_MAX ? HINT_MINUTES_MAX : atoi(val);
        else if (!strcmp(key, "infinite_energy")) g_settings.inf_energy = atoi(val) != 0;
        else if (!strcmp(key, "infinite_missiles")) g_settings.inf_missiles = atoi(val) != 0;
        else if (!strcmp(key, "infinite_power_bombs")) g_settings.inf_power_bombs = atoi(val) != 0;
        else if (!strcmp(key, "gyro")) g_settings.gyro = atoi(val) != 0;
        else if (!strcmp(key, "mouse_aim")) g_settings.mouse_aim = atoi(val) != 0;
        else if (!strcmp(key, "mouse_sens")) g_settings.mouse_sens = (float)atof(val);
        else if (!strcmp(key, "mouse_invert")) g_settings.mouse_invert = atoi(val) != 0;
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
        else if (!strcmp(key, "face_layout")) bind_load_face_layout(!strcmp(val, "xbox") ? FACE_XBOX : FACE_NINTENDO);
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(val, g_language_codes[i])) g_settings.language = i;
        }
        else if (!strcmp(key, "menu_tip_seen")) g_settings.menu_tip_seen = atoi(val) != 0, tip_key = true;
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
    }
    fclose(f);
    if (!tip_key) g_settings.menu_tip_seen = true;  // (a settings file from before the note: not a first launch)
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    if (!(g_settings.mouse_sens >= MOUSE_SENS_MIN)) g_settings.mouse_sens = MOUSE_SENS_MIN;  // (NaN too)
    if (g_settings.mouse_sens > MOUSE_SENS_MAX) g_settings.mouse_sens = MOUSE_SENS_MAX;
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
    if (migrated) settings_save();
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# PrimeRecomp settings (edited from the in-game menu: F1, or L3+R3 on a controller)\n");
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_ORIGINAL ? "original" : "unlocked");
    fprintf(f, "hud = %s\n", g_settings.hud == HUD_MIDDLE ? "middle" : g_settings.hud == HUD_21_9 ? "21:9" : "edges");
    fprintf(f, "pause = %s\n", g_settings.pause == PAUSE_MIDDLE ? "middle" : "fill");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "rumble = %d\n", g_settings.rumble);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "frame_rate = %d\n", g_settings.frame_rate);
    fprintf(f, "vsync = %d\n", g_settings.vsync);
    fprintf(f, "fov = %d\nskippable_cutscenes = %d\ngyro = %d\n", g_settings.fov, g_settings.skippable_cutscenes,
            g_settings.gyro);
    fprintf(f, "mouse_aim = %d\nmouse_sens = %.2f\nmouse_invert = %d\n", g_settings.mouse_aim, g_settings.mouse_sens,
            g_settings.mouse_invert);
    fprintf(f, "hint_minutes = %d\ninfinite_energy = %d\ninfinite_missiles = %d\ninfinite_power_bombs = %d\n",
            g_settings.hint_minutes, g_settings.inf_energy, g_settings.inf_missiles, g_settings.inf_power_bombs);
    if (g_settings.steam_controller_direct) fprintf(f, "steam_controller_direct = 1\n");  // (written only when set)
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    fprintf(f, "aniso = %d\n", g_settings.aniso);
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode == RES_FIXED) fprintf(f, "resolution = %dp\n", g_settings.res_lines);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "face_layout = %s\n", g_settings.face_layout == FACE_XBOX ? "xbox" : "nintendo");
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    fprintf(f, "menu_tip_seen = %d\n", g_settings.menu_tip_seen);
    bind_settings_save(f);
    fclose(f);
}

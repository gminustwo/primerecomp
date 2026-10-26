// User settings (primerecomp.ini in the data folder; larecomp.ini before 2026-10-25), edited from the in-game menu.
#pragma once
#include <stdbool.h>

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum { RES_MATCH = 0, /* 1..8: LAS's 1x..8x, now RES_FIXED */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12,
       RES_FIXED = 13 /* g_settings.res_lines lines */ };
// (11 was 67% of the display: "67%" in the settings file now loads as RES_Q75)
// The fixed render heights the Video tab offers (lines; 900 = the Switch's docked image) and their names
enum { N_RES_LINES = 5 };
extern const int g_res_lines[N_RES_LINES];
extern const char *const g_res_names[N_RES_LINES];
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { PROMPTS_AUTO = 0, PROMPTS_NINTENDO = 1, PROMPTS_XBOX = 2, PROMPTS_PLAYSTATION = 3 };
// Face buttons: Nintendo = by position (right = A, bottom = B, top = X, left = Y), Xbox = by the Xbox labels (bottom
// = A, right = B, left = X, top = Y). Swaps the controller bindings (runtime/hle/bindings.c) and the menus' confirm /
// back buttons.
enum { FACE_NINTENDO = 0, FACE_XBOX = 1 };
enum { FOV_GAME = 55, FOV_MIN = 45, FOV_MAX = 90 };
#define MOUSE_SENS_MIN 0.1f  // (mouse aim sensitivity range: Controls tab)
#define MOUSE_SENS_MAX 10.0f
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 12 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ..., "Spanish")

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars
    bool fullscreen;
    int res_mode;        // RES_MATCH, RES_FIXED (res_lines), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int res_lines;       // RES_FIXED: render height in lines, one of g_res_lines (2160 = 4K)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    int aniso;           // texture filtering: anisotropic level forced on the game's mipmapped samplers (2..16), 0 =
                         // the game's own (runtime/nvn/vulkan/textures.c vk_sampler, renderer.m)
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool rumble;         // controller rumble (runtime/hle/input.c)
    bool movie_audio;
    bool show_fps;
    int frame_rate;      // frames per second: 60 (the Switch's), 120 (the simulation too), 30 (two 60 Hz steps a frame), or
                         // 0 = uncapped at 60 Hz simulation (runtime/hle/game.c); LARECOMP_FPS overrides it
    int fov;             // first-person vertical field of view in degrees, FOV_MIN..FOV_MAX (55: the game's): game.c
    bool skippable_cutscenes;  // every cutscene can be skipped, seen before or not (runtime/hle/game.c)
    bool gyro;           // the controller's gyro / accelerometer reach the game's motion controls (runtime/hle/motion.c)
    bool mouse_aim;      // the mouse turns and tilts the view itself (runtime/hle/game.c), captured while playing
    float mouse_sens;    // mouse aim: 1 = 0.044 degrees per mouse count (MOUSE_SENS_MIN..MAX)
    bool mouse_invert;   // mouse aim: moving the mouse forward looks down
    bool steam_controller_direct;  // (settings file only) SDL's own Steam Controller driver, for its gyro without
                                   // Steam Input (the window code sets SDL_HINT_JOYSTICK_HIDAPI_STEAM)
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int face_layout;     // FACE_*: which face button is A (set through bind_set_face_layout: it swaps the bindings)
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
    // Link's Awakening's texture patches (runtime/nvn/texpatch.c) still read these; nothing sets them any more (they
    // match none of Metroid Prime's textures), and the settings file no longer has them
    bool hide_hero_icon; // no Hero Mode icon on the HUD
    int prompts;         // PROMPTS_*: button glyphs (auto = follow the controller SDL reports)
    int tunic;           // index into g_tunic_colours (0 = Default)
    int hearts;          // index into g_heart_colours (0 = Default)
    char rando_seed[128];  // (runtime/main.c: LAS's randomizer seed argument) always empty
} Settings;

enum { N_TUNIC_COLOURS = 13, N_HEART_COLOURS = 9 };
extern const char *const g_tunic_colours[N_TUNIC_COLOURS];
extern const char *const g_heart_colours[N_HEART_COLOURS];

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
// Running on a Steam Deck (Linux: Steam's SteamDeck=1, or Valve's Jupiter / Galileo board)
bool settings_on_steam_deck(void);
// The Video tab's Steam Deck presets (also a Deck's first start, with no settings file yet): 30 = a steady 30 fps at
// the screen's resolution, 60 = 60 fps from 75% of it with FSR 1
void settings_steam_deck_preset(int fps);
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);

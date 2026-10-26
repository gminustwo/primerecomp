// Where PrimeRecomp keeps its files (runtime/paths.c).
//
// Data root: everything the app writes or the user brings - extracted/ (game files), save/,
// cache/ (shaders / pipelines), fonts/ (imported), primerecomp.ini. rt_paths_init() picks it and makes it the
// working directory, so the rest of the runtime opens those by their relative names. In order:
//   1. LARECOMP_DATA_ROOT (tests, scratch installs)
//   2. dev / portable mode: the starting directory already has extracted/ (a source checkout: `./build/primerecomp
//      extracted`, the test scripts) - used as is
//   3. macOS: ~/Library/Application Support/<RT_BUNDLE_ID> (the .app bundle itself is never written to)
//      Windows: the folder of the .exe (portable, no installer); refuses to start, with a message, if not writable
//      Linux: $XDG_DATA_HOME/primerecomp (~/.local/share/primerecomp)
//
// Resources: files that ship with the app (helper programs, the window icon) - next to the executable (the
// Windows folder, the .app's Contents/MacOS), in the .app's Contents/Resources, or in the source tree.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_BUNDLE_ID "io.github.primerecomp"

void rt_paths_init(void);         // first thing in main(), after plat_init
const char *rt_data_root(void);   // absolute
bool rt_dev_mode(void);           // dev / portable mode (rule 2 above): e.g. a source checkout

// A shipped file: `name` next to the executable or in Contents/Resources, else `dev_rel` in the source tree (the
// starting directory or the executable's parent; NULL = shipped builds only). Absolute path in out; false = none.
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n);

// The only way the app deletes game files. Removes the named entry of game_dir (an extracted/ folder) only if
// game_dir is <data root>/extracted itself (symlinks resolved; a symlinked extracted/ is refused) and the entry is
// one the setup creates (exefs, romfs, romfs.bin, leftover folders) and not a link. The folder the user picked
// never gets here. true = removed or not there.
bool rt_remove_game_file(const char *game_dir, const char *name);
// All the game files set up there (rt_remove_game_file on each, then the empty folder). Saves and settings stay.
bool rt_remove_game_files(const char *game_dir);
// Bytes taken by the extracted game files in game_dir (links not followed).
unsigned long long rt_game_files_size(const char *game_dir);

// Updates without setting up again. macOS and Linux keep one data root for every version. On Windows each copy keeps
// its own (the .exe's folder), so a copy notes where it is once its game files are set up -
// %LOCALAPPDATA%\PrimeRecomp\copies.txt, one data root per line, the latest first - and a newer version unpacked into
// another folder takes an earlier copy's game files, saves and settings from there (runtime/ui/launcher.mm). Only
// copies whose data root is the .exe's folder (no LARECOMP_DATA_ROOT, no other starting folder) take part, and only
// with a real extracted/ folder (a dev worktree's link is left out).
void rt_note_copy(void);  // this copy's game files are set up: noted first
// The other copies' data roots, latest first (at most max). 0 elsewhere, or when this copy doesn't take part.
int rt_earlier_copies(char (*out)[1100], int max);
// The saves (save/) and, with settings, primerecomp.ini of the data root `from`, copied into this one where it has
// none of its own. Real copies, not links: the game writes them in place.
void rt_copy_saves(const char *from, bool settings);

typedef struct SDL_Window SDL_Window;
void rt_window_icon(SDL_Window *w);  // the shipped icon.png, if any

#ifdef __cplusplus
}
#endif

// Atmosphere cheat files of the emulator mods (runtime/hle/atmocheats.c): listed and switched in the Cheats tab
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ATMO_HEADING, ATMO_MASTER, ATMO_DATA, ATMO_CODE, ATMO_UNSUPPORTED };
typedef struct {
    char mod[256], name[200];
    int kind;        // ATMO_*: a heading (no code), a master cheat (always on), one that can be switched, or one that
    bool on;         // can't run here (CODE: it writes the game's code; UNSUPPORTED: note says why)
    char note[160];
} AtmoCheat;

void atmo_cheats_load(void);  // (again) the cheat files of the mods switched on (at the game's start, the Cheats tab)
int atmo_cheats_count(void);
bool atmo_cheat_info(int i, AtmoCheat *out);
void atmo_cheat_set(int i, bool on);  // (into the settings: saved with them)
const char *atmo_cheats_file(void);   // the file name a mod's cheats folder needs: "<build id>.txt"
void atmo_cheats_frame(void);         // (the game's thread, each frame: runs them 12 times a second)

#ifdef __cplusplus
}
#endif

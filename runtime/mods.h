// Emulator mods (runtime/mods.c): mods made for Ryujinx / yuzu and the Switch, a folder each in mods/
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODS_MAX 64
typedef struct {
    char name[256];                               // the folder's name in mods/
    char romfs[1100], exefs[1100], cheats[1100];  // its parts ("": none)
    bool on;
    int romfs_files, exefs_files, cheat_files;
    int replaced, added, covered;  // as the game last opened its RomFS (counted): romfs files it replaced / added, and
    bool counted;                  // those of its files a higher mod had too (mods_note_romfs)
} ModInfo;

void mods_scan(void);                 // (again) what's in mods/: at start, and when the Mods tab opens
int mods_count(void);                 // in priority order, the highest first
bool mods_info(int i, ModInfo *out);  // a copy
void mods_set_on(int i, bool on);     // (into the settings: saved with them)
void mods_move(int i, int to);        // to place `to` in the order
const char *mods_folder(void);
// the romfs folders of the mods switched on, the highest first, with their names; n of them
int mods_romfs_roots(char (*root)[1100], char (*name)[256], int max);
void mods_note_romfs(const char *name, int replaced, int added, int covered);  // (services/fs.c, as the game opens it)

#ifdef __cplusplus
}
#endif

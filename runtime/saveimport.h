// Saves from elsewhere (start screen: Settings > Prelaunch, "Saves"): Metroid Prime Remastered's save as the Switch
// emulators keep it - Ryujinx, yuzu and the projects that carried it on (their folders: saveimport.c emu_dirs) - or a folder of
// its files (a backup made on a Switch, a portable emulator's), copied into PrimeRecomp's save folder (save/save_0, the
// game's save mount: runtime/services/fs.c). Only while no game is running: the start screen comes after teardown.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char from[24];           // "Ryujinx", "Eden", ... - "Backup" for one an import kept
    char user[64];           // the emulator's profile name ("" = not known)
    char dir[1100];          // the folder holding the save's files
    long long mtime;         // the newest file's (seconds since 1970)
    unsigned long long bytes;
    int files;
} FoundSave;

// The game's saves the emulators have, and the backups imports kept, newest first (at most max). Scans the disk.
int saves_find(FoundSave *out, int max);
// A folder picked by the player: its save (the game's save.bin in it, or in the one folder level under it). false:
// none there.
bool saves_from_folder(const char *dir, FoundSave *out);
// Copies s in as PrimeRecomp's save. The current one, if any, is moved aside first to save/save_0.before-import-<date
// and time> (its path into backup, "" = there was none). false: nothing changed (why in msg).
bool saves_import(const FoundSave *s, char *backup, size_t backup_cap, char *msg, size_t msg_cap);
// The save folder (absolute: for showing it)
const char *saves_dir(void);

#ifdef __cplusplus
}
#endif

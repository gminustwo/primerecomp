// The game's RomFS image, laid out from a folder of game files (runtime/romfs_build.c). The game reads its data through
// one raw RomFS image; runtime/services/fs.c serves that image from this layout - the header and tables in memory, each
// file's data straight from its file in the player's romfs/ folder - so no second copy of the game data is kept.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROMFS_DATA_START 0x200u
typedef struct { uint64_t start, size; char *path; } RomfsLayoutFile;  // start: offset in the image
typedef struct {
    uint8_t header[ROMFS_DATA_START];  // the image's first bytes (the 0x50-byte header, then zeros)
    uint64_t tables_off, tables_size;  // dir hash, dir entries, file hash and file entries, one blob
    uint8_t *tables;
    RomfsLayoutFile *files;  // sorted by start; the bytes between files read as zeros
    int nfiles;
    uint64_t size;  // the whole image
} RomfsLayout;

// Lays out the RomFS image of the folder `dir`. false = failed (why: a short message).
bool romfs_layout(const char *dir, RomfsLayout *out, char *why, size_t why_size);
void romfs_layout_free(RomfsLayout *l);

// Bytes of file data under dir (the image is about this plus its tables).
uint64_t romfs_dir_bytes(const char *dir);

// The romfs folder dir against the game's own files (v1.0.0's, runtime/romfs_files.h): how many of them are missing.
// *wrong_size (if given): how many are there at another size (modded, or cut short). each (if given) is called for
// every one missing (have -1) or at another size (have: its size there; want: the game's).
typedef void (*RomfsCheckFn)(void *ctx, const char *path, long long have, unsigned long long want);
int romfs_check_files(const char *dir, int *wrong_size, RomfsCheckFn each, void *ctx);
int romfs_game_file_count(void);  // (341)
const char *romfs_game_file_path(int i);  // the i-th of them ("Worlds/MP1/!IceWorld/01_Ice_Plaza.pak")

// Copies the folder tree src to dst (hard links where the file system allows: no second copy of 6 GB). progress as
// above. false = failed.
bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));
// The same with only the top-level entries keep says yes to (name, whether it's a folder): an exefs and a romfs
// extracted into one folder copied apart (issue #25: both copies held all of it).
bool romfs_copy_tree_filtered(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total),
                              bool (*keep)(const char *name, bool dir));
// One file, the same way.
bool romfs_copy_file(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));

#ifdef __cplusplus
}
#endif

// The player's game package (NSP / XCI / program NCA) read in place (runtime/nxpkg.c): its ExeFS written out, and
// the RomFS served straight from the package, decrypted (AES-CTR) and, where the package compresses it, decompressed
// (BucketTree / LZ4) on the fly - no 7 GB copy of the game data. From EoWRecomp's runtime/nxpkg (MIT; see nxpkg.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NX_OK = 0, NX_ERR_KEYS, NX_ERR_PACKAGE, NX_ERR_WRITE };

// Setup: finds the program NCA in `package` (keys: the player's prod.keys; for a title-key-encrypted NCA, the NSP's
// ticket or title.keys beside prod.keys), writes its ExeFS files into <out_dir>/exefs/ and the RomFS description to
// <out_dir>/romfs.src (the package's path and size, where its RomFS section is and its key: what nx_romfs_open needs).
// title_id: the program's, out (may be NULL). Returns NX_OK, or what failed: NX_ERR_KEYS (these keys can't open it:
// another keys file may), NX_ERR_PACKAGE (not a usable game package), NX_ERR_WRITE (the files couldn't be written);
// why: a short message for the player.
int nx_setup(const char *package, const char *keys, const char *out_dir, uint64_t *title_id, char *why, size_t why_size);

// Keys files that may open a package, best first: prod.keys / keys.txt beside it, then the emulators' own (Ryujinx,
// yuzu and its successors) and ~/.switch. Up to max paths into out; returns the count.
int nx_find_keys(const char *package, char out[][1100], int max);

// The Switch emulators' data folders on this computer: Ryujinx's (ryujinx: keys in system/, saves in bis/) and yuzu's
// and its successors' (keys/, nand/), from the user's config / data folders. Up to max into out; returns the count.
typedef struct { char name[16]; char root[1100]; bool ryujinx; } NxEmuDir;
int nx_emu_dirs(NxEmuDir *out, int max);

typedef struct NxRomfs NxRomfs;
// From a romfs.src written by nx_setup. NULL = missing, or the package moved or changed (why).
NxRomfs *nx_romfs_open(const char *src_path, char *why, size_t why_size);
uint64_t nx_romfs_size(const NxRomfs *r);
// pread of the RomFS image; thread-safe. Returns size (less past the end), or -1 (an I/O error / corrupt package).
ssize_t nx_romfs_pread(NxRomfs *r, void *dst, uint64_t size, uint64_t off);
// A file in the RomFS by its path ("UI/MPR1/UIMP1.pak"): its offset in the image and its size. false: not there.
bool nx_romfs_find(NxRomfs *r, const char *path, uint64_t *off, uint64_t *size);
void nx_romfs_close(NxRomfs *r);

// (tests) the AES / LZ4 primitives; nx_aes_ctr_soft is the plain-C path the others are checked against
void nx_aes128_ecb_decrypt(const uint8_t key[16], uint8_t *data, size_t len);
void nx_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t len);
void nx_aes_ctr_soft(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t len);
int nx_lz4_decompress(const uint8_t *src, int src_len, uint8_t *dst, int dst_len);

#ifdef __cplusplus
}
#endif

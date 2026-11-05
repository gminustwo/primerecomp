// Saves from elsewhere (saveimport.h). Where the emulators keep a game's save:
//  - yuzu and its successors: <root>/nand/user/save/0000000000000000/<user>/<title id>/, the files as the game wrote
//    them (with .yuzu_save_size beside: the emulator's, not copied). <user>: the profile's UUID, its second half's
//    64-bit value in hex then its first's - as profiles.dat (nand/system/save/8000000000000010/su/avators/) has them,
//    with the names. Older versions also left an empty folder under a mangled user id: skipped (no files).
//  - Ryujinx: <root>/bis/user/save/<save id>/0/ (1/: a save in progress), the save's attribute - program id at 0,
//    user id at 8, type at 0x20 (1 account, 3 device) - in its ExtraData1 / ExtraData0 beside (LibHac's directory
//    saves), or else in the save index (bis/system/save/8000000000000000/0/imkvdb.arc: "IMKV", count at 8, then
//    "IMEN" key-size value-size key value: the attribute, then the save id). Names: system/Profiles.json.
// Metroid Prime Remastered's save is one file, save.bin: a checksum, its size, then 1C 00 00 00 "5ESU" - how a folder
// the player picks is checked.
#include "saveimport.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nxpkg.h"
#include "paths.h"
#include "platform.h"  // (Windows: a POSIX mkdir)

#define TITLE_ID 0x010012101468C000ull
#define TITLE_HEX "010012101468C000"
#define BACKUP_PREFIX "save_0.before-import-"

static const char *save_root(void) {  // (as services/fs.c: test runs use a scratch folder)
    const char *d = getenv("LARECOMP_SAVE_DIR");
    return d && *d ? d : "save";
}

const char *saves_dir(void) {
    static char p[1200];
    const char *r = save_root();
    bool abs = r[0] == '/' || r[0] == '\\' || (r[0] && r[1] == ':');
    snprintf(p, sizeof p, abs ? "%s" : "%s/%s", abs ? r : rt_data_root(), r);
#ifdef _WIN32
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
#endif
    return p;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool skipped(const char *name) {  // (not the game's files)
    size_t l = strlen(name);
    return !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".yuzu_save_size") ||
           (l > 8 && !strcmp(name + l - 8, ".pending"));  // (PrimeRecomp's: a save in progress, never committed)
}

typedef struct { int files; unsigned long long bytes; long long mtime; } Stats;

static void tree_stats(const char *dir, Stats *st, int depth) {
    DIR *d = depth < 8 ? opendir(dir) : NULL;
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (skipped(e->d_name)) continue;
        char p[1400];
        struct stat s;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &s)) continue;
        if (S_ISDIR(s.st_mode)) { tree_stats(p, st, depth + 1); continue; }
        st->files++, st->bytes += (unsigned long long)s.st_size;
        if ((long long)s.st_mtime > st->mtime) st->mtime = (long long)s.st_mtime;
    }
    closedir(d);
}

static bool find_ci(const char *dir, const char *name, char *out, size_t cap) {  // name in dir, any case
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        if (strlen(e->d_name) != strlen(name)) continue;
        bool same = true;
        for (size_t i = 0; same && name[i]; i++) same = tolower((unsigned char)e->d_name[i]) == tolower((unsigned char)name[i]);
        if (same) snprintf(out, cap, "%s/%s", dir, e->d_name), found = true;
    }
    closedir(d);
    return found;
}

static uint8_t *read_all(const char *path, size_t *size, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 && (size_t)n <= max ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *size = (size_t)n;
    return b;
}

static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static bool add(FoundSave *out, int *n, int max, const char *from, const char *user, const char *dir) {
    Stats st = {0};
    tree_stats(dir, &st, 0);
    if (!st.files || *n >= max) return false;
    FoundSave *s = &out[(*n)++];
    memset(s, 0, sizeof *s);
    snprintf(s->from, sizeof s->from, "%s", from);
    snprintf(s->user, sizeof s->user, "%s", user ? user : "");
    snprintf(s->dir, sizeof s->dir, "%s", dir);
#ifdef _WIN32
    for (char *c = s->dir; *c; c++) if (*c == '/') *c = '\\';
#endif
    s->mtime = st.mtime, s->bytes = st.bytes, s->files = st.files;
    return true;
}

// ---- yuzu and its successors
static void yuzu_name(const char *root, const char *user_dir, char *out, size_t cap) {
    out[0] = 0;
    char p[1300];
    snprintf(p, sizeof p, "%s/nand/system/save/8000000000000010/su/avators/profiles.dat", root);
    size_t n;
    uint8_t *d = read_all(p, &n, 1 << 16);
    for (size_t off = 0x10; d && off + 0xC8 <= n; off += 0xC8) {
        char id[40];
        snprintf(id, sizeof id, "%016llX%016llX", (unsigned long long)le64(d + off + 8), (unsigned long long)le64(d + off));
        if (strcasecmp(id, user_dir)) continue;
        snprintf(out, cap, "%.32s", (const char *)d + off + 0x28);
        break;
    }
    free(d);
}

static void find_yuzu(const char *root, const char *name, FoundSave *out, int *n, int max) {
    char base[1300];
    snprintf(base, sizeof base, "%s/nand/user/save/0000000000000000", root);
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strlen(e->d_name) != 32) continue;
        char u[1400], t[1500], who[64];
        snprintf(u, sizeof u, "%s/%s", base, e->d_name);
        if (!find_ci(u, TITLE_HEX, t, sizeof t)) continue;
        yuzu_name(root, e->d_name, who, sizeof who);
        add(out, n, max, name, who, t);
    }
    closedir(d);
}

// ---- Ryujinx
typedef struct { uint64_t id; uint8_t attr[0x40]; } IndexEntry;

static int ryujinx_index(const char *root, IndexEntry *out, int max) {
    char p[1300];
    snprintf(p, sizeof p, "%s/bis/system/save/8000000000000000/0/imkvdb.arc", root);
    size_t n;
    uint8_t *d = read_all(p, &n, 1 << 22);
    int k = 0;
    if (d && n >= 12 && !memcmp(d, "IMKV", 4)) {
        uint32_t count;
        memcpy(&count, d + 8, 4);
        for (size_t off = 12; count-- && off + 12 <= n && k < max;) {
            uint32_t ks, vs;
            memcpy(&ks, d + off + 4, 4);
            memcpy(&vs, d + off + 8, 4);
            if (memcmp(d + off, "IMEN", 4) || off + 12 + (size_t)ks + vs > n) break;
            if (ks >= 0x40 && vs >= 8) {
                memcpy(out[k].attr, d + off + 12, 0x40);
                out[k++].id = le64(d + off + 12 + ks);
            }
            off += 12 + (size_t)ks + vs;
        }
    }
    free(d);
    return k;
}

static void ryujinx_name(const char *root, const uint8_t *attr, char *out, size_t cap) {
    out[0] = 0;
    char p[1300], id1[40], id2[40];
    snprintf(p, sizeof p, "%s/system/Profiles.json", root);
    snprintf(id1, sizeof id1, "%016llx%016llx", (unsigned long long)le64(attr + 8), (unsigned long long)le64(attr + 16));
    snprintf(id2, sizeof id2, "%016llx%016llx", (unsigned long long)le64(attr + 16), (unsigned long long)le64(attr + 8));
    size_t n;
    char *j = (char *)read_all(p, &n, 1 << 24);
    for (char *q = j; q && (q = strstr(q, "\"user_id\""));) {  // ("user_id": "<32 hex>", "name": "<name>", ...)
        q += 9;
        char *v = strchr(q, '"');
        if (!v || strlen(v) < 34) break;
        v++;
        bool match = !strncasecmp(v, id1, 32) || !strncasecmp(v, id2, 32);
        char *next = strstr(v, "\"user_id\""), *nm = strstr(v, "\"name\"");
        if (match && nm && (!next || nm < next) && (nm = strchr(nm + 6, '"'))) {
            nm++;
            size_t l = strcspn(nm, "\"");
            snprintf(out, cap, "%.*s", (int)(l < cap ? l : cap - 1), nm);
            break;
        }
        q = v;
    }
    free(j);
}

static void find_ryujinx(const char *root, const char *name, FoundSave *out, int *n, int max) {
    char base[1300];
    snprintf(base, sizeof base, "%s/bis/user/save", root);
    DIR *d = opendir(base);
    if (!d) return;
    static IndexEntry index[512];
    int nindex = -1;  // (read when a save has no extra data)
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end;
        uint64_t id = strtoull(e->d_name, &end, 16);
        if (strlen(e->d_name) != 16 || *end) continue;
        uint8_t attr[0x40];
        bool known = false;
        for (int k = 1; k >= 0 && !known; k--) {
            char p[1600];
            snprintf(p, sizeof p, "%s/%s/ExtraData%d", base, e->d_name, k);
            FILE *f = fopen(p, "rb");
            known = f && fread(attr, 1, sizeof attr, f) == sizeof attr;
            if (f) fclose(f);
        }
        if (!known && nindex < 0) nindex = ryujinx_index(root, index, 512);
        for (int k = 0; !known && k < nindex; k++)
            if (index[k].id == id) memcpy(attr, index[k].attr, sizeof attr), known = true;
        if (!known || le64(attr) != TITLE_ID || (attr[0x20] != 1 && attr[0x20] != 3)) continue;
        char dir[1500], who[64];
        snprintf(dir, sizeof dir, "%s/%s/0", base, e->d_name);
        ryujinx_name(root, attr, who, sizeof who);
        add(out, n, max, name, who, dir);
    }
    closedir(d);
}

// ---- the game's save.bin
static bool game_save_in(const char *dir) {
    char p[1300];
    if (!find_ci(dir, "save.bin", p, sizeof p)) return false;
    FILE *f = fopen(p, "rb");
    uint8_t h[16];
    bool ok = f && fread(h, 1, 16, f) == 16;
    long size = ok && !fseek(f, 0, SEEK_END) ? ftell(f) : -1;
    if (f) fclose(f);
    uint32_t len;
    memcpy(&len, h + 4, 4);
    return ok && (long)len == size && !memcmp(h + 8, "\x1c\0\0\0" "5ESU", 8);
}

static int newest_first(const void *a, const void *b) {
    long long x = ((const FoundSave *)a)->mtime, y = ((const FoundSave *)b)->mtime;
    return x < y ? 1 : x > y ? -1 : 0;
}

int saves_find(FoundSave *out, int max) {
    int n = 0;
    NxEmuDir emu[16];
    for (int i = 0, ne = nx_emu_dirs(emu, 16); i < ne; i++) {
        if (emu[i].ryujinx) find_ryujinx(emu[i].root, emu[i].name, out, &n, max);
        else find_yuzu(emu[i].root, emu[i].name, out, &n, max);
    }
    DIR *d = opendir(save_root());  // (the saves earlier imports moved aside)
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[1300];
        snprintf(p, sizeof p, "%s/%s", save_root(), e->d_name);
        if (!strncmp(e->d_name, BACKUP_PREFIX, strlen(BACKUP_PREFIX)) && is_dir(p)) add(out, &n, max, "Backup", "", p);
    }
    if (d) closedir(d);
    qsort(out, (size_t)n, sizeof *out, newest_first);
    return n;
}

bool saves_from_folder(const char *dir, FoundSave *out) {
    FoundSave found[32];
    int n = 0;
    if (game_save_in(dir)) add(found, &n, 32, "Folder", "", dir);
    char p[1300];  // (an emulator's own folder, as a portable one keeps it: user/ for yuzu's, portable/ for Ryujinx's)
    static const char *const yuzu[] = {"", "/user"}, *const ryu[] = {"", "/portable"};
    for (int i = 0; i < 2 && !n; i++) {
        snprintf(p, sizeof p, "%s%s", dir, yuzu[i]);
        find_yuzu(p, "Folder", found, &n, 32);
        snprintf(p, sizeof p, "%s%s", dir, ryu[i]);
        find_ryujinx(p, "Folder", found, &n, 32);
    }
    DIR *d = n ? NULL : opendir(dir);  // (one level down: a backup tool's folder of dated copies)
    struct dirent *e;
    while (d && (e = readdir(d))) {
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!skipped(e->d_name) && is_dir(p) && game_save_in(p)) add(found, &n, 32, "Folder", "", p);
    }
    if (d) closedir(d);
    if (!n) return false;
    qsort(found, (size_t)n, sizeof *found, newest_first);
    *out = found[0];
    return true;
}

// ---- the import
static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    bool ok = in && out;
    char buf[1 << 15];
    size_t k;
    while (ok && (k = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, k, out) == k;
    if (in && ferror(in)) ok = false;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    return ok;
}

static bool copy_tree(const char *src, const char *dst, int depth) {  // src's contents into dst (made)
    if (depth > 8 || (mkdir(dst, 0755) && errno != EEXIST)) return false;
    DIR *d = opendir(src);
    if (!d) return false;
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        if (skipped(e->d_name)) continue;
        char s[1400], t[1400];
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
        ok = is_dir(s) ? copy_tree(s, t, depth + 1) : copy_file(s, t);
    }
    closedir(d);
    return ok;
}

static void remove_tree(const char *p, int depth) {
    DIR *d = is_dir(p) && depth < 8 ? opendir(p) : NULL;
    if (!d) { unlink(p); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1400];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        remove_tree(c, depth + 1);
    }
    closedir(d);
    rmdir(p);
}

bool saves_import(const FoundSave *s, char *backup, size_t backup_cap, char *msg, size_t msg_cap) {
    backup[0] = msg[0] = 0;
    const char *root = save_root();
    char dst[1100];
    mkdir(root, 0755);
    snprintf(dst, sizeof dst, "%s/save_0", root);
    Stats st = {0};
    tree_stats(dst, &st, 0);
    if (st.files) {  // (the current save, moved aside whole: a rename, nothing copied or lost)
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char stamp[32];
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm);
        snprintf(backup, backup_cap, "%s/" BACKUP_PREFIX "%s", root, stamp);
        for (int k = 2; is_dir(backup) && k < 100; k++) snprintf(backup, backup_cap, "%s/" BACKUP_PREFIX "%s-%d", root, stamp, k);
        if (rename(dst, backup)) {
            snprintf(msg, msg_cap, "Couldn't move your current save aside (%s): nothing was changed.", strerror(errno));
            backup[0] = 0;
            return false;
        }
    } else {
        remove_tree(dst, 0);  // (empty, or only working copies a crash left)
    }
    if (!copy_tree(s->dir, dst, 0)) {
        int err = errno;
        remove_tree(dst, 0);
        bool back = !backup[0] || !rename(backup, dst);
        snprintf(msg, msg_cap, "Couldn't copy that save (%s). %s", strerror(err),
                 back ? "Your own save is as it was." : "Your own save is in the backup folder.");
        if (back) backup[0] = 0;
        return false;
    }
    fprintf(stderr, "[save] imported %s%s%s (%s, %d files, %llu bytes)%s%s\n", s->from, s->user[0] ? " / " : "",
            s->user, s->dir, s->files, s->bytes, backup[0] ? "; the earlier save kept in " : "", backup);
    const char *base = strrchr(s->dir, '/'), *b2 = strrchr(s->dir, '\\');
    base = b2 > base ? b2 : base;
    if (!strcmp(s->from, "Backup") && base && !strncmp(base + 1, BACKUP_PREFIX, strlen(BACKUP_PREFIX))) {
        remove_tree(s->dir, 0);  // (a backup put back: it's the save again, not kept twice)
        snprintf(msg, msg_cap, "Your earlier save is back.");
    } else {
        snprintf(msg, msg_cap, "Save imported from %s.", s->from);
    }
    return true;
}

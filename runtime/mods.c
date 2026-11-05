// Emulator mods (F1 menu, Mods tab): the mods made for Ryujinx / yuzu and the Switch's Atmosphere, each a folder in
// mods/ (where texture mods go too - those are image files, not parts: never listed here). A mod's parts are
// found where such a mod has them, its archive unpacked into its folder as it comes:
//   <mod>/romfs, <mod>/exefs, <mod>/cheats
//   <mod>/atmosphere/contents/010012101468C000/... , <mod>/contents/<that>/... , <mod>/<that>/...
//   (and all that one folder further down, when the archive held a folder with the mod in it)
// romfs/: game files - each served in place of the game's own file of that path, or added to the game's RomFS when
// it has none (services/fs.c rom_index, as the game opens its RomFS). cheats/: Atmosphere cheat files
// (runtime/hle/cheatvm.c). exefs/: code patches (.ips / .pchtxt) - listed, not used: the game's code is compiled
// ahead of time, from the game files as they are.
// The order decides when two mods have the same file: the higher one's is used. Order and switches are kept by
// folder name in the settings (mods_order, mods_disabled); a new folder goes on, below the named ones. Changes are
// used from the game's next start (a reboot to the title screen does).
#include "mods.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rt.h"
#include "settings.h"

#define MODS_DIR "mods"
#define TITLE_ID "010012101468c000"

static ModInfo g_mods[MODS_MAX];
static int g_n;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// what the game's last RomFS open counted, by name (kept across scans)
static struct { char name[256]; int replaced, added, covered; } g_counts[MODS_MAX];
static int g_ncounts;

const char *mods_folder(void) { return MODS_DIR; }

static bool is_dir(const char *p) {
    struct stat sb;
    return stat(p, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static bool eq_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

// dir's sub-folder named `name` in any case (a Linux file system's case matters, an archive's names vary)
static bool child_dir(const char *dir, const char *name, char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        if (!eq_ci(e->d_name, name)) continue;
        snprintf(out, cap, "%s/%s", dir, e->d_name);
        found = is_dir(out);
    }
    closedir(d);
    return found;
}

static int count_files(const char *dir, const char *ext) {  // files under dir (ext: only those ending so, any case)
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_dir(p)) { n += count_files(p, ext); continue; }
        size_t l = strlen(e->d_name), el = ext ? strlen(ext) : 0;
        n += !ext || (l >= el && eq_ci(e->d_name + l - el, ext));
    }
    closedir(d);
    return n;
}

// romfs / exefs / cheats in base or where an Atmosphere / emulator layout has them; true if any
static bool find_parts(const char *base, ModInfo *m) {
    char roots[4][1100], tmp[1100];
    int nr = 0;
    snprintf(roots[nr++], sizeof roots[0], "%s", base);
    char a[1100], c[1100];
    if (child_dir(base, "atmosphere", a, sizeof a) && child_dir(a, "contents", c, sizeof c) &&
        child_dir(c, TITLE_ID, roots[nr], sizeof roots[0]))
        nr++;
    if (child_dir(base, "contents", c, sizeof c) && child_dir(c, TITLE_ID, roots[nr], sizeof roots[0])) nr++;
    if (child_dir(base, TITLE_ID, roots[nr], sizeof roots[0])) nr++;
    for (int i = 0; i < nr; i++) {
        if (!m->romfs[0] && child_dir(roots[i], "romfs", tmp, sizeof tmp)) snprintf(m->romfs, sizeof m->romfs, "%s", tmp);
        if (!m->exefs[0] && child_dir(roots[i], "exefs", tmp, sizeof tmp)) snprintf(m->exefs, sizeof m->exefs, "%s", tmp);
        if (!m->cheats[0] && child_dir(roots[i], "cheats", tmp, sizeof tmp)) snprintf(m->cheats, sizeof m->cheats, "%s", tmp);
    }
    // Atmosphere's code patch folders: atmosphere/exefs_patches/<name>/<build id>.ips
    if (!m->exefs[0] && child_dir(base, "atmosphere", a, sizeof a) && child_dir(a, "exefs_patches", tmp, sizeof tmp))
        snprintf(m->exefs, sizeof m->exefs, "%s", tmp);
    return m->romfs[0] || m->exefs[0] || m->cheats[0];
}

// the place of name in a '|' list (-1: not there)
static int list_index(const char *list, const char *name) {
    int i = 0;
    for (const char *p = list; *p; i++) {
        const char *bar = strchr(p, '|');
        size_t l = bar ? (size_t)(bar - p) : strlen(p);
        if (l == strlen(name) && !strncmp(p, name, l)) return i;
        if (!bar) break;
        p = bar + 1;
    }
    return -1;
}

typedef struct { int rank; ModInfo m; } Ranked;
static int by_rank(const void *a, const void *b) {
    const Ranked *x = a, *y = b;
    if (x->rank != y->rank) return x->rank < y->rank ? -1 : 1;
    return strcmp(x->m.name, y->m.name);
}

static void save_lists(void) {  // (g_lock held) the order and switches into the settings
    char order[sizeof g_settings.mods_order] = "", off[sizeof g_settings.mods_disabled] = "";
    for (int i = 0; i < g_n; i++) {
        size_t l = strlen(order);
        snprintf(order + l, sizeof order - l, "%s%s", l ? "|" : "", g_mods[i].name);
        if (!g_mods[i].on) {
            l = strlen(off);
            snprintf(off + l, sizeof off - l, "%s%s", l ? "|" : "", g_mods[i].name);
        }
    }
    snprintf(g_settings.mods_order, sizeof g_settings.mods_order, "%s", order);
    snprintf(g_settings.mods_disabled, sizeof g_settings.mods_disabled, "%s", off);
}

static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;  // (the UI's thread, or the game's as it opens its
                                                                 // RomFS: one scan at a time - its lists are static)
static void scan(void);
void mods_scan(void) {
    pthread_mutex_lock(&g_scan_lock);
    scan();
    pthread_mutex_unlock(&g_scan_lock);
}

static void scan(void) {
    static ModInfo found[MODS_MAX];
    int n = 0;
    DIR *d = opendir(MODS_DIR);
    struct dirent *e;
    while (d && n < MODS_MAX && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, '|')) continue;
        char base[1100];
        snprintf(base, sizeof base, "%s/%s", MODS_DIR, e->d_name);
        if (!is_dir(base)) continue;
        ModInfo *m = &found[n];
        memset(m, 0, sizeof *m);
        snprintf(m->name, sizeof m->name, "%s", e->d_name);
        if (!find_parts(base, m)) {  // (one folder further down: an archive that held a folder)
            DIR *s = opendir(base);
            struct dirent *f;
            char only[1100] = "";
            int dirs = 0;
            while (s && (f = readdir(s))) {
                if (f->d_name[0] == '.') continue;
                char p[1400];
                snprintf(p, sizeof p, "%s/%s", base, f->d_name);
                if (is_dir(p) && dirs++ == 0) snprintf(only, sizeof only, "%s", p);
            }
            if (s) closedir(s);
            if (dirs != 1 || !find_parts(only, m)) continue;  // (a folder of texture mods, or nothing a mod has)
        }
        m->romfs_files = m->romfs[0] ? count_files(m->romfs, NULL) : 0;
        m->exefs_files = m->exefs[0] ? count_files(m->exefs, ".ips") + count_files(m->exefs, ".pchtxt") : 0;
        m->cheat_files = m->cheats[0] ? count_files(m->cheats, ".txt") : 0;
        n++;
    }
    if (d) closedir(d);
    static Ranked r[MODS_MAX];  // (the saved order first, then new ones by name; the switches)
    for (int i = 0; i < n; i++) {
        int k = list_index(g_settings.mods_order, found[i].name);
        r[i].rank = k < 0 ? 1 << 20 : k;
        r[i].m = found[i];
        r[i].m.on = list_index(g_settings.mods_disabled, found[i].name) < 0;
    }
    qsort(r, n, sizeof *r, by_rank);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < n; i++) g_mods[i] = r[i].m;
    g_n = n;
    for (int i = 0; i < g_n; i++)
        for (int k = 0; k < g_ncounts; k++)
            if (!strcmp(g_counts[k].name, g_mods[i].name)) {
                g_mods[i].replaced = g_counts[k].replaced, g_mods[i].added = g_counts[k].added;
                g_mods[i].covered = g_counts[k].covered, g_mods[i].counted = true;
            }
    if (g_n) save_lists();  // (the order as it is now, new mods in it)
    pthread_mutex_unlock(&g_lock);
    static bool logged;
    for (int i = 0; i < n && !logged; i++)
        LOG("[mods] %d. %s (%s): %s%s%s%d game files, %d code patches, %d cheat files\n", i + 1, g_mods[i].name,
            g_mods[i].on ? "on" : "off", g_mods[i].romfs[0] ? "romfs, " : "", g_mods[i].exefs[0] ? "exefs, " : "",
            g_mods[i].cheats[0] ? "cheats, " : "", g_mods[i].romfs_files, g_mods[i].exefs_files, g_mods[i].cheat_files);
    logged = true;
}

int mods_count(void) {
    pthread_mutex_lock(&g_lock);
    int n = g_n;
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool mods_info(int i, ModInfo *out) {
    pthread_mutex_lock(&g_lock);
    bool ok = i >= 0 && i < g_n;
    if (ok) *out = g_mods[i];
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void mods_set_on(int i, bool on) {
    pthread_mutex_lock(&g_lock);
    if (i >= 0 && i < g_n) g_mods[i].on = on, save_lists();
    pthread_mutex_unlock(&g_lock);
}

void mods_move(int i, int to) {
    pthread_mutex_lock(&g_lock);
    if (i >= 0 && i < g_n && to >= 0 && to < g_n && to != i) {
        ModInfo m = g_mods[i];
        if (to < i) memmove(&g_mods[to + 1], &g_mods[to], sizeof m * (size_t)(i - to));
        else memmove(&g_mods[i], &g_mods[i + 1], sizeof m * (size_t)(to - i));
        g_mods[to] = m;
        save_lists();
    }
    pthread_mutex_unlock(&g_lock);
}

int mods_romfs_roots(char (*root)[1100], char (*name)[256], int max) {
    pthread_mutex_lock(&g_lock);
    int n = 0;
    for (int i = 0; i < g_n && n < max; i++) {
        if (!g_mods[i].on || !g_mods[i].romfs[0]) continue;
        snprintf(root[n], 1100, "%s", g_mods[i].romfs);
        snprintf(name[n], 256, "%s", g_mods[i].name);
        n++;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

void mods_note_romfs(const char *name, int replaced, int added, int covered) {
    pthread_mutex_lock(&g_lock);
    int k = 0;
    while (k < g_ncounts && strcmp(g_counts[k].name, name)) k++;
    if (k == g_ncounts && g_ncounts < MODS_MAX) g_ncounts++;
    if (k < MODS_MAX) {
        snprintf(g_counts[k].name, sizeof g_counts[k].name, "%s", name);
        g_counts[k].replaced = replaced, g_counts[k].added = added, g_counts[k].covered = covered;
    }
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_mods[i].name, name))
            g_mods[i].replaced = replaced, g_mods[i].added = added, g_mods[i].covered = covered, g_mods[i].counted = true;
    pthread_mutex_unlock(&g_lock);
    LOG("[mods] %s: %d game files replaced, %d added%s\n", name, replaced, added,
        covered ? " (some of its files are a higher mod's)" : "");
}

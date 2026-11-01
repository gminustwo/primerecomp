// Texture mods (Video tab "Texture mods", "Dump textures"): the game's textures replaced by image files in mods/, the
// way Dolphin packs work for GameCube games, but named after Remastered's own textures.
//
// A texture's name is tex_<width>x<height>_<key>, key = texmods_key over its full-size level as the game stores it,
// its size and its format: the same texture gets the same name in every run, on every machine. "Dump textures" writes
// each texture the game loads to dump/textures/ under its name as a PNG - its pixels as stored (R, G, B, A; top row
// first; the colour channels as the game's shaders read them: a normal map's X and Y in red and green) - the starting
// point for a replacement. A file of that name anywhere under mods/ (in folders of any names) is drawn instead:
//   PNG, any size (the mipmaps are made here);
//   DDS, BC1 / BC2 / BC3 / BC4 / BC5 / BC7 or RGBA8, with its own mipmaps - the small, fast choice for big packs.
// Larger than the original is fine (4K over 2K: the game samples by coordinates, it just gets more detail). Loaded in
// the background (texmods_request): the game's own texture shows until the replacement is ready. textures.c puts
// the replacement in place of the texture's image (texmod_poll); colour textures are read in the colour space the
// game's texture had, whatever the file says.
#include "texmods.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../settings.h"

#define MODS_DIR "mods"
#define DUMP_DIR "dump/textures"

// ---- the name
// (Never to change: the files on players' disks are named by it.) 64-bit, four lanes over 8-byte words, the size and
// format folded in.
uint64_t texmods_key(const uint8_t *p, uint64_t n, uint32_t w, uint32_t h, uint32_t format) {
    uint64_t a = 0x9E3779B97F4A7C15ull ^ n, b = 0xC2B2AE3D27D4EB4Full ^ w, c = 0x165667B19E3779F9ull ^ h,
             d = 0x27D4EB2F165667C5ull ^ format;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        a = (a ^ v[0]) * 0x100000001B3ull; a ^= a >> 29;
        b = (b ^ v[1]) * 0x100000001B3ull; b ^= b >> 29;
        c = (c ^ v[2]) * 0x100000001B3ull; c ^= c >> 29;
        d = (d ^ v[3]) * 0x100000001B3ull; d ^= d >> 29;
    }
    uint64_t k = a ^ (b * 0x9E3779B97F4A7C15ull) ^ (c * 0xC2B2AE3D27D4EB4Full) ^ (d * 0x165667B19E3779F9ull);
    for (; i < n; i++) k = (k ^ p[i]) * 0x100000001B3ull;
    k ^= k >> 31;
    return k ? k : 1;
}

// ---- the mods folder: replacement files by key
typedef struct { uint64_t key; char *path; uint32_t asked; } ModFile;  // asked: requests (texmods_request)
static ModFile *g_tab;  // open addressing, g_cap slots
static size_t g_cap, g_n;
static bool g_init;

static void tab_put(uint64_t key, const char *path) {
    if ((g_n + 1) * 2 > g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 256;
        ModFile *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < g_cap; i++)
            if (g_tab[i].key) {
                size_t s = g_tab[i].key & (cap - 1);
                while (t[s].key) s = (s + 1) & (cap - 1);
                t[s] = g_tab[i];
            }
        free(g_tab);
        g_tab = t, g_cap = cap;
    }
    size_t s = key & (g_cap - 1);
    while (g_tab[s].key && g_tab[s].key != key) s = (s + 1) & (g_cap - 1);
    if (g_tab[s].key) {  // (two files for one texture: the first found stays)
        LOG("[mods] %s: another file for this texture is used (%s)\n", path, g_tab[s].path);
        return;
    }
    g_tab[s].key = key;
    g_tab[s].path = strdup(path);
    g_n++;
}

static ModFile *tab_find(uint64_t key) {
    if (!g_cap) return NULL;
    for (size_t s = key & (g_cap - 1); g_tab[s].key; s = (s + 1) & (g_cap - 1))
        if (g_tab[s].key == key) return &g_tab[s];
    return NULL;
}

// A texture the game makes anew again and again (a working image of its own, the same contents each time - the
// screen-sized ones it fills each frame): loaded and let go each time, thousands of loads (a test with every dumped
// texture put back had two such, 3680 loads each in 90 s). After a few, it stays the game's.
#define MAX_ASKS 8
static const char *tab_get(uint64_t key) {
    ModFile *f = tab_find(key);
    return f && f->asked < MAX_ASKS ? f->path : NULL;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void scan(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (is_dir(path)) {
            if (depth < 8) scan(path, depth + 1);
            continue;
        }
        unsigned w, h;
        unsigned long long key;
        char ext[8] = "";
        if (sscanf(e->d_name, "tex_%ux%u_%16llx.%7s", &w, &h, &key, ext) != 4) continue;
        for (char *c = ext; *c; c++) *c = (char)(*c | 0x20);
        if (strcmp(ext, "png") && strcmp(ext, "dds")) continue;
        tab_put(key ? key : 1, path);
    }
    closedir(d);
}

void texmods_init(void) {
    if (g_init) return;
    g_init = true;
    mkdir(MODS_DIR, 0755);  // (there to put mods in)
    if (!g_settings.texture_mods) {
        LOG("[mods] texture mods off\n");
        return;
    }
    scan(MODS_DIR, 0);
    LOG("[mods] %zu replacement texture%s in %s/\n", g_n, g_n == 1 ? "" : "s", MODS_DIR);
}

bool texmods_active(void) { return g_n || g_settings.dump_textures; }
int texmods_count(void) { return (int)g_n; }
const char *texmods_folder(void) { return MODS_DIR; }
bool texmods_has(uint64_t key) { return tab_get(key) != NULL; }

// ---- loading (one thread, in the background)
typedef struct Job { ModImage *m; char *path; struct Job *next; } Job;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static Job *q_head, *q_tail;

static uint8_t *read_all(const char *path, uint64_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 ? malloc((size_t)sz) : NULL;
    *n = b ? fread(b, 1, (size_t)sz, f) : 0;
    fclose(f);
    return b;
}

static uint32_t levels_for(uint32_t w, uint32_t h) {
    uint32_t n = 1, s = w > h ? w : h;
    while (s > 1) s >>= 1, n++;
    return n > 16 ? 16 : n;
}

// PNG: RGBA8, then each smaller level a 2x2 average of the one above
static bool load_png(ModImage *m, const char *path) {
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) { LOG("[mods] %s: %s\n", path, SDL_GetError()); return false; }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);  // (R, G, B, A bytes)
    SDL_DestroySurface(s);
    if (!c) return false;
    m->w = (uint32_t)c->w, m->h = (uint32_t)c->h, m->kind = MOD_RGBA8, m->levels = levels_for(m->w, m->h);
    uint64_t total = 0;
    for (uint32_t l = 0, w = m->w, h = m->h; l < m->levels; l++) {
        m->off[l] = total, m->len[l] = (uint64_t)w * h * 4, total += m->len[l];
        w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1;
    }
    m->data = malloc(total);
    for (uint32_t y = 0; y < m->h; y++) memcpy(m->data + (uint64_t)y * m->w * 4, (uint8_t *)c->pixels + (uint64_t)y * c->pitch, m->w * 4);
    SDL_DestroySurface(c);
    for (uint32_t l = 1, pw = m->w, ph = m->h; l < m->levels; l++) {
        uint32_t w = pw > 1 ? pw / 2 : 1, h = ph > 1 ? ph / 2 : 1;
        const uint8_t *src = m->data + m->off[l - 1];
        uint8_t *dst = m->data + m->off[l];
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : x0;
                uint32_t y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : y0;
                for (int k = 0; k < 4; k++)
                    dst[((uint64_t)y * w + x) * 4 + k] = (uint8_t)((src[((uint64_t)y0 * pw + x0) * 4 + k] + src[((uint64_t)y0 * pw + x1) * 4 + k] +
                                                                    src[((uint64_t)y1 * pw + x0) * 4 + k] + src[((uint64_t)y1 * pw + x1) * 4 + k] + 2) / 4);
            }
        pw = w, ph = h;
    }
    return true;
}

// DDS: its own levels, as they are
static bool load_dds(ModImage *m, const char *path) {
    uint64_t n;
    uint8_t *f = read_all(path, &n);
    if (!f) return false;
    bool ok = false;
    if (n < 128 || memcmp(f, "DDS ", 4)) { LOG("[mods] %s: not a DDS file\n", path); goto out; }
    uint32_t h, w, mips, pf_flags, bits, rmask;
    memcpy(&h, f + 12, 4), memcpy(&w, f + 16, 4), memcpy(&mips, f + 28, 4), memcpy(&pf_flags, f + 80, 4);
    memcpy(&bits, f + 88, 4), memcpy(&rmask, f + 92, 4);
    const uint8_t *cc = f + 84;
    uint64_t at = 128;
    int kind = -1;
    bool bgra = false;
    if (!memcmp(cc, "DX10", 4)) {
        if (n < 148) goto out;
        uint32_t dxgi;
        memcpy(&dxgi, f + 128, 4);
        at = 148;
        kind = dxgi >= 70 && dxgi <= 72 ? MOD_BC1 : dxgi >= 73 && dxgi <= 75 ? MOD_BC2 : dxgi >= 76 && dxgi <= 78 ? MOD_BC3
             : dxgi >= 79 && dxgi <= 81 ? MOD_BC4 : dxgi >= 82 && dxgi <= 84 ? MOD_BC5 : dxgi >= 97 && dxgi <= 99 ? MOD_BC7
             : dxgi >= 27 && dxgi <= 29 ? MOD_RGBA8 : (dxgi == 87 || dxgi == 91) ? (bgra = true, MOD_RGBA8) : -1;
        if (kind < 0) LOG("[mods] %s: DXGI format %u isn't one taken (BC1-5, BC7, RGBA8)\n", path, dxgi);
    } else if (pf_flags & 4) {  // (a FourCC)
        kind = !memcmp(cc, "DXT1", 4) ? MOD_BC1 : !memcmp(cc, "DXT3", 4) ? MOD_BC2 : !memcmp(cc, "DXT5", 4) ? MOD_BC3
             : (!memcmp(cc, "ATI1", 4) || !memcmp(cc, "BC4U", 4)) ? MOD_BC4 : (!memcmp(cc, "ATI2", 4) || !memcmp(cc, "BC5U", 4)) ? MOD_BC5 : -1;
        if (kind < 0) LOG("[mods] %s: FourCC %.4s isn't one taken\n", path, (const char *)cc);
    } else if ((pf_flags & 0x40) && bits == 32) {  // (uncompressed RGB(A))
        kind = MOD_RGBA8, bgra = rmask == 0x00FF0000u;
    }
    if (kind < 0 || !w || !h) goto out;
    if (!mips) mips = 1;
    if (mips > 16) mips = 16;
    m->w = w, m->h = h, m->kind = kind;
    uint32_t bb = kind == MOD_BC1 || kind == MOD_BC4 ? 8 : 16;
    uint64_t total = 0;
    for (uint32_t l = 0, lw = w, lh = h; l < mips; l++) {
        uint64_t len = kind == MOD_RGBA8 ? (uint64_t)lw * lh * 4 : (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * bb;
        if (at + total + len > n) { mips = l; break; }  // (a cut-off file: the levels that are there)
        m->off[l] = total, m->len[l] = len, total += len;
        lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1;
    }
    if (!mips) { LOG("[mods] %s: shorter than its first level\n", path); goto out; }
    m->levels = mips;
    m->data = malloc(total);
    memcpy(m->data, f + at, total);
    if (bgra)
        for (uint64_t i = 0; i + 3 < total; i += 4) { uint8_t t = m->data[i]; m->data[i] = m->data[i + 2]; m->data[i + 2] = t; }
    ok = true;
out:
    free(f);
    return ok;
}

static void *loader(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!q_head) pthread_cond_wait(&q_cond, &q_lock);
        Job *j = q_head;
        q_head = j->next;
        if (!q_head) q_tail = NULL;
        pthread_mutex_unlock(&q_lock);
        ModImage *m = j->m;
        uint64_t t0 = SDL_GetTicksNS();
        size_t pl = strlen(j->path);
        bool dds = pl > 4 && (j->path[pl - 3] | 0x20) == 'd';
        bool ok = atomic_load(&m->state) == 0 && (dds ? load_dds(m, j->path) : load_png(m, j->path));
        if (ok) LOG("[mods] loaded %s: %ux%u, %u levels (%.0f ms)\n", j->path, m->w, m->h, m->levels, (SDL_GetTicksNS() - t0) / 1e6);
        int want = 0;
        if (!atomic_compare_exchange_strong(&m->state, &want, ok ? 1 : 2)) {  // (let go meanwhile: ours to free)
            free(m->data);
            free(m);
        }
        free(j->path);
        free(j);
    }
    return NULL;
}

ModImage *texmods_request(uint64_t key) {
    const char *path = tab_get(key);
    if (!path) return NULL;
    if (++tab_find(key)->asked == MAX_ASKS)
        LOG("[mods] %s: the game keeps making this texture anew - it stays the game's from now on\n", path);
    ModImage *m = calloc(1, sizeof *m);
    m->key = key;
    Job *j = malloc(sizeof *j);
    j->m = m, j->path = strdup(path), j->next = NULL;
    pthread_mutex_lock(&q_lock);
    static bool started;
    if (!started) {
        started = true;
        pthread_t th;
        pthread_create(&th, NULL, loader, NULL);
        pthread_detach(th);
    }
    if (q_tail) q_tail->next = j; else q_head = j;
    q_tail = j;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
    return m;
}

void texmods_release(ModImage *m) {
    if (!m) return;
    int want = 0;
    if (atomic_compare_exchange_strong(&m->state, &want, 3)) return;  // (still loading: the loader frees it)
    free(m->data);
    free(m);
}

// ---- Dump textures
// Written at the start of a presented frame (no pass open), at most a few a frame so the game keeps going: each a
// read back from the GPU and a PNG. Once per texture (and not again for a file already there from earlier).
void renderer_save_texture_raw(TexBackend *b, const char *path);  // exec.c
typedef struct { TexBackend *b; uint64_t key; uint32_t w, h; } Dump;
static Dump *g_dumps;
static int g_ndumps, g_capdumps;
static uint64_t *g_dumped;  // keys written (open addressing)
static size_t g_dumped_cap, g_dumped_n;

static bool dumped_has(uint64_t k) {
    if (!g_dumped_cap) return false;
    for (size_t s = k & (g_dumped_cap - 1); g_dumped[s]; s = (s + 1) & (g_dumped_cap - 1))
        if (g_dumped[s] == k) return true;
    return false;
}
static void dumped_add(uint64_t k) {
    if ((g_dumped_n + 1) * 2 > g_dumped_cap) {
        size_t cap = g_dumped_cap ? g_dumped_cap * 2 : 1024;
        uint64_t *t = calloc(cap, 8);
        for (size_t i = 0; i < g_dumped_cap; i++)
            if (g_dumped[i]) { size_t s = g_dumped[i] & (cap - 1); while (t[s]) s = (s + 1) & (cap - 1); t[s] = g_dumped[i]; }
        free(g_dumped);
        g_dumped = t, g_dumped_cap = cap;
    }
    size_t s = k & (g_dumped_cap - 1);
    while (g_dumped[s]) s = (s + 1) & (g_dumped_cap - 1);
    g_dumped[s] = k;
    g_dumped_n++;
}

void texmods_dump_request(TexBackend *b, uint64_t key, uint32_t w, uint32_t h) {
    if (dumped_has(key)) return;
    dumped_add(key);
    for (int i = 0; i < g_ndumps; i++) if (g_dumps[i].key == key) return;
    if (g_ndumps == g_capdumps) g_dumps = realloc(g_dumps, (g_capdumps = g_capdumps ? g_capdumps * 2 : 64) * sizeof *g_dumps);
    g_dumps[g_ndumps++] = (Dump){b, key, w, h};
}

void texmods_forget(TexBackend *b) {
    for (int i = 0; i < g_ndumps; i++)
        if (g_dumps[i].b == b) g_dumps[i--] = g_dumps[--g_ndumps];
}

void texmods_dump_frame(void) {
    if (!g_ndumps) return;
    static bool made;
    if (!made) { made = true; mkdir("dump", 0755); mkdir(DUMP_DIR, 0755); }
    int done = 0;
    while (g_ndumps && done < 4) {
        Dump d = g_dumps[0];
        memmove(g_dumps, g_dumps + 1, --g_ndumps * sizeof *g_dumps);
        char path[160];
        snprintf(path, sizeof path, DUMP_DIR "/tex_%ux%u_%016llx.png", d.w, d.h, (unsigned long long)d.key);
        struct stat st;
        if (stat(path, &st) == 0) continue;  // (from an earlier run)
        renderer_save_texture_raw(d.b, path);
        done++;
    }
}

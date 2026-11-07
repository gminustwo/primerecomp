// Suit colors (Gameplay tab "Suit colors"; settings.h SUIT_*): Samus' suit drawn in the player's own three colors, from
// a palette, for the helmet, the armor and the rest. (The Patriot style - with a white star on the helmet's brow - and
// a picture of the player's on the helmet or chest are on branch suit-patriot-image.) Nothing of the game's ships: as a
// suit's texture is loaded, its own pixels are recolored here and put in its place through the texture mod path
// (textures.c mod_upload, texmods.h's ModImage).
//
// The suits' color ("base color") textures are 2048 x 2048 ASTC 5x5 sRGB, one a suit (Power, Varia, Gravity,
// Phazon), known by texmods_key as texture mods are. They share one layout (one model): the helmet's pieces - its
// shell, the brow over the visor, a strip of the dome and the side bulbs (one island for both sides) - are at the
// same places in each, given below as outlines in texels. They were found by painting test textures and looking at
// the pause screen's Samus: the shell is its island in the suits' packed material texture, the rest by color.
// The rest of a suit splits into its armor (the Power Suit's maroon chest and shoulders, the Gravity Suit's
// brown-red) and its body (the gold, cream and purple parts) by hue; the Phazon Suit's gray texture by brightness, its
// light parts the armor. Grays and dark parts (joints, the under-suit), the visor's glass and the lights (a texture of
// their own) stay as they are.
//
// A part's color replaces its own in linear light, times the texel's brightness over the part's average, so the panel
// lines, wear and shading painted into the texture stay. Made on a thread of its own (about a quarter second a
// texture); the game's texture shows meanwhile.
#include "suitmods.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../../settings.h"
#include "../astc.h"
#include "../tegra.h"

typedef struct { uint64_t key; const char *name; bool gray; float split; } Suit;
static const Suit k_suits[] = {
    {0xdde30352490a659eull, "Power Suit", false, 16},  // (split: the hue, degrees, between the armor's and the body's)
    {0x7aa9d98e11da4d9full, "Varia Suit", false, 16},
    {0xd71573c62c6a746bull, "Gravity Suit", false, 27},
    {0xd7a9c48ee5d46a46ull, "Phazon Suit", true, 0},
};
#define N_SUITS ((int)(sizeof k_suits / sizeof *k_suits))

// The helmet's pieces, in texels of the 2048 x 2048 texture (top row first, as stored; +y is up on the helmet)
static const int16_t k_shell[][2] = {
    {1225, 1250}, {1232, 1140}, {1300, 1092}, {1345, 1060}, {1420, 1060}, {1468, 1092}, {1535, 1140}, {1549, 1248},
    {1564, 1256}, {1576, 1272}, {1578, 1296}, {1563, 1320}, {1572, 1336}, {1565, 1392}, {1569, 1416}, {1555, 1432},
    {1577, 1496}, {1592, 1512}, {1584, 1520}, {1604, 1536}, {1612, 1576}, {1614, 1656}, {1610, 1680}, {1603, 1704},
    {1586, 1720}, {1594, 1728}, {1593, 1736}, {1570, 1752}, {1560, 1768}, {1539, 1776}, {1548, 1784}, {1543, 1792},
    {1519, 1800}, {1528, 1808}, {1516, 1824}, {1517, 1840}, {1501, 1848}, {1507, 1864}, {1504, 1872}, {1472, 1896},
    {1459, 1912}, {1467, 1920}, {1461, 1928}, {1460, 1944}, {1451, 1952}, {1455, 1960}, {1468, 1968}, {1462, 1976},
    {1466, 1992}, {1473, 2000}, {1459, 2016}, {1342, 2016}, {1325, 1992}, {1348, 1976}, {1351, 1968}, {1338, 1944},
    {1356, 1920}, {1354, 1912}, {1332, 1896}, {1294, 1856}, {1287, 1832}, {1272, 1816}, {1276, 1808}, {1256, 1800},
    {1251, 1792}, {1256, 1784}, {1237, 1776}, {1227, 1760}, {1204, 1744}, {1202, 1736}, {1210, 1728}, {1211, 1720},
    {1202, 1688}, {1179, 1664}, {1178, 1576}, {1191, 1512}, {1208, 1504}, {1227, 1440}, {1212, 1424}, {1214, 1368},
    {1210, 1352}, {1215, 1312}, {1205, 1304}, {1210, 1264}};
static const int16_t k_brow[][2] = {
    {992, 1272}, {1132, 1278}, {1209, 1294}, {1225, 1312}, {1226, 1336}, {1203, 1354}, {1161, 1390}, {1109, 1422},
    {1088, 1422}, {1044, 1386}, {1000, 1370}, {990, 1340}};
static const int16_t k_strip[][2] = {
    {949, 1134}, {983, 1134}, {996, 1148}, {994, 1190}, {998, 1290}, {992, 1340}, {976, 1376}, {953, 1378},
    {933, 1330}, {917, 1324}, {914, 1152}, {922, 1144}};
static const int16_t k_bulb[][2] = {
    {687, 1508}, {669, 1520}, {664, 1532}, {647, 1544}, {646, 1556}, {656, 1560}, {645, 1564}, {643, 1572},
    {657, 1576}, {665, 1596}, {683, 1620}, {700, 1632}, {744, 1648}, {752, 1660}, {765, 1664}, {818, 1664},
    {874, 1656}, {896, 1636}, {894, 1616}, {905, 1536}, {885, 1516}, {756, 1512}, {747, 1508}};

// ------------------------------------------------------------------ which colors
bool suitmods_on(void) { return g_settings.suit_style == SUIT_CUSTOM; }

static void chosen(int idx[3]) {  // helmet, armor, body
    const int v[3] = {g_settings.suit_helmet, g_settings.suit_armor, g_settings.suit_body};
    for (int k = 0; k < 3; k++) idx[k] = v[k] >= 0 && v[k] < N_SUIT_COLORS ? v[k] : 0;
}

static _Atomic uint32_t g_gen = 1;
uint32_t suitmods_generation(void) {
    static _Atomic uint64_t sig;
    int idx[3];
    chosen(idx);
    uint64_t s = (uint64_t)(suitmods_on() ? g_settings.suit_style : 0) | (uint64_t)idx[0] << 8 | (uint64_t)idx[1] << 16 |
                 (uint64_t)idx[2] << 24;
    if (atomic_exchange(&sig, s) != s) g_gen++;
    return g_gen;
}

bool suitmods_candidate(uint32_t w, uint32_t h, uint32_t format) {
    return suitmods_on() && w == 2048 && h == 2048 && format == 0x89;  // (ASTC 5x5 sRGB)
}

static int suit_of(uint64_t key) {
    for (int i = 0; i < N_SUITS; i++)
        if (k_suits[i].key == key) return i;
    return -1;
}
bool suitmods_wants(uint64_t key) { return suitmods_on() && suit_of(key) >= 0; }

// ------------------------------------------------------------------ outlines to coverage
static void span(float *row, float x0, float x1, int w, float wt) {
    if (x1 <= 0 || x0 >= w) return;
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = (float)w;
    int i0 = (int)x0, i1 = (int)x1;
    if (i0 == i1) { row[i0] += (x1 - x0) * wt; return; }
    row[i0] += (i0 + 1 - x0) * wt;
    for (int i = i0 + 1; i < i1; i++) row[i] += wt;
    if (i1 < w) row[i1] += (x1 - i1) * wt;
}

// A polygon's coverage (0..1) over w x h texels, kept where it's more than what's there: four sample rows a texel row,
// the spans along each exact
static void raster(const float (*p)[2], int n, float *cov, int w, int h) {
    float y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) y0 = fminf(y0, p[i][1]), y1 = fmaxf(y1, p[i][1]);
    float *row = calloc((size_t)w, sizeof *row), xs[256];
    if (!row) return;
    for (int y = y0 < 0 ? 0 : (int)y0; y <= (int)y1 && y < h; y++) {
        memset(row, 0, (size_t)w * sizeof *row);
        for (int s = 0; s < 4; s++) {
            float yy = y + (s + 0.5f) / 4;
            int nx = 0;
            for (int i = 0, j = n - 1; i < n && nx < 256; j = i++)
                if ((p[i][1] > yy) != (p[j][1] > yy))
                    xs[nx++] = p[i][0] + (yy - p[i][1]) * (p[j][0] - p[i][0]) / (p[j][1] - p[i][1]);
            for (int i = 1; i < nx; i++)  // (a few crossings: insertion sort)
                for (int k = i; k > 0 && xs[k - 1] > xs[k]; k--) { float t = xs[k]; xs[k] = xs[k - 1], xs[k - 1] = t; }
            for (int a = 0; a + 1 < nx; a += 2) span(row, xs[a], xs[a + 1], w, 0.25f);
        }
        for (int x = 0; x < w; x++)
            if (row[x] > cov[(size_t)y * w + x]) cov[(size_t)y * w + x] = row[x] > 1 ? 1 : row[x];
    }
    free(row);
}

static void raster_table(const int16_t (*t)[2], int n, float *cov, int w, int h) {
    float (*p)[2] = malloc(sizeof *p * (size_t)n);
    if (!p) return;
    for (int i = 0; i < n; i++) p[i][0] = t[i][0] * (w / 2048.0f), p[i][1] = t[i][1] * (h / 2048.0f);
    raster((const float (*)[2])p, n, cov, w, h);
    free(p);
}

// ------------------------------------------------------------------ the recoloring
static float s2l[256];          // sRGB byte -> linear
static uint8_t l2s[4097];       // linear (x4096) -> sRGB byte
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;
static void make_tables(void) {
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0f;
        s2l[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    for (int i = 0; i <= 4096; i++) {
        float c = i / 4096.0f, e = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f;
        l2s[i] = (uint8_t)lroundf(fminf(fmaxf(e, 0), 1) * 255);
    }
}
static inline uint8_t enc(float v) { return l2s[(int)(fminf(fmaxf(v, 0), 1) * 4096 + 0.5f)]; }
static inline float smooth(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

// How much of a texel is a part to recolor (colorful), and of that how much is the armor (else the body)
static void classify(const uint8_t *px, const Suit *s, float *colorful, float *armor) {
    float r = px[0] / 255.0f, g = px[1] / 255.0f, b = px[2] / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn, sat = mx > 0 ? d / mx : 0;
    if (s->gray) {
        *colorful = (1 - smooth(0.25f, 0.45f, sat)) * smooth(0.05f, 0.15f, mx);
        *armor = smooth(0.40f, 0.55f, mx);
        return;
    }
    float h = 0;
    if (d > 1e-6f) {
        if (mx == r) { h = fmodf((g - b) / d, 6); if (h < 0) h += 6; }
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
    }
    h *= 60;
    if (h > 300) h -= 360;  // (reds around 0)
    bool warm = h > -40 && h < 60, purple = h > 200 && h < 290;
    *colorful = warm || purple ? smooth(0.10f, 0.22f, sat) * smooth(0.06f, 0.16f, mx) : 0;
    *armor = warm ? 1 - smooth(s->split - 4, s->split + 4, h) : 0;
}

static void recolor(uint8_t *rgba, int w, int h, const Suit *s, const float col[3][3]) {
    pthread_once(&tables_once, make_tables);
    size_t n = (size_t)w * h;
    float *helm = calloc(n, sizeof *helm);
    if (!helm) return;
    raster_table(k_shell, (int)(sizeof k_shell / sizeof *k_shell), helm, w, h);
    raster_table(k_brow, (int)(sizeof k_brow / sizeof *k_brow), helm, w, h);
    raster_table(k_strip, (int)(sizeof k_strip / sizeof *k_strip), helm, w, h);
    raster_table(k_bulb, (int)(sizeof k_bulb / sizeof *k_bulb), helm, w, h);
    // each part's average brightness (linear), the helmet's over all its color
    double sum[3] = {0}, wsum[3] = {0};
    for (size_t i = 0; i < n; i++) {
        const uint8_t *px = rgba + i * 4;
        float c, a;
        classify(px, s, &c, &a);
        if (c <= 0) continue;
        float Y = 0.2126f * s2l[px[0]] + 0.7152f * s2l[px[1]] + 0.0722f * s2l[px[2]], hm = helm[i];
        float wt[3] = {c * hm, c * a * (1 - hm), c * (1 - a) * (1 - hm)};
        for (int k = 0; k < 3; k++) sum[k] += Y * wt[k], wsum[k] += wt[k];
    }
    float avg[3];
    for (int k = 0; k < 3; k++) avg[k] = wsum[k] > 0 && sum[k] > 0 ? (float)(sum[k] / wsum[k]) : 0.2f;
    float lc[3][3];
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++) lc[k][j] = s2l[(int)lroundf(fminf(fmaxf(col[k][j], 0), 1) * 255)];
    for (size_t i = 0; i < n; i++) {
        uint8_t *px = rgba + i * 4;
        float c, a;
        classify(px, s, &c, &a);
        if (c <= 0) continue;
        float lin[3] = {s2l[px[0]], s2l[px[1]], s2l[px[2]]};
        float Y = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2], hm = helm[i];
        float rh = fminf(Y / avg[0], 2.5f), ra = fminf(Y / avg[1], 2.5f), rb = fminf(Y / avg[2], 1.25f);
        for (int j = 0; j < 3; j++) {
            float t = hm * lc[0][j] * rh + (1 - hm) * (a * lc[1][j] * ra + (1 - a) * lc[2][j] * rb);
            px[j] = enc(lin[j] * (1 - c) + t * c);
        }
    }
    free(helm);
}

// ------------------------------------------------------------------ the thread making them
typedef struct Job {
    ModImage *m;
    SuitSource src;   // (level0: our copy)
    int suit;
    float col[3][3];
    uint32_t gen;
    struct Job *next;
} Job;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static Job *q_head, *q_tail;

static bool make(Job *j) {
    const SuitSource *s = &j->src;
    if (j->gen != g_gen) return false;  // (the colors changed since: a newer request follows)
    uint8_t *blocks = malloc((size_t)s->wb * s->hb * 16), *rgba = malloc((size_t)s->w * s->h * 4);
    bool ok = false;
    if (blocks && rgba) {
        tegra_deswizzle(s->level0, blocks, s->wb, s->hb, 1, 16, s->block_height_log2, s->block_depth_log2);
        astc_decode_ldr(blocks, s->w, s->h, s->bw, s->bh, s->srgb, rgba);
        recolor(rgba, (int)s->w, (int)s->h, &k_suits[j->suit], (const float (*)[3])j->col);
        ok =texmods_set_rgba(j->m, rgba, s->w, s->h, (uint64_t)s->w * 4);
    }
    free(blocks);
    free(rgba);
    return ok;
}

static void *worker(void *arg) {
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
        bool ok = atomic_load(&m->state) == 0 && make(j);
        if (ok) LOG("[suit] %s recolored (%.0f ms)\n", k_suits[j->suit].name, (SDL_GetTicksNS() - t0) / 1e6);
        int want = 0;
        if (!atomic_compare_exchange_strong(&m->state, &want, ok ? 1 : 2)) {  // (let go meanwhile: ours to free)
            free(m->data);
            free(m);
        }
        free((void *)j->src.level0);
        free(j);
    }
    return NULL;
}

ModImage *suitmods_request(uint64_t key, const SuitSource *src) {
    int suit = suit_of(key);
    if (suit < 0 || !src->level0 || src->bw == 0 || src->bh == 0) return NULL;
    ModImage *m = calloc(1, sizeof *m);
    Job *j = calloc(1, sizeof *j);
    uint8_t *copy = malloc(src->size);
    if (!m || !j || !copy) { free(m); free(j); free(copy); return NULL; }
    memcpy(copy, src->level0, src->size);
    m->key = key;
    j->m = m, j->src = *src, j->src.level0 = copy, j->suit = suit, j->gen = suitmods_generation();
    int idx[3];
    chosen(idx);
    for (int k = 0; k < 3; k++) memcpy(j->col[k], g_suit_colors[idx[k]], sizeof j->col[k]);
    pthread_mutex_lock(&q_lock);
    static bool started;
    if (!started) {
        started = true;
        pthread_t th;
        pthread_create(&th, NULL, worker, NULL);
        pthread_detach(th);
    }
    if (q_tail) q_tail->next = j; else q_head = j;
    q_tail = j;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
    return m;
}

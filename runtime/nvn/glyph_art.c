// Button prompt icons, redrawn (see glyph_art.h): decode the game's icon (bcdec), measure its colours, take its
// symbol out, draw the new one with 4x4 supersampling, encode it back to BC7 (mode 6). Everything here works on the
// icon upright, in its own pixels; the game's 36 x 36 icons are the reference size the shapes are laid out for.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "glyph_art.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "../../third_party/bcdec/bcdec.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/imgui/imstb_truetype.h"

#define SS 4        // supersampling per axis
#define MAXW 64     // largest icon handled

typedef struct { float c[4]; } Px;  // straight-alpha RGBA, 0..255

static float lum(const float *c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

// ------------------------------------------------------------------ BC7 mode 6 encoder
// One subset, RGBA endpoints of 7 bits + a shared low bit each, 4-bit indices. Endpoints from the block's principal
// axis, then refined by least squares on the chosen indices; colour errors count by the pixel's alpha (a transparent
// pixel's colour doesn't show).
static const int k_w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

static void quantize(const float e[4], int q[4], int *p) {  // -> 7-bit values and the shared bit, best of both bits
    float best = 1e30f;
    for (int pb = 0; pb < 2; pb++) {
        int t[4];
        float err = 0;
        for (int i = 0; i < 4; i++) {
            int v = (int)floorf((e[i] - pb) / 2 + 0.5f);
            t[i] = v < 0 ? 0 : v > 127 ? 127 : v;
            float d = (float)(t[i] * 2 + pb) - e[i];
            err += d * d;
        }
        if (err < best) best = err, *p = pb, memcpy(q, t, sizeof t);
    }
}

static float assign(const float px[16][4], const float wt[16][4], const int e0[4], const int e1[4], uint8_t idx[16]) {
    float pal[16][4], total = 0;
    for (int k = 0; k < 16; k++)
        for (int i = 0; i < 4; i++) pal[k][i] = (float)(((64 - k_w4[k]) * e0[i] + k_w4[k] * e1[i] + 32) >> 6);
    for (int j = 0; j < 16; j++) {
        float best = 1e30f;
        for (int k = 0; k < 16; k++) {
            float err = 0;
            for (int i = 0; i < 4; i++) { float d = pal[k][i] - px[j][i]; err += wt[j][i] * d * d; }
            if (err < best) best = err, idx[j] = (uint8_t)k;
        }
        total += best;
    }
    return total;
}

static void put_bits(uint8_t *b, int *pos, uint32_t v, int n) {
    for (int i = 0; i < n; i++, (*pos)++)
        if (v >> i & 1) b[*pos >> 3] |= (uint8_t)(1 << (*pos & 7));
}

static void bc7_encode_block(const Px *src, int stride, uint8_t out[16]) {
    float px[16][4], wt[16][4], mean[4] = {0};
    for (int j = 0; j < 16; j++) {
        const Px *p = &src[(j / 4) * stride + j % 4];
        float a = p->c[3] / 255;
        for (int i = 0; i < 4; i++) px[j][i] = p->c[i], wt[j][i] = i == 3 ? 1.0f : 0.05f + 0.95f * a;
        for (int i = 0; i < 4; i++) mean[i] += px[j][i] / 16;
    }
    // principal axis (power iteration on the covariance)
    float cov[4][4] = {{0}};
    for (int j = 0; j < 16; j++)
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++) cov[a][b] += (px[j][a] - mean[a]) * (px[j][b] - mean[b]);
    float ax[4] = {1, 1, 1, 1};
    for (int it = 0; it < 12; it++) {
        float n[4] = {0}, len = 0;
        for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) n[a] += cov[a][b] * ax[b];
        for (int a = 0; a < 4; a++) len += n[a] * n[a];
        if (len < 1e-12f) break;
        len = sqrtf(len);
        for (int a = 0; a < 4; a++) ax[a] = n[a] / len;
    }
    float tmin = 1e30f, tmax = -1e30f;
    for (int j = 0; j < 16; j++) {
        float t = 0;
        for (int i = 0; i < 4; i++) t += (px[j][i] - mean[i]) * ax[i];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    float e[2][4];
    for (int i = 0; i < 4; i++) {
        e[0][i] = fminf(255, fmaxf(0, mean[i] + ax[i] * tmin));
        e[1][i] = fminf(255, fmaxf(0, mean[i] + ax[i] * tmax));
    }
    int bq[2][4] = {{0}}, bp[2] = {0};
    uint8_t bidx[16] = {0};
    float berr = 1e30f;
    for (int it = 0; it < 4; it++) {
        int q[2][4], p[2], r0[4], r1[4];
        uint8_t idx[16];
        quantize(e[0], q[0], &p[0]);
        quantize(e[1], q[1], &p[1]);
        for (int i = 0; i < 4; i++) r0[i] = q[0][i] * 2 + p[0], r1[i] = q[1][i] * 2 + p[1];
        float err = assign(px, wt, r0, r1, idx);
        if (err < berr) berr = err, memcpy(bq, q, sizeof q), memcpy(bp, p, sizeof p), memcpy(bidx, idx, sizeof idx);
        if (err == 0) break;
        // least squares for the two endpoints, channel by channel, on these indices
        for (int i = 0; i < 4; i++) {
            double aa = 0, ab = 0, bb = 0, ax_ = 0, bx = 0;
            for (int j = 0; j < 16; j++) {
                double w1 = k_w4[idx[j]] / 64.0, w0 = 1 - w1, ww = wt[j][i];
                aa += ww * w0 * w0, ab += ww * w0 * w1, bb += ww * w1 * w1;
                ax_ += ww * w0 * px[j][i], bx += ww * w1 * px[j][i];
            }
            double det = aa * bb - ab * ab;
            if (fabs(det) < 1e-9) continue;
            e[0][i] = (float)fmin(255, fmax(0, (ax_ * bb - bx * ab) / det));
            e[1][i] = (float)fmin(255, fmax(0, (bx * aa - ax_ * ab) / det));
        }
    }
    if (bidx[0] >= 8) {  // the first index' top bit is implied 0: swap the endpoints
        int tq[4], tp = bp[0];
        memcpy(tq, bq[0], sizeof tq), memcpy(bq[0], bq[1], sizeof tq), memcpy(bq[1], tq, sizeof tq);
        bp[0] = bp[1], bp[1] = tp;
        for (int j = 0; j < 16; j++) bidx[j] = (uint8_t)(15 - bidx[j]);
    }
    memset(out, 0, 16);
    int pos = 0;
    put_bits(out, &pos, 1u << 6, 7);  // mode 6
    for (int i = 0; i < 4; i++) put_bits(out, &pos, (uint32_t)bq[0][i], 7), put_bits(out, &pos, (uint32_t)bq[1][i], 7);
    put_bits(out, &pos, (uint32_t)bp[0], 1);
    put_bits(out, &pos, (uint32_t)bp[1], 1);
    for (int j = 0; j < 16; j++) put_bits(out, &pos, bidx[j], j ? 4 : 3);
}

// ------------------------------------------------------------------ drawing
typedef struct {
    int w, h;
    Px px[MAXW * MAXW];
    float cov[MAXW * SS * MAXW * SS];  // the symbol being drawn, supersampled
    float fill[4], ring[3], glyph[3];  // the icon's colours: its inside (with its alpha), its rim, its symbol
} Icon;

static stbtt_fontinfo g_font;
static const uint8_t *g_font_data;
static bool g_font_ok;

static void cov_clear(Icon *ic) { memset(ic->cov, 0, sizeof(float) * ic->w * SS * ic->h * SS); }

// coverage of a shape given by an inside test at each subsample (icon pixel coordinates)
#define COV_SHAPE(ic, inside)                                                                  \
    for (int sy = 0; sy < (ic)->h * SS; sy++)                                                  \
        for (int sx = 0; sx < (ic)->w * SS; sx++) {                                            \
            float x = (sx + 0.5f) / SS, y = (sy + 0.5f) / SS;                                  \
            (void)x, (void)y;                                                                  \
            if (inside) (ic)->cov[sy * (ic)->w * SS + sx] = 1;                                 \
        }

static float seg_dist(float x, float y, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0, l = dx * dx + dy * dy;
    float t = l > 0 ? ((x - x0) * dx + (y - y0) * dy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ex = x0 + t * dx - x, ey = y0 + t * dy - y;
    return sqrtf(ex * ex + ey * ey);
}

static float box_dist(float x, float y, float cx, float cy, float hx, float hy, float r) {  // rounded box, signed
    float qx = fabsf(x - cx) - hx + r, qy = fabsf(y - cy) - hy + r;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

static void draw_seg(Icon *ic, float x0, float y0, float x1, float y1, float hw) {
    COV_SHAPE(ic, seg_dist(x, y, x0, y0, x1, y1) <= hw);
}

static bool in_tri(float x, float y, const float *t) {
    float d0 = (t[2] - t[0]) * (y - t[1]) - (t[3] - t[1]) * (x - t[0]);
    float d1 = (t[4] - t[2]) * (y - t[3]) - (t[5] - t[3]) * (x - t[2]);
    float d2 = (t[0] - t[4]) * (y - t[5]) - (t[1] - t[5]) * (x - t[4]);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}

// a label, centred on cx, cy, its capitals cap pixels tall, no wider than maxw
static void draw_text(Icon *ic, const char *s, float cx, float cy, float cap, float maxw) {
    if (!g_font_ok || !*s) return;
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(&g_font, 'H', &x0, &y0, &x1, &y1) || y1 <= 0) return;
    float scale = cap * SS / y1;
    float width = 0;
    for (const char *p = s; *p; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)*p, &adv, &lsb);
        width += adv * scale;
        if (p[1]) width += stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)*p, (unsigned char)p[1]) * scale;
    }
    if (width > maxw * SS) scale *= maxw * SS / width, width = maxw * SS;
    float pen = cx * SS - width / 2, base = cy * SS + y1 * scale / 2;
    int cw = ic->w * SS, ch = ic->h * SS;
    for (const char *p = s; *p; p++) {
        int cp = (unsigned char)*p, adv, lsb, bx0, by0, bx1, by1;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        float fx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, cp, scale, scale, fx, 0, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw > 0 && bh > 0 && bw <= cw && bh <= ch) {
            unsigned char *bm = malloc((size_t)bw * bh);
            stbtt_MakeCodepointBitmapSubpixel(&g_font, bm, bw, bh, bw, scale, scale, fx, 0, cp);
            int ox = (int)floorf(pen) + bx0, oy = (int)floorf(base) + by0;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int tx = ox + x, ty = oy + y;
                    if (tx < 0 || ty < 0 || tx >= cw || ty >= ch) continue;
                    float v = bm[y * bw + x] / 255.0f;
                    if (v > ic->cov[ty * cw + tx]) ic->cov[ty * cw + tx] = v;
                }
            free(bm);
        }
        pen += adv * scale;
        if (p[1]) pen += stbtt_GetCodepointKernAdvance(&g_font, cp, (unsigned char)p[1]) * scale;
    }
}

// the supersampled coverage, downsampled, laid over the icon in colour c (a: its opacity); glow: a soft halo first
static void composite(Icon *ic, const float *c, float a, float glow) {
    int cw = ic->w * SS;
    float *k = malloc(sizeof(float) * ic->w * ic->h);
    for (int y = 0; y < ic->h; y++)
        for (int x = 0; x < ic->w; x++) {
            float s = 0;
            for (int j = 0; j < SS; j++) for (int i = 0; i < SS; i++) s += ic->cov[(y * SS + j) * cw + x * SS + i];
            k[y * ic->w + x] = s / (SS * SS);
        }
    for (int pass = glow > 0 ? 0 : 1; pass < 2; pass++)
        for (int y = 0; y < ic->h; y++)
            for (int x = 0; x < ic->w; x++) {
                float v;
                if (pass == 0) {  // (the halo: a 3 x 3 blur of the symbol)
                    v = 0;
                    for (int j = -1; j <= 1; j++)
                        for (int i = -1; i <= 1; i++) {
                            int xx = x + i, yy = y + j;
                            if (xx >= 0 && yy >= 0 && xx < ic->w && yy < ic->h) v += k[yy * ic->w + xx] * (i || j ? 1 : 2);
                        }
                    v = v / 10 * glow;
                } else {
                    v = k[y * ic->w + x];
                }
                v *= a;
                if (v <= 0) continue;
                Px *p = &ic->px[y * ic->w + x];
                float pa = p->c[3] / 255, oa = pa + v * (1 - pa);
                for (int i = 0; i < 3; i++) p->c[i] = oa > 0 ? (p->c[i] * pa * (1 - v) + c[i] * v) / oa : c[i];
                p->c[3] = oa * 255;
            }
    free(k);
}

// replaces the icon where the coverage is, with colour c and alpha a (a shape's own body, not laid over)
static void paint(Icon *ic, const float *c, float a) {
    int cw = ic->w * SS;
    for (int y = 0; y < ic->h; y++)
        for (int x = 0; x < ic->w; x++) {
            float s = 0;
            for (int j = 0; j < SS; j++) for (int i = 0; i < SS; i++) s += ic->cov[(y * SS + j) * cw + x * SS + i];
            s /= SS * SS;
            if (s <= 0) continue;
            Px *p = &ic->px[y * ic->w + x];
            float pa = p->c[3] / 255, oa = pa * (1 - s) + a * s;
            for (int i = 0; i < 3; i++) p->c[i] = oa > 0 ? (p->c[i] * pa * (1 - s) + c[i] * a * s) / oa : c[i];
            p->c[3] = oa * 255;
        }
}

// ------------------------------------------------------------------ the icon's own colours, and its symbol taken out
// to the nearest transparent pixel (or the image's edge), up to 8 pixels (all that's asked: is it 2.5 / 5 inside)
static float edge_dist(const Icon *ic, int x, int y) {
    float best = fminf(fminf(fminf(x + 0.5f, y + 0.5f), fminf(ic->w - x - 0.5f, ic->h - y - 0.5f)), 8);
    for (int j = y - 8 < 0 ? 0 : y - 8; j < ic->h && j <= y + 8; j++)
        for (int i = x - 8 < 0 ? 0 : x - 8; i < ic->w && i <= x + 8; i++)
            if (ic->px[j * ic->w + i].c[3] < 100) {
                float d = sqrtf((float)((i - x) * (i - x) + (j - y) * (j - y)));
                if (d < best) best = d;
            }
    return best;
}

static int cmp_float(const void *a, const void *b) { float d = *(const float *)a - *(const float *)b; return d < 0 ? -1 : d > 0; }

// inner: pixels well inside the shape (where the symbol is); letter: the bright ones there (the symbol, a pixel more)
static void measure(Icon *ic, bool *letter, float inner_d) {
    int n = ic->w * ic->h, ni = 0;
    float *d = malloc(sizeof(float) * n), *ls = malloc(sizeof(float) * n);
    for (int y = 0; y < ic->h; y++) for (int x = 0; x < ic->w; x++) d[y * ic->w + x] = edge_dist(ic, x, y);
    float lmax = 0;
    for (int i = 0; i < n; i++)
        if (d[i] >= inner_d) { float l = lum(ic->px[i].c); ls[ni++] = l; if (l > lmax) lmax = l; }
    qsort(ls, ni, sizeof(float), cmp_float);
    float lmed = ni ? ls[ni / 2] : 0, thr = lmed + 0.3f * (lmax - lmed);
    bool *raw = calloc(n, 1);
    for (int i = 0; i < n; i++) raw[i] = d[i] >= inner_d && lum(ic->px[i].c) > thr;
    // the colours: the symbol's (its brightest), the inside's (the middle of the rest), the rim's (bright, at the edge)
    float g[3] = {0}, f[4] = {0}, r[3] = {0};
    int ng = 0, nf = 0, nr = 0;
    for (int i = 0; i < n; i++) {
        const float *c = ic->px[i].c;
        if (d[i] >= inner_d && lum(c) >= lmed + 0.85f * (lmax - lmed)) { for (int k = 0; k < 3; k++) g[k] += c[k]; ng++; }
        if (d[i] >= inner_d && !raw[i] && fabsf(lum(c) - lmed) <= 0.1f * (lmax - lmed) + 2) { for (int k = 0; k < 4; k++) f[k] += c[k]; nf++; }
    }
    float rl = 0;  // rim: the brightest quarter of the opaque pixels within 2.5 of the edge
    for (int i = 0; i < n; i++) if (d[i] < 2.5f && ic->px[i].c[3] > 200 && lum(ic->px[i].c) > rl) rl = lum(ic->px[i].c);
    for (int i = 0; i < n; i++) {
        const float *c = ic->px[i].c;
        if (d[i] < 2.5f && c[3] > 200 && lum(c) >= 0.75f * rl) { for (int k = 0; k < 3; k++) r[k] += c[k]; nr++; }
    }
    for (int k = 0; k < 3; k++) ic->glyph[k] = ng ? g[k] / ng : 255, ic->ring[k] = nr ? r[k] / nr : 200;
    for (int k = 0; k < 4; k++) ic->fill[k] = nf ? f[k] / nf : (k == 3 ? 224 : 30);
    if (letter)
        for (int y = 0; y < ic->h; y++)
            for (int x = 0; x < ic->w; x++) {
                bool m = false;
                for (int j = -1; j <= 1 && !m; j++)
                    for (int i = -1; i <= 1 && !m; i++) {
                        int xx = x + i, yy = y + j;
                        m = xx >= 0 && yy >= 0 && xx < ic->w && yy < ic->h && raw[yy * ic->w + xx];
                    }
                letter[y * ic->w + x] = m && d[y * ic->w + x] >= 2.5f;
            }
    free(raw), free(d), free(ls);
}

// the symbol's pixels filled in from around them (the inside's gradient carried across: Laplace, by relaxation)
static void erase(Icon *ic, const bool *letter) {
    int n = ic->w * ic->h;
    for (int i = 0; i < n; i++) if (letter[i]) memcpy(ic->px[i].c, ic->fill, sizeof ic->fill);
    for (int it = 0; it < 300; it++)
        for (int y = 0; y < ic->h; y++)
            for (int x = 0; x < ic->w; x++) {
                if (!letter[y * ic->w + x]) continue;
                float s[4] = {0};
                int c = 0;
                static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; k++) {
                    int xx = x + dx[k], yy = y + dy[k];
                    if (xx < 0 || yy < 0 || xx >= ic->w || yy >= ic->h) continue;
                    for (int i = 0; i < 4; i++) s[i] += ic->px[yy * ic->w + xx].c[i];
                    c++;
                }
                if (c) for (int i = 0; i < 4; i++) ic->px[y * ic->w + x].c[i] = s[i] / c;
            }
}

// ------------------------------------------------------------------ the symbols
// (k: the icon's size over the reference 36 pixels; cx, cy: where the symbol is centred)
static void draw_symbol(Icon *ic, const GaSpec *s, int shape, float k, float cx, float cy) {
    float hw = 1.3f * k;  // stroke half-width (the game's letters: about 2.6 pixels)
    switch (s->sym) {
    case GS_TEXT: {
        size_t n = strlen(s->text);
        float cap = (n <= 1 ? 15.0f : n == 2 ? 11.5f : 8.5f) * k;
        float maxw = (shape == GA_CIRCLE ? 22.0f : 26.0f) * k;
        draw_text(ic, s->text, cx, cy, cap, maxw);
        break;
    }
    case GS_CROSS:
        draw_seg(ic, cx - 5.6f * k, cy - 5.6f * k, cx + 5.6f * k, cy + 5.6f * k, hw);
        draw_seg(ic, cx - 5.6f * k, cy + 5.6f * k, cx + 5.6f * k, cy - 5.6f * k, hw);
        break;
    case GS_RING: {
        float r = 6.4f * k;
        COV_SHAPE(ic, fabsf(sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r) <= hw);
        break;
    }
    case GS_SQUARE:
        COV_SHAPE(ic, fabsf(box_dist(x, y, cx, cy, 6.0f * k, 6.0f * k, 0.8f * k)) <= hw);
        break;
    case GS_TRIANGLE: {
        float t[6] = {cx, cy - 6.6f * k, cx + 7.2f * k, cy + 5.4f * k, cx - 7.2f * k, cy + 5.4f * k};
        COV_SHAPE(ic, seg_dist(x, y, t[0], t[1], t[2], t[3]) <= hw || seg_dist(x, y, t[2], t[3], t[4], t[5]) <= hw ||
                      seg_dist(x, y, t[4], t[5], t[0], t[1]) <= hw);
        break;
    }
    case GS_MENU:  // Xbox Menu, PlayStation Options: three bars
        for (int i = -1; i <= 1; i++) draw_seg(ic, cx - 6.0f * k, cy + i * 4.4f * k, cx + 6.0f * k, cy + i * 4.4f * k, 1.15f * k);
        break;
    case GS_VIEW: {  // Xbox View: two overlapping windows, the front one hiding the back one's corner
        float bx = cx + 2.2f * k, by = cy - 2.2f * k, fx = cx - 2.2f * k, fy = cy + 2.2f * k, hx = 4.6f * k, hy = 3.6f * k;
        float t = 1.05f * k;
        COV_SHAPE(ic, (fabsf(box_dist(x, y, bx, by, hx, hy, 1.0f * k)) <= t && box_dist(x, y, fx, fy, hx, hy, 1.0f * k) > t + 0.9f * k) ||
                      fabsf(box_dist(x, y, fx, fy, hx, hy, 1.0f * k)) <= t);
        break;
    }
    case GS_SHARE: {  // PlayStation Create / Share: three short rays fanning up and out from a point below them
        float ox = cx, oy = cy + 6.5f * k;
        for (int i = -1; i <= 1; i++) {
            float a = i * 0.72f, dx = sinf(a), dy = -cosf(a);
            draw_seg(ic, ox + dx * 4.5f * k, oy + dy * 4.5f * k, ox + dx * 12.5f * k, oy + dy * 12.5f * k, 1.2f * k);
        }
        break;
    }
    case GS_DPAD: {  // the cross, its arm for this direction solid and the rest outlined
        float a = 2.6f * k, l = 7.6f * k;
        COV_SHAPE(ic, fabsf(fminf(box_dist(x, y, cx, cy, a, l, 0.8f * k), box_dist(x, y, cx, cy, l, a, 0.8f * k))) <= 0.7f * k);
        float ax = s->dir == GD_LEFT ? -1 : s->dir == GD_RIGHT ? 1 : 0, ay = s->dir == GD_UP ? -1 : s->dir == GD_DOWN ? 1 : 0;
        float mx = cx + ax * (l + a) / 2, my = cy + ay * (l + a) / 2;
        COV_SHAPE(ic, box_dist(x, y, mx, my, ax ? (l - a) / 2 : a, ay ? (l - a) / 2 : a, 0.6f * k) <= 0);
        break;
    }
    default:
        break;
    }
}

// a key: its body in the icon's colours (rim around, a deeper lower edge), its label in the symbol's
static void draw_key(Icon *ic, float cx, float cy, float hx, float hy, float r, float rim, const char *label,
                     float cap, int arrow, bool lit) {
    float k = ic->w / 36.0f;
    cov_clear(ic);
    COV_SHAPE(ic, box_dist(x, y, cx, cy, hx, hy, r) <= 0);
    paint(ic, ic->ring, 1);
    cov_clear(ic);
    float lip = rim * 1.8f;  // (the lower edge: thicker, a key's side seen from above)
    COV_SHAPE(ic, box_dist(x, y, cx, cy - (lip - rim) / 2, hx - rim, hy - (rim + lip) / 2, fmaxf(r - rim, 0.5f)) <= 0);
    if (lit) paint(ic, ic->glyph, 1);
    else paint(ic, ic->fill, ic->fill[3] / 255);
    cov_clear(ic);
    float ty = cy - (lip - rim) / 2;
    if (arrow >= 0) {
        float a = fminf(hx, hy) * 0.55f, t[6];
        switch (arrow) {
        case GD_UP: t[0] = cx, t[1] = ty - a, t[2] = cx + a, t[3] = ty + a * 0.6f, t[4] = cx - a, t[5] = ty + a * 0.6f; break;
        case GD_DOWN: t[0] = cx, t[1] = ty + a, t[2] = cx + a, t[3] = ty - a * 0.6f, t[4] = cx - a, t[5] = ty - a * 0.6f; break;
        case GD_LEFT: t[0] = cx - a, t[1] = ty, t[2] = cx + a * 0.6f, t[3] = ty - a, t[4] = cx + a * 0.6f, t[5] = ty + a; break;
        default: t[0] = cx + a, t[1] = ty, t[2] = cx - a * 0.6f, t[3] = ty - a, t[4] = cx - a * 0.6f, t[5] = ty + a; break;
        }
        COV_SHAPE(ic, in_tri(x, y, t));
    } else {
        draw_text(ic, label, cx, ty, cap, (hx - rim) * 2 - 2.0f * k);
    }
    composite(ic, lit ? ic->fill : ic->glyph, 1, 0);
}

static float key_cap(const char *label, float k) {  // a key label's capital height by its length
    size_t n = strlen(label);
    return (n <= 1 ? 14.0f : n == 2 ? 11.0f : n == 3 ? 8.5f : 7.0f) * k;
}

static void draw_mouse(Icon *ic, int which) {
    float k = ic->w / 36.0f, cx = ic->w / 2.0f, top = 3.0f * k, bot = ic->h - 3.0f * k, hx = 9.5f * k;
    float cy = (top + bot) / 2, hy = (bot - top) / 2, split = top + 12.0f * k, rim = 1.6f * k;
    cov_clear(ic);
    COV_SHAPE(ic, box_dist(x, y, cx, cy, hx, hy, hx) <= 0);
    paint(ic, ic->ring, 1);
    cov_clear(ic);
    COV_SHAPE(ic, box_dist(x, y, cx, cy, hx - rim, hy - rim, hx - rim) <= 0);
    paint(ic, ic->fill, ic->fill[3] / 255);
    cov_clear(ic);  // the buttons: the one meant lit
    if (which == GM_LEFT || which == GM_RIGHT) {
        float side = which == GM_LEFT ? -1 : 1;
        COV_SHAPE(ic, box_dist(x, y, cx, cy, hx - rim, hy - rim, hx - rim) <= 0 && y < split && (x - cx) * side > 0.9f * k);
        paint(ic, ic->glyph, 1);
        cov_clear(ic);
    }
    // the seams: down the middle to the buttons' edge, and across under them; the wheel
    COV_SHAPE(ic, box_dist(x, y, cx, cy, hx - rim, hy - rim, hx - rim) <= 0 &&
                  ((fabsf(x - cx) <= 0.6f * k && y < split) || fabsf(y - split) <= 0.6f * k));
    paint(ic, ic->ring, 1);
    cov_clear(ic);
    float wy = top + 6.5f * k;
    COV_SHAPE(ic, box_dist(x, y, cx, wy, 1.6f * k, 3.2f * k, 1.6f * k) <= 0);
    paint(ic, which == GM_MIDDLE || which == GM_WHEEL_UP || which == GM_WHEEL_DOWN ? ic->glyph : ic->ring, 1);
    if (which == GM_WHEEL_UP || which == GM_WHEEL_DOWN) {  // an arrow under the buttons
        cov_clear(ic);
        float ay = split + 7.5f * k, d = which == GM_WHEEL_UP ? -1 : 1, a = 4.0f * k;
        float t[6] = {cx, ay + d * a, cx + a, ay - d * a * 0.5f, cx - a, ay - d * a * 0.5f};
        COV_SHAPE(ic, in_tri(x, y, t));
        paint(ic, ic->glyph, 1);
    }
}

// ------------------------------------------------------------------
bool glyph_art_render(uint8_t *bc7, int w, int h, int shape, int dot_lit, const GaSpec *s, const uint8_t *ttf) {
    if (w <= 0 || h <= 0 || w > MAXW || h > MAXW || (w & 3) || (h & 3)) return false;
    if (ttf != g_font_data) {
        g_font_data = ttf;
        g_font_ok = ttf && stbtt_InitFont(&g_font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
    }
    Icon *ic = calloc(1, sizeof *ic);
    ic->w = w, ic->h = h;
    int bw = w / 4, bh = h / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            uint8_t texel[64];
            bcdec_bc7(bc7 + (by * bw + bx) * 16, texel, 16);
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    Px *p = &ic->px[(h - 1 - (by * 4 + j)) * w + bx * 4 + i];  // (upright)
                    for (int c = 0; c < 4; c++) p->c[c] = texel[(j * 4 + i) * 4 + c];
                }
        }
    float k = w / 36.0f, cx = w / 2.0f, cy = h / 2.0f;
    bool *letter = calloc((size_t)w * h, 1);
    bool keyboard = s->sym >= GS_KEY;
    measure(ic, keyboard ? NULL : letter, 5.0f * k);
    if (keyboard) {  // the icon redrawn whole: a key (or a mouse, or four keys)
        memset(ic->px, 0, sizeof(Px) * w * h);
        if (s->sym == GS_MOUSE) {
            draw_mouse(ic, s->dir);
        } else if (s->sym == GS_KEYS4) {  // up above, then left, down, right
            float kh = 5.6f * k, gap = 0.7f * k, r = 1.6f * k, rim = 1.0f * k;
            float row2 = h / 2.0f + kh + gap / 2 + 0.5f * k, row1 = row2 - 2 * kh - gap;
            float cap = 6.0f * k, x[4] = {cx, cx - 2 * kh - gap, cx, cx + 2 * kh + gap}, y[4] = {row1, row2, row2, row2};
            for (int i = 0; i < 4; i++) {
                const char *l = s->keys[i];
                int arrow = !strcmp(l, "^") ? GD_UP : !strcmp(l, "v") ? GD_DOWN : !strcmp(l, "<") ? GD_LEFT : !strcmp(l, ">") ? GD_RIGHT : -1;
                draw_key(ic, x[i], y[i], kh, kh, r, rim, l, cap, arrow, false);
            }
        } else {
            draw_key(ic, cx, cy, 15.5f * k, 14.5f * k, 4.5f * k, 1.6f * k, s->text, key_cap(s->text, k),
                     s->sym == GS_ARROW_KEY ? s->dir : -1, false);
        }
    } else if (shape == GA_DOTS && s->sym == GS_DOT) {  // the four dots: the light moved to another one
        static const int org[4][2] = {{12, 1}, {12, 22}, {2, 12}, {22, 12}};  // (GD_*: each dot's 12 x 12 box)
        int from = dot_lit, to = s->dir, other = -1;
        for (int i = 0; i < 4 && other < 0; i++) if (i != from && i != to) other = i;
        if (from != to && from >= 0 && from < 4 && to >= 0 && to < 4) {
            Px lit[24 * 24], dark[24 * 24];
            int n = (int)(12 * k);
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) {
                    lit[y * n + x] = ic->px[((int)(org[from][1] * k) + y) * w + (int)(org[from][0] * k) + x];
                    dark[y * n + x] = ic->px[((int)(org[other][1] * k) + y) * w + (int)(org[other][0] * k) + x];
                }
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++) {
                    ic->px[((int)(org[to][1] * k) + y) * w + (int)(org[to][0] * k) + x] = lit[y * n + x];
                    ic->px[((int)(org[from][1] * k) + y) * w + (int)(org[from][0] * k) + x] = dark[y * n + x];
                }
        }
    } else {  // the symbol replaced, in the shape's own colours
        if (shape == GA_DOTS || shape == GA_STICK) {  // (no letter to take out: a round button drawn over it)
            memset(ic->px, 0, sizeof(Px) * w * h);
            cov_clear(ic);
            COV_SHAPE(ic, (x - cx) * (x - cx) + (y - cy) * (y - cy) <= 16.6f * k * 16.6f * k);
            paint(ic, ic->ring, 1);
            cov_clear(ic);
            COV_SHAPE(ic, (x - cx) * (x - cx) + (y - cy) * (y - cy) <= 15.0f * k * 15.0f * k);
            paint(ic, ic->fill, ic->fill[3] / 255);
            shape = GA_CIRCLE;
        } else {
            erase(ic, letter);
        }
        if (shape == GA_TRIGGER) cy += 1.0f * k;  // (its rounded corner makes its middle sit lower)
        cov_clear(ic);
        draw_symbol(ic, s, shape, k, cx, cy);
        composite(ic, ic->glyph, 1, 0.45f);
    }
    free(letter);
    // encoded back, rows bottom first again; colour under fully transparent pixels black, as the game's
    Px *flip = malloc(sizeof(Px) * w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            Px p = ic->px[(h - 1 - y) * w + x];
            for (int c = 0; c < 4; c++) p.c[c] = fminf(255, fmaxf(0, p.c[c]));
            if (p.c[3] < 0.5f) memset(&p, 0, sizeof p);
            flip[y * w + x] = p;
        }
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) bc7_encode_block(&flip[(by * 4) * w + bx * 4], w, bc7 + (by * bw + bx) * 16);
    free(flip);
    free(ic);
    return true;
}

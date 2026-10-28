// CPU run of a simple vertex shader: where a draw's vertices land on screen, without the GPU.
// Only straight-line shaders made of attribute loads / stores, float MOV / ADD / MUL / FMA and EXIT, none
// predicated: the shaders that put HUD pieces on screen (a matrix from a constant buffer times the vertex position).
// Anything else is refused at build time. The instruction encodings are read as translate.c reads them.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

#define RZ 255

enum { V_ALD, V_AST, V_MOV, V_FADD, V_FMUL, V_FFMA, V_EXIT };
enum { O_REG, O_CBUF, O_IMM };
typedef struct {
    uint8_t kind, ak, bk;  // operand kinds of b and c (O_*)
    uint8_t d, a, n;       // destination, operand a; ALD / AST: first register, component count
    uint8_t neg_a, abs_a, neg_b, abs_b, neg_c, sat, fmz, scale;
    uint32_t b, c;         // register / immediate bits / cbuf (index << 16 | byte offset); ALD / AST: attribute address
} VsOp;

struct VsSim {
    int nops;
    uint32_t attr_mask, cbuf_mask;
    VsOp ops[];
};

static uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & ((1ull << n) - 1); }

// operand B in its register / cbuf / immediate forms (translate.c operand_b)
static void operand_b(uint64_t w, bool float_imm, uint8_t *k, uint32_t *v) {
    uint64_t top = w >> 60;
    if (top == 0x5) *k = O_REG, *v = (uint32_t)bits(w, 20, 8);
    else if (top == 0x4) *k = O_CBUF, *v = (uint32_t)bits(w, 34, 5) << 16 | (uint32_t)bits(w, 20, 14) * 4;
    else {
        uint32_t x = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (float_imm) x <<= 12;
        else if (x & 0x80000u) x |= 0xFFF00000u;
        *k = O_IMM, *v = x;
    }
}

VsSim *maxwell_vsim_build(const uint8_t *blob, size_t max_size) {
    if (!blob || max_size <= 0x80) return NULL;
    const uint64_t *code = (const uint64_t *)(blob + 0x80);
    size_t n = (max_size - 0x80) / 8;
    if (n > 1024) n = 1024;
    VsSim *s = calloc(1, sizeof *s + n * sizeof(VsOp));
    bool pos = false;
    for (size_t i = 0; i < n; i++) {
        if (i % 4 == 0) continue;  // (scheduling word)
        uint64_t w = code[i];
#define OPM(val, mask) ((w & (uint64_t)(mask)) == (uint64_t)(val))
        int pr = (int)bits(w, 16, 3);
        bool prn = bits(w, 19, 1);
        if (OPM(0xf0f0000000000000ull, 0xfff8000000000000ull) || OPM(0x50b0000000000000ull, 0xfff8000000000000ull))
            continue;  // DEPBAR / NOP
        if (pr == 7 && prn) continue;  // @!PT: never runs
        if (pr != 7) goto refuse;      // (predicated)
        VsOp *o = &s->ops[s->nops];
        memset(o, 0, sizeof *o);
        o->d = (uint8_t)bits(w, 0, 8), o->a = (uint8_t)bits(w, 8, 8);
        if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
            if (bits(w, 0, 5) != 15) goto refuse;
            o->kind = V_EXIT;
            s->nops++;
            break;
        } else if (OPM(0xefd8000000000000ull, 0xfff8000000000000ull) || OPM(0xeff0000000000000ull, 0xfff8000000000000ull)) {
            bool load = OPM(0xefd8000000000000ull, 0xfff8000000000000ull);  // ALD / AST
            o->kind = load ? V_ALD : V_AST;
            o->n = (uint8_t)(1 + bits(w, 47, 2));
            o->b = (uint32_t)bits(w, 20, 10);
            if (o->a != RZ) goto refuse;  // (indexed)
            if (load) {
                if (o->b < 0x80 || o->b + 4u * o->n > 0x280) goto refuse;  // (generic attributes only)
                for (int c = 0; c < o->n; c++) s->attr_mask |= 1u << ((o->b + 4 * c - 0x80) >> 4);
            } else if (o->b < 0x80 && o->b + 4u * o->n > 0x70) {
                pos = true;
            }
        } else if (OPM(0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(0x4c58000000000000ull, 0xfff8000000000000ull) ||
                   OPM(0x3858000000000000ull, 0xfef8000000000000ull)) {  // FADD
            o->kind = V_FADD;
            operand_b(w, true, &o->bk, &o->b);
            o->neg_a = bits(w, 48, 1), o->abs_a = bits(w, 46, 1), o->neg_b = bits(w, 45, 1), o->abs_b = bits(w, 49, 1);
            o->sat = bits(w, 50, 1);
        } else if (OPM(0x0800000000000000ull, 0xfc00000000000000ull)) {  // FADD32I
            o->kind = V_FADD, o->bk = O_IMM, o->b = (uint32_t)bits(w, 20, 32);
            o->neg_a = bits(w, 56, 1), o->abs_a = bits(w, 54, 1), o->neg_b = bits(w, 53, 1), o->abs_b = bits(w, 57, 1);
        } else if (OPM(0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(0x4c68000000000000ull, 0xfff8000000000000ull) ||
                   OPM(0x3868000000000000ull, 0xfef8000000000000ull)) {  // FMUL
            o->kind = V_FMUL;
            operand_b(w, true, &o->bk, &o->b);
            o->neg_b = bits(w, 48, 1), o->fmz = bits(w, 44, 2) == 2, o->scale = (uint8_t)bits(w, 41, 3), o->sat = bits(w, 50, 1);
        } else if (OPM(0x1e00000000000000ull, 0xff00000000000000ull)) {  // FMUL32I
            o->kind = V_FMUL, o->bk = O_IMM, o->b = (uint32_t)bits(w, 20, 32);
            o->fmz = bits(w, 53, 2) == 2, o->sat = bits(w, 55, 1);
        } else if (OPM(0x5980000000000000ull, 0xff80000000000000ull) || OPM(0x4980000000000000ull, 0xff80000000000000ull) ||
                   OPM(0x5180000000000000ull, 0xff80000000000000ull) || OPM(0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
            o->kind = V_FFMA;
            uint64_t op8 = w >> 56;
            if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {  // (0x51: B = REG_39, C = cbuf)
                o->bk = O_REG, o->b = (uint32_t)bits(w, 39, 8);
                o->ak = O_CBUF, o->c = (uint32_t)bits(w, 34, 5) << 16 | (uint32_t)bits(w, 20, 14) * 4;
            } else {
                operand_b(w, true, &o->bk, &o->b);
                o->ak = O_REG, o->c = (uint32_t)bits(w, 39, 8);
            }
            o->neg_b = bits(w, 48, 1), o->neg_c = bits(w, 49, 1), o->fmz = bits(w, 53, 2) == 2, o->sat = bits(w, 50, 1);
        } else if (OPM(0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FFMA32I (C = the destination)
            o->kind = V_FFMA, o->bk = O_IMM, o->b = (uint32_t)bits(w, 20, 32), o->ak = O_REG, o->c = o->d;
            o->neg_a = bits(w, 56, 1), o->neg_c = bits(w, 57, 1), o->fmz = bits(w, 53, 2) == 2, o->sat = bits(w, 55, 1);
        } else if (OPM(0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(0x4c98000000000000ull, 0xfff8000000000000ull) ||
                   OPM(0x3898000000000000ull, 0xfef8000000000000ull)) {  // MOV
            o->kind = V_MOV;
            operand_b(w, false, &o->bk, &o->b);
        } else if (OPM(0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
            o->kind = V_MOV, o->bk = O_IMM, o->b = (uint32_t)bits(w, 20, 32);
        } else {
            goto refuse;
        }
        if (o->kind >= V_FADD && bits(w, 47, 1)) goto refuse;  // (.CC)
        if (o->bk == O_CBUF) s->cbuf_mask |= 1u << (o->b >> 16);
        if (o->ak == O_CBUF) s->cbuf_mask |= 1u << (o->c >> 16);
        s->nops++;
#undef OPM
    }
    if (!pos || !s->nops || s->ops[s->nops - 1].kind != V_EXIT) goto refuse;
    return s;
refuse:
    free(s);
    return NULL;
}

void maxwell_vsim_free(VsSim *s) { free(s); }
uint32_t maxwell_vsim_attrs(const VsSim *s) { return s->attr_mask; }
uint32_t maxwell_vsim_cbufs(const VsSim *s) { return s->cbuf_mask; }

static float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t u_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

bool maxwell_vsim_run(const VsSim *s, const float attr[32][4], const uint8_t *const cbuf[18], const uint32_t cbuf_size[18],
                      float pos[4]) {
    uint32_t r[256] = {0};
    pos[0] = pos[1] = pos[2] = 0, pos[3] = 1;
#define RD(i) ((i) == RZ ? 0u : r[i])
    for (int i = 0; i < s->nops; i++) {
        const VsOp *o = &s->ops[i];
        uint32_t bv = 0, cv = 0;
        for (int k = 0; k < 2; k++) {
            uint8_t kind = k ? o->ak : o->bk;
            uint32_t v = k ? o->c : o->b, x;
            if (k && o->kind != V_FFMA) break;
            if (kind == O_REG) x = RD(v);
            else if (kind == O_IMM) x = v;
            else {
                uint32_t idx = v >> 16, off = v & 0xFFFF;
                if (idx >= 18 || !cbuf[idx] || off + 4 > cbuf_size[idx]) return false;
                memcpy(&x, cbuf[idx] + off, 4);
            }
            if (k) cv = x; else bv = x;
        }
        float a = f_of(RD(o->a)), b = f_of(bv), c = f_of(cv), res;
        switch (o->kind) {
        case V_EXIT: return true;
        case V_ALD:
            for (int k = 0; k < o->n; k++) {
                uint32_t ad = o->b + 4 * k - 0x80;
                if (o->d + k < RZ) r[o->d + k] = u_of(attr[ad >> 4][(ad >> 2) & 3]);
            }
            continue;
        case V_AST:
            for (int k = 0; k < o->n; k++) {
                uint32_t ad = o->b + 4 * k;
                if (ad >= 0x70 && ad < 0x80) pos[(ad >> 2) & 3] = f_of(RD(o->d + k));
            }
            continue;
        case V_MOV:
            if (o->d != RZ) r[o->d] = bv;
            continue;
        case V_FADD:
            if (o->abs_a) a = fabsf(a);
            if (o->neg_a) a = -a;
            if (o->abs_b) b = fabsf(b);
            if (o->neg_b) b = -b;
            res = a + b;
            break;
        case V_FMUL: {
            static const float scale[] = {1, 0.5f, 0.25f, 0.125f, 8, 4, 2, 1};
            if (o->neg_b) b = -b;
            res = o->fmz && (a == 0 || b == 0) ? 0 : a * b * scale[o->scale];
            break;
        }
        default:  // V_FFMA
            if (o->neg_a) a = -a;
            if (o->neg_b) b = -b;
            if (o->neg_c) c = -c;
            res = o->fmz && (a == 0 || b == 0) ? c : fmaf(a, b, c);
            break;
        }
        if (o->sat) res = isnan(res) ? 0 : res < 0 ? 0 : res > 1 ? 1 : res;
        if (o->d != RZ) r[o->d] = u_of(res);
    }
#undef RD
    return true;
}

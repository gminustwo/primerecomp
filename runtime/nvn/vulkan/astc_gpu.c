// ASTC decoded by a compute shader, for GPUs without ASTC (desktop NVIDIA / AMD / Intel, the Steam Deck). The CPU
// decoder (runtime/nvn/astc.c) ran on the render thread at a texture's first use: entering a room decoded ~50 textures
// of 1024x1024 at ~10 ms each - half a second of frozen frame at a door. Here the blocks go up as they are (a quarter
// of the RGBA bytes) and one invocation per block decodes them into RGBA8 in the same transient span, which is then
// copied into the image. The shader is astc.c's astc_decode_block line by line, with its tables (astc_tables), so the
// result is the same bytes (LARECOMP_ASTC_SELFTEST=<blocks> compares the two on random blocks of every footprint at
// startup). LARECOMP_ASTC_CPU=1: the CPU decoder instead.
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "shaders.h"
#include "../astc.h"
#include "../maxwell/shader.h"

static const char *const astc_glsl =
    "#version 450\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, set = 0, binding = 0) readonly buffer Blocks { uvec4 blk[]; };\n"
    "layout(std430, set = 0, binding = 1) writeonly buffer Texels { uint px[]; };\n"
    "layout(std430, set = 0, binding = 2) readonly buffer Tables { uint tab[]; };\n"
    // in: first block (16-byte units), out: first texel (4-byte units), both from the bound span's start
    "layout(push_constant) uniform Pc { uint in_off, out_off, w, h, bw, bh, srgb, bx, nblk; } pc;\n"
    "#define QB(q) int(tab[(q)])\n"
    "#define QT(q) (tab[21 + (q)] != 0u)\n"
    "#define QQ(q) (tab[42 + (q)] != 0u)\n"
    "#define WUNQ(i) tab[63 + (i)]\n"
    "#define WOFF(q) int(tab[205 + (q)])\n"
    "#define CUNQ(i) tab[217 + (i)]\n"
    "#define COFF(q) int(tab[1153 + (q)])\n"
    "uint ise[64];\n"
    "uint grid0[80], grid1[80];\n"
    "uint cv[18];\n"
    "int ps_s[8]; uint ps_r[4]; int ps_count;\n"
    "int X0, Y0, BW, BH;\n"
    // n (<= 32) bits at pos; bits past 127 read as zero
    "uint rbits(uvec4 b, int pos, int n) {\n"
    "    if (n == 0 || pos >= 128) return 0u;\n"
    "    int wi = pos >> 5, sh = pos & 31;\n"
    "    uint v = b[wi] >> sh;\n"
    "    if (sh != 0 && wi < 3) v |= b[wi + 1] << (32 - sh);\n"
    "    return n >= 32 ? v : v & ((1u << n) - 1u);\n"
    "}\n"
    "int ise_bitcount(int count, int q) {\n"
    "    int n = count * QB(q);\n"
    "    if (QT(q)) n += (8 * count + 4) / 5;\n"
    "    if (QQ(q)) n += (7 * count + 2) / 3;\n"
    "    return n;\n"
    "}\n"
    "void trits_of(uint T, out uint t[5]) {\n"
    "    uint C;\n"
    "    if (((T >> 2) & 7u) == 7u) { C = (((T >> 5) & 7u) << 2) | (T & 3u); t[4] = 2u; t[3] = 2u; }\n"
    "    else {\n"
    "        C = T & 0x1Fu;\n"
    "        if (((T >> 5) & 3u) == 3u) { t[4] = 2u; t[3] = (T >> 7) & 1u; }\n"
    "        else { t[4] = (T >> 7) & 1u; t[3] = (T >> 5) & 3u; }\n"
    "    }\n"
    "    if ((C & 3u) == 3u) {\n"
    "        t[2] = 2u; t[1] = (C >> 4) & 1u;\n"
    "        t[0] = (((C >> 3) & 1u) << 1) | (((C >> 2) & 1u) & ~(C >> 3) & 1u);\n"
    "    } else if (((C >> 2) & 3u) == 3u) {\n"
    "        t[2] = 2u; t[1] = 2u; t[0] = C & 3u;\n"
    "    } else {\n"
    "        t[2] = (C >> 4) & 1u; t[1] = (C >> 2) & 3u;\n"
    "        t[0] = (((C >> 1) & 1u) << 1) | ((C & 1u) & ~(C >> 1) & 1u);\n"
    "    }\n"
    "}\n"
    "void quints_of(uint Q, out uint q[3]) {\n"
    "    if (((Q >> 1) & 3u) == 3u && ((Q >> 5) & 3u) == 0u) {\n"
    "        uint q0 = Q & 1u;\n"
    "        q[2] = (q0 << 2) | ((((Q >> 4) & 1u) & ~q0 & 1u) << 1) | (((Q >> 3) & 1u) & ~q0 & 1u);\n"
    "        q[1] = 4u; q[0] = 4u;\n"
    "        return;\n"
    "    }\n"
    "    uint C;\n"
    "    if (((Q >> 1) & 3u) == 3u) { q[2] = 4u; C = (((Q >> 3) & 3u) << 3) | ((~(Q >> 5) & 3u) << 1) | (Q & 1u); }\n"
    "    else { q[2] = (Q >> 5) & 3u; C = Q & 0x1Fu; }\n"
    "    if ((C & 7u) == 5u) { q[1] = 4u; q[0] = (C >> 3) & 3u; }\n"
    "    else { q[1] = (C >> 3) & 3u; q[0] = C & 7u; }\n"
    "}\n"
    // count raw ISE values (trit/quint << bits | bits) from bit pos, into ise[]
    "void ise_decode(uvec4 b, int pos, int count, int q) {\n"
    "    int nb = QB(q);\n"
    "    if (QT(q)) {\n"
    "        for (int i = 0; i < count; i += 5) {\n"
    "            uint m[5] = uint[5](0u, 0u, 0u, 0u, 0u);\n"
    "            uint T = 0u;\n"
    "            int k = 0;\n"
    "            for (; k < 5 && i + k < count; k++) {\n"
    "                m[k] = rbits(b, pos, nb); pos += nb;\n"
    "                int tb = k == 2 || k == 4 ? 1 : 2, ts = k == 0 ? 0 : k == 1 ? 2 : k == 2 ? 4 : k == 3 ? 5 : 7;\n"
    "                T |= rbits(b, pos, tb) << ts; pos += tb;\n"
    "            }\n"
    "            uint t[5];\n"
    "            trits_of(T, t);\n"
    "            for (int j = 0; j < k; j++) ise[i + j] = (t[j] << nb) | m[j];\n"
    "        }\n"
    "    } else if (QQ(q)) {\n"
    "        for (int i = 0; i < count; i += 3) {\n"
    "            uint m[3] = uint[3](0u, 0u, 0u);\n"
    "            uint Q = 0u;\n"
    "            int k = 0;\n"
    "            for (; k < 3 && i + k < count; k++) {\n"
    "                m[k] = rbits(b, pos, nb); pos += nb;\n"
    "                int qb = k == 0 ? 3 : 2, qs = k == 0 ? 0 : k == 1 ? 3 : 5;\n"
    "                Q |= rbits(b, pos, qb) << qs; pos += qb;\n"
    "            }\n"
    "            uint v[3];\n"
    "            quints_of(Q, v);\n"
    "            for (int j = 0; j < k; j++) ise[i + j] = (v[j] << nb) | m[j];\n"
    "        }\n"
    "    } else {\n"
    "        for (int i = 0; i < count; i++, pos += nb) ise[i] = rbits(b, pos, nb);\n"
    "    }\n"
    "}\n"
    "bool block_mode(uint m, int bw, int bh, out int wx, out int wy, out int dual, out int quant, out int wbits) {\n"
    "    uint r = (m >> 4) & 1u, h = (m >> 9) & 1u, d = (m >> 10) & 1u, a = (m >> 5) & 3u;\n"
    "    wx = 0; wy = 0; dual = 0; quant = 0; wbits = 0;\n"
    "    if ((m & 3u) != 0u) {\n"
    "        r |= (m & 3u) << 1;\n"
    "        uint b = (m >> 7) & 3u, sel = (m >> 2) & 3u;\n"
    "        if (sel == 0u) { wx = int(b) + 4; wy = int(a) + 2; }\n"
    "        else if (sel == 1u) { wx = int(b) + 8; wy = int(a) + 2; }\n"
    "        else if (sel == 2u) { wx = int(a) + 2; wy = int(b) + 8; }\n"
    "        else {\n"
    "            b &= 1u;\n"
    "            if ((m & 0x100u) != 0u) { wx = int(b) + 2; wy = int(a) + 2; }\n"
    "            else { wx = int(a) + 2; wy = int(b) + 6; }\n"
    "        }\n"
    "    } else {\n"
    "        r |= ((m >> 2) & 3u) << 1;\n"
    "        if (((m >> 2) & 3u) == 0u) return false;\n"
    "        uint b = (m >> 9) & 3u, sel = (m >> 7) & 3u;\n"
    "        if (sel == 0u) { wx = 12; wy = int(a) + 2; }\n"
    "        else if (sel == 1u) { wx = int(a) + 2; wy = 12; }\n"
    "        else if (sel == 2u) { wx = int(a) + 6; wy = int(b) + 6; d = 0u; h = 0u; }\n"
    "        else if (a == 0u) { wx = 6; wy = 10; }\n"
    "        else if (a == 1u) { wx = 10; wy = 6; }\n"
    "        else return false;\n"
    "    }\n"
    "    int count = wx * wy * int(d + 1u);\n"
    "    dual = int(d);\n"
    "    quant = int(r) - 2 + 6 * int(h);\n"
    "    wbits = ise_bitcount(count, quant);\n"
    "    return count <= 64 && wbits >= 24 && wbits <= 96 && wx <= bw && wy <= bh;\n"
    "}\n"
    "uint hash52(uint p) {\n"
    "    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4;\n"
    "    p ^= p >> 5;  p += p << 16; p ^= p >> 7; p ^= p >> 3;\n"
    "    p ^= p << 6;  p ^= p >> 17;\n"
    "    return p;\n"
    "}\n"
    "void part_init(int seed, int count) {\n"
    "    seed += (count - 1) * 1024;\n"
    "    uint rnum = hash52(uint(seed));\n"
    "    int s[8];\n"
    "    for (int i = 0; i < 8; i++) { int v = int((rnum >> (4 * i)) & 0xFu); s[i] = v * v; }\n"
    "    int sh1, sh2;\n"
    "    if ((seed & 1) != 0) { sh1 = (seed & 2) != 0 ? 4 : 5; sh2 = count == 3 ? 6 : 5; }\n"
    "    else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) != 0 ? 4 : 5; }\n"
    "    for (int i = 0; i < 8; i++) ps_s[i] = s[i] >> ((i & 1) != 0 ? sh2 : sh1);\n"
    "    ps_r[0] = rnum >> 14; ps_r[1] = rnum >> 10; ps_r[2] = rnum >> 6; ps_r[3] = rnum >> 2;\n"
    "    ps_count = count;\n"
    "}\n"
    "int part_of(int x, int y) {\n"
    "    int a = int((uint(ps_s[0] * x + ps_s[1] * y) + ps_r[0]) & 0x3Fu);\n"
    "    int b = int((uint(ps_s[2] * x + ps_s[3] * y) + ps_r[1]) & 0x3Fu);\n"
    "    int c = ps_count >= 3 ? int((uint(ps_s[4] * x + ps_s[5] * y) + ps_r[2]) & 0x3Fu) : 0;\n"
    "    int d = ps_count >= 4 ? int((uint(ps_s[6] * x + ps_s[7] * y) + ps_r[3]) & 0x3Fu) : 0;\n"
    "    if (a >= b && a >= c && a >= d) return 0;\n"
    "    if (b >= c && b >= d) return 1;\n"
    "    if (c >= d) return 2;\n"
    "    return 3;\n"
    "}\n"
    "int V(int k, int i) { return int(cv[min(k + i, 17)]); }\n"
    // endpoints of cv[k..] (8-bit); false for an HDR mode
    "bool unpack(int cem, int k, out ivec4 e0, out ivec4 e1) {\n"
    "    e0 = ivec4(0); e1 = ivec4(0);\n"
    "    if (cem == 0) { int l0 = V(k, 0), l1 = V(k, 1); e0 = ivec4(l0, l0, l0, 255); e1 = ivec4(l1, l1, l1, 255); return true; }\n"
    "    if (cem == 1) {\n"
    "        int l0 = (V(k, 0) >> 2) | (V(k, 1) & 0xC0);\n"
    "        int l1 = min(l0 + (V(k, 1) & 0x3F), 255);\n"
    "        e0 = ivec4(l0, l0, l0, 255); e1 = ivec4(l1, l1, l1, 255);\n"
    "        return true;\n"
    "    }\n"
    "    if (cem == 4) { int l0 = V(k, 0), l1 = V(k, 1); e0 = ivec4(l0, l0, l0, V(k, 2)); e1 = ivec4(l1, l1, l1, V(k, 3)); return true; }\n"
    "    if (cem == 5) {\n"
    "        int l0 = V(k, 0), l1 = V(k, 1), a0 = V(k, 2), a1 = V(k, 3);\n"
    "        l0 |= (l1 & 0x80) << 1; a0 |= (a1 & 0x80) << 1;\n"
    "        l1 &= 0x7F; a1 &= 0x7F;\n"
    "        if ((l1 & 0x40) != 0) l1 -= 0x80;\n"
    "        if ((a1 & 0x40) != 0) a1 -= 0x80;\n"
    "        l0 >>= 1; a0 >>= 1; l1 >>= 1; a1 >>= 1;\n"
    "        l1 = clamp(l1 + l0, 0, 255); a1 = clamp(a1 + a0, 0, 255);\n"
    "        e0 = ivec4(l0, l0, l0, a0); e1 = ivec4(l1, l1, l1, a1);\n"
    "        return true;\n"
    "    }\n"
    "    if (cem == 6 || cem == 10) {\n"
    "        int s = V(k, 3);\n"
    "        ivec3 v = ivec3(V(k, 0), V(k, 1), V(k, 2));\n"
    "        e1 = ivec4(v, 255); e0 = ivec4((v * s) >> 8, 255);\n"
    "        if (cem == 10) { e0.a = V(k, 4); e1.a = V(k, 5); }\n"
    "        return true;\n"
    "    }\n"
    "    if (cem == 8 || cem == 12) {\n"
    "        ivec4 a = ivec4(V(k, 0), V(k, 2), V(k, 4), cem == 12 ? V(k, 6) : 0);\n"
    "        ivec4 b = ivec4(V(k, 1), V(k, 3), V(k, 5), cem == 12 ? V(k, 7) : 0);\n"
    "        if (a.r + a.g + a.b > b.r + b.g + b.b) {\n"
    "            e0 = b; e1 = a;\n"
    "            e0.r = (e0.r + e0.b) >> 1; e0.g = (e0.g + e0.b) >> 1;\n"
    "            e1.r = (e1.r + e1.b) >> 1; e1.g = (e1.g + e1.b) >> 1;\n"
    "        } else { e0 = a; e1 = b; }\n"
    "        if (cem == 8) { e0.a = 255; e1.a = 255; }\n"
    "        return true;\n"
    "    }\n"
    "    if (cem == 9 || cem == 13) {\n"
    "        int n = cem == 13 ? 4 : 3;\n"
    "        ivec4 a = ivec4(0), b = ivec4(0);\n"
    "        for (int c = 0; c < n; c++) {\n"
    "            int base = V(k, 2 * c), delta = V(k, 2 * c + 1);\n"
    "            a[c] = (base >> 1) | (delta & 0x80);\n"
    "            b[c] = (delta >> 1) & 0x3F;\n"
    "            if ((b[c] & 0x20) != 0) b[c] -= 0x40;\n"
    "        }\n"
    "        int sum = b.r + b.g + b.b;\n"
    "        b += a;\n"
    "        if (sum < 0) {\n"
    "            e0 = b; e1 = a;\n"
    "            e0.r = (e0.r + e0.b) >> 1; e0.g = (e0.g + e0.b) >> 1;\n"
    "            e1.r = (e1.r + e1.b) >> 1; e1.g = (e1.g + e1.b) >> 1;\n"
    "        } else { e0 = a; e1 = b; }\n"
    "        e0 = clamp(e0, 0, 255); e1 = clamp(e1, 0, 255);\n"
    "        if (cem == 9) { e0.a = 255; e1.a = 255; }\n"
    "        return true;\n"
    "    }\n"
    "    return false;\n"
    "}\n"
    "void put(int x, int y, uint c) {\n"
    "    int px_x = X0 + x, px_y = Y0 + y;\n"
    "    if (px_x < int(pc.w) && px_y < int(pc.h)) px[pc.out_off + uint(px_y) * pc.w + uint(px_x)] = c;\n"
    "}\n"
    "void fill(uint c) { for (int y = 0; y < BH; y++) for (int x = 0; x < BW; x++) put(x, y, c); }\n"
    "void main() {\n"
    "    uint gid = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationID.x;\n"
    "    if (gid >= pc.nblk) return;\n"
    "    BW = int(pc.bw); BH = int(pc.bh);\n"
    "    X0 = int(gid % pc.bx) * BW; Y0 = int(gid / pc.bx) * BH;\n"
    "    const uint MAGENTA = 0xFFFF00FFu;\n"
    "    bool srgb = pc.srgb != 0u;\n"
    "    uvec4 B = blk[pc.in_off + gid];\n"
    "    uint mode = B.x & 0x7FFu;\n"
    "    if ((mode & 0x1FFu) == 0x1FCu) {\n"  // void extent
    "        if ((mode & 0x200u) != 0u || ((B.x >> 10) & 3u) != 3u) { fill(MAGENTA); return; }\n"
    "        uint s0 = rbits(B, 12, 13), s1 = rbits(B, 25, 13), t0 = rbits(B, 38, 13), t1 = rbits(B, 51, 13);\n"
    "        bool all1 = s0 == 0x1FFFu && s1 == 0x1FFFu && t0 == 0x1FFFu && t1 == 0x1FFFu;\n"
    "        if ((s0 >= s1 || t0 >= t1) && !all1) { fill(MAGENTA); return; }\n"
    "        fill(((B.z >> 8) & 0xFFu) | ((B.z >> 24) << 8) | (((B.w >> 8) & 0xFFu) << 16) | ((B.w >> 24) << 24));\n"
    "        return;\n"
    "    }\n"
    "    int wx, wy, dual, quant, wbits;\n"
    "    if (!block_mode(mode, BW, BH, wx, wy, dual, quant, wbits)) { fill(MAGENTA); return; }\n"
    "    int pcount = int((B.x >> 11) & 3u) + 1;\n"
    "    int wcount = wx * wy;\n"
    // weights, read backwards from the top of the block
    "    uvec4 R = uvec4(bitfieldReverse(B.w), bitfieldReverse(B.z), bitfieldReverse(B.y), bitfieldReverse(B.x));\n"
    "    ise_decode(R, 0, wcount << dual, quant);\n"
    "    if (dual != 0 && pcount == 4) { fill(MAGENTA); return; }\n"
    "    for (int i = 0; i < 80; i++) { grid0[i] = 0u; grid1[i] = 0u; }\n"
    "    int wo = WOFF(quant);\n"
    "    if (dual != 0) {\n"
    "        for (int i = 0; i < wcount; i++) { grid0[i] = WUNQ(wo + int(ise[2 * i])); grid1[i] = WUNQ(wo + int(ise[2 * i + 1])); }\n"
    "    } else {\n"
    "        for (int i = 0; i < wcount; i++) grid0[i] = WUNQ(wo + int(ise[i]));\n"
    "    }\n"
    // colour endpoint modes
    "    int cem[4] = int[4](0, 0, 0, 0);\n"
    "    int below = 128 - wbits, high = 0;\n"
    "    if (pcount == 1) {\n"
    "        cem[0] = int((B.x >> 13) & 0xFu);\n"
    "    } else {\n"
    "        high = 3 * pcount - 4;\n"
    "        below -= high;\n"
    "        uint enc = ((B.x >> 23) & 0x3Fu) | (rbits(B, below, high) << 6);\n"
    "        uint base = enc & 3u;\n"
    "        if (base == 0u) {\n"
    "            for (int i = 0; i < pcount; i++) cem[i] = int((enc >> 2) & 0xFu);\n"
    "            below += high;\n"
    "            high = 0;\n"
    "        } else {\n"
    "            base--;\n"
    "            for (int i = 0; i < pcount; i++) cem[i] = int((((enc >> (2 + i)) & 1u) + base) << 2);\n"
    "            for (int i = 0; i < pcount; i++) cem[i] |= int((enc >> (2 + pcount + 2 * i)) & 3u);\n"
    "        }\n"
    "    }\n"
    "    int ccount = 0;\n"
    "    for (int i = 0; i < pcount; i++) ccount += ((cem[i] >> 2) + 1) * 2;\n"
    "    if (ccount > 18) { fill(MAGENTA); return; }\n"
    "    int cbits = (pcount == 1 ? 111 : 99) - wbits - high - (dual != 0 ? 2 : 0);\n"
    "    if (cbits < 0) cbits = 0;\n"
    "    int cq = -1;\n"
    "    for (int q = 20; q >= 0; q--) if (ise_bitcount(ccount, q) <= cbits) { cq = q; break; }\n"
    "    if (cq < 4) { fill(MAGENTA); return; }\n"
    "    ise_decode(B, pcount == 1 ? 17 : 29, ccount, cq);\n"
    "    for (int i = 0; i < ccount; i++) cv[i] = cq != 20 ? CUNQ(COFF(cq - 4) + int(ise[i])) : ise[i];\n"
    "    int ccs = dual != 0 ? int(rbits(B, below - 2, 2)) : -1;\n"
    // endpoints, expanded to 16 bits
    "    ivec4 ep0[4], ep1[4];\n"
    "    for (int p = 0, k = 0; p < pcount; p++) {\n"
    "        ivec4 e0, e1;\n"
    "        if (!unpack(cem[p], k, e0, e1)) { e0 = ivec4(255, 0, 255, 255); e1 = e0; }\n"
    "        k += ((cem[p] >> 2) + 1) * 2;\n"
    "        if (srgb) { e0 = (e0 << 8) | 0x80; e1 = (e1 << 8) | 0x80; }\n"
    "        else { e0 *= 257; e1 *= 257; }\n"
    "        ep0[p] = e0; ep1[p] = e1;\n"
    "    }\n"
    // weight grid -> texel weights (bilinear infill), per axis
    "    int xi[12], xf[12], yi[12], yf[12];\n"
    "    for (int x = 0; x < BW; x++) {\n"
    "        int v = int(((1024u + uint(BW) / 2u) / uint(BW - 1)) * uint(x) * uint(wx - 1) + 32u) >> 6;\n"
    "        xi[x] = v >> 4; xf[x] = v & 0xF;\n"
    "    }\n"
    "    for (int y = 0; y < BH; y++) {\n"
    "        int v = int(((1024u + uint(BH) / 2u) / uint(BH - 1)) * uint(y) * uint(wy - 1) + 32u) >> 6;\n"
    "        yi[y] = (v >> 4) * wx; yf[y] = v & 0xF;\n"
    "    }\n"
    "    if (pcount > 1) part_init(int((B.x >> 13) & 0x3FFu), pcount);\n"
    "    bool small = BW * BH < 32;\n"
    "    for (int y = 0; y < BH; y++) {\n"
    "        for (int x = 0; x < BW; x++) {\n"
    "            int fs = xf[x], ft = yf[y];\n"
    "            int w11 = (fs * ft + 8) >> 4, w10 = fs - w11, w01 = ft - w11, w00 = 16 - fs - ft + w11;\n"
    "            int i0 = yi[y] + xi[x];\n"
    "            int wv0 = (int(grid0[i0]) * w00 + int(grid0[i0 + 1]) * w10 + int(grid0[i0 + wx]) * w01 +\n"
    "                       int(grid0[i0 + wx + 1]) * w11 + 8) >> 4;\n"
    "            int wv1 = 0;\n"
    "            if (dual != 0)\n"
    "                wv1 = (int(grid1[i0]) * w00 + int(grid1[i0 + 1]) * w10 + int(grid1[i0 + wx]) * w01 +\n"
    "                       int(grid1[i0 + wx + 1]) * w11 + 8) >> 4;\n"
    "            int p = pcount > 1 ? part_of(small ? x << 1 : x, small ? y << 1 : y) : 0;\n"
    "            ivec4 e0 = ep0[p], e1 = ep1[p];\n"
    "            uint c = 0u;\n"
    "            for (int ch = 0; ch < 4; ch++) {\n"
    "                int wc = ch == ccs ? wv1 : wv0;\n"
    "                c |= (uint((e0[ch] * (64 - wc) + e1[ch] * wc + 32) >> 14) & 0xFFu) << (8 * ch);\n"
    "            }\n"
    "            put(x, y, c);\n"
    "        }\n"
    "    }\n"
    "}\n";

static int state;  // 0 not tried yet, 1 ready, -1 unavailable (the CPU decoder then)
static VkPipeline pipe;
static VkPipelineLayout layout;
static VkDescriptorSetLayout set_layout;
static VkBuffer tab_buf;
static VmaAllocation tab_mem;
static VkDescriptorSet cur_set;  // the set vk_astc_gpu_bind made for the span being decoded

typedef struct { uint32_t in_off, out_off, w, h, bw, bh, srgb, bx, nblk; } Push;

bool vk_astc_gpu_ready(void) {
    if (state) return state > 0;
    state = -1;
    if (getenv("LARECOMP_ASTC_CPU")) { LOG("[astc] decoded on the CPU (LARECOMP_ASTC_CPU)\n"); return false; }
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(astc_glsl, STAGE_COMPUTE, "astc_decode", &n);
    if (!spv) { LOG("[astc] compute decoder failed to compile: decoded on the CPU\n"); return false; }
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    VkDescriptorSetLayoutBinding binds[3];
    for (uint32_t i = 0; i < 3; i++)
        binds[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo sl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3,
                                          .pBindings = binds};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &sl, NULL, &set_layout));
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &pipe);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (r != VK_SUCCESS) { LOG("[astc] compute pipeline failed (%d): decoded on the CPU\n", r); return false; }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ASTC_TABLE_WORDS * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &tab_buf, &tab_mem, &info));
    uint32_t tab[ASTC_TABLE_WORDS];
    astc_tables(tab);
    memcpy(info.pMappedData, tab, sizeof tab);
    vmaFlushAllocation(g_vk.vma, tab_mem, 0, VK_WHOLE_SIZE);
    state = 1;
    LOG("[astc] decoded on the GPU (compute)\n");
    return true;
}

uint64_t vk_astc_gpu_align(void) {
    uint64_t a = g_vk.props.limits.minStorageBufferOffsetAlignment;
    return a > 256 ? a : 256;
}

// The span [off, off + size) of buf: blocks and texels, addressed from its start (Push in_off / out_off)
void vk_astc_gpu_bind(VkCommandBuffer cb, VkBuffer buf, uint64_t off, uint64_t size) {
    cur_set = vk_alloc_set(set_layout);
    VkDescriptorBufferInfo span = {buf, off, size}, tabs = {tab_buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = cur_set, .dstBinding = i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = i == 2 ? &tabs : &span};
    vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &cur_set, 0, NULL);
}

// One level: bx blocks a row from byte in_off of the span (16-aligned) -> w x h RGBA8 texels from out_off (4-aligned)
void vk_astc_gpu_dispatch(VkCommandBuffer cb, uint64_t in_off, uint64_t out_off, uint32_t w, uint32_t h, uint32_t bw,
                          uint32_t bh, bool srgb) {
    uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    Push p = {(uint32_t)(in_off / 16), (uint32_t)(out_off / 4), w, h, bw, bh, srgb, bx, bx * by};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    uint32_t groups = (p.nblk + 63) / 64, gx = groups < 32768 ? groups : 32768;  // (a dispatch's x is at most 65535)
    vkCmdDispatch(cb, gx, (groups + gx - 1) / gx, 1);
}

// After the dispatches: their texels are what the copies into the image read
void vk_astc_gpu_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// (test) Blocks decoded by both decoders, at once (a submission and a wait: outside the frame's recording): how
// many texels differ, the first few logged
static uint64_t compare_decoders(const uint8_t *blocks, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb,
                                 const char *what) {
    uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    uint64_t in_sz = (uint64_t)bx * by * 16, out_sz = (uint64_t)w * h * 4;
    uint8_t *cpu = malloc(out_sz), *gpu = malloc(out_sz);
    astc_decode_ldr(blocks, w, h, bw, bh, srgb, cpu);
    VkCommandBuffer cb = vk_upload_cmd();
    VkSpan s = vk_transient(in_sz + out_sz, vk_astc_gpu_align());
    memcpy(s.ptr, blocks, in_sz);
    vk_astc_gpu_bind(cb, s.buf, s.off, in_sz + out_sz);
    vk_astc_gpu_dispatch(cb, 0, in_sz, w, h, bw, bh, srgb);
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_wait_seq(seq);
    memcpy(gpu, (uint8_t *)s.ptr + in_sz, out_sz);
    uint64_t bad = 0;
    for (uint64_t i = 0; i < out_sz / 4; i++) {
        if (!memcmp(cpu + 4 * i, gpu + 4 * i, 4) || bad++ >= 4) continue;
        uint32_t x = (uint32_t)(i % w), y = (uint32_t)(i / w), blk = y / bh * bx + x / bw;
        const uint8_t *b = blocks + 16 * blk;
        LOG("[astc] %s %ux%u%s block %u texel %u,%u: cpu %02x%02x%02x%02x gpu %02x%02x%02x%02x (block "
            "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x)\n", what, bw, bh, srgb ? " srgb" : "", blk,
            x % bw, y % bh, cpu[4 * i], cpu[4 * i + 1], cpu[4 * i + 2], cpu[4 * i + 3], gpu[4 * i], gpu[4 * i + 1],
            gpu[4 * i + 2], gpu[4 * i + 3], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
            b[12], b[13], b[14], b[15]);
    }
    free(cpu), free(gpu);
    return bad;
}

// (test) LARECOMP_ASTC_CHECK=<n>: the first n ASTC levels the game uploads are kept (textures.c) and decoded again by
// both decoders after a present - the game's own textures, as against the selftest's random blocks
typedef struct Check { uint8_t *blocks; uint32_t w, h, bw, bh; bool srgb; struct Check *next; } Check;
static Check *checks;
static long check_left = -1;
void vk_astc_gpu_check_note(const uint8_t *blocks, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb) {
    if (check_left < 0) check_left = getenv("LARECOMP_ASTC_CHECK") ? atol(getenv("LARECOMP_ASTC_CHECK")) : 0;
    if (check_left <= 0) return;
    check_left--;
    Check *c = malloc(sizeof *c);
    uint64_t n = (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    c->blocks = malloc(n);
    memcpy(c->blocks, blocks, n);
    c->w = w, c->h = h, c->bw = bw, c->bh = bh, c->srgb = srgb, c->next = checks;
    checks = c;
}
void vk_astc_gpu_check_run(void) {  // (device.c, after a present)
    static uint64_t levels, texels, bad;
    if (!checks) return;
    while (checks) {
        Check *c = checks;
        checks = c->next;
        bad += compare_decoders(c->blocks, c->w, c->h, c->bw, c->bh, c->srgb, "check");
        levels++, texels += (uint64_t)c->w * c->h;
        free(c->blocks), free(c);
    }
    LOG("[astc] check: %llu levels of the game's textures, %llu of %llu texels differ\n", (unsigned long long)levels,
        (unsigned long long)bad, (unsigned long long)texels);
}

// (test) LARECOMP_ASTC_SELFTEST=<blocks>: that many random blocks of every 2D footprint, linear and sRGB, decoded by
// both decoders; the differences are logged ("[astc] selftest")
void vk_astc_gpu_selftest(void) {
    const char *e = getenv("LARECOMP_ASTC_SELFTEST");
    if (!e || !vk_astc_gpu_ready()) return;
    uint32_t nblk = (uint32_t)atoi(e);
    if (nblk < 64) nblk = 64;
    static const uint8_t fp[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6},
                                      {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    uint64_t total = 0, bad_texels = 0;
    for (int f = 0; f < 14; f++) {
        for (int srgb = 0; srgb < 2; srgb++) {
            uint32_t bw = fp[f][0], bh = fp[f][1], bx = 256, by = (nblk + bx - 1) / bx, w = bx * bw, h = by * bh;
            uint64_t in_sz = (uint64_t)bx * by * 16;
            uint8_t *blocks = malloc(in_sz);
            for (uint64_t i = 0; i < in_sz; i += 8) {  // xorshift64*; every 4th block a void extent's shape
                rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
                uint64_t v = rng * 0x2545F4914F6CDD1Dull;
                memcpy(blocks + i, &v, 8);
            }
            for (uint64_t i = 0; i < in_sz; i += 64) blocks[i] = 0xFC, blocks[i + 1] = (uint8_t)(blocks[i + 1] | 0xD);
            uint32_t legal = 0;  // (blocks that aren't error blocks: what the comparison is worth)
            for (uint32_t i = 0; i < bx * by; i++) {
                uint8_t tmp[12 * 12 * 4];
                legal += astc_decode_block(blocks + 16 * i, bw, bh, srgb, tmp);
            }
            uint64_t bad = compare_decoders(blocks, w, h, bw, bh, srgb, "selftest");
            LOG("[astc] selftest %ux%u%s: %u blocks (%u legal), %llu of %llu texels differ\n", bw, bh, srgb ? " srgb" : "",
                bx * by, legal, (unsigned long long)bad, (unsigned long long)w * h);
            total += (uint64_t)w * h, bad_texels += bad;
            free(blocks);
        }
    }
    LOG("[astc] selftest: %llu of %llu texels differ\n", (unsigned long long)bad_texels, (unsigned long long)total);
}

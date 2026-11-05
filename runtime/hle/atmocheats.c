// Atmosphere cheat files (F1 menu, Cheats tab: "Cheat files"): cheats made for a modded Switch and the emulators, in
// the cheats folder of an emulator mod (runtime/mods.c), named after the game's build id - the first 8 bytes of
// main's NSO build id, as Atmosphere and the emulators name them (any case). Format: "[name]" starts a cheat, "{name}"
// a master cheat (always on), then its opcodes as 8-digit hex words; a name with no opcodes is a heading.
// The cheats run in a VM after Atmosphere's dmnt cheat VM, opcodes 0-A (0x8 key presses: the buttons the game is
// given), as LAS Recomp has none and TotK Recomp (MIT, runtime/cheats.c) does: 12 times a second (as dmnt and yuzu
// run theirs) on the game's thread at a frame's start (cheats_frame), the master cheats first, then those switched
// on, registers cleared before each run. Memory is the guest's at its own addresses: main is at MAIN_BASE (region 0,
// also the address space's start: region 3), the heap at HEAP_BASE (1), the alias region at ALIAS_BASE (2) - what
// GetInfo tells the game. A cheat that writes main's code can't run here: that code is compiled ahead of time (its
// bytes in memory are only the original's); such cheats and those with opcodes the VM doesn't run (0xC.. and 0xF..
// extensions) are listed but can't be switched on. Switches are kept by "<mod>/<cheat name>" (cheats_on).
#include "atmocheats.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../mods.h"
#include "../settings.h"

#define CHEATS_MAX 512
#define RUN_INTERVAL_NS (1000000000ull / 12)
#define OPS_PER_RUN (1 << 20)

typedef struct {
    AtmoCheat info;
    uint32_t *words;
    int n;
} Cheat;

static Cheat g_cheats[CHEATS_MAX];
static int g_n;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // (the list: the menu's thread loads / switches, the game's runs)
static char g_file[32];  // "<build id>.txt"

const char *atmo_cheats_file(void) {
    if (g_file[0] || !g_romfs_dir) return g_file;
    char path[1100];  // (the exefs folder beside the romfs one: exefs/main's NSO header, the build id at 0x40)
    snprintf(path, sizeof path, "%s", g_romfs_dir);
    size_t l = strlen(path);
    if (l < 5 || strcmp(path + l - 5, "romfs")) return g_file;
    memcpy(path + l - 5, "exefs/main", 11);
    FILE *f = fopen(path, "rb");
    uint8_t head[0x48];
    if (f && fread(head, 1, sizeof head, f) == sizeof head && !memcmp(head, "NSO0", 4)) {
        for (int i = 0; i < 8; i++) snprintf(g_file + 2 * i, 3, "%02X", head[0x40 + i]);
        strcat(g_file, ".txt");
    }
    if (f) fclose(f);
    return g_file;
}

static const Module *main_module(void) {
    for (int i = 0; i < g_nmodules; i++) if (!strcmp(g_modules[i].name, "main")) return &g_modules[i];
    return NULL;
}

// ---- opcodes (as dmnt_cheat_vm: the first word's top nibble the type, the next the width T)
static int op_len(const uint32_t *w, int left) {  // words of the opcode at w; 0 when it's cut short
    uint32_t f = w[0];
    int t = f >> 28, width = (f >> 24) & 0xF, n;
    switch (t) {
    case 0: case 1: n = width == 8 ? 4 : 3; break;
    case 2: case 8: n = 1; break;
    case 3: n = width == 0 ? 2 : 1; break;
    case 4: case 6: n = 3; break;
    case 5: case 7: n = 2; break;
    case 9: n = 1 + (((f >> 8) & 0xF) ? (width == 8 ? 2 : 1) : 0); break;
    case 0xA: n = ((f >> 8) & 0xF) == 2 || ((f >> 8) & 0xF) == 4 || ((f >> 8) & 0xF) == 5 ? 2 : 1; break;
    default: n = 1; break;
    }
    return n <= left ? n : 0;
}

static void classify(Cheat *c) {
    if (c->info.kind == ATMO_MASTER && c->n) goto check;
    c->info.kind = c->n ? ATMO_DATA : ATMO_HEADING;
    if (!c->n) return;
check:;
    const Module *m = main_module();
    for (int i = 0; i < c->n;) {
        int len = op_len(c->words + i, c->n - i);
        uint32_t f = c->words[i];
        int t = f >> 28, width = (f >> 24) & 0xF;
        if (!len) {
            snprintf(c->info.note, sizeof c->info.note, "opcode %08X is cut short", f);
            c->info.kind = ATMO_UNSUPPORTED;
            return;
        }
        if (t > 0xA || (t == 5 && ((f >> 12) & 0xF) > 1) || (t == 0xA && ((f >> 8) & 0xF) > 5)) {
            snprintf(c->info.note, sizeof c->info.note, "opcode %08X isn't one this runs", f);
            c->info.kind = ATMO_UNSUPPORTED;
            return;
        }
        if ((t <= 1 || t == 5 || t == 6 || t == 7 || t == 9 || t == 0xA) && width != 1 && width != 2 && width != 4 &&
            width != 8) {
            snprintf(c->info.note, sizeof c->info.note, "opcode %08X has a bad width", f);
            c->info.kind = ATMO_UNSUPPORTED;
            return;
        }
        if (t == 0 && ((f >> 20) & 0xF) == 0 && m) {  // (a store into main: its code?)
            uint64_t off = ((uint64_t)(f & 0xFF) << 32) | c->words[i + 1];
            if (off >= m->text_off && off < m->text_off + m->text_size) {
                snprintf(c->info.note, sizeof c->info.note, "it changes the game's code, which is compiled ahead of "
                         "time here");
                c->info.kind = ATMO_CODE;
                return;
            }
        }
        i += len;
    }
}

// ---- guest memory
static uint64_t region(int m) {
    switch (m) {
    case 0: case 3: return MAIN_BASE;
    case 1: return HEAP_BASE;
    case 2: return ALIAS_BASE;
    default: return 0;
    }
}

static bool mem_ok(uint64_t a, int n) { return a >= GUEST_LO && a + (uint64_t)n <= GUEST_HI && a + (uint64_t)n > a; }

static bool rd(uint64_t a, int width, uint64_t *v) {
    if (!mem_ok(a, width)) return false;
    uint64_t x = 0;
    memcpy(&x, (const void *)(uintptr_t)a, (size_t)width);
    *v = x;
    return true;
}

static void wr(uint64_t a, int width, uint64_t v) {
    const Module *m = main_module();
    if (!mem_ok(a, width)) return;
    if (m && a < m->base + m->text_off + m->text_size && a + (uint64_t)width > m->base + m->text_off) return;  // (code)
    memcpy((void *)(uintptr_t)a, &v, (size_t)width);
}

static uint64_t masked(int width, uint64_t v) { return width >= 8 ? v : v & ((1ull << (8 * width)) - 1); }

static uint64_t arith(int op, uint64_t a, uint64_t b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b < 64 ? a << b : 0;
    case 4: return b < 64 ? a >> b : 0;
    case 5: return a & b;
    case 6: return a | b;
    case 7: return ~a;
    case 8: return a ^ b;
    default: return a;  // (9: move)
    }
}

static bool compare(int cond, uint64_t a, uint64_t b) {
    switch (cond) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    default: return false;
    }
}

// from i, after a condition that failed (else_ok: or an else reached while running), the opcode after the matching
// else (else_ok) or end
static int skip(const uint32_t *w, int n, int i, bool else_ok) {
    int depth = 0;
    while (i < n) {
        int len = op_len(w + i, n - i);
        if (!len) return n;
        uint32_t f = w[i];
        int t = f >> 28;
        i += len;
        if (t == 1 || t == 8) depth++;
        else if (t == 2 && ((f >> 24) & 0xF) == 0 && depth-- == 0) return i;
        else if (t == 2 && depth == 0 && else_ok) return i;
    }
    return n;
}

static uint64_t imm64(const uint32_t *w) { return (uint64_t)w[0] << 32 | w[1]; }

static void run(const Cheat *c, uint64_t keys) {
    uint64_t r[16] = {0};
    int loop[16] = {0};
    const uint32_t *w = c->words;
    int n = c->n, budget = OPS_PER_RUN;
    for (int i = 0; i < n && budget-- > 0;) {
        int len = op_len(w + i, n - i);
        if (!len) return;
        uint32_t f = w[i];
        int t = f >> 28, width = (f >> 24) & 0xF, x = (f >> 20) & 0xF, y = (f >> 16) & 0xF, z = (f >> 12) & 0xF;
        uint64_t rel = ((uint64_t)(f & 0xFF) << 32) | (len > 1 ? w[i + 1] : 0), v;
        switch (t) {
        case 0:  // 0TMR00AA AAAAAAAA V..: a value at region M + A + register R
            wr(region(x) + rel + r[y], width, width == 8 ? imm64(w + i + 2) : w[i + 2]);
            break;
        case 1:  // 1TMC00AA AAAAAAAA V..: if (region M + A) <cond C> V
            if (!rd(region(x) + rel, width, &v) || !compare(y, v, width == 8 ? imm64(w + i + 2) : w[i + 2])) {
                i = skip(w, n, i + len, true);
                continue;
            }
            break;
        case 2:  // 20000000 end; 21000000 else: reached while running, skips to the end
            if (width == 1) {
                i = skip(w, n, i + len, false);
                continue;
            }
            break;
        case 3:  // 300R0000 VVVVVVVV loop start, 310R0000 loop end
            if (width == 0) {
                r[y] = w[i + 1];
                loop[y] = i + len;
            } else if (r[y] && --r[y]) {
                i = loop[y];
                continue;
            }
            break;
        case 4:  // 400R0000 V V: register R = V
            r[y] = imm64(w + i + 1);
            break;
        case 5:  // 5TMRI0AA AAAAAAAA: R = [(I ? R : region M) + A]
            if (!rd(z ? r[y] + rel : region(x) + rel, width, &v)) return;  // (a broken pointer chain: nothing more)
            r[y] = v;
            break;
        case 6: {  // 6T0RIor0 V V: V at R (+ register r); I: R += width
            uint64_t a = r[y] + (((f >> 8) & 0xF) ? r[(f >> 4) & 0xF] : 0);
            wr(a, width, imm64(w + i + 1));
            if (z) r[y] += (uint64_t)width;
            break;
        }
        case 7:  // 7T0RC000 VVVVVVVV: R = R <op C> V
            r[y] = masked(width, arith(z, r[y], w[i + 1]));
            break;
        case 8:  // 8kkkkkkk: if those buttons are held
            if ((keys & (f & 0x0FFFFFFF)) != (f & 0x0FFFFFFF)) {
                i = skip(w, n, i + len, true);
                continue;
            }
            break;
        case 9: {  // 9TCRS0s0 / 9TCRS100 V..: R = S <op C> (register s, or V)
            uint64_t b = (f >> 8) & 0xF ? (width == 8 ? imm64(w + i + 1) : w[i + 1]) : r[(f >> 4) & 0xF];
            r[y] = masked(width, arith(x, r[z], b));
            break;
        }
        case 0xA: {  // ATSRIOxa (aaaaaaaa): register S at an address of kind O
            int o = (f >> 8) & 0xF, m = (f >> 4) & 0xF;
            uint64_t imm = ((uint64_t)(f & 0xF) << 32) | (len > 1 ? w[i + 1] : 0), a;
            switch (o) {
            case 0: a = r[y]; break;
            case 1: a = r[y] + r[m]; break;
            case 2: a = r[y] + imm; break;
            case 3: a = region(m) + r[y]; break;
            case 4: a = region(m) + imm; break;
            default: a = region(m) + r[y] + imm; break;
            }
            wr(a, width, r[x]);
            if (z) r[y] += (uint64_t)width;
            break;
        }
        default:
            return;
        }
        i += len;
    }
}

// ---- the files
static int list_has(const char *list, const char *key) {  // key in a '|' list
    size_t kl = strlen(key);
    for (const char *p = list; *p;) {
        const char *bar = strchr(p, '|');
        size_t l = bar ? (size_t)(bar - p) : strlen(p);
        if (l == kl && !strncmp(p, key, l)) return 1;
        if (!bar) break;
        p = bar + 1;
    }
    return 0;
}

static void parse(char *s, const char *mod) {
    int first = g_n, cap = 0;
    for (char *line = s; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        while (*line == ' ' || *line == '\t') line++;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
        if (len >= 2 && (line[0] == '[' || line[0] == '{') && (line[len - 1] == ']' || line[len - 1] == '}')) {
            if (g_n == CHEATS_MAX) break;
            Cheat *c = &g_cheats[g_n++];
            memset(c, 0, sizeof *c);
            line[len - 1] = 0;
            char *name = line + 1;
            while (*name == ' ' || *name == '\t') name++;
            snprintf(c->info.name, sizeof c->info.name, "%s", name);
            for (char *p = c->info.name; *p; p++) if (*p == '|') *p = '/';  // (the switches' list separator)
            snprintf(c->info.mod, sizeof c->info.mod, "%s", mod);
            c->info.kind = line[0] == '{' ? ATMO_MASTER : ATMO_DATA;
            cap = 0;
        } else if (g_n > first) {
            Cheat *c = &g_cheats[g_n - 1];
            for (char *tok = strtok(line, " \t"); tok; tok = strtok(NULL, " \t")) {
                char *e;
                unsigned long v = strtoul(tok, &e, 16);
                if (e - tok != 8 || *e) continue;
                if (c->n == cap) c->words = realloc(c->words, sizeof *c->words * (size_t)(cap = cap ? cap * 2 : 16));
                c->words[c->n++] = (uint32_t)v;
            }
        }
        line = next;
    }
    for (int i = first; i < g_n; i++) classify(&g_cheats[i]);
}

static bool find_file(const char *dir, const char *name, char *out, size_t cap) {  // name in dir, any case
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        const char *a = e->d_name, *b = name;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
        if (*a || *b) continue;
        snprintf(out, cap, "%s/%s", dir, e->d_name);
        found = true;
    }
    closedir(d);
    return found;
}

void atmo_cheats_load(void) {
    const char *want = atmo_cheats_file();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_n; i++) free(g_cheats[i].words);
    g_n = 0;
    int files = 0;
    for (int m = 0, n = mods_count(); want[0] && m < n; m++) {
        ModInfo mi;
        char path[1400];
        if (!mods_info(m, &mi) || !mi.on || !mi.cheats[0] || !find_file(mi.cheats, want, path, sizeof path)) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *s = sz > 0 ? calloc(1, (size_t)sz + 1) : NULL;
        if (s && fread(s, 1, (size_t)sz, f) == (size_t)sz) {
            parse(s + (sz >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3) ? 3 : 0), mi.name);  // (a UTF-8 BOM)
            files++;
        }
        free(s);
        fclose(f);
    }
    int counts[5] = {0}, on = 0;
    for (int i = 0; i < g_n; i++) {
        char key[460];
        snprintf(key, sizeof key, "%s/%s", g_cheats[i].info.mod, g_cheats[i].info.name);
        g_cheats[i].info.on = g_cheats[i].info.kind == ATMO_MASTER || (g_cheats[i].info.kind == ATMO_DATA &&
                                                                       list_has(g_settings.cheats_on, key));
        counts[g_cheats[i].info.kind]++;
        on += g_cheats[i].info.on && g_cheats[i].info.kind == ATMO_DATA;
    }
    pthread_mutex_unlock(&g_lock);
    if (files)
        LOG("[cheats] %s in %d mod%s: %d cheats (%d on), %d master, %d that change the game's code, %d with opcodes "
            "not run here\n", want, files, files == 1 ? "" : "s", counts[ATMO_DATA], on, counts[ATMO_MASTER],
            counts[ATMO_CODE], counts[ATMO_UNSUPPORTED]);
}

static bool g_loaded;  // (the list read at the game's first frame; again after a reboot: the mods may have changed)
RT_ON_RESET(atmo_cheats_reset) { g_loaded = false; }

int atmo_cheats_count(void) {
    pthread_mutex_lock(&g_lock);
    int n = g_n;
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool atmo_cheat_info(int i, AtmoCheat *out) {
    pthread_mutex_lock(&g_lock);
    bool ok = i >= 0 && i < g_n;
    if (ok) *out = g_cheats[i].info;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void atmo_cheat_set(int i, bool on) {
    pthread_mutex_lock(&g_lock);
    if (i >= 0 && i < g_n && g_cheats[i].info.kind == ATMO_DATA) {
        g_cheats[i].info.on = on;
        char list[sizeof g_settings.cheats_on] = "";  // (the switches: those on now, and those of mods not loaded)
        for (int k = 0; k < g_n; k++) {
            if (!g_cheats[k].info.on || g_cheats[k].info.kind != ATMO_DATA) continue;
            size_t l = strlen(list);
            snprintf(list + l, sizeof list - l, "%s%s/%s", l ? "|" : "", g_cheats[k].info.mod, g_cheats[k].info.name);
        }
        for (const char *p = g_settings.cheats_on; *p;) {  // (keep the others: a mod switched off for now)
            const char *bar = strchr(p, '|');
            size_t l = bar ? (size_t)(bar - p) : strlen(p);
            char key[460];
            snprintf(key, sizeof key, "%.*s", (int)l, p);
            bool listed = false;
            for (int k = 0; k < g_n && !listed; k++) {
                char kk[460];
                snprintf(kk, sizeof kk, "%s/%s", g_cheats[k].info.mod, g_cheats[k].info.name);
                listed = !strcmp(kk, key);
            }
            if (!listed && key[0]) {
                size_t ll = strlen(list);
                snprintf(list + ll, sizeof list - ll, "%s%s", ll ? "|" : "", key);
            }
            if (!bar) break;
            p = bar + 1;
        }
        snprintf(g_settings.cheats_on, sizeof g_settings.cheats_on, "%s", list);
        LOG("[cheats] %s/%s %s\n", g_cheats[i].info.mod, g_cheats[i].info.name, on ? "on" : "off");
    }
    pthread_mutex_unlock(&g_lock);
}

void atmo_cheats_frame(void) {
    if (!g_loaded) g_loaded = true, atmo_cheats_load();  // (the game's first frame: its mods are known by now)
    static uint64_t next;
    uint64_t now = plat_now_ns();
    if (now < next) return;
    next = now + RUN_INTERVAL_NS;
    if (pthread_mutex_trylock(&g_lock)) return;  // (the menu is reloading the list: next time)
    extern _Atomic uint64_t g_input_buttons;  // (runtime/hle/input.c: HidNpadButton bits, as the cheats' key masks)
    uint64_t keys = g_input_buttons;
    for (int pass = 0; pass < 2; pass++)  // (master cheats first)
        for (int i = 0; i < g_n; i++)
            if (g_cheats[i].info.on && (g_cheats[i].info.kind == ATMO_MASTER) == (pass == 0)) run(&g_cheats[i], keys);
    pthread_mutex_unlock(&g_lock);
}

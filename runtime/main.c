// LAS Recomp entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // (main is SDL_main: SDL's Java activity calls it in libmain.so)
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "paths.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"

const char *g_romfs_dir;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start calls x2 right after nnosInitialize and x3 after nninitStartup, just before
// nnMain: SDK constructors run early, the game's run once nninitStartup has set up its heap.
// rtld runs a module's DT_INIT, which registers it (and its TLS template) with musl via
// __nnmusl_init_dso and then runs its init_array; fall back to the array if there is no DT_INIT.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

// Every module but main, last loaded first (the SDK before any subsdk linking against it)
static void rtld_init_sdk(Cpu *c) {
    for (int i = g_nmodules - 1; i > 0; i--) module_init(c, &g_modules[i]);
}
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }
static void rtld_fini(Cpu *c) { (void)c; }  // after nnMain returns: the process ends (finalizers not run)

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
// Android: the app's process is forked from the zygote with its libraries already mapped, and can't re-exec - the
// range is claimed as this library loads, and a collision is reported by main (what holds the range: the log).
static bool g_guest_reserved __attribute__((unused));  // (read on Android only)
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) { g_guest_reserved = true; plat_guest_shared(HEAP_BASE, HEAP_BASE + HEAP_MAX); return; }
#ifdef __ANDROID__
    return;
#endif
    const char *tries = getenv("LARECOMP_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Switching seeds (or vanilla) and going back to the start screen reboot the game inside this process: the window,
// GPU device, settings and shader caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the
// main thread, which then: stops every guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call /
// wait), resets the renderer (textures, address-keyed caches) and all per-boot host state (RT_ON_RESET hooks:
// services, HLE features, seed data, audio), empties the kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// LARECOMP_REBOOT_AT=<frame>:<game | title>[,...] (test): at that presented frame, boot the game again in place, or go
// back to the start screen (with LARECOMP_LAUNCHER_AUTOPLAY it plays again). Frames count on across reboots. Called by
// the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    rt_request_reboot(!strcmp(what, "title") ? RT_REBOOT_TITLE : RT_REBOOT_GAME);
}

#ifdef __APPLE__
extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");
#elif defined(__linux__)
extern const ResetFn __start_resets[], __stop_resets[];  // (ELF: defined by the linker)
#else
// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];
#endif

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    plat_guest_shared(HEAP_BASE, HEAP_BASE + HEAP_MAX);  // (the heap as dma-bufs again: platform.h)
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);

    char why[512];
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    { extern void settings_load(void); settings_load(); }
    kernel_init();
    dispatch_init();
    { extern void game_hooks_init(void); game_hooks_init(); }

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    if (!start) FATAL("nn::init::Start not found\n");
    // SDK 12 nn::init::Start(thread_handle, args_region, init_sdk, init_main, fini): the SDK modules' constructors
    // run after nnosInitialize, main's just before nnMain, fini after nnMain returns
    uint64_t init_sdk = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t init_main = hle_register("<rtld_init_main>", rtld_init_main);
    uint64_t fini = hle_register("<rtld_fini>", rtld_fini);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_sdk, init_main, fini);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n < (int)sizeof buf) { fwrite(buf, 1, (size_t)n, stderr); return; }
    char *big = malloc((size_t)n + 1);  // (rare: a long line)
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    fwrite(big, 1, (size_t)n, stderr);
    free(big);
}

int main(int argc, char **argv) {
    plat_init();
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = "extracted";  // (in the data root)
    bool skip_launcher = getenv("LARECOMP_NO_LAUNCHER") || getenv("LARECOMP_INPUT");  // tests boot straight in
    static char arg_root[1024];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) skip_launcher = true;
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    extern const char *rt_version(void);  // runtime/ui/launcher.mm
#if defined(_WIN32)
    const char *os = "Windows";
#elif defined(__APPLE__)
    const char *os = "macOS";
#elif defined(__ANDROID__)
    const char *os = "Android";
#else
    const char *os = "Linux";
#endif
#if defined(__aarch64__)
    const char *arch = "arm64";
#else
    const char *arch = "x86-64";
#endif
    LOG("[version] PrimeRecomp %s (%s, %s)\n", rt_version(), os, arch);  // (bug reports: tools/log_triage.py)
#ifdef __ANDROID__
    if (!g_guest_reserved) {  // (no re-exec here: what took the range, then out)
        FILE *m = fopen("/proc/self/maps", "r");
        char line[512];
        while (m && fgets(line, sizeof line, m)) {
            unsigned long long a = 0, b = 0;
            if (sscanf(line, "%llx-%llx", &a, &b) == 2 && a < GUEST_HI && b > GUEST_LO) LOG("[guest] taken: %s", line);
        }
        if (m) fclose(m);
        FATAL("guest address range %llx-%llx is not free in this process\n", GUEST_LO, GUEST_HI);
    }
    skip_launcher = true;  // (for now: the start screen's setup builds the game code, which a phone can't)
#endif
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    for (bool title = !skip_launcher;;) {
        if (title) {  // start screen (runtime/ui/launcher.mm): game files setup, seed choice, settings
            extern void launcher_run(const char *root);
            launcher_run(root);
        }
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        title = kind == RT_REBOOT_TITLE;
    }
}

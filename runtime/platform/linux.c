// Linux host layer (runtime/platform.h). x86-64 and aarch64.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"
#include "posix.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the 12 GB window doesn't count against overcommit).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// The guest heap as dma-bufs (exec.c gmem_import_dmabuf). A host-pointer import of it is an amdgpu userptr buffer, and
// the kernel walks every page of every userptr buffer a submission uses, at each submission: ~5 ms for the 3.3 GB heap
// on a Steam Deck, three times a frame. udmabuf makes dma-bufs of memfd pages, which the driver imports as ordinary
// GPU memory. (udmabuf caps each at size_limit_mb, 64 MB by default: the importer asks for pieces.)
// LARECOMP_NO_DMABUF=1: private memory as before.
#ifndef UDMABUF_CREATE
struct udmabuf_create { uint32_t memfd, flags; uint64_t offset, size; };
#define UDMABUF_FLAGS_CLOEXEC 0x01
#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)
#endif
static int g_shared_fd = -1;
static uint64_t g_shared_lo, g_shared_hi;

// Write tracking over the shared memory (plat_written_pages), as Windows' MEM_WRITE_WATCH: userfaultfd's asynchronous
// write protection (the kernel lifts it at the first write, no handler) and PAGEMAP_SCAN to read and re-arm it (both
// Linux 6.7; Wine's GetWriteWatch uses the same). Only over memory the GPU holds as dma-bufs: changing the protection of
// pages under a userptr import makes amdgpu wait for the GPU (its MMU notifier), so a host-pointer import turns it off
// (plat_guest_untrack). LARECOMP_NO_WRITE_WATCH=1: none.
struct uffd_api_ { uint64_t api, features, ioctls; };
struct uffd_range_ { uint64_t start, len; };
struct uffd_register_ { struct uffd_range_ range; uint64_t mode, ioctls; };
#define UFFD_API_ 0xAAull
#define UFFD_USER_MODE_ONLY_ 1
#define UFFD_FEATURE_WP_HUGETLBFS_SHMEM_ (1ull << 12)
#define UFFD_FEATURE_WP_UNPOPULATED_ (1ull << 13)
#define UFFD_FEATURE_WP_ASYNC_ (1ull << 15)
#define UFFDIO_API_ _IOWR(0xAA, 0x3F, struct uffd_api_)
#define UFFDIO_REGISTER_ _IOWR(0xAA, 0x00, struct uffd_register_)
#define UFFDIO_REGISTER_MODE_WP_ (1ull << 1)
struct page_region_ { uint64_t start, end, categories; };
struct pm_scan_arg_ {
    uint64_t size, flags, start, end, walk_end, vec, vec_len, max_pages, category_inverted, category_mask,
        category_anyof_mask, return_mask;
};
#define PAGEMAP_SCAN_ _IOWR('f', 16, struct pm_scan_arg_)
#define PM_SCAN_WP_MATCHING_ (1ull << 0)
#define PM_SCAN_CHECK_WPASYNC_ (1ull << 1)
#define PAGE_IS_WRITTEN_ (1ull << 1)
static int g_uffd = -1, g_pagemap = -1;

static void track_stop(void) {
    if (g_uffd >= 0) close(g_uffd), g_uffd = -1;  // (closing it lifts the protection)
    if (g_pagemap >= 0) close(g_pagemap), g_pagemap = -1;
}

static void track_start(void) {
    track_stop();
    if (getenv("LARECOMP_NO_WRITE_WATCH")) return;
    int u = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY_);
    if (u < 0) return;
    struct uffd_api_ api = {.api = UFFD_API_,
                            .features = UFFD_FEATURE_WP_ASYNC_ | UFFD_FEATURE_WP_HUGETLBFS_SHMEM_ | UFFD_FEATURE_WP_UNPOPULATED_};
    struct uffd_register_ reg = {.range = {g_shared_lo, g_shared_hi - g_shared_lo}, .mode = UFFDIO_REGISTER_MODE_WP_};
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm < 0 || ioctl(u, UFFDIO_API_, &api) != 0 || ioctl(u, UFFDIO_REGISTER_, &reg) != 0) {
        if (pm >= 0) close(pm);
        close(u);
        return;
    }
    g_uffd = u, g_pagemap = pm;  // (nothing protected yet: every page counts as written until its first query)
}

void plat_guest_untrack(void) { track_stop(); }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (g_uffd < 0 || addr < g_shared_lo || addr + size > g_shared_hi || max <= 0) return -1;
    *page = 4096;
    struct page_region_ rg[256];
    struct pm_scan_arg_ a = {.size = sizeof a, .flags = PM_SCAN_WP_MATCHING_ | PM_SCAN_CHECK_WPASYNC_, .start = addr & ~0xFFFull,
                             .end = (addr + size + 0xFFF) & ~0xFFFull, .vec = (uintptr_t)rg, .vec_len = 256,
                             .category_mask = PAGE_IS_WRITTEN_, .return_mask = PAGE_IS_WRITTEN_};
    int n = 0;
    for (;;) {
        a.max_pages = (uint64_t)(max - n);  // (more than fit: the rest stays unprotected, reported next time)
        a.walk_end = 0;
        int r = ioctl(g_pagemap, PAGEMAP_SCAN_, &a);
        if (r < 0) return -1;
        for (int i = 0; i < r; i++)
            for (uint64_t p = rg[i].start; p < rg[i].end && n < max; p += 4096) pages[n++] = p;
        if (n >= max || !a.walk_end || a.walk_end >= a.end) break;
        a.start = a.walk_end;
    }
    return n;
}

bool plat_guest_shared(uint64_t lo, uint64_t hi) {
    track_stop();
    if (g_shared_fd >= 0) close(g_shared_fd), g_shared_fd = -1;  // (after a reset: its pages are gone from the window)
    if (getenv("LARECOMP_NO_DMABUF") || access("/dev/udmabuf", R_OK | W_OK) != 0) return false;
    int fd = memfd_create("primerecomp-heap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return false;
    // (udmabuf wants the size sealed)
    if (ftruncate(fd, (off_t)(hi - lo)) != 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) != 0 ||
        mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) !=
            (void *)(uintptr_t)lo) {
        mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        close(fd);
        return false;
    }
    g_shared_fd = fd, g_shared_lo = lo, g_shared_hi = hi;
    track_start();
    return true;
}

int plat_guest_dmabuf(uint64_t addr, uint64_t size) {
    if (g_shared_fd < 0 || addr < g_shared_lo || addr + size > g_shared_hi || ((addr | size) & 0xFFF) || !size) return -1;
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0) return -1;
    struct udmabuf_create c = {.memfd = (uint32_t)g_shared_fd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = addr - g_shared_lo,
                               .size = size};
    int fd = ioctl(dev, UDMABUF_CREATE, &c);
    close(dev);
    return fd;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}

// ---- process
#if defined(__x86_64__)
// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    fprintf(stderr, "FATAL: PrimeRecomp needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or "
                    "newer).\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

void plat_init(void) {
    // (the AppImage is read-only): $XDG_DATA_HOME/primerecomp, ~/.local/share/primerecomp by default (runtime/paths.c)
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char dir[900];
    if (xdg && *xdg == '/') snprintf(dir, sizeof dir, "%s/primerecomp", xdg);
    else snprintf(dir, sizeof dir, "%s/.local/share/primerecomp", home && *home ? home : "/tmp");
    posix_log_to_file(dir);
    require_cpu();
}

// (inside an AppImage: the binary in its mount, which stays mounted while this process lives - re-exec included)
bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// Our own command line, from /proc/self/cmdline (NUL-separated).
static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

// ---- diagnostics

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

// Static runtime functions come out as the nearest guest function (plat_symbol knows exported ones only: the Makefile
// links with -rdynamic), so backtrace lines also give the offset in the executable (addr2line -fe primerecomp <offset>).
void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (primerecomp+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

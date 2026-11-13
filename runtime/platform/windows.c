// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <timeapi.h>
#include <io.h>

#include "../platform.h"
#include "../launcher_cli.h"

// ---- guest address space
// The window is reserved up front and committed per region (plat_commit from the loader / kernel). Anything
// the guest touches outside a committed region is committed on its first access by the exception handler
// below, so the window behaves like macOS's all-RW reservation. The OS itself (ReadFile into a guest buffer)
// does not fault: such buffers must be inside committed regions.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    if (lazy_commit(fa)) return EXCEPTION_CONTINUE_EXECUTION;
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The window keeps write tracking (MEM_WRITE_WATCH: the Vulkan renderer's vertex / index cache re-copies only pages the
// CPU wrote, runtime/nvn/vulkan/exec.c). Without it (refused): no tracking, the renderer reads guest memory in place.
// (debug) LARECOMP_NO_WRITE_WATCH=1: none, as on Linux / macOS.
static bool g_write_watch;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = getenv("LARECOMP_NO_WRITE_WATCH") ? NULL
            : VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)lo) g_write_watch = true;
    else p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) return false;
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || max <= 0) return -1;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size, (PVOID *)pages, &n, &gran) != 0) return -1;
    *page = gran;
    if (n >= (ULONG_PTR)max) ResetWriteWatch((void *)(uintptr_t)addr, (SIZE_T)size);  // (more than fit: caller takes all)
    return (int)n;
}

// Decommit everything: the range stays reserved, and pages come back zeroed when committed again (lazily).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

bool plat_guest_shared(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; return false; }  // (Linux only)
int plat_guest_dmabuf(uint64_t addr, uint64_t size) { (void)addr; (void)size; return -1; }
void plat_guest_untrack(void) {}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }  // last 0.2 ms: spin (timer slack)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};  // relative, 100 ns units
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

void plat_thread_background(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

// ---- performance cores (hybrid CPUs: P-cores + E-cores), as LAS Recomp's f7731d3
// Windows may run the game's update, graphics and vsync threads on efficiency cores, where they run far slower
// (another Switch recomp measured 40%). Each of those threads opts out of power throttling (EcoQoS) and, on a hybrid
// CPU, selects the CPU sets of the faster cores (EfficiencyClass above the lowest). Selected CPU sets are a soft
// preference, never a hard pin, and only the cores the process affinity allows: when the affinity already excludes
// the E-cores (or the P-cores), nothing is selected, so an affinity-limited run schedules exactly as before. Not
// hybrid: no CPU sets. LARECOMP_PCORE=0: none of this (the process-wide opt-out in plat_init stays).
static INIT_ONCE pcore_once = INIT_ONCE_STATIC_INIT;
static bool pcore_off;
static ULONG *pcore_ids;  // the selected CPU set ids (NULL: no selection)
static ULONG pcore_n;
static const char *pcore_how = "";

static BOOL CALLBACK pcore_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o, (void)p, (void)c;
    const char *e = getenv("LARECOMP_PCORE");
    if (e && *e && atoi(e) == 0) {
        pcore_off = true;
        fprintf(stderr, "[pcore] off (LARECOMP_PCORE=0): default scheduling, thread power throttling unchanged\n");
        return TRUE;
    }
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (!buf || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        free(buf);
        pcore_how = " (CPU sets unavailable)";
        fprintf(stderr, "[pcore] CPU set information unavailable (error %lu): power throttling off only\n", GetLastError());
        return TRUE;
    }
    // The process affinity mask covers its primary group only: filter by it when the system has a single group.
    DWORD_PTR proc = 0, sys = 0;
    bool filter = GetActiveProcessorGroupCount() == 1 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc;
    int lo = 255, hi = -1, all_p = 0, all_e = 0, ok_p = 0, ok_e = 0, n = 0;
    for (ULONG off = 0; off < len;) {  // pass 1: the efficiency classes
        SYSTEM_CPU_SET_INFORMATION *i = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (!i->Size) break;
        if (i->Type == CpuSetInformation) {
            int ec = i->CpuSet.EfficiencyClass;
            if (ec < lo) lo = ec;
            if (ec > hi) hi = ec;
            n++;
        }
        off += i->Size;
    }
    bool hybrid = n && hi > lo;
    if (hybrid) pcore_ids = calloc((size_t)n, sizeof *pcore_ids);
    for (ULONG off = 0; off < len;) {  // pass 2: count, and the P-cores the affinity allows
        SYSTEM_CPU_SET_INFORMATION *i = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (!i->Size) break;
        if (i->Type == CpuSetInformation) {
            bool perf = i->CpuSet.EfficiencyClass > lo;
            bool allowed = !filter || (i->CpuSet.Group == 0 && i->CpuSet.LogicalProcessorIndex < 64 &&
                                       (proc >> i->CpuSet.LogicalProcessorIndex) & 1);
            perf ? all_p++ : all_e++;
            if (allowed) perf ? ok_p++ : ok_e++;
            if (allowed && perf && pcore_ids) pcore_ids[pcore_n++] = i->CpuSet.Id;
        }
        off += i->Size;
    }
    free(buf);
    if (!hybrid) {
        pcore_how = " (not hybrid: no CPU sets)";
        fprintf(stderr, "[pcore] CPU not hybrid (%d logical processors, one efficiency class): power throttling off, no CPU sets\n", n);
    } else if (!ok_p || !ok_e) {
        free(pcore_ids), pcore_ids = NULL, pcore_n = 0;
        pcore_how = " (affinity decides: no CPU sets)";
        fprintf(stderr, "[pcore] hybrid CPU: %d performance + %d efficiency logical processors; the process affinity allows %d + %d: "
            "no CPU sets (the affinity decides), power throttling off\n", all_p, all_e, ok_p, ok_e);
    } else {
        pcore_how = " (performance-core CPU sets)";
        fprintf(stderr, "[pcore] hybrid CPU: %d performance + %d efficiency logical processors%s; game/graphics/vsync threads "
            "prefer the %d performance ones, power throttling off\n", all_p, all_e,
            filter && (ok_p != all_p || ok_e != all_e) ? " (affinity-limited)" : "", ok_p);
    }
    return TRUE;
}

void plat_thread_perf_core(int role) {
    InitOnceExecuteOnce(&pcore_once, pcore_init, NULL, NULL);
    if (pcore_off) return;
    THREAD_POWER_THROTTLING_STATE tp = {
        .Version = THREAD_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED,
        .StateMask = 0,  // (controlled, and off: never EcoQoS)
    };
    bool thr = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tp, sizeof tp) != 0;
    bool sets = !pcore_ids || SetThreadSelectedCpuSets(GetCurrentThread(), pcore_ids, pcore_n) != 0;
    static volatile LONG logged;  // (once per role: a reboot's new game thread applies the same, quietly)
    static const char *const names[] = {"game update", "graphics", "vsync"};
    if (role < 0 || role > 2 || (InterlockedOr(&logged, 1 << role) & (1 << role))) return;
    fprintf(stderr, "[pcore] %s thread: power throttling %s%s%s\n", names[role], thr ? "off" : "unchanged (refused)", pcore_how,
        sets ? "" : " (CPU sets refused)");
}

// ---- process
// Release builds are GUI-subsystem executables (no console window): stdout / stderr go to primerecomp.log next to the
// exe, and a fatal error shows a message box (plat_fatal_notice) instead of vanishing. Processes started by this one
// (the start screen re-executing itself for the game) append to the same log.
static char g_log_path[MAX_PATH];

static void log_to_file_without_console(void) {
    if (GetStdHandle(STD_ERROR_HANDLE) && GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE) return;  // console / pipe
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(g_log_path, sizeof g_log_path, "%sprimerecomp.log", exe);
    bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!append) {  // the run before this one stays readable (started straight from Steam: no .bat rotates it)
        char prev[MAX_PATH], old[MAX_PATH];
        snprintf(prev, sizeof prev, "%sprimerecomp.previous.log", exe);
        MoveFileExA(g_log_path, prev, MOVEFILE_REPLACE_EXISTING);
        // (the log was lasrecomp.log until 2026-10-25: the last one of those becomes the previous log once)
        snprintf(old, sizeof old, "%slasrecomp.log", exe);
        if (MoveFileExA(old, prev, MOVEFILE_REPLACE_EXISTING)) {
            snprintf(old, sizeof old, "%slasrecomp.previous.log", exe);
            DeleteFileA(old);
        }
    }
    if (!freopen(g_log_path, append ? "ab" : "wb", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), _fileno(stdout));
    _putenv("LASRECOMP_LOG_APPEND=1");
}

// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2), or for x86-64-v2 on a CPU
// without those (runtime/gamebuild.c gamebuild_x86_level, issue #28): say so instead of crashing with an illegal
// instruction on a CPU older still.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt") && __builtin_cpu_supports("ssse3")) return;
    MessageBoxA(NULL, "PrimeRecomp needs a CPU with SSE4.2 (Intel Nehalem / 2008 or newer, AMD Bulldozer / 2011 or newer).",
                "PrimeRecomp", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

// Fatal error (rt_fatal_trace): the last FATAL / exception line of the log in a message box, when there is no console.
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1024] = "PrimeRecomp stopped because of an error.";
    FILE *f = fopen(g_log_path, "rb");
    if (f) {
        static char tail[16384];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(tail, 1, sizeof tail - 1, f);
        tail[got] = 0;
        fclose(f);
        char *line = NULL;
        for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
        if (!line) for (char *p = tail; (p = strstr(p, "*** ")) != NULL; p++) line = p;
        if (line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            snprintf(text, sizeof text, "PrimeRecomp stopped because of an error:\n\n%.600s", line);
            // (issue #50: a GPU reset mid-game said "needs Vulkan 1.3" to a player with a Vulkan 1.4 driver - the
            // driver advice is for the start, when no Vulkan 1.3 device or instance could be made)
            size_t l = strlen(text);
            if (strstr(line, "device lost") || strstr(line, "DEVICE_LOST"))
                snprintf(text + l, sizeof text - l, "\n\nThe graphics driver stopped the game's GPU work and reset the "
                         "GPU (\"device lost\"). It isn't a missing feature: please report it with the log below. A newer "
                         "GPU driver can help.");
            else if (strstr(line, "vkCreateInstance") || strstr(line, "Vulkan 1.3") || strstr(line, "vkCreateDevice") ||
                     strstr(line, "no Vulkan device"))
                snprintf(text + l, sizeof text - l, "\n\nPrimeRecomp needs a graphics driver with Vulkan 1.3: install the "
                         "latest driver for your GPU (NVIDIA, AMD or Intel).");
        }
    }
    size_t l = strlen(text);
    snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    MessageBoxA(NULL, text, "PrimeRecomp", MB_ICONERROR | MB_OK);
}

void plat_init(void) {
    if (prime_headless) return;  // stdout must remain the launcher's progress pipe; no dialogs
    log_to_file_without_console();
    require_cpu();
    _fmode = _O_BINARY;  // fopen/open default to binary (the runtime's "r"/"w" files are binary or LF text)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);  // 1 ms Sleep / condition-variable timeouts (default 15.6 ms)
    // Opt out of EcoQoS and of Windows 11 ignoring the timer resolution while the window is occluded.
    PROCESS_POWER_THROTTLING_STATE pt = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
        .StateMask = 0,
    };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// Quote one argument the way CommandLineToArgvW / the CRT parse it back.
static void append_arg(char *out, size_t cap, const char *a) {
    size_t n = strlen(out);
    bool q = !*a || strpbrk(a, " \t\"") != NULL;
    if (n && n + 1 < cap) out[n++] = ' ';
    if (q && n + 1 < cap) out[n++] = '"';
    for (const char *p = a; *p; p++) {
        int bs = 0;
        while (*p == '\\') { bs++; p++; }
        int reps = !*p ? (q ? 2 * bs : bs) : *p == '"' ? 2 * bs + 1 : bs;
        for (int i = 0; i < reps && n + 1 < cap; i++) out[n++] = '\\';
        if (!*p) break;
        if (n + 1 < cap) out[n++] = *p;
    }
    if (q && n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

// No exec() on Windows: start the new process with the same console / standard handles, then exit.
void plat_exec(char *const argv[]) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok;
    fflush(stdout);
    fflush(stderr);
    if (!argv) {
        ok = CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    } else {
        static char cmd[32768];
        cmd[0] = 0;
        for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
        static wchar_t wcmd[32768];
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    if (!ok) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

// ---- diagnostics
// DLL frames are named module+offset; frames in our own executable are left to the caller, which maps
// them to guest functions (runtime/dispatch.c) - mingw builds carry DWARF, not PDB symbols.
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = plat_symbol(pc, &off);
        if (!s) s = rt_host_fn_name(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx primerecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe));
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char what[48];
    snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    // x64 has no link register: report the return address on top of the stack (valid at function entry).
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

void plat_crash_handler_install(void) {
    ULONG guarantee = 64 << 10;  // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    if (g_report_crashes) return;
    g_report_crashes = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// ---- POSIX shims (win_compat.h)
static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            static volatile LONG nlog;
            if (InterlockedIncrement(&nlog) <= 20)
                fprintf(stderr, "[plat] %s fd %d at %llx (%u bytes, buffer %p) failed: error %lu\n", write ? "write" : "read",
                        fd, (unsigned long long)o, (unsigned)chunk, (char *)buf + done, err);
            errno = EIO;
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }

// ---- child processes
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    static __thread char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
    static __thread wchar_t wcmd[32768], wlog[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
    MultiByteToWideChar(CP_UTF8, 0, log_path ? log_path : "NUL", -1, wlog, MAX_PATH);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(wlog, append_log ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, append_log ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return 0;
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = NULL, .hStdOutput = log,
                       .hStdError = log};
    PROCESS_INFORMATION pi;
    // CreateProcess doesn't resolve relative paths written with '/' (e.g. a helper under tools/): pass the program
    // as a full path, like posix_spawn takes it as given
    static __thread wchar_t wprog[MAX_PATH], wfull[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, wprog, MAX_PATH);
    for (wchar_t *p = wprog; *p; p++) if (*p == L'/') *p = L'\\';
    const wchar_t *app = GetFullPathNameW(wprog, MAX_PATH, wfull, NULL) ? wfull : NULL;
    BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (!ok) fprintf(stderr, "[plat] could not start %s (error %lu)\n", argv[0], GetLastError());
    CloseHandle(log);
    if (!ok) return 0;
    // every child in one job that ends with this process (a compiler's own children too: no leftovers on quit)
    static HANDLE job;
    if (!job && (job = CreateJobObjectW(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return (PlatProc)pi.hProcess;
}

void plat_proc_kill(PlatProc p) {  // (what it started ends with this process: the job above)
    TerminateProcess((HANDLE)p, 255);
    WaitForSingleObject((HANDLE)p, INFINITE);
    CloseHandle((HANDLE)p);
}

int plat_proc_wait(PlatProc p, bool block) {
    HANDLE h = (HANDLE)p;
    DWORD w = WaitForSingleObject(h, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT) return -1;
    if (w != WAIT_OBJECT_0) return 255;  // (not a process we can wait for: ended as far as the caller is concerned)
    DWORD code = 255;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code > 255 ? 255 : (int)code;  // (a crash's NTSTATUS, e.g. 0xC0000409, is not "still running" (-1))
}

bool plat_remove_tree(const char *dir) {
    // SHFileOperation wants a double-NUL-terminated path with backslashes
    static __thread wchar_t w[MAX_PATH + 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, MAX_PATH);
    if (n <= 1) return false;
    w[n] = 0;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    SHFILEOPSTRUCTW op = {.wFunc = FO_DELETE, .pFrom = w, .fFlags = FOF_NO_UI};
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// (rename() won't replace a file here: the old one stays until the new one takes its name - no moment without either)
// (write-through: the rename is on the disk when it returns - a save's commit, services/fs.c)
bool plat_replace_file(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// End the process at once, skipping DLL teardown (from an atexit handler, after flushing). Exit status 0.
void plat_terminate(void) { TerminateProcess(GetCurrentProcess(), 0); }

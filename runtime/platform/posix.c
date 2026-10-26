// Host layer parts macOS and Linux share (runtime/platform.h): the log file, the fatal error box, child processes.
// The rest is in runtime/platform/macos.c and linux.c; the Makefile builds this file with either.
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../platform.h"
#include "posix.h"

// ---- log
// Started without a terminal (Finder, a desktop, Steam) stdout / stderr go to primerecomp.log in the data folder. The
// folder is chosen as runtime/paths.c does (rt_paths_init runs later): LARECOMP_DATA_ROOT, the starting folder in dev
// mode (it has extracted/: stderr stays where it goes), else data_dir. The run before stays readable as
// primerecomp.previous.log; processes started by this one (the start screen re-executing itself) append to the same log.
static char g_log_path[1024];

void posix_log_to_file(const char *data_dir) {
    if (isatty(STDERR_FILENO)) return;
    char dir[900];
    const char *env = getenv("LARECOMP_DATA_ROOT");
    struct stat st;
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
    else if (stat("extracted", &st) == 0 && S_ISDIR(st.st_mode)) return;
    else snprintf(dir, sizeof dir, "%s", data_dir);
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    snprintf(g_log_path, sizeof g_log_path, "%s/primerecomp.log", dir);
    bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!append) {
        char prev[1024];
        snprintf(prev, sizeof prev, "%s/primerecomp.previous.log", dir);
        rename(g_log_path, prev);
    }
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("LASRECOMP_LOG_APPEND", "1", 1);
}

// Fatal error without a terminal: the last FATAL / signal line of the log in a message box rather than vanishing
// (as on Windows).
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;  // (a terminal shows it)
    fflush(stderr);
    char text[1200] = "PrimeRecomp stopped because of an error.";
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
#ifdef __linux__
            if (strstr(line, "Vulkan") || strstr(line, "VK_") || strstr(line, "vk")) {
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nPrimeRecomp needs a graphics driver with Vulkan 1.3 (Mesa "
                         "RADV / ANV, or NVIDIA's): update your distribution's graphics drivers.");
            }
#endif
        }
    }
    size_t l = strlen(text);
    snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PrimeRecomp", text, NULL);
}

// ---- process
// End the process at once, skipping the library teardown: as on Windows, the GPU driver's threads crashed in exit()
// while the game's threads still submitted work.
void plat_terminate(void) { _exit(0); }

// Exported symbols only (Linux links with -rdynamic); frames in the generated code are left to the caller, which maps
// them to guest functions (runtime/dispatch.c).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// ---- child processes
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

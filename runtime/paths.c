// Data root and shipped resources (runtime/paths.h).
#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "gamelib.h"
#include "paths.h"
#include "platform.h"

static char g_root[1024], g_start[1024], g_exe_dir[1024];

#ifdef _WIN32
#define SEP "\\"
static bool is_link(const char *p) {  // symlink or junction
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
static bool resolve(const char *p, char *out, size_t n) { return _fullpath(out, p, n) != NULL; }
#define path_eq(a, b) (_stricmp(a, b) == 0)
#else
#define SEP "/"
static bool is_link(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}
static bool resolve(const char *p, char *out, size_t n) {
    char buf[PATH_MAX];
    if (!realpath(p, buf)) return false;
    snprintf(out, n, "%s", buf);
    return true;
}
#define path_eq(a, b) (strcmp(a, b) == 0)
#endif

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/' || *c == '\\') { char s = *c; *c = 0; mkdir(p, 0755); *c = s; }
    mkdir(p, 0755);
}

static bool writable(const char *dir) {
    char t[1100];
    snprintf(t, sizeof t, "%s" SEP ".write_test", dir);
    FILE *f = fopen(t, "wb");
    if (!f) return false;
    fclose(f);
    remove(t);
    return true;
}

static void fail(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PrimeRecomp", msg, NULL);
    exit(1);
}

static bool g_dev;  // the data root is the starting folder because it has extracted/ (a source checkout)
bool rt_dev_mode(void) { return g_dev; }

void rt_paths_init(void) {
    if (!getcwd(g_start, sizeof g_start)) g_start[0] = 0;
    if (plat_exe_path(g_exe_dir, sizeof g_exe_dir)) {
        char *s = strrchr(g_exe_dir, '/'), *b = strrchr(g_exe_dir, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
    }
    char root[1024];
    const char *env = getenv("LARECOMP_DATA_ROOT");
    if (env && *env) snprintf(root, sizeof root, "%s", env);
    else if (is_dir("extracted")) snprintf(root, sizeof root, "%s", g_start), g_dev = true;  // dev / portable: as before
    else {
#ifdef _WIN32
        snprintf(root, sizeof root, "%s", g_exe_dir);
#elif defined(__linux__)
        // (the AppImage is read-only): $XDG_DATA_HOME/primerecomp, ~/.local/share/primerecomp by default
        const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
        if (xdg && *xdg == '/') snprintf(root, sizeof root, "%s/primerecomp", xdg);
        else snprintf(root, sizeof root, "%s/.local/share/primerecomp", home && *home ? home : "/tmp");
#else
        const char *home = getenv("HOME");
        snprintf(root, sizeof root, "%s/Library/Application Support/" RT_BUNDLE_ID, home && *home ? home : "/tmp");
#endif
    }
    mkdirs(root);
    char msg[1400];
    if (!is_dir(root) || !writable(root) || chdir(root) != 0) {
#ifdef _WIN32
        snprintf(msg, sizeof msg, "PrimeRecomp can't write to its folder:\n%s\n\nIt keeps game files, and saves "
                 "next to the program. Move the \"PrimeRecomp\" folder somewhere you can write to (e.g. Documents or "
                 "Desktop, not Program Files) and start it again.", root);
#else
        snprintf(msg, sizeof msg, "PrimeRecomp can't write to its data folder:\n%s", root);
#endif
        fail(msg);
    }
    if (!getcwd(g_root, sizeof g_root)) snprintf(g_root, sizeof g_root, "%s", root);
    fprintf(stderr, "[paths] data: %s\n", g_root);
#ifdef __ANDROID__
    // the APK's assets (tools/android/apk.sh), where rt_resource looks for what ships next to the program: the data
    // root (runtime/platform/linux.c plat_exe_path). SDL opens a relative path from the APK's assets on Android.
    static const char *const assets[] = {"Baloo2-SemiBold.ttf", "OFL.txt", "icon.png"};
    for (size_t i = 0; i < sizeof assets / sizeof *assets; i++) {
        if (exists(assets[i])) continue;
        size_t n = 0;
        void *data = SDL_LoadFile(assets[i], &n);
        FILE *f = data ? fopen(assets[i], "wb") : NULL;
        if (f) { fwrite(data, 1, n, f); fclose(f); }
        SDL_free(data);
        fprintf(stderr, "[paths] asset %s: %s\n", assets[i], f ? "copied" : "not in the APK");
    }
#endif
}

const char *rt_data_root(void) { return g_root; }

bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) {
    char c[4][1100] = {{0}};
    if (name) {
        snprintf(c[0], sizeof c[0], "%s" SEP "%s", g_exe_dir, name);
        snprintf(c[1], sizeof c[1], "%s" SEP ".." SEP "Resources" SEP "%s", g_exe_dir, name);  // .app/Contents/Resources
    }
    if (dev_rel) {
        snprintf(c[2], sizeof c[2], "%s" SEP "%s", g_start, dev_rel);
        snprintf(c[3], sizeof c[3], "%s" SEP ".." SEP "%s", g_exe_dir, dev_rel);  // build/primerecomp -> repo root
    }
    for (int i = 0; i < 4; i++)
        if (c[i][0] && exists(c[i])) return (size_t)snprintf(out, n, "%s", c[i]) < n;  // (as is: a venv python must stay a link)
    return false;
}

// ---- game files
// What the setup (runtime/ui/launcher.mm) creates in extracted/, plus what older versions' extraction and manual
// extractions left there (never read at run time).
static const char *const GAME_FILES[] = {"exefs", "romfs", "romfs.bin", GAMELIB_NAME, GAMELIB_NAME ".id", ".gamebuild",
                                         ".extract", "extract.log", "xci_secure", "nso_raw", "update_pfs", ".DS_Store"};

// game_dir must be <data root>/extracted, a real folder (a dev worktree's symlinked extracted/ is refused)
static bool game_dir_ok(const char *game_dir) {
    char want[1100], got[1100];
    snprintf(want, sizeof want, "%s" SEP "extracted", g_root);
    if (!g_root[0] || is_link(game_dir) || !resolve(game_dir, got, sizeof got)) return false;
    return path_eq(want, got);
}

bool rt_remove_game_file(const char *game_dir, const char *name) {
    bool known = false;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) known |= !strcmp(name, GAME_FILES[i]);
    if (!known || !game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete %s/%s (not app-created game files in the data root)\n", game_dir, name);
        return false;
    }
    char p[1100];
    snprintf(p, sizeof p, "%s" SEP "%s", game_dir, name);
    if (!exists(p) && !is_link(p)) return true;
    if (is_link(p)) {
        fprintf(stderr, "[paths] refusing to delete %s (a link)\n", p);
        return false;
    }
    return is_dir(p) ? plat_remove_tree(p) : remove(p) == 0;  // (plat_remove_tree doesn't follow links inside)
}

bool rt_remove_game_files(const char *game_dir) {
    if (!game_dir_ok(game_dir)) {
        fprintf(stderr, "[paths] refusing to delete game files in %s (not <data root>/extracted)\n", game_dir);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) ok &= rt_remove_game_file(game_dir, GAME_FILES[i]);
    rmdir(game_dir);  // only if empty: anything else in there stays
    return ok;
}

static unsigned long long tree_size(const char *p) {
    struct stat st;
    if (is_link(p) || stat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (unsigned long long)st.st_size;
    unsigned long long n = 0;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[2048];
            snprintf(c, sizeof c, "%s" SEP "%s", p, e->d_name);
            n += tree_size(c);
        }
        closedir(d);
    }
    return n;
}

unsigned long long rt_game_files_size(const char *game_dir) {
    unsigned long long n = 0;
    char p[1100];
    for (size_t i = 0; i < sizeof GAME_FILES / sizeof *GAME_FILES; i++) {
        snprintf(p, sizeof p, "%s" SEP "%s", game_dir, GAME_FILES[i]);
        n += tree_size(p);
    }
    return n;
}

// ---- earlier copies (paths.h)
#define MAX_COPIES 16

static bool copies_file(char *out, size_t n) {  // false: none here (not Windows, or this copy doesn't take part)
#ifdef _WIN32
    const char *la = getenv("LOCALAPPDATA"), *env = getenv("LARECOMP_DATA_ROOT");
    char exe[1100];
    if (!la || !*la || (env && *env) || !resolve(g_exe_dir, exe, sizeof exe) || !path_eq(exe, g_root)) return false;
    snprintf(out, n, "%s\\PrimeRecomp\\copies.txt", la);
    return true;
#else
    (void)out, (void)n;
    return false;
#endif
}

static int read_copies(const char *path, char (*out)[1100], int max) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1100];
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(out[n++], 1100, "%s", line);
    }
    fclose(f);
    return n;
}

static bool real_game_dir(const char *root) {  // <root>/extracted: a folder, not a link
    char p[1100];
    snprintf(p, sizeof p, "%s" SEP "extracted", root);
    return is_dir(p) && !is_link(p);
}

void rt_note_copy(void) {
    char path[1100], list[MAX_COPIES][1100];
    if (!copies_file(path, sizeof path) || !real_game_dir(g_root)) return;
    int n = read_copies(path, list, MAX_COPIES);
    if (n && path_eq(list[0], g_root)) return;  // (noted first already)
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '\\') = 0;
    mkdirs(dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", g_root);
    for (int i = 0, k = 1; i < n && k < MAX_COPIES; i++)  // (copies deleted since are left out)
        if (!path_eq(list[i], g_root) && is_dir(list[i])) fprintf(f, "%s\n", list[i]), k++;
    fclose(f);
}

int rt_earlier_copies(char (*out)[1100], int max) {
    char path[1100], list[MAX_COPIES][1100];
    if (!copies_file(path, sizeof path)) return 0;
    int n = read_copies(path, list, MAX_COPIES), k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (!path_eq(list[i], g_root) && real_game_dir(list[i])) snprintf(out[k++], 1100, "%s", list[i]);
    return k;
}

static bool empty_dir(const char *p) {  // (or not there)
    DIR *d = opendir(p);
    if (!d) return true;
    struct dirent *e;
    bool empty = true;
    while (empty && (e = readdir(d))) empty = !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..");
    closedir(d);
    return empty;
}

static bool copy_plain(const char *src, const char *dst) {  // a file or folder tree, byte by byte (links skipped)
    if (is_link(src)) return true;
    if (is_dir(src)) {
        mkdir(dst, 0755);
        DIR *d = opendir(src);
        if (!d) return false;
        bool ok = true;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char s[2048], t[2048];
            snprintf(s, sizeof s, "%s" SEP "%s", src, e->d_name);
            snprintf(t, sizeof t, "%s" SEP "%s", dst, e->d_name);
            ok &= copy_plain(s, t);
        }
        closedir(d);
        return ok;
    }
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    bool ok = in && out;
    static char buf[1 << 16];
    size_t k;
    while (ok && (k = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, k, out) == k;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    return ok;
}

void rt_copy_saves(const char *from, bool settings) {
    char p[1100];
    snprintf(p, sizeof p, "%s" SEP "save", from);
    if (is_dir(p) && !empty_dir(p) && empty_dir("save"))
        fprintf(stderr, "[paths] saves from %s: %s\n", from, copy_plain(p, "save") ? "copied" : "not all copied");
    // (larecomp.ini: a copy from before 2026-10-25, renamed by settings_load)
    const char *ini[] = {"primerecomp.ini", "larecomp.ini"};
    if (!settings || getenv("LARECOMP_SETTINGS") || exists(ini[0]) || exists(ini[1])) return;
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof p, "%s" SEP "%s", from, ini[i]);
        if (!exists(p)) continue;
        fprintf(stderr, "[paths] settings from %s: %s\n", p, copy_plain(p, ini[i]) ? "copied" : "not copied");
        break;
    }
}

// Window / Dock icon: icon.png shipped next to the exe or in the .app (none for a plain source build). Windows keeps
// the .exe's own icon (SDL's window class takes it): its .ico has the small taskbar sizes drawn without the margin.
void rt_window_icon(SDL_Window *w) {
#ifdef _WIN32
    (void)w;
    return;
#endif
    char p[1100];
    if (!rt_resource("icon.png", NULL, p, sizeof p)) return;
    SDL_Surface *s = SDL_LoadPNG(p);
    if (s) SDL_SetWindowIcon(w, s), SDL_DestroySurface(s);
}

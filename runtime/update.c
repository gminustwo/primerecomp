// In-app updates (issue #36: "will this launcher allow you to update from the gui rather than having to manually
// replace file and folder?"). The start screen asks GitHub for the latest release (Settings > Prelaunch: Check for
// updates, on by default) and, when it's newer, offers it:
// - Windows (a release folder: primegen/ next to the exe): its zip downloaded and unpacked into update/, then the
//   program files swapped in place - the ones there now renamed to <name>.old-update (a running exe and the DLLs it
//   has loaded can be renamed, not overwritten), the new ones moved in - so a Steam or Playnite shortcut keeps
//   working; game files, saves, settings, caches aren't in the zip and stay. Started again: the .old-update files
//   are deleted (update_cleanup).
// - Linux: the AppImage replaced by the new one, at the same path ($APPIMAGE: the Steam shortcut keeps working).
// - macOS and the rest: the release page, to download it from.
// Everything goes through the system's curl (Windows 10+, macOS and Linux have it; the FFmpeg here has no network) and,
// on Windows, its tar (bsdtar: zip files). Nothing is sent but the requests themselves.
// Tests: LARECOMP_UPDATE_FROM=<version>: as if this were that version (also with a source checkout, which doesn't
// check otherwise); LARECOMP_UPDATE_AUTO=1: install what's found at once and exit (no restart).
#include "update.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "paths.h"
#include "platform.h"
#include "settings.h"

extern const char *rt_version(void);  // runtime/ui/launcher.mm

#define LATEST "https://api.github.com/repos/gminustwo/primerecomp/releases/latest"

#if defined(_WIN32)
#define ASSET_END "-windows.zip"
#elif defined(__APPLE__)
#define ASSET_END "-macOS.dmg"
#elif defined(__ANDROID__)
#define ASSET_END "-android.apk"
#elif defined(__aarch64__)
#define ASSET_END "-linux-aarch64.AppImage"
#else
#define ASSET_END "-linux-x86_64.AppImage"
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_state = UPD_IDLE;
static UpdateInfo g_info;
static float g_progress;
static char g_msg[300];
static bool g_checked, g_force_msg;

static void set_state(int s, const char *msg) {
    pthread_mutex_lock(&g_lock);
    g_state = s;
    if (msg) snprintf(g_msg, sizeof g_msg, "%s", msg);
    pthread_mutex_unlock(&g_lock);
    if (msg && *msg) fprintf(stderr, "[update] %s\n", msg);
}

int update_state(UpdateInfo *info, float *progress, char *msg, size_t msg_size) {
    pthread_mutex_lock(&g_lock);
    int s = g_state;
    if (info) *info = g_info;
    if (progress) *progress = g_progress;
    if (msg && msg_size) snprintf(msg, msg_size, "%s", g_msg);
    pthread_mutex_unlock(&g_lock);
    return s;
}

// ------------------------------------------------------------------ helpers
static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool exe_dir(char *out, size_t n) {
    if (!plat_exe_path(out, n)) return false;
    char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > s) s = b;
    if (!s) return false;
    *s = 0;
    return true;
}

static bool find_curl(char *out, size_t n) {
#ifdef _WIN32
    const char *root = getenv("SystemRoot");
    snprintf(out, n, "%s\\System32\\curl.exe", root ? root : "C:\\Windows");
    return exists(out);
#else
    static const char *const c[] = {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", "/opt/homebrew/bin/curl"};
    for (size_t i = 0; i < sizeof c / sizeof *c; i++)
        if (access(c[i], X_OK) == 0) { snprintf(out, n, "%s", c[i]); return true; }
    return false;
#endif
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) free(d), d = NULL;
    fclose(f);
    if (d) d[n] = 0, *len = (size_t)n;
    return d;
}

// "v0.0.8" -> 0, 0, 8 (a playtest label's letters after it don't count): false if it isn't one
static bool parse_version(const char *s, int v[3]) {
    if (*s == 'v' || *s == 'V') s++;
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}
static bool newer(const char *remote, const char *local) {
    int r[3], l[3];
    if (!parse_version(remote, r) || !parse_version(local, l)) return false;
    for (int i = 0; i < 3; i++)
        if (r[i] != l[i]) return r[i] > l[i];
    return false;
}

// The JSON string after "key": (the first from at on), unescaped into out (UTF-8). Returns where its value ends, NULL
// if there's none. (GitHub's release JSON: flat strings are all that's read.)
static const char *json_str(const char *at, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(at, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return NULL;
    p++;
    size_t k = 0;
    while (*p && *p != '"') {
        unsigned c = (unsigned char)*p++;
        if (c == '\\' && *p) {
            char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else if (e == 'u' && p[0] && p[1] && p[2] && p[3]) {
                char h[5] = {p[0], p[1], p[2], p[3], 0};
                c = (unsigned)strtoul(h, NULL, 16);
                p += 4;
                if (c >= 0x80) {  // (UTF-8: two or three bytes)
                    if (c < 0x800) {
                        if (k + 2 < n) out[k++] = (char)(0xC0 | c >> 6), out[k++] = (char)(0x80 | (c & 0x3F));
                    } else if (k + 3 < n) {
                        out[k++] = (char)(0xE0 | c >> 12), out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
                        out[k++] = (char)(0x80 | (c & 0x3F));
                    }
                    continue;
                }
            } else c = (unsigned char)e;
        }
        if (k + 1 < n) out[k++] = (char)c;
    }
    if (n) out[k < n ? k : n - 1] = 0;
    return *p ? p + 1 : p;
}

// The release notes' "What's new" items: the bold titles of their "- **...**" lines
static void read_news(const char *body, UpdateInfo *u) {
    u->nnews = 0;
    for (const char *l = body; *l && u->nnews < 8;) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        if (len > 6 && !strncmp(l, "- **", 4)) {
            const char *t = l + 4, *te = strstr(t, "**");
            if (te && te < l + len) {
                size_t tl = (size_t)(te - t);
                while (tl && (t[tl - 1] == '.' || t[tl - 1] == ' ')) tl--;
                snprintf(u->news[u->nnews++], sizeof u->news[0], "%.*s", (int)tl, t);
            }
        }
        l += len + (e ? 1 : 0);
    }
}

static const char *local_version(void) {
    const char *e = getenv("LARECOMP_UPDATE_FROM");
    return e && *e ? e : rt_version();
}

static bool copy_installs(void) {
#ifdef _WIN32
    char dir[1024], p[1100];
    if (!exe_dir(dir, sizeof dir)) return false;
    snprintf(p, sizeof p, "%s/primegen/primegen.exe", dir);  // (a release folder, not a build from source)
    return exists(p);
#elif defined(__APPLE__) || defined(__ANDROID__)
    return false;
#else
    const char *ai = getenv("APPIMAGE");
    return ai && *ai && exists(ai);
#endif
}

// ------------------------------------------------------------------ the check
static void *check_thread(void *arg) {
    (void)arg;
    char curl[512], out[1100], log[1100];
    if (!find_curl(curl, sizeof curl)) { set_state(g_force_msg ? UPD_FAILED : UPD_NONE, "no curl on this system"); return NULL; }
    snprintf(out, sizeof out, "%s/cache/latest_release.json", rt_data_root());
    snprintf(log, sizeof log, "%s/cache/update.log", rt_data_root());
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/cache", rt_data_root());
    mkdir(dir, 0755);
    remove(out);
    const char *argv[] = {curl, "-fsSL", "--max-time", "20", "-H", "Accept: application/vnd.github+json",
                          "-A", "PrimeRecomp-updater", "-o", out, LATEST, NULL};
    PlatProc p = plat_spawn(argv, log, false);
    int r = p ? plat_proc_wait(p, true) : -1;
    size_t len = 0;
    char *j = r == 0 ? read_all(out, &len) : NULL;
    remove(out);
    if (!j) {
        char m[120];
        snprintf(m, sizeof m, "couldn't reach GitHub (curl %d)", r);
        set_state(g_force_msg ? UPD_FAILED : UPD_NONE, m);
        return NULL;
    }
    UpdateInfo u;
    memset(&u, 0, sizeof u);
    json_str(j, "tag_name", u.tag, sizeof u.tag);
    json_str(j, "html_url", u.page, sizeof u.page);
    json_str(j, "name", u.name, sizeof u.name);  // (the release's: before any asset's)
    // this platform's asset: its download URL ends with the name; its size comes before it in the same object
    for (const char *at = j; (at = strstr(at, "\"browser_download_url\":"));) {
        char url[400];
        const char *end = json_str(at, "browser_download_url", url, sizeof url);
        size_t ul = strlen(url), el = strlen(ASSET_END);
        if (ul > el && !strcmp(url + ul - el, ASSET_END)) {
            snprintf(u.asset, sizeof u.asset, "%s", url);
            for (const char *s = at; s > j; s--)
                if (!strncmp(s, "\"size\":", 7)) { u.size = strtoull(s + 7, NULL, 10); break; }
            break;
        }
        at = end ? end : at + 1;
    }
    {
        char *body = malloc(len + 1);
        if (body && json_str(j, "body", body, len + 1)) read_news(body, &u);
        free(body);
    }
    free(j);
    u.installs = u.asset[0] && copy_installs();
    bool avail = u.tag[0] && newer(u.tag, local_version());
    pthread_mutex_lock(&g_lock);
    g_info = u;
    pthread_mutex_unlock(&g_lock);
    char m[200];
    snprintf(m, sizeof m, avail ? "%s is out (this is %s)%s" : "up to date (%s; this is %s)%s", u.tag, local_version(),
             avail && !u.installs ? ": the release page has it" : "");
    set_state(avail ? UPD_AVAILABLE : UPD_NONE, m);
    if (avail && u.installs && getenv("LARECOMP_UPDATE_AUTO")) update_install_start();
    return NULL;
}

void update_check_start(bool force) {
    if (!force && (g_checked || !g_settings.check_updates)) return;
#ifdef __ANDROID__
    return;
#endif
    if (!force && rt_dev_mode() && !getenv("LARECOMP_UPDATE_FROM")) {  // (a source checkout: no network in tests)
        char dir[1024], p[1100];
        if (!exe_dir(dir, sizeof dir)) return;
        snprintf(p, sizeof p, "%s/primegen", dir);
        if (!exists(p)) return;
    }
    pthread_mutex_lock(&g_lock);
    bool busy = g_state == UPD_CHECKING || g_state == UPD_DOWNLOADING || g_state == UPD_INSTALLING;
    if (!busy) g_state = UPD_CHECKING;
    pthread_mutex_unlock(&g_lock);
    if (busy) return;
    g_checked = true, g_force_msg = force;
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL) == 0) pthread_detach(t);
    else set_state(UPD_IDLE, NULL);
}

// ------------------------------------------------------------------ the install
// curl downloading url to path, the progress from the file's size as it grows
static bool download(const char *url, const char *path, uint64_t size, char *why, size_t n) {
    char curl[512], log[1100];
    if (!find_curl(curl, sizeof curl)) { snprintf(why, n, "no curl on this system"); return false; }
    snprintf(log, sizeof log, "%s/cache/update.log", rt_data_root());
    remove(path);
    const char *argv[] = {curl, "-fsSL", "--retry", "2", "-A", "PrimeRecomp-updater", "-o", path, url, NULL};
    PlatProc p = plat_spawn(argv, log, false);
    if (!p) { snprintf(why, n, "curl didn't start"); return false; }
    int r;
    while ((r = plat_proc_wait(p, false)) == -1) {
        struct stat st;
        if (size && stat(path, &st) == 0) {
            pthread_mutex_lock(&g_lock);
            g_progress = (float)((double)st.st_size / (double)size);
            pthread_mutex_unlock(&g_lock);
        }
        plat_sleep_until_ns(plat_now_ns() + 100000000ull);
    }
    struct stat st;
    if (r != 0 || stat(path, &st) != 0) { snprintf(why, n, "the download failed (curl %d)", r); return false; }
    if (size && (uint64_t)st.st_size != size) {
        snprintf(why, n, "the download is %lld bytes, not %llu", (long long)st.st_size, (unsigned long long)size);
        return false;
    }
    return true;
}

#ifdef _WIN32
// The new folder's entries moved into the copy's: each one there now renamed <name>.old-update first; anything that
// fails puts it all back as it was
static bool swap_in(const char *dir, const char *fresh, char *why, size_t n) {
    char names[128][260];
    int count = 0;
    DIR *d = opendir(fresh);
    if (!d) { snprintf(why, n, "the unpacked update isn't there"); return false; }
    for (struct dirent *e; (e = readdir(d)) && count < 128;)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) snprintf(names[count++], sizeof names[0], "%s", e->d_name);
    closedir(d);
    bool moved_old[128] = {false}, moved_new[128] = {false};
    bool ok = true;
    char a[1400], b[1400];
    for (int i = 0; i < count && ok; i++) {  // the old ones aside
        snprintf(a, sizeof a, "%s/%s", dir, names[i]);
        snprintf(b, sizeof b, "%s/%s.old-update", dir, names[i]);
        if (!exists(a)) continue;
        if (exists(b) && !plat_remove_tree(b) && remove(b) != 0) {  // (an earlier update's, still in use: another name)
            snprintf(b, sizeof b, "%s/%s.old-update-%u", dir, names[i], (unsigned)GetTickCount());
        }
        if (rename(a, b) == 0) moved_old[i] = true;
        else ok = false, snprintf(why, n, "%s couldn't be moved aside (%s)", names[i], strerror(errno));
    }
    for (int i = 0; i < count && ok; i++) {  // the new ones in
        snprintf(a, sizeof a, "%s/%s", fresh, names[i]);
        snprintf(b, sizeof b, "%s/%s", dir, names[i]);
        if (rename(a, b) == 0) moved_new[i] = true;
        else ok = false, snprintf(why, n, "%s couldn't be put in place (%s)", names[i], strerror(errno));
    }
    if (ok) return true;
    for (int i = 0; i < count; i++) {  // (back as it was)
        snprintf(a, sizeof a, "%s/%s", dir, names[i]);
        snprintf(b, sizeof b, "%s/%s", fresh, names[i]);
        if (moved_new[i]) rename(a, b);
        snprintf(b, sizeof b, "%s/%s.old-update", dir, names[i]);
        if (moved_old[i]) rename(b, a);
    }
    return false;
}
#endif

static void *install_thread(void *arg) {
    (void)arg;
    UpdateInfo u;
    update_state(&u, NULL, NULL, 0);
    char why[300] = "";
    bool ok = false;
#ifdef _WIN32
    char dir[1024], upd[1100], zip[1300], staging[1300], fresh[1400], tar[600];
    if (exe_dir(dir, sizeof dir)) {
        snprintf(upd, sizeof upd, "%s/update", dir);
        mkdir(upd, 0755);
        snprintf(zip, sizeof zip, "%s/PrimeRecomp-%s%s", upd, u.tag, ASSET_END);
        snprintf(staging, sizeof staging, "%s/staging", upd);
        set_state(UPD_DOWNLOADING, "");
        if (download(u.asset, zip, u.size, why, sizeof why)) {
            set_state(UPD_INSTALLING, "");
            plat_remove_tree(staging);
            mkdir(staging, 0755);
            const char *root = getenv("SystemRoot");
            snprintf(tar, sizeof tar, "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
            const char *argv[] = {tar, "-xf", zip, "-C", staging, NULL};
            char log[1100];
            snprintf(log, sizeof log, "%s/cache/update.log", rt_data_root());
            PlatProc p = exists(tar) ? plat_spawn(argv, log, true) : 0;
            int r = p ? plat_proc_wait(p, true) : -1;
            snprintf(fresh, sizeof fresh, "%s/PrimeRecomp", staging);
            char exe[1500];
            snprintf(exe, sizeof exe, "%s/primerecomp.exe", fresh);
            if (r != 0 || !exists(exe)) snprintf(why, sizeof why, "the download couldn't be unpacked (tar %d)", r);
            else ok = swap_in(dir, fresh, why, sizeof why);
            plat_remove_tree(staging);
        }
        remove(zip);
        rmdir(upd);
    } else snprintf(why, sizeof why, "where PrimeRecomp is isn't known");
#elif !defined(__APPLE__) && !defined(__ANDROID__)
    const char *ai = getenv("APPIMAGE");
    char tmp[1200];
    if (ai && *ai) {
        snprintf(tmp, sizeof tmp, "%s.update", ai);
        set_state(UPD_DOWNLOADING, "");
        if (download(u.asset, tmp, u.size, why, sizeof why)) {
            set_state(UPD_INSTALLING, "");
            if (chmod(tmp, 0755) != 0 || rename(tmp, ai) != 0)
                snprintf(why, sizeof why, "the AppImage couldn't be replaced (%s)", strerror(errno));
            else ok = true;
        }
        if (!ok) remove(tmp);
    } else snprintf(why, sizeof why, "not an AppImage");
#else
    snprintf(why, sizeof why, "updates are downloaded from the release page here");
#endif
    char m[400];
    snprintf(m, sizeof m, ok ? "%s installed: restarting" : "the update didn't finish: %s", ok ? u.tag : why);
    set_state(ok ? UPD_DONE : UPD_FAILED, m);
    if (getenv("LARECOMP_UPDATE_AUTO")) { fflush(stderr); exit(ok ? 0 : 1); }
    return NULL;
}

void update_install_start(void) {
    pthread_mutex_lock(&g_lock);
    bool go = g_state == UPD_AVAILABLE && g_info.installs;
    if (go) g_state = UPD_DOWNLOADING, g_progress = 0;
    pthread_mutex_unlock(&g_lock);
    if (!go) return;
    pthread_t t;
    if (pthread_create(&t, NULL, install_thread, NULL) == 0) pthread_detach(t);
    else set_state(UPD_AVAILABLE, NULL);
}

void update_restart(void) {
    fflush(stderr);
#ifdef _WIN32
    plat_exec(NULL);  // (the same command line: the exe that's there now)
#elif !defined(__APPLE__) && !defined(__ANDROID__)
    const char *ai = getenv("APPIMAGE");
    if (ai && *ai) {
        char *const argv[] = {(char *)ai, NULL};
        plat_exec(argv);
    }
#endif
}

void update_cleanup(void) {
#ifdef _WIN32
    char dir[1024];
    if (!exe_dir(dir, sizeof dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    int removed = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strstr(e->d_name, ".old-update")) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode) ? plat_remove_tree(p) : remove(p) == 0) removed++;
    }
    closedir(d);
    if (removed) fprintf(stderr, "[update] the previous version's %d files deleted\n", removed);
    char upd[1100];  // (an update cut short: its download, its unpacked files)
    snprintf(upd, sizeof upd, "%s/update", dir);
    if (exists(upd) && plat_remove_tree(upd)) fprintf(stderr, "[update] an unfinished update's files deleted\n");
#elif !defined(__APPLE__) && !defined(__ANDROID__)
    const char *ai = getenv("APPIMAGE");
    if (ai && *ai) {
        char tmp[1200];
        snprintf(tmp, sizeof tmp, "%s.update", ai);
        remove(tmp);
    }
#endif
}

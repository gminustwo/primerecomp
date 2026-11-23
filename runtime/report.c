// Bug reports sent from PrimeRecomp to its GitHub Issues (Settings: the Report a bug tab, runtime/ui/menu.mm).
//
// The player signs in with their own GitHub account through GitHub's device flow - a code shown in the tab, entered at
// github.com/login/device (in the browser PrimeRecomp opens, or on a phone) - so the report is theirs: GitHub tells
// them of the replies, and they can add screenshots to it. The sign-in is kept in memory only, until PrimeRecomp
// closes: nothing of it is written anywhere (the log doesn't name the account either).
//
// A report always carries its log, so a bug report sent from here can't come without one: the log chosen (this run's
// primerecomp.log or the run before's primerecomp.previous.log) goes up first, as a secret gist on the player's
// account, and only then the issue, laid out as the Bug report form lays one out, linking it (the repository's log
// triage reads it from there: tools/log_triage.py). If the issue can't be made, the gist is deleted again. In the log
// sent, the home folder is written ~ (%USERPROFILE% on Windows), so the account's name on this computer isn't in it.
//
// Everything goes through the system's curl, as the updater (runtime/update.c). The sign-in token is handed to it in a
// curl config file in cache/ that's deleted as soon as curl is done, not on its command line (other programs can read
// those).
//
// The GitHub App it signs in to: REPORT_CLIENT_ID below (a client id isn't a secret), registered with Device Flow
// enabled, Issues: read and write on gminustwo/primerecomp (installed there) and Gists: read and write for the user;
// PRIMERECOMP_GITHUB_CLIENT_ID overrides it. Tests: PRIMERECOMP_REPORT_REPO=<owner>/<repo> sends to another repository.
#include "report.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "mods.h"
#include "paths.h"
#include "platform.h"
#include "settings.h"
#include "update.h"

#ifndef REPORT_CLIENT_ID
#define REPORT_CLIENT_ID "Iv23liCxs7FQV9jAtvsd"  // (empty: the tab offers the form on GitHub instead)
#endif
#define REPORT_REPO "gminustwo/primerecomp"
#define LOG_MAX (10 * 1024 * 1024)  // a log sent: at most this much (a bigger one: its start and its end)

extern const char *rt_version(void);     // runtime/ui/launcher.mm
extern const char *vk_gpu_name(void);    // runtime/nvn/vulkan/device.c
extern int texmods_count(void);          // runtime/nvn/vulkan/texmods.c
extern SDL_Window *ui_window(void);      // runtime/nvn/vulkan/device.c

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ReportState g_st;      // (g_lock)
static char g_token[256];     // the sign-in (g_lock): in memory only
static bool g_cancel;         // the sign-in was cancelled (g_lock)
static int g_seq;             // (curl's files: one name each)

const char *report_repo(void) {
    const char *e = getenv("PRIMERECOMP_REPORT_REPO");
    if (!e || !*e) return REPORT_REPO;
    for (const char *c = e; *c; c++)  // (owner/repo characters only: it goes into a URL)
        if (!(SDL_isalnum((unsigned char)*c) || *c == '-' || *c == '_' || *c == '.' || *c == '/')) return REPORT_REPO;
    return e;
}
static const char *client_id(void) {
    const char *e = getenv("PRIMERECOMP_GITHUB_CLIENT_ID");
    return e && *e ? e : REPORT_CLIENT_ID;
}
bool report_available(void) {  // (and curl to send with: Android has none)
    static int curl = -1;
    if (curl < 0) {
        char p[512];
        curl = find_curl(p, sizeof p);
    }
    return client_id()[0] != 0 && curl;
}

void report_state(ReportState *out) {
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}

static void fail(int state, const char *fmt, ...) {
    char m[sizeof g_st.error];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_lock);
    g_st.state = state;
    snprintf(g_st.error, sizeof g_st.error, "%s", m);
    pthread_mutex_unlock(&g_lock);
    if (m[0]) fprintf(stderr, "[report] %s\n", m);
}

// ------------------------------------------------------------------ text helpers
typedef struct { char *s; size_t n, cap; } Str;
static void str_add(Str *b, const char *t, size_t len) {
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + len + 1) c *= 2;
        char *s = realloc(b->s, c);
        if (!s) return;
        b->s = s, b->cap = c;
    }
    memcpy(b->s + b->n, t, len);
    b->n += len;
    b->s[b->n] = 0;
}
static void str_cat(Str *b, const char *t) { str_add(b, t, strlen(t)); }
static void str_printf(Str *b, const char *fmt, ...) {
    char t[2048];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (k > 0) str_add(b, t, (size_t)k < sizeof t ? (size_t)k : sizeof t - 1);
}

// A JSON string's contents: quotes, backslashes and control characters escaped, bytes that aren't UTF-8 made '?'
// (a log can hold a path in the system's code page: GitHub takes UTF-8 only)
static void json_esc(Str *b, const char *t, size_t len) {
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)t[i];
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; str_add(b, e, 2); i++; continue; }
        if (c < 0x20) {
            if (c == '\n') str_cat(b, "\\n");
            else if (c == '\t') str_cat(b, "\\t");
            else if (c != '\r') { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); str_cat(b, e); }
            i++;
            continue;
        }
        if (c < 0x80) { str_add(b, t + i, 1); i++; continue; }
        int n = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
        bool ok = n && i + n <= len;
        for (int k = 1; ok && k < n; k++) ok = ((unsigned char)t[i + k] & 0xC0) == 0x80;
        if (ok) str_add(b, t + i, (size_t)n), i += (size_t)n;
        else str_cat(b, "?"), i++;
    }
}
static void json_field(Str *b, const char *key, const char *val, bool comma) {
    str_printf(b, "%s\"%s\":\"", comma ? "," : "", key);
    json_esc(b, val, strlen(val));
    str_cat(b, "\"");
}

// A JSON number after "key": (0 if there's none)
static long long json_num(const char *j, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = j ? strstr(j, pat) : NULL;
    return p ? strtoll(p + strlen(pat), NULL, 10) : 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = n >= 0 ? malloc((size_t)n + 1) : NULL;
    size_t got = d ? fread(d, 1, (size_t)n, f) : 0;  // (this run's log grows meanwhile: what was there)
    fclose(f);
    if (d) d[got] = 0, *len = got;
    return d;
}

// every `from` in b (case-insensitive: Windows paths) replaced with `to`
static void replace_all(Str *b, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to);
    if (fl < 3 || !b->s) return;  // (a home folder that short would only replace bits of words)
    Str o = {0};
    size_t i = 0;
    while (i < b->n) {
        if (i + fl <= b->n && !SDL_strncasecmp(b->s + i, from, fl)) {
            char next = i + fl < b->n ? b->s[i + fl] : 0;  // (whole folder names: /home/jo isn't /home/joe)
            if (!SDL_isalnum((unsigned char)next) && next != '_' && next != '-' && next != '.') {
                str_add(&o, to, tl);
                i += fl;
                continue;
            }
        }
        str_add(&o, b->s + i, 1);
        i++;
    }
    free(b->s);
    *b = o;
}

// The log as it's sent: the home folder written ~ (%USERPROFILE%), so the account's name isn't in it
static void scrub(Str *b) {
#ifdef _WIN32
    char home[MAX_PATH] = "";
    const char *up = getenv("USERPROFILE");
    if (up && *up) snprintf(home, sizeof home, "%s", up);
    if (home[0]) {
        replace_all(b, home, "%USERPROFILE%");
        for (char *c = home; *c; c++) if (*c == '\\') *c = '/';
        replace_all(b, home, "%USERPROFILE%");
    }
#else
    const char *home = getenv("HOME");
    if (home && strlen(home) > 1) {
        char h[1024];
        snprintf(h, sizeof h, "%s", home);
        size_t l = strlen(h);
        while (l > 1 && h[l - 1] == '/') h[--l] = 0;
        replace_all(b, h, "~");
    }
#endif
}

// ------------------------------------------------------------------ curl
// curl's config file holds one option a line; quoted values take \\ and \"
static void cfg_put(FILE *f, const char *opt, const char *val) {
    fprintf(f, "%s = \"", opt);
    for (const char *c = val; *c; c++) {
        if (*c == '\\' || *c == '"') fputc('\\', f);
        fputc(*c, f);
    }
    fputs("\"\n", f);
}

// One request: method and url, a form (application/x-www-form-urlencoded) or a JSON file as its body, the sign-in
// as its Authorization when auth. The reply's body (malloc'd; NULL: none) and HTTP status (0: no reply).
static bool http(const char *method, const char *url, const char *form, const char *json_path, bool auth, char **reply,
                 int *status) {
    *reply = NULL, *status = 0;
    char curl[512];
    if (!find_curl(curl, sizeof curl)) return false;
    char dir[1100], cfg[1200], out[1200], hdr[1200], log[1200];
    snprintf(dir, sizeof dir, "%s/cache", rt_data_root());
    mkdir(dir, 0755);
    pthread_mutex_lock(&g_lock);
    int n = ++g_seq;
    char token[sizeof g_token];
    snprintf(token, sizeof token, "%s", g_token);
    pthread_mutex_unlock(&g_lock);
    snprintf(cfg, sizeof cfg, "%s/report%d.curl", dir, n);
    snprintf(out, sizeof out, "%s/report%d.reply", dir, n);
    snprintf(hdr, sizeof hdr, "%s/report%d.headers", dir, n);
    snprintf(log, sizeof log, "%s/report.log", dir);
    FILE *f = fopen(cfg, "wb");
    if (!f) return false;
    fputs("silent\nshow-error\nmax-time = 120\n", f);
    cfg_put(f, "user-agent", "PrimeRecomp-report");
    cfg_put(f, "url", url);
    cfg_put(f, "request", method);
    bool api = !strncmp(url, "https://api.github.com/", 23);
    cfg_put(f, "header", api ? "Accept: application/vnd.github+json" : "Accept: application/json");
    if (api) cfg_put(f, "header", "X-GitHub-Api-Version: 2022-11-28");
    if (auth) {
        char h[sizeof token + 32];
        snprintf(h, sizeof h, "Authorization: Bearer %s", token);
        cfg_put(f, "header", h);
    }
    if (form) cfg_put(f, "data", form);
    if (json_path) {
        char d[1300];
        snprintf(d, sizeof d, "@%s", json_path);
        cfg_put(f, "data-binary", d);
        cfg_put(f, "header", "Content-Type: application/json; charset=utf-8");
    }
    cfg_put(f, "output", out);
    cfg_put(f, "dump-header", hdr);
    fclose(f);
    memset(token, 0, sizeof token);
    remove(out), remove(hdr);
    const char *argv[] = {curl, "-K", cfg, NULL};
    PlatProc p = plat_spawn(argv, log, true);
    int r = p ? plat_proc_wait(p, true) : -1;
    remove(cfg);  // (the token: gone at once)
    size_t len = 0;
    char *h = read_file(hdr, &len);
    for (const char *l = h; l && *l;) {  // (the last status line: after a "100 Continue")
        if (!strncmp(l, "HTTP/", 5)) {
            const char *sp = strchr(l, ' ');
            if (sp) *status = atoi(sp + 1);
        }
        const char *e = strchr(l, '\n');
        l = e ? e + 1 : NULL;
    }
    free(h);
    *reply = read_file(out, &len);
    remove(out), remove(hdr);
    if (r != 0 && !*status) fprintf(stderr, "[report] curl %d: no reply from %s\n", r, url);
    return *status > 0;
}

// ------------------------------------------------------------------ sign-in (GitHub's device flow)
static bool cancelled(void) {
    pthread_mutex_lock(&g_lock);
    bool c = g_cancel;
    pthread_mutex_unlock(&g_lock);
    return c;
}

static void *sign_in_thread(void *arg) {
    (void)arg;
    char form[512], *j = NULL;
    int st;
    // (scope: for an OAuth app; a GitHub App goes by its own permissions and ignores it)
    snprintf(form, sizeof form, "client_id=%s&scope=public_repo%%20gist", client_id());
    if (!http("POST", "https://github.com/login/device/code", form, NULL, false, &j, &st) || st != 200 || !j) {
        free(j);
        if (st) fail(REP_SIGNED_OUT, "GitHub didn't give a sign-in code (HTTP %d).", st);
        else fail(REP_SIGNED_OUT, "Couldn't reach GitHub: check the internet connection.");
        return NULL;
    }
    char device[256] = "", code[32] = "", uri[160] = "";
    json_str(j, "device_code", device, sizeof device);
    json_str(j, "user_code", code, sizeof code);
    json_str(j, "verification_uri", uri, sizeof uri);
    long long interval = json_num(j, "interval"), expires = json_num(j, "expires_in");
    char err[200] = "";
    json_str(j, "error_description", err, sizeof err);
    free(j);
    if (!device[0] || !code[0]) {
        fail(REP_SIGNED_OUT, "GitHub didn't give a sign-in code%s%s", err[0] ? ": " : ".", err);
        return NULL;
    }
    if (strncmp(uri, "https://github.com/", 19)) snprintf(uri, sizeof uri, "https://github.com/login/device");
    if (interval < 1) interval = 5;
    time_t until = time(NULL) + (expires > 0 ? expires : 900);
    pthread_mutex_lock(&g_lock);
    g_st.state = REP_AUTHORIZE;
    snprintf(g_st.user_code, sizeof g_st.user_code, "%s", code);
    snprintf(g_st.verify_url, sizeof g_st.verify_url, "%s", uri);
    g_st.code_expires = until;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[report] GitHub sign-in: a code to enter at %s\n", uri);
    snprintf(form, sizeof form, "client_id=%s&device_code=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code",
             client_id(), device);
    for (;;) {
        for (long long w = 0; w < interval * 4; w++) {  // (the wait, a quarter second at a time: Cancel acts at once)
            if (cancelled()) { fail(REP_SIGNED_OUT, ""); return NULL; }
            SDL_Delay(250);
        }
        if (time(NULL) > until) { fail(REP_SIGNED_OUT, "The sign-in code ran out: sign in again."); return NULL; }
        if (!http("POST", "https://github.com/login/oauth/access_token", form, NULL, false, &j, &st) || !j) {
            free(j);  // (a lost connection: keep asking until the code runs out)
            continue;
        }
        char token[sizeof g_token] = "", e[64] = "";
        json_str(j, "access_token", token, sizeof token);
        json_str(j, "error", e, sizeof e);
        free(j);
        if (token[0]) {
            pthread_mutex_lock(&g_lock);
            snprintf(g_token, sizeof g_token, "%s", token);
            pthread_mutex_unlock(&g_lock);
            memset(token, 0, sizeof token);
            break;
        }
        if (!strcmp(e, "authorization_pending")) continue;
        if (!strcmp(e, "slow_down")) { interval += 5; continue; }
        if (!strcmp(e, "expired_token")) { fail(REP_SIGNED_OUT, "The sign-in code ran out: sign in again."); return NULL; }
        if (!strcmp(e, "access_denied")) { fail(REP_SIGNED_OUT, "The sign-in was canceled on GitHub."); return NULL; }
        fail(REP_SIGNED_OUT, "GitHub turned the sign-in down (%s).", e[0] ? e : "no reason given");
        return NULL;
    }
    char login[64] = "";
    if (http("GET", "https://api.github.com/user", NULL, NULL, true, &j, &st) && st == 200 && j)
        json_str(j, "login", login, sizeof login);
    free(j);
    pthread_mutex_lock(&g_lock);
    bool cancel = g_cancel;
    if (cancel) memset(g_token, 0, sizeof g_token);
    else {
        g_st.state = REP_SIGNED_IN;
        snprintf(g_st.login, sizeof g_st.login, "%s", login);
        g_st.user_code[0] = 0, g_st.error[0] = 0;
    }
    pthread_mutex_unlock(&g_lock);
    if (!cancel) fprintf(stderr, "[report] signed in to GitHub\n");  // (not the account's name: the log is shared)
    else fail(REP_SIGNED_OUT, "");
    return NULL;
}

void report_sign_in(void) {
    pthread_mutex_lock(&g_lock);
    bool go = g_st.state == REP_SIGNED_OUT && report_available();
    if (go) g_st.state = REP_CODE, g_st.error[0] = 0, g_cancel = false;
    pthread_mutex_unlock(&g_lock);
    if (!go) return;
    pthread_t t;
    if (pthread_create(&t, NULL, sign_in_thread, NULL) == 0) pthread_detach(t);
    else fail(REP_SIGNED_OUT, "The sign-in couldn't start.");
}

void report_sign_in_cancel(void) {
    pthread_mutex_lock(&g_lock);
    if (g_st.state == REP_CODE || g_st.state == REP_AUTHORIZE) g_cancel = true;
    pthread_mutex_unlock(&g_lock);
}

void report_sign_out(void) {
    pthread_mutex_lock(&g_lock);
    if (g_st.state == REP_SIGNED_IN || g_st.state == REP_SENT) {
        memset(g_token, 0, sizeof g_token);
        g_st.state = REP_SIGNED_OUT, g_st.login[0] = 0, g_st.error[0] = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

void report_sent_done(void) {
    pthread_mutex_lock(&g_lock);
    if (g_st.state == REP_SENT) g_st.state = REP_SIGNED_IN, g_st.issue_url[0] = 0, g_st.issue_number = 0;
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ the logs
int report_logs(ReportLog out[2]) {
    const char *cur = plat_log_path();
    char dir[1024];
    if (cur && *cur) {
        snprintf(dir, sizeof dir, "%s", cur);
        char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
    } else {  // (what this run prints goes elsewhere: the logs of earlier runs, where they're written)
#ifdef _WIN32
        if (!plat_exe_path(dir, sizeof dir)) return 0;
        char *s = strrchr(dir, '\\'), *f = strrchr(dir, '/');
        if (f > s) s = f;
        if (s) *s = 0;
#else
        snprintf(dir, sizeof dir, "%s", rt_data_root());
#endif
    }
    static const char *const names[2] = {"primerecomp.log", "primerecomp.previous.log"};
    int n = 0;
    for (int i = 0; i < 2; i++) {
        ReportLog *l = &out[n];
        snprintf(l->path, sizeof l->path, "%s/%s", dir, names[i]);
        struct stat st;
        if (stat(l->path, &st) != 0 || st.st_size <= 0) continue;
        snprintf(l->name, sizeof l->name, "%s", names[i]);
        l->this_run = i == 0 && cur && *cur;
        l->size = st.st_size, l->modified = st.st_mtime;
        n++;
    }
    return n;
}

// ------------------------------------------------------------------ the report
// A source checkout's build, not a package: started from a folder with extracted/ (rt_dev_mode - as a release folder
// is too, once its game files are set up) and no primegen shipped beside the program (packages bring it; the updater,
// runtime/update.c, tells them apart the same way)
static bool from_source(void) {
    char p[1100];
    return rt_dev_mode() && !rt_resource("primegen", NULL, p, sizeof p);
}

const char *report_platform(void) {  // (the Bug report form's Platform options, word for word)
#if defined(__ANDROID__)
    return "Android (the APK)";
#elif defined(__APPLE__)
    return from_source() ? "Built from source" : "macOS, Apple Silicon (the dmg)";
#elif defined(_WIN32)
    return from_source() ? "Built from source" : "Windows (the release zip)";
#else
    const char *ai = getenv("APPIMAGE");
    if (!ai || !*ai) return "Built from source";
    if (settings_on_steam_deck()) return "Steam Deck (the AppImage)";
#if defined(__aarch64__)
    return "Linux, ARM64 (the AppImage)";
#else
    return "Linux desktop, x86_64 (the AppImage)";
#endif
#endif
}

void report_setup_text(char *buf, size_t n) {
    Str b = {0};
    int np = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&np);
    for (int i = 0; i < np && i < 3; i++) {
        const char *name = SDL_GetGamepadNameForID(ids[i]);
        str_printf(&b, "%s%s", i ? ", " : "Controller: ", name && *name ? name : "a controller");
    }
    SDL_free(ids);
    if (!np) str_cat(&b, g_settings.keyboard_only ? "Keyboard only" : "Keyboard and mouse");
    const char *how = getenv("RECOMPLAUNCHER") ? "RecompLauncher"
                    : getenv("SteamGameId") || getenv("SteamAppId") || getenv("SteamOverlayGameId") ? "Steam" : NULL;
    str_printf(&b, how ? ", started from %s" : ", not started from Steam", how);
    SDL_Window *w = ui_window();
    const SDL_DisplayMode *dm = w ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)) : NULL;
    if (dm) str_printf(&b, "; display %dx%d at %.0f Hz%s", dm->w, dm->h, dm->refresh_rate,
                       w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) ? ", fullscreen" : ", windowed");
    static const char *const ups[] = {"bilinear", "?", "FSR 1", "FSR 3", "FSR 4"};
    int u = g_settings.upscaler >= 0 && g_settings.upscaler <= 4 ? g_settings.upscaler : 1;
    str_printf(&b, "; %d FPS, V-Sync %s, HDR %s, upscaler %s", g_settings.frame_rate, g_settings.vsync ? "on" : "off",
               g_settings.hdr ? "on" : "off", ups[u]);
    int mods = mods_count(), tex = texmods_count();
    if (mods || tex) str_printf(&b, "; %d mods, %d replacement textures", mods, tex);
    else str_cat(&b, "; no mods");
    const char *gpu = vk_gpu_name();
    if (gpu && *gpu) str_printf(&b, "; GPU: %s", gpu);
    snprintf(buf, n, "%s", b.s ? b.s : "");
    free(b.s);
}

static void url_esc(Str *b, const char *t) {
    for (const unsigned char *c = (const unsigned char *)t; *c; c++) {
        if (SDL_isalnum(*c) || *c == '-' || *c == '_' || *c == '.' || *c == '~') str_add(b, (const char *)c, 1);
        else str_printf(b, "%%%02X", *c);
    }
}

void report_form_url(const ReportForm *f, char *buf, size_t n) {
    Str b = {0};
    str_printf(&b, "https://github.com/%s/issues/new?template=bug_report.yml", report_repo());
    const char *v = rt_version();
    const struct { const char *id, *val; } fields[] = {
        {"title", f->title}, {"version", *v == 'v' ? v + 1 : v}, {"what", f->what}, {"where", f->where}, {"setup", f->setup}};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        if (!fields[i].val[0]) continue;
        str_printf(&b, "&%s=", fields[i].id);
        url_esc(&b, fields[i].val);
    }
    snprintf(buf, n, "%s", b.s ? b.s : "");
    free(b.s);
}

static bool write_json(const char *path, const Str *b) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(b->s, 1, b->n, f) == b->n;
    return fclose(f) == 0 && ok;
}

static void set_step(const char *s) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_st.step, sizeof g_st.step, "%s", s);
    pthread_mutex_unlock(&g_lock);
}

// After a failed request: what to tell the player (401: the sign-in has ended - signed out)
static void request_failed(const char *what, int st, const char *reply) {
    char m[200] = "";
    if (reply) json_str(reply, "message", m, sizeof m);
    if (st == 401) {
        pthread_mutex_lock(&g_lock);
        memset(g_token, 0, sizeof g_token);
        g_st.login[0] = 0;
        pthread_mutex_unlock(&g_lock);
        fail(REP_SIGNED_OUT, "Your GitHub sign-in has ended: sign in again, then send it.");
        return;
    }
    if (!st) fail(REP_SIGNED_IN, "Couldn't reach GitHub to %s: check the internet connection and send it again.", what);
    else fail(REP_SIGNED_IN, "GitHub didn't let PrimeRecomp %s (HTTP %d%s%s).", what, st, m[0] ? ": " : "", m);
}

static void *send_thread(void *arg) {
    ReportForm *f = arg;
    char dir[1100], body_path[1200];
    snprintf(dir, sizeof dir, "%s/cache", rt_data_root());
    mkdir(dir, 0755);
    snprintf(body_path, sizeof body_path, "%s/report_request.json", dir);
    char *j = NULL;
    int st = 0;
    // 1. the log, as a secret gist
    set_step("Sending the log...");
    size_t len = 0;
    char *raw = read_file(f->log_path, &len);
    if (!raw || !len) {
        free(raw), free(f);
        fail(REP_SIGNED_IN, "The log couldn't be read, so nothing was sent.");
        return NULL;
    }
    Str log = {0};
    if (len > LOG_MAX) {  // (its start - the build, the PC, the settings - and its end, where the problem is)
        size_t head = LOG_MAX / 5, tail = LOG_MAX - head;
        str_add(&log, raw, head);
        str_printf(&log, "\n[report] ... %llu bytes of the log left out here (it's %llu bytes in all) ...\n",
                   (unsigned long long)(len - head - tail), (unsigned long long)len);
        str_add(&log, raw + len - tail, tail);
    } else str_add(&log, raw, len);
    free(raw);
    scrub(&log);
    const char *slash = strrchr(f->log_path, '/'), *bs = strrchr(f->log_path, '\\');
    const char *name = bs > slash ? bs + 1 : slash ? slash + 1 : f->log_path;
    Str req = {0};
    str_cat(&req, "{\"public\":false");
    json_field(&req, "description", "PrimeRecomp log for a bug report", true);
    str_cat(&req, ",\"files\":{\"");
    json_esc(&req, name, strlen(name));
    str_cat(&req, "\":{\"content\":\"");
    json_esc(&req, log.s ? log.s : "", log.n);
    str_cat(&req, "\"}}}");
    size_t sent_bytes = log.n;
    free(log.s);
    bool wrote = write_json(body_path, &req);
    free(req.s);
    if (!wrote || !http("POST", "https://api.github.com/gists", NULL, body_path, true, &j, &st) || st != 201 || !j) {
        remove(body_path);
        request_failed("upload the log", st, j);
        free(j), free(f);
        return NULL;
    }
    remove(body_path);
    char gist_id[80] = "", raw_url[600] = "", gist_url[300] = "";
    json_str(j, "id", gist_id, sizeof gist_id);
    json_str(j, "html_url", gist_url, sizeof gist_url);
    json_str(j, "raw_url", raw_url, sizeof raw_url);
    free(j), j = NULL;
    if (!raw_url[0]) {
        fail(REP_SIGNED_IN, "GitHub took the log but gave no link to it: nothing else was sent.");
        free(f);
        return NULL;
    }
    // 2. the issue, as the Bug report form lays one out (.github/ISSUE_TEMPLATE/bug_report.yml)
    set_step("Sending the report...");
    const char *v = rt_version();
    Str body = {0};
    str_printf(&body, "### PrimeRecomp version\n\n%s\n\n### Platform\n\n%s\n\n### What happened?\n\n", *v == 'v' ? v + 1 : v,
               report_platform());
    str_cat(&body, f->what);
    str_cat(&body, "\n\n### Where, and how to make it happen\n\n");
    str_cat(&body, f->where);
    str_printf(&body, "\n\n### Log (required)\n\n[%s](%s) - %s, %.0f KB, sent from PrimeRecomp ([its gist](%s); the home "
               "folder is written as ~ or %%USERPROFILE%% in it)\n\n### Your setup\n\n", name, raw_url, f->log_label,
               (double)((sent_bytes + 1023) / 1024), gist_url);
    str_cat(&body, f->setup[0] ? f->setup : "_No response_");
    str_printf(&body, "\n\n### Screenshots, video or a capture\n\n_No response_\n\n### Before you send it\n\n"
               "- [X] I'm playing my own copy of Metroid Prime Remastered for Nintendo Switch, version 1.0.0 (no updates)\n"
               "- [X] The log I attached is from the run where the problem happened, not from a later start of "
               "PrimeRecomp (if I started it again since, I attached primerecomp.previous.log)\n"
               "- [%c] I looked at the open issues, and this one isn't there yet\n\n"
               "<sub>Sent from PrimeRecomp's Report a bug tab.</sub>\n", f->checked_issues ? 'X' : ' ');
    Str issue = {0};
    str_cat(&issue, "{");
    json_field(&issue, "title", f->title, false);
    json_field(&issue, "body", body.s ? body.s : "", true);
    str_cat(&issue, ",\"labels\":[\"bug\"]}");  // (GitHub keeps labels only from those who can push: else dropped)
    free(body.s);
    char url[300];
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/issues", report_repo());
    wrote = write_json(body_path, &issue);
    free(issue.s);
    if (!wrote || !http("POST", url, NULL, body_path, true, &j, &st) || st != 201 || !j) {
        remove(body_path);
        request_failed("open the issue", st, j);
        free(j);
        if (gist_id[0] && st != 401) {  // (no report: the log's gist goes too)
            char gu[200], *r2 = NULL;
            int s2;
            snprintf(gu, sizeof gu, "https://api.github.com/gists/%s", gist_id);
            http("DELETE", gu, NULL, NULL, true, &r2, &s2);
            free(r2);
        }
        free(f);
        return NULL;
    }
    remove(body_path);
    char issue_url[300] = "";
    json_str(j, "html_url", issue_url, sizeof issue_url);
    int number = (int)json_num(j, "number");
    free(j), free(f);
    pthread_mutex_lock(&g_lock);
    g_st.state = REP_SENT;
    g_st.issue_number = number;
    snprintf(g_st.issue_url, sizeof g_st.issue_url, "%s", issue_url);
    g_st.step[0] = 0, g_st.error[0] = 0;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[report] bug report sent: issue #%d, with %s (%.0f KB)\n", number, name,
            (double)((sent_bytes + 1023) / 1024));
    return NULL;
}

bool report_send(const ReportForm *form) {
    if (!form->title[0] || !form->what[0] || !form->where[0] || !form->log_path[0]) return false;
    ReportForm *f = malloc(sizeof *f);
    if (!f) return false;
    *f = *form;
    pthread_mutex_lock(&g_lock);
    bool go = g_st.state == REP_SIGNED_IN;
    if (go) g_st.state = REP_SENDING, g_st.error[0] = 0, g_st.step[0] = 0;
    pthread_mutex_unlock(&g_lock);
    if (!go) { free(f); return false; }
    // (this run's log, sent, ends with this line: tools/log_triage.py reads "... with the log of the run it was sent
    // from" as a run still going, not one that stopped without an exit line)
    fprintf(stderr, "[report] sending a bug report with %s\n", form->log_label);
    pthread_t t;
    if (pthread_create(&t, NULL, send_thread, f) == 0) { pthread_detach(t); return true; }
    free(f);
    fail(REP_SIGNED_IN, "The report couldn't start.");
    return false;
}

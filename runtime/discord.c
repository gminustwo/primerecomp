// Discord status ("Rich Presence", issue #96): the player's Discord profile says they're playing, where Samus is and
// the share of items found ("Phendrana Drifts", "Items 47%"), and for how long, as games with Discord's own SDK do.
//
// Only the Discord app on this computer is talked to, through its local IPC endpoint: \\.\pipe\discord-ipc-N on
// Windows; a Unix socket discord-ipc-N in $XDG_RUNTIME_DIR, $TMPDIR or /tmp on Linux and macOS (also where the Flatpak
// and Snap Discords and Vesktop put theirs). Each frame is an opcode and a length (two little-endian u32s), then JSON:
// a handshake with the application id, then SET_ACTIVITY commands; the app answers PINGs. Nothing goes out to the
// network from here. Without the app running it looks again every 20 s. Gameplay tab "Discord status" turns it off,
// and the status is cleared at once. Not on Android (no Discord app endpoint), nor in scripted test runs.
//
// What the status says (status_now): the start screen, the game's title menus, or in a game the world and the item
// percentage (runtime/hle/playlog.c playlog_presence). Discord takes 5 updates in 20 s: a change goes out at most
// every 5 s, the latest one winning.
#include "discord.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __ANDROID__
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "rt.h"
#include "settings.h"

// PrimeRecomp's application on Discord (discord.com/developers/applications): its name is what the profile says is
// being played, its Rich Presence art asset "icon" the picture. LARECOMP_DISCORD_APP_ID overrides it (tests).
#define DISCORD_APP_ID "1573197983963418655"

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };
enum { ST_OFF, ST_NO_APP_ID, ST_LOOKING, ST_CONNECTED, ST_REFUSED, ST_TEST };
static _Atomic int g_state = ST_OFF;

static void nap(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

// ------------------------------------------------------------------ the connection (one thread does all its I/O)
#ifdef _WIN32
typedef HANDLE Conn;
#define NO_CONN INVALID_HANDLE_VALUE

static Conn conn_open(char *name, size_t cap) {
    for (int i = 0; i < 10; i++) {
        char path[64];
        snprintf(path, sizeof path, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            snprintf(name, cap, "discord-ipc-%d", i);
            return h;
        }
    }
    return NO_CONN;
}

static bool conn_write(Conn c, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(c, b, (DWORD)n, &w, NULL) || !w) return false;
        b += w, n -= w;
    }
    return true;
}

// The bytes waiting, without waiting for any (-1: the pipe is gone). No read of this thread's is ever pending on the
// handle, so PeekNamedPipe doesn't wait behind one.
static long conn_avail(Conn c) {
    DWORD a = 0;
    return PeekNamedPipe(c, NULL, 0, NULL, &a, NULL) ? (long)a : -1;
}

static bool conn_read(Conn c, void *p, size_t n) {  // (n bytes known to be waiting)
    DWORD r = 0;
    return ReadFile(c, p, (DWORD)n, &r, NULL) && r == n;
}

static void conn_close(Conn c) { CloseHandle(c); }
static int self_pid(void) { return (int)GetCurrentProcessId(); }
#else
typedef int Conn;
#define NO_CONN (-1)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // (macOS: SO_NOSIGPIPE on the socket instead)
#endif

static Conn conn_open(char *name, size_t cap) {
    const char *dirs[6];
    int nd = 0;
    static const char *const envs[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    for (size_t i = 0; i < sizeof envs / sizeof *envs; i++) {
        const char *d = getenv(envs[i]);
        if (d && *d) dirs[nd++] = d;
    }
    dirs[nd++] = "/tmp";
    static const char *const subs[] = {"", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/",
                                       "snap.discord/", "snap.discord-canary/", ".flatpak/dev.vencord.Vesktop/xdg-run/"};
    for (int i = 0; i < 10; i++)
        for (int d = 0; d < nd; d++)
            for (size_t s = 0; s < sizeof subs / sizeof *subs; s++) {
                struct sockaddr_un a;
                memset(&a, 0, sizeof a);
                a.sun_family = AF_UNIX;
                int k = snprintf(a.sun_path, sizeof a.sun_path, "%s/%sdiscord-ipc-%d", dirs[d], subs[s], i);
                if (k <= 0 || k >= (int)sizeof a.sun_path || access(a.sun_path, F_OK)) continue;
                int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) return NO_CONN;
#ifdef SO_NOSIGPIPE
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
                    snprintf(name, cap, "%sdiscord-ipc-%d", subs[s], i);  // (not the folder: the log stays private)
                    return fd;
                }
                close(fd);
            }
    return NO_CONN;
}

static bool conn_write(Conn c, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = send(c, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        b += w, n -= (size_t)w;
    }
    return true;
}

static long conn_avail(Conn c) {
    struct pollfd p = {c, POLLIN, 0};
    if (poll(&p, 1, 0) < 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return -1;
    if (!(p.revents & (POLLIN | POLLHUP))) return 0;
    int n = 0;
    if (ioctl(c, FIONREAD, &n) < 0) return -1;
    return n > 0 ? n : -1;  // (readable with nothing in it: closed)
}

static bool conn_read(Conn c, void *p, size_t n) {
    char *b = p;
    while (n) {
        ssize_t r = recv(c, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        b += r, n -= (size_t)r;
    }
    return true;
}

static void conn_close(Conn c) { close(c); }
static int self_pid(void) { return (int)getpid(); }
#endif

// ------------------------------------------------------------------ frames
static Conn g_conn = NO_CONN;
static char g_in[8192];  // the latest frame's JSON (cut short past this; NUL-terminated)

static bool send_frame(uint32_t op, const char *json) {
    size_t n = strlen(json);
    char *f = malloc(8 + n);
    if (!f) return false;
    uint32_t len = (uint32_t)n;  // (little-endian, as every computer this runs on)
    memcpy(f, &op, 4), memcpy(f + 4, &len, 4), memcpy(f + 8, json, n);
    bool ok = conn_write(g_conn, f, 8 + n);
    free(f);
    return ok;
}

// A frame, if one comes within wait_ms: 1 (its opcode in *op, its JSON in g_in), 0 none, -1 the connection is gone
static int recv_frame(uint32_t *op, int wait_ms) {
    long a;
    for (int waited = 0; (a = conn_avail(g_conn)) < 8; waited += 10) {
        if (a < 0) return -1;
        if (waited >= wait_ms) return 0;
        nap(10);
    }
    uint8_t h[8];
    uint32_t len;
    if (!conn_read(g_conn, h, 8)) return -1;
    memcpy(op, h, 4), memcpy(&len, h + 4, 4);
    if (len > (1u << 20)) return -1;
    size_t got = 0;
    for (int waited = 0; got < len;) {  // (the app writes a frame at once: the rest is there or nearly)
        if ((a = conn_avail(g_conn)) < 0) return -1;
        if (!a) {
            if ((waited += 5) > 2000) return -1;
            nap(5);
            continue;
        }
        char tmp[4096];
        size_t k = len - got < (size_t)a ? len - got : (size_t)a;
        if (k > sizeof tmp) k = sizeof tmp;
        if (!conn_read(g_conn, tmp, k)) return -1;
        if (got < sizeof g_in - 1) memcpy(g_in + got, tmp, got + k < sizeof g_in - 1 ? k : sizeof g_in - 1 - got);
        got += k;
    }
    g_in[len < sizeof g_in - 1 ? len : sizeof g_in - 1] = 0;
    return 1;
}

// A JSON string's text from g_in ("message":"..."), for the log
static void json_field(const char *key, char *out, size_t cap) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(g_in, pat);
    size_t k = 0;
    if (p)
        for (p += strlen(pat); *p && *p != '"' && k + 1 < cap; p++) {
            if (*p == '\\' && p[1]) p++;
            out[k++] = *p;
        }
    out[k] = 0;
}

// s as the inside of a JSON string
static void json_escape(char *out, size_t cap, const char *s) {
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') out[k++] = '\\', out[k++] = (char)ch;
        else if (ch < 0x20) k += (size_t)snprintf(out + k, cap - k, "\\u%04x", ch);
        else out[k++] = (char)ch;
    }
    out[k] = 0;
}

// ------------------------------------------------------------------ the status
extern int g_start_screen;  // (runtime/hle/game.c)
bool playlog_presence(char *world, size_t cap, int *items_pct);  // (runtime/hle/playlog.c)
const char *rt_version(void);  // (runtime/ui/launcher.mm)

static void status_now(char *details, char *state, size_t cap) {
    char world[96];
    int pct = -1;
    state[0] = 0;
    if (g_start_screen) snprintf(details, cap, "Start screen");
    else if (playlog_presence(world, sizeof world, &pct)) {
        snprintf(details, cap, "%s", world);
        if (pct >= 0) snprintf(state, cap, "Items %d%%", pct);
    } else snprintf(details, cap, "Main menu");
}

static bool send_status(const char *details, const char *state, long long start_ms, int nonce) {
    char d[256], s[256], v[64], j[1024];
    json_escape(d, sizeof d, details);
    json_escape(s, sizeof s, state);
    json_escape(v, sizeof v, rt_version());
    snprintf(j, sizeof j,
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{\"details\":\"%s\",%s%s%s"
             "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"icon\",\"large_text\":\"Metroid Prime "
             "Remastered - PrimeRecomp %s\"}}},\"nonce\":\"%d\"}",
             self_pid(), d, *s ? "\"state\":\"" : "", s, *s ? "\"," : "", start_ms, v, nonce);
    return send_frame(OP_FRAME, j);
}

static void drop(void) {
    if (g_conn != NO_CONN) conn_close(g_conn);
    g_conn = NO_CONN;
}

static const char *app_id(void) {
    const char *e = getenv("LARECOMP_DISCORD_APP_ID");
    return e && *e ? e : DISCORD_APP_ID;
}

static void *discord_thread(void *arg) {
    (void)arg;
    const char *id = app_id();
    char name[96] = "", sent[2][128] = {"", ""};
    bool sent_any = false, said_missing = false, said_refused = false;
    uint64_t next_try = 0, last_send = 0;
    int nonce = 0, errors = 0;
    long long start_ms = (long long)time(NULL) * 1000;  // (the profile's "elapsed": since PrimeRecomp started)
    for (;;) {
        uint64_t now = plat_now_ns();
        if (!*id || !g_settings.discord) {
            if (g_conn != NO_CONN) {  // (turned off: cleared at once)
                char j[128];
                snprintf(j, sizeof j, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d},\"nonce\":\"%d\"}", self_pid(),
                         ++nonce);
                send_frame(OP_FRAME, j);
                drop();
                LOG("[discord] off: the status is cleared\n");
            }
            g_state = *id ? ST_OFF : ST_NO_APP_ID;
            next_try = 0;
            nap(500);
            continue;
        }
        if (g_conn == NO_CONN) {
            if (g_state != ST_REFUSED) g_state = ST_LOOKING;
            if (now < next_try) { nap(500); continue; }
            next_try = now + 20000000000ull;
            if ((g_conn = conn_open(name, sizeof name)) == NO_CONN) {
                if (!said_missing) LOG("[discord] the Discord app isn't running: looking for it every 20 s\n");
                said_missing = true;
                continue;
            }
            char j[160];
            uint32_t op = 0;
            snprintf(j, sizeof j, "{\"v\":1,\"client_id\":\"%s\"}", id);
            int r = send_frame(OP_HANDSHAKE, j) ? recv_frame(&op, 5000) : -1;
            if (r > 0 && op == OP_FRAME && strstr(g_in, "\"READY\"")) {  // (its user isn't logged: privacy)
                LOG("[discord] connected to the Discord app (%s)\n", name);
                g_state = ST_CONNECTED;
                said_missing = said_refused = sent_any = false;
                errors = 0;
            } else {
                char msg[160];
                json_field("message", msg, sizeof msg);
                if (r > 0 && op == OP_CLOSE) {
                    if (!said_refused) LOG("[discord] the Discord app said no: %s\n", msg[0] ? msg : "(no reason)");
                    said_refused = true;
                    g_state = ST_REFUSED;
                } else LOG("[discord] no answer from the Discord app (%s)\n", name);
                drop();
                continue;
            }
        }
        // what came in: pings to answer, errors, the app closing
        uint32_t op = 0;
        int r;
        while ((r = recv_frame(&op, 0)) > 0) {
            if (op == OP_PING) send_frame(OP_PONG, g_in);
            else if (op == OP_CLOSE) { r = -1; break; }
            else if (op == OP_FRAME && strstr(g_in, "\"evt\":\"ERROR\"") && errors < 20) {
                char msg[160];
                json_field("message", msg, sizeof msg);
                LOG("[discord] the Discord app: %s%s\n", msg, ++errors == 20 ? " (no more of these)" : "");
            }
        }
        if (r < 0) {
            LOG("[discord] the Discord app closed the connection: looking for it every 20 s\n");
            drop();
            said_missing = true;
            next_try = now + 20000000000ull;
            continue;
        }
        char details[128], state[128];
        status_now(details, state, sizeof details);
        if ((!sent_any || strcmp(details, sent[0]) || strcmp(state, sent[1])) &&
            (!sent_any || now - last_send >= 5000000000ull)) {
            if (!send_status(details, state, start_ms, ++nonce)) {
                LOG("[discord] the Discord app closed the connection: looking for it every 20 s\n");
                drop();
                continue;
            }
            last_send = now, sent_any = true;
            snprintf(sent[0], sizeof sent[0], "%s", details);
            snprintf(sent[1], sizeof sent[1], "%s", state);
            LOG("[discord] status: %s%s%s\n", details, *state ? " | " : "", state);
        }
        nap(250);
    }
    return NULL;
}

void discord_start(void) {
    static bool started;
    if (started) return;
    started = true;
    // (scripted test runs leave the player's Discord alone, unless a test names an application)
    if ((getenv("LARECOMP_INPUT") || getenv("LARECOMP_HIDDEN")) && !getenv("LARECOMP_DISCORD_APP_ID")) {
        g_state = ST_TEST;
        return;
    }
    if (!*app_id()) LOG("[discord] no Discord application in this build: no Discord status\n");
    pthread_t t;
    if (pthread_create(&t, NULL, discord_thread, NULL) == 0) pthread_detach(t);
}

const char *discord_status_text(void) {
    switch (g_state) {
    case ST_CONNECTED: return "Shown on your Discord profile now.";
    case ST_LOOKING: return "Discord isn't running on this computer: it's shown once Discord starts.";
    case ST_REFUSED: return "The Discord app turned PrimeRecomp away (primerecomp.log has its reason).";
    case ST_NO_APP_ID: return "Not available in this build.";
    case ST_TEST: return "Off in test runs.";
    default: return "Off.";
    }
}
#else  // (Android: no Discord app endpoint to talk to)
void discord_start(void) {}
const char *discord_status_text(void) { return "Not on Android."; }
#endif

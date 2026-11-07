// Patriot sounds (Gameplay tab, with Suit colors on Patriot): the player's own sound files played at moments of the
// game - one as a save is loaded (or a new game begins), one as Samus fires a missile. Nothing ships: the files are
// the player's, copied into the data folder by the menu's file pickers as patriot_load.<ext> and
// patriot_missile.<ext> (MP3, WAV, FLAC and Ogg Vorbis by runtime/hle/audiodec.c; M4A / AAC by FFmpeg).
//
// The moments, read from the game's own state each frame (runtime/hle/cheats.c cheats_frame, at CStateManager::
// BeginFrame - before the Infinite missiles cheat fills them up again): a save loaded is the play log's (all of
// Samus' items changing at once: runtime/hle/playlog.c); a missile fired is her missile count going down in play (the
// game's missile launch is a direct call in the game code, nothing the runtime can step into). A Super Missile is one
// missile fired; the beam combos that use missiles while held count once as they start (counts going down within
// 0.35 s of each other are one).
//
// Played on a stream of their own on the default output (SDL mixes it with the game's), at the menu's volume, decoded
// as they play (a thread each; audiodec.c's decoders, else FFmpeg - the one shipped has no MP3). A sound started again stops the one before it on its own slot. LARECOMP_NO_AUDIO: decoded
// (and logged) but not played - the test runs stay quiet.
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

enum { SLOT_LOAD, SLOT_MISSILE, SLOTS };
static const char *const k_slot_names[SLOTS] = {"load", "missile"};
static const char *const k_exts[] = {"mp3", "ogg", "wav", "flac", "m4a", "opus", "aac"};
#define N_EXTS ((int)(sizeof k_exts / sizeof *k_exts))

// ------------------------------------------------------------------ decoding: audiodec.c's, else FFmpeg's
typedef struct AudioDec AudioDec;
AudioDec *audiodec_open(const char *path);
int audiodec_channels(const AudioDec *d), audiodec_rate(const AudioDec *d);
int audiodec_read(AudioDec *d, float *out, int frames);
void audiodec_close(AudioDec *d);

typedef struct {
    AudioDec *ad;
    AVFormatContext *fc;
    AVCodecContext *cc;
    AVPacket *pkt;
    AVFrame *fr;
    int si, nch, rate;
    bool eof;
    float *buf;
    size_t cap;  // (floats)
} Src;

static void src_close(Src *s) {
    audiodec_close(s->ad);
    av_frame_free(&s->fr);
    av_packet_free(&s->pkt);
    avcodec_free_context(&s->cc);
    if (s->fc) avformat_close_input(&s->fc);
    free(s->buf);
    memset(s, 0, sizeof *s);
}

static bool src_open(Src *s, const char *path) {
    memset(s, 0, sizeof *s);
    if ((s->ad = audiodec_open(path))) {
        s->nch = audiodec_channels(s->ad), s->rate = audiodec_rate(s->ad);
        return true;
    }
    const AVCodec *codec = NULL;
    if (avformat_open_input(&s->fc, path, NULL, NULL) < 0 || avformat_find_stream_info(s->fc, NULL) < 0 ||
        (s->si = av_find_best_stream(s->fc, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) < 0 || !(s->cc = avcodec_alloc_context3(codec)) ||
        avcodec_parameters_to_context(s->cc, s->fc->streams[s->si]->codecpar) < 0 || avcodec_open2(s->cc, codec, NULL) < 0) {
        src_close(s);
        return false;
    }
    s->pkt = av_packet_alloc(), s->fr = av_frame_alloc();
    s->nch = s->cc->ch_layout.nb_channels > 0 ? s->cc->ch_layout.nb_channels : 2;
    s->rate = s->cc->sample_rate > 0 ? s->cc->sample_rate : 44100;
    return s->pkt && s->fr;
}

static float sample_at(const AVFrame *f, int ch, int i);
// The next piece of sound (interleaved float, s->nch channels) in *out: its frames, 0 at the end
static int src_next(Src *s, float **out) {
    if (s->ad) {
        if (!s->buf) s->buf = malloc((s->cap = (size_t)4096 * s->nch) * sizeof *s->buf);
        *out = s->buf;
        return s->buf ? audiodec_read(s->ad, s->buf, 4096) : 0;
    }
    for (;;) {
        if (avcodec_receive_frame(s->cc, s->fr) == 0) {
            int n = s->fr->nb_samples, fch = s->fr->ch_layout.nb_channels;
            if ((size_t)n * s->nch > s->cap) s->buf = realloc(s->buf, (s->cap = (size_t)n * s->nch) * sizeof *s->buf);
            for (int i = 0; i < n; i++)
                for (int c = 0; c < s->nch; c++) s->buf[(size_t)i * s->nch + c] = c < fch ? sample_at(s->fr, c, i) : 0;
            *out = s->buf;
            return n;
        }
        if (s->eof) return 0;
        if (av_read_frame(s->fc, s->pkt) < 0) {
            avcodec_send_packet(s->cc, NULL), s->eof = true;  // (the decoder's last frames)
            continue;
        }
        if (s->pkt->stream_index == s->si) avcodec_send_packet(s->cc, s->pkt);
        av_packet_unref(s->pkt);
    }
}

// ------------------------------------------------------------------ the files
// The slot's file in the data folder (patriot_<slot>.<ext>), or NULL; (test) LARECOMP_PATRIOT_<LOAD|MISSILE>=<path>
static bool slot_file(int slot, char *out, size_t cap) {
    char env[48];
    snprintf(env, sizeof env, "LARECOMP_PATRIOT_%s", slot == SLOT_LOAD ? "LOAD" : "MISSILE");
    if (getenv(env) && *getenv(env)) { snprintf(out, cap, "%s", getenv(env)); return true; }
    for (int i = 0; i < N_EXTS; i++) {
        snprintf(out, cap, "patriot_%s.%s", k_slot_names[slot], k_exts[i]);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(out, &info) && info.type == SDL_PATHTYPE_FILE) return true;
    }
    return false;
}

const char *suit_sound_file(int slot) {  // (the menu: the file's name, or NULL)
    static char names[SLOTS][64];
    if (slot < 0 || slot >= SLOTS) return NULL;
    char path[512];
    if (!slot_file(slot, path, sizeof path)) return NULL;
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    snprintf(names[slot], sizeof names[slot], "%s", base);
    return names[slot];
}

// A file of the player's copied in as the slot's (the menu's file dialog, on whatever thread it calls back on): its
// extension kept, the slot's file in any other format removed
bool suit_sound_import(int slot, const char *src) {
    if (slot < 0 || slot >= SLOTS) return false;
    Src probe;  // (a file with a sound this plays, or nothing changes)
    if (!src_open(&probe, src)) { LOG("[sound] %s has no sound this can play\n", src); return false; }
    src_close(&probe);
    const char *dot = strrchr(src, '.');
    char ext[8] = "mp3";
    if (dot && strlen(dot + 1) < sizeof ext) {
        snprintf(ext, sizeof ext, "%s", dot + 1);
        for (char *c = ext; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    }
    size_t n = 0;
    void *data = SDL_LoadFile(src, &n);
    char dst[96], tmp[112];
    snprintf(dst, sizeof dst, "patriot_%s.%s", k_slot_names[slot], ext);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    bool ok = data && SDL_SaveFile(tmp, data, n);
    SDL_free(data);
    if (ok)
        for (int i = 0; i < N_EXTS; i++) {  // (the slot's earlier file, another format)
            char old[96];
            snprintf(old, sizeof old, "patriot_%s.%s", k_slot_names[slot], k_exts[i]);
            if (strcmp(old, dst)) SDL_RemovePath(old);
        }
    ok = ok && SDL_RenamePath(tmp, dst);
    if (!ok) { LOG("[sound] couldn't copy %s to %s: %s\n", src, dst, SDL_GetError()); return false; }
    LOG("[sound] %s: %s (%zu bytes) copied to %s\n", k_slot_names[slot], src, n, dst);
    return true;
}

// ------------------------------------------------------------------ playing
static _Atomic uint32_t g_slot_gen[SLOTS];  // a sound started on a slot: the one before stops

typedef struct { int slot; uint32_t gen; char path[512]; } Play;

static float sample_at(const AVFrame *f, int ch, int i) {
    int fmt = f->format, nch = f->ch_layout.nb_channels;
    bool planar = av_sample_fmt_is_planar(fmt);
    const uint8_t *p = planar ? f->extended_data[ch] : f->extended_data[0];
    int k = planar ? i : i * nch + ch;
    switch (av_get_packed_sample_fmt(fmt)) {
    case AV_SAMPLE_FMT_FLT: return ((const float *)p)[k];
    case AV_SAMPLE_FMT_DBL: return (float)((const double *)p)[k];
    case AV_SAMPLE_FMT_S16: return ((const int16_t *)p)[k] / 32768.0f;
    case AV_SAMPLE_FMT_S32: return ((const int32_t *)p)[k] / 2147483648.0f;
    case AV_SAMPLE_FMT_U8: return (((const uint8_t *)p)[k] - 128) / 128.0f;
    default: return 0;
    }
}

static void *play_thread(void *arg) {
    Play *pl = arg;
    bool muted = getenv("LARECOMP_NO_AUDIO") && strcmp(getenv("LARECOMP_NO_AUDIO"), "0");
    SDL_AudioStream *st = NULL;
    double seconds = 0;
    Src src;
    if (!src_open(&src, pl->path)) {
        LOG("[sound] %s: can't play %s\n", k_slot_names[pl->slot], pl->path);
        free(pl);
        return NULL;
    }
    if (!muted) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { LOG("[sound] no audio: %s\n", SDL_GetError()); goto out; }
        SDL_AudioSpec spec = {SDL_AUDIO_F32, src.nch, src.rate};
        st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!st) { LOG("[sound] can't open the output: %s\n", SDL_GetError()); goto out; }
        SDL_SetAudioStreamGain(st, g_settings.volume);
        SDL_ResumeAudioStreamDevice(st);
    }
    LOG("[sound] %s: playing %s (%d Hz, %d channels)%s\n", k_slot_names[pl->slot], pl->path, src.rate, src.nch,
        muted ? " (muted: LARECOMP_NO_AUDIO)" : "");
    float *chunk;
    int n;
    while (g_slot_gen[pl->slot] == pl->gen && (n = src_next(&src, &chunk)) > 0) {
        seconds += (double)n / src.rate;
        if (st) SDL_PutAudioStreamData(st, chunk, (int)((size_t)n * src.nch * sizeof(float)));
        // (decoded ahead about half a second, no more: a long song isn't held in memory, and a stop is heard at once)
        while (st && g_slot_gen[pl->slot] == pl->gen && SDL_GetAudioStreamQueued(st) > src.rate * src.nch * (int)sizeof(float) / 2)
            SDL_Delay(10);
    }
    while (st && g_slot_gen[pl->slot] == pl->gen && SDL_GetAudioStreamQueued(st) > 0) SDL_Delay(20);  // (played out)
    if (g_slot_gen[pl->slot] != pl->gen) LOG("[sound] %s: stopped (another one started)\n", k_slot_names[pl->slot]);
    else LOG("[sound] %s: done (%.1f s)\n", k_slot_names[pl->slot], seconds);
out:
    if (st) SDL_DestroyAudioStream(st);
    src_close(&src);
    free(pl);
    return NULL;
}

static void play(int slot) {
    Play *pl = calloc(1, sizeof *pl);
    if (!pl) return;
    if (!slot_file(slot, pl->path, sizeof pl->path)) { free(pl); return; }
    pl->slot = slot;
    pl->gen = ++g_slot_gen[slot];
    pthread_t th;
    if (pthread_create(&th, NULL, play_thread, pl) == 0) pthread_detach(th);
    else free(pl);
}

// ------------------------------------------------------------------ the moments
// Each frame from cheats_frame: Samus' missiles (amount and capacity) as they are before the cheats fill them up
// again; returns after the moments are noted. (A game's missiles taken back by the cheat afterwards: read again next
// frame through suit_sounds_missiles_now.)
static int g_missiles = -1, g_missile_cap = -1, g_loads = -1;
static uint64_t g_last_drop_ns;

void suit_sounds_frame(int missiles, int capacity, bool in_play) {
    extern int playlog_save_loads(void);
    bool on = g_settings.suit_style == SUIT_PATRIOT && g_settings.suit_sounds;
    int loads = playlog_save_loads();
    if (loads != g_loads) {
        if (g_loads >= 0 || loads > 0) {  // (a save came in since the last frame: the play log saw it)
            if (on) play(SLOT_LOAD);
            g_missiles = -1;  // (the count read fresh: not a missile fired)
        }
        g_loads = loads;
    }
    if (!in_play) return;
    if (g_missiles >= 0 && capacity == g_missile_cap && missiles < g_missiles) {
        uint64_t now = SDL_GetTicksNS();
        if (on && now - g_last_drop_ns > 350000000ull) play(SLOT_MISSILE);
        g_last_drop_ns = now;
    }
    g_missiles = missiles, g_missile_cap = capacity;
}

// After the cheats have filled the missiles up again (cheats_frame): the count to compare the next frame's with
void suit_sounds_missiles_now(int missiles) { if (g_missiles >= 0) g_missiles = missiles; }

RT_ON_RESET(suit_sounds_reset) { g_missiles = -1, g_missile_cap = -1; }

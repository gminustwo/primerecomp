// Sound files decoded for runtime/hle/suitsounds.c (Patriot sounds): MP3, WAV, FLAC and Ogg Vorbis by small public
// domain decoders - dr_mp3 / dr_wav / dr_flac (third_party/dr_libs, public domain or MIT-0) and stb_vorbis
// (third_party/stb_vorbis, public domain or MIT). The FFmpeg PrimeRecomp ships has only what the game's movies need
// (Matroska and MP4, VP9 / VP8 / H.264, Vorbis / Opus / AAC), so a player's MP3 didn't play; suitsounds.c still tries
// FFmpeg for the rest (M4A / AAC). The decoder is picked by the file's first bytes, not its name.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define DR_MP3_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_flac.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/stb_vorbis/stb_vorbis.c"
#pragma clang diagnostic pop

enum { DEC_MP3 = 1, DEC_WAV, DEC_FLAC, DEC_VORBIS };
typedef struct AudioDec {
    int kind, channels, rate;
    drmp3 mp3;
    drwav wav;
    drflac *flac;
    stb_vorbis *vorbis;
} AudioDec;

// The file's sound, or NULL: not one of these formats (or broken)
AudioDec *audiodec_open(const char *path) {
    unsigned char m[12] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = fread(m, 1, sizeof m, f);
    fclose(f);
    if (n < 4) return NULL;
    AudioDec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if (!memcmp(m, "fLaC", 4)) {
        d->flac = drflac_open_file(path, NULL);
        if (d->flac) d->kind = DEC_FLAC, d->channels = d->flac->channels, d->rate = (int)d->flac->sampleRate;
    } else if ((!memcmp(m, "RIFF", 4) || !memcmp(m, "RIFX", 4) || !memcmp(m, "RF64", 4)) && n >= 12 && !memcmp(m + 8, "WAVE", 4)) {
        if (drwav_init_file(&d->wav, path, NULL)) d->kind = DEC_WAV, d->channels = d->wav.channels, d->rate = (int)d->wav.sampleRate;
    } else if (!memcmp(m, "OggS", 4)) {
        int err = 0;
        d->vorbis = stb_vorbis_open_filename(path, &err, NULL);
        if (d->vorbis) {
            stb_vorbis_info info = stb_vorbis_get_info(d->vorbis);
            d->kind = DEC_VORBIS, d->channels = info.channels, d->rate = (int)info.sample_rate;
        }
    } else if (!memcmp(m, "ID3", 3) || (m[0] == 0xFF && (m[1] & 0xE0) == 0xE0)) {
        if (drmp3_init_file(&d->mp3, path, NULL)) d->kind = DEC_MP3, d->channels = (int)d->mp3.channels, d->rate = (int)d->mp3.sampleRate;
    }
    if (!d->kind || d->channels <= 0 || d->rate <= 0) {
        if (d->kind == DEC_MP3) drmp3_uninit(&d->mp3);
        if (d->kind == DEC_WAV) drwav_uninit(&d->wav);
        if (d->flac) drflac_close(d->flac);
        if (d->vorbis) stb_vorbis_close(d->vorbis);
        free(d);
        return NULL;
    }
    return d;
}

int audiodec_channels(const AudioDec *d) { return d->channels; }
int audiodec_rate(const AudioDec *d) { return d->rate; }

// Up to `frames` frames into out (interleaved float, channels each); 0 at the end
int audiodec_read(AudioDec *d, float *out, int frames) {
    switch (d->kind) {
    case DEC_MP3: return (int)drmp3_read_pcm_frames_f32(&d->mp3, (drmp3_uint64)frames, out);
    case DEC_WAV: return (int)drwav_read_pcm_frames_f32(&d->wav, (drwav_uint64)frames, out);
    case DEC_FLAC: return (int)drflac_read_pcm_frames_f32(d->flac, (drflac_uint64)frames, out);
    case DEC_VORBIS: return stb_vorbis_get_samples_float_interleaved(d->vorbis, d->channels, out, frames * d->channels);
    default: return 0;
    }
}

void audiodec_close(AudioDec *d) {
    if (!d) return;
    if (d->kind == DEC_MP3) drmp3_uninit(&d->mp3);
    if (d->kind == DEC_WAV) drwav_uninit(&d->wav);
    if (d->flac) drflac_close(d->flac);
    if (d->vorbis) stb_vorbis_close(d->vorbis);
    free(d);
}

/*
 * hello_audio - Pixelica's optional audio mixer, no renderer or window needed.
 *
 * Both sounds are generated in code as tiny in-memory WAV files (no asset files):
 *   BGM     2 s looping 16-bit mono arpeggio
 *   effect  0.2 s 8-bit mono rising blip
 * and loaded with pixelica_audio_clip_wav, the same call a real game uses on bytes
 * it read from disk.
 *
 * Always:  an offline mix to int16 (pixelica_audio_render_s16) printed as a
 *          checksum plus sample statistics. Needs no speaker, and is rendered
 *          twice to check that it is deterministic.
 * With the SDL3 adapter (HELLO_AUDIO_SDL3=1): then a live run through the default
 *          output device. The scripted controls are
 *            - master volume 0.7 and music bus 0.6
 *            - the effect repeats while its pan sweeps left to right
 *            - the music pauses and resumes
 *            - the music fades out and stops by itself
 *
 *   hello_audio                 live run of about 8 s (offline mix only without SDL)
 *   hello_audio --seconds N     live run of N seconds (1..60)
 *   hello_audio --smoke         about 1 s live run for tests; exits by itself
 *   hello_audio --offline       only the offline mix
 *
 * The live run never waits on the console. SDL reads the audio driver from
 * SDL_AUDIO_DRIVER; SDL_AUDIODRIVER=dummy is accepted too, so tests need no speaker.
 */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pixelica/audio.h"

#ifdef HELLO_AUDIO_SDL3
#include <SDL3/SDL.h>

#include "pixelica/audio_sdl3.h"
#endif

#define MIX_RATE 44100u
#define CLIP_RATE 22050u
#define PI_F 3.14159265f

#define BGM_NOTE_FRAMES (CLIP_RATE / 4u)
#define BGM_FRAMES (8u * BGM_NOTE_FRAMES)
#define FX_FRAMES (CLIP_RATE / 5u)

#define WAV_HEADER 44u
#define BGM_FILE_BYTES (WAV_HEADER + BGM_FRAMES * 2u)
#define FX_FILE_BYTES (WAV_HEADER + FX_FRAMES)

#define OFFLINE_FRAMES MIX_RATE /* one second */
#define OFFLINE_BLOCK 512u

/* The WAV bytes must stay alive while a voice plays them (clips borrow them). */
static uint8_t g_bgm_file[BGM_FILE_BYTES];
static uint8_t g_fx_file[FX_FILE_BYTES];
static PIXELICA_AudioClip g_bgm;
static PIXELICA_AudioClip g_fx;

static void put_u16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, (unsigned)(v & 0xFFFFu));
    put_u16(p + 2, (unsigned)(v >> 16));
}

/* Rising pluck-like notes; every note fades to zero so the loop is seamless. */
static float bgm_sample(size_t i)
{
    static const float notes[8] = {261.63f, 329.63f, 392.00f, 523.25f,
                                   392.00f, 329.63f, 293.66f, 392.00f};
    const size_t pos = i % BGM_NOTE_FRAMES;
    const float t = (float)pos / (float)CLIP_RATE;
    const float u = (float)pos / (float)BGM_NOTE_FRAMES;
    const float attack = t < 0.005f ? t / 0.005f : 1.0f;
    const float decay = (1.0f - u) * (1.0f - u);
    const float w = 2.0f * PI_F * notes[i / BGM_NOTE_FRAMES] * t;

    return 0.3f * attack * decay * (sinf(w) + 0.3f * sinf(2.0f * w));
}

/* Frequency sweep 880 Hz -> 1760 Hz over the clip. */
static float fx_sample(size_t i)
{
    const float t = (float)i / (float)CLIP_RATE;
    const float u = (float)i / (float)FX_FRAMES;
    const float sweep = (1760.0f - 880.0f) / ((float)FX_FRAMES / (float)CLIP_RATE);
    const float phase = 2.0f * PI_F * (880.0f * t + 0.5f * sweep * t * t);

    return 0.6f * (1.0f - u) * (1.0f - u) * sinf(phase);
}

/* Canonical mono PCM WAV (44-byte header) of `frames` frames; bits is 8 or 16. */
static size_t build_wav(uint8_t *out, size_t frames, unsigned bits, float (*sample)(size_t))
{
    const unsigned bytes_per_frame = bits / 8u;
    const uint32_t data_bytes = (uint32_t)(frames * bytes_per_frame);

    memcpy(out, "RIFF", 4);
    put_u32(out + 4, 36u + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_u32(out + 16, 16u);
    put_u16(out + 20, 1u); /* PCM */
    put_u16(out + 22, 1u); /* mono */
    put_u32(out + 24, CLIP_RATE);
    put_u32(out + 28, CLIP_RATE * bytes_per_frame);
    put_u16(out + 32, bytes_per_frame);
    put_u16(out + 34, bits);
    memcpy(out + 36, "data", 4);
    put_u32(out + 40, data_bytes);
    for (size_t i = 0u; i < frames; ++i) {
        const float v = sample(i);
        if (bits == 16u) {
            const int s = (int)(v * 32767.0f);
            put_u16(out + WAV_HEADER + i * 2u, (unsigned)(s < 0 ? s + 65536 : s));
        } else {
            out[WAV_HEADER + i] = (uint8_t)(128 + (int)(v * 127.0f));
        }
    }
    return WAV_HEADER + (size_t)data_bytes;
}

static bool make_clips(void)
{
    const size_t bgm_size = build_wav(g_bgm_file, BGM_FRAMES, 16u, bgm_sample);
    const size_t fx_size = build_wav(g_fx_file, FX_FRAMES, 8u, fx_sample);

    if (pixelica_audio_clip_wav(&g_bgm, g_bgm_file, bgm_size) != PIXELICA_OK ||
        pixelica_audio_clip_wav(&g_fx, g_fx_file, fx_size) != PIXELICA_OK) {
        fprintf(stderr, "hello_audio: generated WAV rejected by the parser\n");
        return false;
    }
    return true;
}

typedef struct OfflineResult {
    uint32_t checksum;
    size_t nonzero;
    int peak;
} OfflineResult;

/* Renders one second of music plus two panned effects into int16 blocks. */
static bool offline_mix(OfflineResult *result)
{
    static PIXELICA_AudioMixer mixer;
    int16_t block[OFFLINE_BLOCK * 2u];
    PIXELICA_AudioPlayOptions left;
    PIXELICA_AudioPlayOptions right;
    PIXELICA_AudioVoice voice = 0u;
    uint32_t hash = 2166136261u;

    memset(result, 0, sizeof(*result));
    left = pixelica_audio_default_options();
    left.pan = -0.8f;
    right = pixelica_audio_default_options();
    right.pan = 0.8f;
    right.volume = 0.5f;

    if (pixelica_audio_init(&mixer, MIX_RATE) != PIXELICA_OK ||
        pixelica_audio_play_music(&mixer, &g_bgm, &voice) != PIXELICA_OK ||
        pixelica_audio_play(&mixer, &g_fx, &left, &voice) != PIXELICA_OK ||
        pixelica_audio_set_master_volume(&mixer, 0.7f) != PIXELICA_OK) {
        return false;
    }
    for (size_t done = 0u; done < OFFLINE_FRAMES;) {
        size_t frames = OFFLINE_FRAMES - done;
        if (frames > OFFLINE_BLOCK) {
            frames = OFFLINE_BLOCK;
        }
        /* A second effect, started a quarter second in (half volume, right). */
        if (done <= MIX_RATE / 4u && done + frames > MIX_RATE / 4u &&
            pixelica_audio_play(&mixer, &g_fx, &right, &voice) != PIXELICA_OK) {
            return false;
        }
        if (pixelica_audio_render_s16(&mixer, block, frames) != PIXELICA_OK) {
            return false;
        }
        for (size_t i = 0u; i < frames * 2u; ++i) {
            const int s = block[i];
            const unsigned u = (unsigned)(s < 0 ? s + 65536 : s);
            const int mag = s < 0 ? -s : s;

            /* FNV-1a over little-endian bytes: independent of host endianness. */
            hash = (hash ^ (u & 0xFFu)) * 16777619u;
            hash = (hash ^ (u >> 8)) * 16777619u;
            if (s != 0) {
                ++result->nonzero;
            }
            if (mag > result->peak) {
                result->peak = mag;
            }
        }
        done += frames;
    }
    result->checksum = hash;
    return true;
}

static int run_offline(void)
{
    OfflineResult first;
    OfflineResult second;

    if (!offline_mix(&first) || !offline_mix(&second)) {
        fprintf(stderr, "hello_audio: offline mix failed\n");
        return 1;
    }
    printf("offline mix: %u frames @ %u Hz, checksum 0x%08X, %zu nonzero samples, peak %d\n",
           (unsigned)OFFLINE_FRAMES, (unsigned)MIX_RATE, (unsigned)first.checksum,
           first.nonzero, first.peak);
    if (first.nonzero == 0u || first.peak == 0) {
        fprintf(stderr, "hello_audio: offline mix is silent\n");
        return 1;
    }
    if (first.checksum != second.checksum) {
        fprintf(stderr, "hello_audio: offline mix is not deterministic\n");
        return 1;
    }
    return 0;
}

#ifdef HELLO_AUDIO_SDL3
static bool check(PIXELICA_Status status, const char *what)
{
    if (status != PIXELICA_OK) {
        fprintf(stderr, "hello_audio: %s failed (status %d) %s\n", what, (int)status, SDL_GetError());
        return false;
    }
    return true;
}

static int run_live(unsigned seconds)
{
    static PIXELICA_AudioSDL3 audio; /* zero-initialised */
    PIXELICA_AudioMixer *mixer;
    PIXELICA_AudioVoice music = 0u;
    const char *driver = SDL_getenv("SDL_AUDIODRIVER");
    const uint64_t total_ms = (uint64_t)seconds * 1000u;
    const uint64_t fx_interval = total_ms / 12u;
    uint64_t next_fx = 0u;
    unsigned fx_count = 0u;
    bool paused = false;
    bool resumed = false;
    bool faded = false;
    int result = 0;
    uint64_t start;

    /* SDL3 names the variable SDL_AUDIO_DRIVER; accept the SDL2 spelling as well. */
    if (driver != NULL && SDL_getenv("SDL_AUDIO_DRIVER") == NULL) {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, driver);
    }
    if (!check(pixelica_audio_sdl3_open(&audio, MIX_RATE), "pixelica_audio_sdl3_open")) {
        return 1;
    }
    mixer = pixelica_audio_sdl3_mixer(&audio);
    if (!check(pixelica_audio_set_master_volume(mixer, 0.7f), "master volume") ||
        !check(pixelica_audio_set_bus_volume(mixer, PIXELICA_AUDIO_MUSIC, 0.6f), "music bus volume") ||
        !check(pixelica_audio_play_music(mixer, &g_bgm, &music), "play_music")) {
        pixelica_audio_sdl3_close(&audio);
        return 1;
    }
    printf("live: %u s through the default output (music bus 0.6, master 0.7)\n", seconds);

    start = SDL_GetTicks();
    for (;;) {
        const uint64_t now = SDL_GetTicks() - start;

        if (now >= total_ms) {
            break;
        }
        if (now >= next_fx) {
            PIXELICA_AudioPlayOptions options = pixelica_audio_default_options();
            PIXELICA_AudioVoice effect = 0u;
            options.pan = -0.8f + 1.6f * (float)(fx_count % 8u) / 7.0f;
            if (!check(pixelica_audio_play(mixer, &g_fx, &options, &effect), "play effect")) {
                result = 1;
                break;
            }
            ++fx_count;
            next_fx += fx_interval;
        }
        if (!paused && now >= total_ms * 45u / 100u) {
            printf("live: music paused\n");
            result |= !check(pixelica_audio_pause(mixer, music, true), "pause");
            paused = true;
        }
        if (!resumed && now >= total_ms * 60u / 100u) {
            printf("live: music resumed\n");
            result |= !check(pixelica_audio_pause(mixer, music, false), "resume");
            resumed = true;
        }
        if (!faded && now >= total_ms * 70u / 100u) {
            printf("live: music fades out\n");
            result |= !check(pixelica_audio_fade(mixer, music, 0.0f, (float)seconds * 0.25f, true), "fade");
            faded = true;
        }
        /* Every frame, even when the game is paused. */
        result |= !check(pixelica_audio_sdl3_update(&audio), "update");
        if (result != 0) {
            break;
        }
        SDL_Delay(8);
    }
    printf("live: %u effects played, music %s\n", fx_count,
           pixelica_audio_is_playing(mixer, music) ? "still playing" : "stopped");
    pixelica_audio_sdl3_close(&audio);
    return result;
}
#endif

int main(int argc, char **argv)
{
    unsigned seconds = 8u;
    bool offline_only = false;
    int status;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--smoke") == 0) {
            seconds = 1u;
        } else if (strcmp(argv[i], "--offline") == 0) {
            offline_only = true;
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            const long value = strtol(argv[++i], NULL, 10);
            if (value < 1 || value > 60) {
                fprintf(stderr, "hello_audio: --seconds takes 1..60\n");
                return 2;
            }
            seconds = (unsigned)value;
        } else {
            fprintf(stderr, "usage: hello_audio [--smoke] [--seconds N] [--offline]\n");
            return 2;
        }
    }

    if (!make_clips()) {
        return 1;
    }
    status = run_offline();
    if (status != 0 || offline_only) {
        return status;
    }
#ifdef HELLO_AUDIO_SDL3
    return run_live(seconds);
#else
    (void)seconds;
    printf("built without the SDL3 audio adapter: offline mix only\n");
    return 0;
#endif
}

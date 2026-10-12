/*
 * Test of the SDL3 audio output (audio_sdl3.h) on SDL's dummy audio driver.
 *
 * Needs no speaker: the dummy driver consumes queued audio at real time and
 * discards it, so this checks the SDL side only (subsystem reference counting,
 * stream creation and pump, load/unload), never what is audible. Checks never
 * use assert(), so they stay active in Release builds.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pixelica/audio_sdl3.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define RATE 8000u
#define WAV_FRAMES 400u /* 50 ms at RATE */
#define GOOD_PATH "pixelica_audio_sdl3_test_good.wav"
#define BAD_PATH "pixelica_audio_sdl3_test_bad.wav"
#define MISSING_PATH "pixelica_audio_sdl3_test_missing.wav"

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

/* Canonical 44-byte header + `frames` mono S16LE frames of a square wave. */
static size_t build_wav(uint8_t *out, size_t frames)
{
    const uint32_t data_bytes = (uint32_t)(frames * 2u);

    memcpy(out, "RIFF", 4);
    put_u32(out + 4, 36u + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_u32(out + 16, 16u);
    put_u16(out + 20, 1u);         /* PCM */
    put_u16(out + 22, 1u);         /* mono */
    put_u32(out + 24, RATE);       /* sample rate */
    put_u32(out + 28, RATE * 2u);  /* byte rate */
    put_u16(out + 32, 2u);         /* block align */
    put_u16(out + 34, 16u);        /* bits */
    memcpy(out + 36, "data", 4);
    put_u32(out + 40, data_bytes);
    for (size_t i = 0u; i < frames; ++i) {
        put_u16(out + 44u + i * 2u, ((i / 20u) & 1u) != 0u ? 0x2000u : 0xE000u);
    }
    return 44u + (size_t)data_bytes;
}

static PIXELICA_AudioClip memory_clip(const uint8_t *pcm, size_t frames)
{
    PIXELICA_AudioClip clip;
    memset(&clip, 0, sizeof(clip));
    clip.samples = pcm;
    clip.frame_count = frames;
    clip.sample_rate = RATE;
    clip.channels = 1u;
    clip.format = PIXELICA_AUDIO_S16LE;
    return clip;
}

/* Calls update until `voice` ends or about three seconds pass. */
static bool pump_until_done(PIXELICA_AudioSDL3 *audio, PIXELICA_AudioVoice voice)
{
    for (int i = 0; i < 300; ++i) {
        CHECK(pixelica_audio_sdl3_update(audio) == PIXELICA_OK);
        if (!pixelica_audio_is_playing(pixelica_audio_sdl3_mixer(audio), voice)) {
            return true;
        }
        SDL_Delay(10);
    }
    return false;
}

static void test_invalid_args(void)
{
    PIXELICA_AudioSDL3 audio;
    PIXELICA_AudioWav wav;

    memset(&audio, 0, sizeof(audio));
    memset(&wav, 0, sizeof(wav));

    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
    CHECK(pixelica_audio_sdl3_open(NULL, RATE) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_sdl3_open(&audio, 100u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_sdl3_open(&audio, 500000u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(!audio.open);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0); /* rejected rates never touch SDL */

    CHECK(pixelica_audio_sdl3_mixer(NULL) == NULL);
    CHECK(pixelica_audio_sdl3_mixer(&audio) == NULL);
    CHECK(pixelica_audio_sdl3_update(NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_sdl3_update(&audio) == PIXELICA_ERR_INVALID_ARG);

    /* close / unload on nothing are no-ops. */
    pixelica_audio_sdl3_close(NULL);
    pixelica_audio_sdl3_close(&audio);
    pixelica_audio_sdl3_unload_wav(NULL, NULL);
    pixelica_audio_sdl3_unload_wav(&audio, NULL);
    pixelica_audio_sdl3_unload_wav(&audio, &wav);
    CHECK(wav.bytes == NULL);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);

    CHECK(pixelica_audio_sdl3_load_wav(NULL, GOOD_PATH) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_sdl3_load_wav(&wav, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(wav.bytes == NULL);
}

/* The adapter's SDL reference is balanced against other users of the subsystem. */
static void test_lifecycle_and_refcount(void)
{
    PIXELICA_AudioSDL3 a;
    PIXELICA_AudioSDL3 b;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    CHECK(pixelica_audio_sdl3_open(&a, RATE) == PIXELICA_OK);
    CHECK(a.open);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
    CHECK(pixelica_audio_sdl3_mixer(&a) != NULL);
    CHECK(pixelica_audio_sdl3_update(&a) == PIXELICA_OK);

    /* Double open is rejected and leaks no reference. */
    CHECK(pixelica_audio_sdl3_open(&a, RATE) == PIXELICA_ERR_INVALID_ARG);
    CHECK(a.open);

    pixelica_audio_sdl3_close(&a);
    CHECK(!a.open);
    CHECK(pixelica_audio_sdl3_mixer(&a) == NULL);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0); /* the double open took no extra reference */
    pixelica_audio_sdl3_close(&a);           /* idempotent */
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);

    /* Reopen after close, with another rate. */
    CHECK(pixelica_audio_sdl3_open(&a, 22050u) == PIXELICA_OK);
    CHECK(pixelica_audio_sdl3_update(&a) == PIXELICA_OK);
    pixelica_audio_sdl3_close(&a);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);

    /* Two adapters and an outside user share the subsystem by reference count. */
    CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO));
    CHECK(pixelica_audio_sdl3_open(&a, RATE) == PIXELICA_OK);
    CHECK(pixelica_audio_sdl3_open(&b, RATE) == PIXELICA_OK);
    pixelica_audio_sdl3_close(&a);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
    CHECK(pixelica_audio_sdl3_update(&b) == PIXELICA_OK);
    pixelica_audio_sdl3_close(&b);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0); /* still held by this test */
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
}

static void test_pump(void)
{
    PIXELICA_AudioSDL3 audio;
    PIXELICA_AudioMixer *mixer;
    uint8_t file[44u + WAV_FRAMES * 2u];
    PIXELICA_AudioClip clip;
    PIXELICA_AudioVoice voice = 0u;
    const int max_queued = (int)(RATE * 50u / 1000u * 8u);

    memset(&audio, 0, sizeof(audio));
    CHECK(build_wav(file, WAV_FRAMES) == sizeof(file));
    clip = memory_clip(file + 44, WAV_FRAMES);

    CHECK(pixelica_audio_sdl3_open(&audio, RATE) == PIXELICA_OK);
    mixer = pixelica_audio_sdl3_mixer(&audio);
    CHECK(mixer != NULL);

    /* The queue never grows beyond the ~50 ms target, however often update runs. */
    for (int i = 0; i < 20; ++i) {
        int queued;
        CHECK(pixelica_audio_sdl3_update(&audio) == PIXELICA_OK);
        queued = SDL_GetAudioStreamQueued((SDL_AudioStream *)audio.stream);
        CHECK(queued >= 0 && queued <= max_queued);
    }

    CHECK(pixelica_audio_play(mixer, &clip, NULL, &voice) == PIXELICA_OK);
    CHECK(voice != 0u);
    CHECK(pixelica_audio_is_playing(mixer, voice));
    CHECK(pump_until_done(&audio, voice)); /* the pump really renders the mixer */

    /* After a stall the next update refills at most the target, never more. */
    SDL_Delay(120);
    CHECK(pixelica_audio_sdl3_update(&audio) == PIXELICA_OK);
    CHECK(SDL_GetAudioStreamQueued((SDL_AudioStream *)audio.stream) <= max_queued);

    pixelica_audio_sdl3_close(&audio);
}

static void test_load_unload(void)
{
    PIXELICA_AudioSDL3 audio;
    PIXELICA_AudioWav wav;
    PIXELICA_AudioWav other;
    PIXELICA_AudioVoice voice = 0u;
    uint8_t file[44u + WAV_FRAMES * 2u];
    static const uint8_t junk[] = {'n', 'o', 't', ' ', 'a', ' ', 'w', 'a', 'v'};
    void *first_bytes;

    memset(&audio, 0, sizeof(audio));
    memset(&wav, 0, sizeof(wav));
    memset(&other, 0, sizeof(other));
    CHECK(build_wav(file, WAV_FRAMES) == sizeof(file));
    CHECK(SDL_SaveFile(GOOD_PATH, file, sizeof(file)));
    CHECK(SDL_SaveFile(BAD_PATH, junk, sizeof(junk)));

    /* Failures leave the target untouched. */
    CHECK(pixelica_audio_sdl3_load_wav(&wav, MISSING_PATH) == PIXELICA_ERR_NOT_FOUND);
    CHECK(wav.bytes == NULL && wav.size == 0u);
    CHECK(pixelica_audio_sdl3_load_wav(&wav, BAD_PATH) != PIXELICA_OK);
    CHECK(wav.bytes == NULL && wav.size == 0u && wav.clip.samples == NULL);

    CHECK(pixelica_audio_sdl3_load_wav(&wav, GOOD_PATH) == PIXELICA_OK);
    CHECK(wav.bytes != NULL);
    CHECK(wav.size == sizeof(file));
    CHECK(wav.clip.frame_count == WAV_FRAMES);
    CHECK(wav.clip.sample_rate == RATE);
    CHECK(wav.clip.channels == 1u);
    CHECK(wav.clip.format == PIXELICA_AUDIO_S16LE);
    CHECK(wav.clip.samples != NULL);

    /* Loading again is rejected instead of leaking the first buffer. */
    first_bytes = wav.bytes;
    CHECK(pixelica_audio_sdl3_load_wav(&wav, GOOD_PATH) == PIXELICA_ERR_INVALID_ARG);
    CHECK(wav.bytes == first_bytes);

    CHECK(pixelica_audio_sdl3_open(&audio, RATE) == PIXELICA_OK);
    CHECK(pixelica_audio_play(pixelica_audio_sdl3_mixer(&audio), &wav.clip, NULL, &voice) == PIXELICA_OK);
    CHECK(pixelica_audio_is_playing(pixelica_audio_sdl3_mixer(&audio), voice));
    CHECK(pixelica_audio_sdl3_update(&audio) == PIXELICA_OK);

    /* Unload stops the voice first, then frees and zeroes. */
    pixelica_audio_sdl3_unload_wav(&audio, &wav);
    CHECK(!pixelica_audio_is_playing(pixelica_audio_sdl3_mixer(&audio), voice));
    CHECK(wav.bytes == NULL && wav.size == 0u && wav.clip.samples == NULL);
    CHECK(pixelica_audio_sdl3_update(&audio) == PIXELICA_OK);
    pixelica_audio_sdl3_unload_wav(&audio, &wav); /* second unload: no-op */

    /* Unload needs neither an open nor a non-NULL adapter. */
    CHECK(pixelica_audio_sdl3_load_wav(&other, GOOD_PATH) == PIXELICA_OK);
    pixelica_audio_sdl3_close(&audio);
    pixelica_audio_sdl3_unload_wav(&audio, &other);
    CHECK(other.bytes == NULL);
    CHECK(pixelica_audio_sdl3_load_wav(&other, GOOD_PATH) == PIXELICA_OK);
    pixelica_audio_sdl3_unload_wav(NULL, &other);
    CHECK(other.bytes == NULL);

    CHECK(SDL_RemovePath(GOOD_PATH));
    CHECK(SDL_RemovePath(BAD_PATH));
}

int main(void)
{
    SDL_SetMainReady();
    /* SDL3 reads SDL_AUDIO_DRIVER; set the hint so the test never needs a speaker. */
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    test_invalid_args();
    test_lifecycle_and_refcount();
    test_pump();
    test_load_unload();

    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
    SDL_Quit();

    printf("pixelica_audio_sdl3_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

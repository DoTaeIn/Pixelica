/*
 * SDL3 output for the optional audio mixer (pixelica/audio.h).
 *
 * Owns one PIXELICA_AudioMixer, one SDL audio device stream and a pump that
 * keeps the stream filled from the game thread. There is no mix callback and no
 * mixer thread: every mixer call and pixelica_audio_sdl3_update() must come from
 * the same thread (normally the main/game loop). The header has no SDL types, so
 * it can be included without SDL headers; link Pixelica::audio_sdl3 (which links
 * SDL3::SDL3 and Pixelica::audio).
 *
 * Usage:
 *   PIXELICA_AudioSDL3 audio = {0};
 *   PIXELICA_AudioWav  boom  = {0};
 *   pixelica_audio_sdl3_open(&audio, 48000);
 *   pixelica_audio_sdl3_load_wav(&boom, "boom.wav");
 *   pixelica_audio_play(pixelica_audio_sdl3_mixer(&audio), &boom.clip, NULL, NULL);
 *   for (;;) {                                   // every frame, even when paused
 *       pixelica_audio_sdl3_update(&audio);
 *   }
 *   pixelica_audio_sdl3_unload_wav(&audio, &boom);
 *   pixelica_audio_sdl3_close(&audio);
 *
 * Output semantics: the pump renders stereo f32 at the mixer rate into the SDL
 * stream, which converts to the device format. Audio is kept queued to about
 * 50 ms of source audio; device/backend buffering adds further latency.
 * Volume, pan, pause, fade and stop
 * affect only audio mixed after the call; audio already queued cannot be
 * recalled. Call update every frame; a stall longer than the queued time
 * underruns (streaming/callback output is not provided).
 */
#ifndef PIXELICA_AUDIO_SDL3_H
#define PIXELICA_AUDIO_SDL3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/audio.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-owned. Zero-initialise (`= {0}`) before the first open; the fields are
 * implementation storage. Do not copy or move it while open: the mixer lives
 * inside. */
typedef struct PIXELICA_AudioSDL3 {
    PIXELICA_AudioMixer mixer;
    void *stream;        /* SDL_AudioStream*, NULL unless open */
    uint32_t sample_rate;
    bool open;
} PIXELICA_AudioSDL3;

/* A WAV file loaded with SDL_LoadFile. Zero-initialise before the first load.
 * `clip` borrows `bytes`; keep this struct (and so the bytes) alive while any
 * voice plays the clip, and release it only with pixelica_audio_sdl3_unload_wav. */
typedef struct PIXELICA_AudioWav {
    void *bytes;
    size_t size;
    PIXELICA_AudioClip clip;
} PIXELICA_AudioWav;

/*
 * Creates the mixer at `sample_rate` (8000..96000), takes one reference on SDL's
 * audio subsystem (SDL_InitSubSystem(SDL_INIT_AUDIO); video and the rest of SDL
 * are left alone) and opens the default playback device with an f32 stereo
 * stream at that rate. The device starts playing silence until update() queues
 * audio.
 *
 * PIXELICA_ERR_INVALID_ARG: NULL, already open (nothing modified) or a rate the
 * mixer rejects. PIXELICA_ERR_PLATFORM: SDL could not init audio or open a
 * device (SDL_GetError() has the reason); the SDL reference is released again.
 */
PIXELICA_Status pixelica_audio_sdl3_open(PIXELICA_AudioSDL3 *audio, uint32_t sample_rate);

/* Stops all voices, destroys the stream (closing its device), releases this
 * adapter's SDL audio reference and zeroes the struct. Safe on NULL, on a zeroed
 * struct and when already closed. */
void pixelica_audio_sdl3_close(PIXELICA_AudioSDL3 *audio);

/* The mixer to pass to the pixelica_audio_* functions, or NULL unless open. */
PIXELICA_AudioMixer *pixelica_audio_sdl3_mixer(PIXELICA_AudioSDL3 *audio);

/*
 * Game-thread pump. Reads the stream's queued bytes and, if less than ~50 ms is
 * queued, renders the difference in blocks of at most 512 frames, at most 8
 * blocks per call (no unbounded catch-up after a stall). Cheap when the queue is
 * full. PIXELICA_ERR_INVALID_ARG: NULL or not open. PIXELICA_ERR_PLATFORM: SDL
 * failed to report or accept data (e.g. the device was lost).
 */
PIXELICA_Status pixelica_audio_sdl3_update(PIXELICA_AudioSDL3 *audio);

/*
 * Reads `path` with SDL_LoadFile and parses it with pixelica_audio_clip_wav.
 * Transactional: on any failure `wav` is unchanged and nothing leaks.
 * PIXELICA_ERR_INVALID_ARG: NULL, or `wav` already holds a file (unload first).
 * PIXELICA_ERR_NOT_FOUND: the file could not be read. Otherwise the parser's
 * status (malformed or unsupported WAV).
 */
PIXELICA_Status pixelica_audio_sdl3_load_wav(PIXELICA_AudioWav *wav, const char *path);

/*
 * Stops every voice of `audio`'s mixer that plays this clip, then frees the file
 * bytes and zeroes `wav`. `audio` may be NULL or closed (nothing to stop). If
 * the clip is also playing on any OTHER mixer, stop those voices first (e.g.
 * pixelica_audio_stop_clip): the bytes are freed here. Samples the pump already
 * queued are copies and still play out. Safe on NULL or a zeroed `wav`.
 */
void pixelica_audio_sdl3_unload_wav(PIXELICA_AudioSDL3 *audio, PIXELICA_AudioWav *wav);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_AUDIO_SDL3_H */

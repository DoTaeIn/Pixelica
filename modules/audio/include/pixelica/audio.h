/* Optional, allocation-free PCM mixer shared by all renderer profiles. */
#ifndef PIXELICA_AUDIO_H
#define PIXELICA_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_AUDIO_MAX_VOICES 16u
#define PIXELICA_AUDIO_MIN_RATE 8000u
#define PIXELICA_AUDIO_MAX_RATE 96000u
#define PIXELICA_AUDIO_MAX_FADE_SECONDS 3600.0f

typedef enum PIXELICA_AudioBus {
    PIXELICA_AUDIO_SFX = 0,
    PIXELICA_AUDIO_MUSIC = 1
} PIXELICA_AudioBus;

typedef enum PIXELICA_AudioFormat {
    PIXELICA_AUDIO_U8 = 0,   /* unsigned 8-bit, 128 = silence */
    PIXELICA_AUDIO_S16LE = 1 /* signed 16-bit little-endian, any alignment */
} PIXELICA_AudioFormat;

/* Generation-safe handle. 0 is never a valid voice; a handle goes stale when
 * its voice ends or is stopped and is never reused within one initialized
 * mixer lifetime. Handles belong to one mixer; discard all on re-init. */
typedef uint64_t PIXELICA_AudioVoice;

/* Borrowed interleaved PCM. Valid clips have non-NULL samples, frame_count > 0,
 * sample_rate in [PIXELICA_AUDIO_MIN_RATE, PIXELICA_AUDIO_MAX_RATE], 1 or 2
 * channels and a known format. The struct is copied on play; the bytes at
 * `samples` must stay alive and unchanged until every voice using them has
 * stopped (see pixelica_audio_stop_clip). */
typedef struct PIXELICA_AudioClip {
    const uint8_t *samples;
    size_t frame_count;
    uint32_t sample_rate;
    uint8_t channels;
    PIXELICA_AudioFormat format;
} PIXELICA_AudioClip;

/* volume: finite [0,1]. pan: finite [-1,1], linear balance (0 keeps both
 * channels at full gain; +1 silences left, -1 silences right). */
typedef struct PIXELICA_AudioPlayOptions {
    PIXELICA_AudioBus bus;
    bool loop;
    float volume;
    float pan;
} PIXELICA_AudioPlayOptions;

/* Implementation storage; use the functions below. */
typedef struct PIXELICA_AudioVoiceSlot {
    PIXELICA_AudioClip clip;
    PIXELICA_AudioVoice id;  /* 0 = free */
    uint64_t frame;          /* integer source position */
    uint64_t step;           /* 32.32 source frames per output frame */
    uint32_t frac;           /* fractional source position (/2^32) */
    uint32_t fade_frames;    /* output frames left in the active fade */
    uint32_t fade_total;
    float volume;
    float pan;
    float fade_start;
    float fade_target;
    PIXELICA_AudioBus bus;
    bool loop;
    bool paused;
    bool fade_stop;
} PIXELICA_AudioVoiceSlot;

/* Caller-owned, call pixelica_audio_init before use (a zero-filled mixer is
 * treated as uninitialized and rejected). Fields are private by convention;
 * no layout/serialization promise. Single-threaded: do not call from an audio
 * callback/thread while another thread uses the same mixer. */
typedef struct PIXELICA_AudioMixer {
    PIXELICA_AudioVoiceSlot voices[PIXELICA_AUDIO_MAX_VOICES];
    uint64_t next_serial;
    PIXELICA_AudioVoice music;
    uint32_t sample_rate;
    float master_volume;
    float bus_volume[2];
} PIXELICA_AudioMixer;

/* Output rate in [PIXELICA_AUDIO_MIN_RATE, PIXELICA_AUDIO_MAX_RATE]. Resets all
 * voices and gains to 1. Accepts fresh, uninitialized storage. Re-init starts
 * a new handle lifetime: discard all previous IDs. On error untouched. */
PIXELICA_Status pixelica_audio_init(PIXELICA_AudioMixer *mixer, uint32_t sample_rate);

/* Bounds-checked RIFF/WAVE parser: PCM format 1, 8/16-bit, mono/stereo.
 * No allocation or copy: out->samples points into `bytes`, which must outlive
 * the clip's voices. Malformed/unsupported data -> PIXELICA_ERR_BAD_ASSET and
 * *out untouched. */
PIXELICA_Status pixelica_audio_clip_wav(PIXELICA_AudioClip *out, const void *bytes,
    size_t size);

/* SFX bus, no loop, volume 1, pan 0. */
PIXELICA_AudioPlayOptions pixelica_audio_default_options(void);

/* Starts a voice (NULL options = defaults). All 16 voices live ->
 * PIXELICA_ERR_CAPACITY (nothing is stolen). `out` may be NULL; on failure a
 * non-NULL *out is set to 0. */
PIXELICA_Status pixelica_audio_play(PIXELICA_AudioMixer *mixer, const PIXELICA_AudioClip *clip,
    const PIXELICA_AudioPlayOptions *options, PIXELICA_AudioVoice *out);
/* Looping MUSIC-bus voice (volume 1, pan 0) that replaces the previous music
 * voice started by this function, reusing its slot. An invalid clip or a full
 * mixer leaves the old music playing. */
PIXELICA_Status pixelica_audio_play_music(PIXELICA_AudioMixer *mixer,
    const PIXELICA_AudioClip *clip, PIXELICA_AudioVoice *out);

/* True for live voices, including paused ones. */
bool pixelica_audio_is_playing(const PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice);
/* Stale/unknown handles and NULL are ignored. */
void pixelica_audio_stop(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice);
void pixelica_audio_stop_all(PIXELICA_AudioMixer *mixer);
/* Stops every voice whose clip uses the same `samples` pointer. Call before
 * freeing clip bytes. */
void pixelica_audio_stop_clip(PIXELICA_AudioMixer *mixer, const PIXELICA_AudioClip *clip);

/* Voice controls: bad values -> PIXELICA_ERR_INVALID_ARG; a handle that does
 * not name a live voice -> PIXELICA_ERR_NOT_FOUND. Paused voices keep their
 * position and freeze their fade. set_volume cancels an active fade. */
PIXELICA_Status pixelica_audio_pause(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice,
    bool paused);
PIXELICA_Status pixelica_audio_set_volume(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice,
    float volume);
PIXELICA_Status pixelica_audio_set_pan(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice,
    float pan);
/* Linear ramp to target_volume over round(seconds * rate) output frames,
 * seconds in [0, PIXELICA_AUDIO_MAX_FADE_SECONDS]; 0 applies immediately.
 * stop_when_done stops the voice when the ramp completes. */
PIXELICA_Status pixelica_audio_fade(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice,
    float target_volume, float seconds, bool stop_when_done);

/* Gains in [0,1]; final sample = clamp(master * sum(voice * bus * pan), -1, 1). */
PIXELICA_Status pixelica_audio_set_master_volume(PIXELICA_AudioMixer *mixer, float volume);
PIXELICA_Status pixelica_audio_set_bus_volume(PIXELICA_AudioMixer *mixer, PIXELICA_AudioBus bus,
    float volume);

/* Overwrites `frames` interleaved stereo frames at the mixer rate (silence when
 * idle) and advances voices. frames = 0 accepts NULL. */
PIXELICA_Status pixelica_audio_render(PIXELICA_AudioMixer *mixer, float *stereo, size_t frames);
/* Same mix as native-endian int16 (scaled by 32767, rounded); no allocation. */
PIXELICA_Status pixelica_audio_render_s16(PIXELICA_AudioMixer *mixer, int16_t *stereo,
    size_t frames);

#ifdef __cplusplus
}
#endif
#endif

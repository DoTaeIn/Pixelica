#include "pixelica/audio_sdl3.h"

#include <SDL3/SDL.h>

#include <string.h>

/* The pump keeps about TARGET_MS of audio queued in SDL's stream. One call renders
 * at most MAX_BLOCKS blocks of BLOCK_FRAMES frames, so a long stall is never
 * "caught up" in one frame: the device underruns instead. */
#define AUDIO_TARGET_MS 50u
#define AUDIO_BLOCK_FRAMES 512u
#define AUDIO_MAX_BLOCKS 8u
#define AUDIO_FRAME_BYTES (2u * (unsigned)sizeof(float))

PIXELICA_Status pixelica_audio_sdl3_open(PIXELICA_AudioSDL3 *audio, uint32_t sample_rate)
{
    SDL_AudioSpec spec;
    SDL_AudioStream *stream;
    PIXELICA_Status status;

    if (audio == NULL || audio->open) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(audio, 0, sizeof(*audio));
    status = pixelica_audio_init(&audio->mixer, sample_rate);
    if (status != PIXELICA_OK) {
        memset(audio, 0, sizeof(*audio));
        return status;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        memset(audio, 0, sizeof(*audio));
        return PIXELICA_ERR_PLATFORM;
    }
    SDL_zero(spec);
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = (int)sample_rate;
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (stream == NULL) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        memset(audio, 0, sizeof(*audio));
        return PIXELICA_ERR_PLATFORM;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        memset(audio, 0, sizeof(*audio));
        return PIXELICA_ERR_PLATFORM;
    }

    audio->stream = stream;
    audio->sample_rate = sample_rate;
    audio->open = true;
    return PIXELICA_OK;
}

void pixelica_audio_sdl3_close(PIXELICA_AudioSDL3 *audio)
{
    if (audio == NULL || !audio->open) {
        return;
    }
    pixelica_audio_stop_all(&audio->mixer);
    SDL_DestroyAudioStream((SDL_AudioStream *)audio->stream);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    memset(audio, 0, sizeof(*audio));
}

PIXELICA_AudioMixer *pixelica_audio_sdl3_mixer(PIXELICA_AudioSDL3 *audio)
{
    return (audio != NULL && audio->open) ? &audio->mixer : NULL;
}

PIXELICA_Status pixelica_audio_sdl3_update(PIXELICA_AudioSDL3 *audio)
{
    SDL_AudioStream *stream;
    float block[AUDIO_BLOCK_FRAMES * 2u];
    size_t target;
    size_t queued_frames;
    int queued;

    if (audio == NULL || !audio->open) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    stream = (SDL_AudioStream *)audio->stream;
    queued = SDL_GetAudioStreamQueued(stream);
    if (queued < 0) {
        return PIXELICA_ERR_PLATFORM;
    }
    queued_frames = (size_t)queued / AUDIO_FRAME_BYTES;
    target = (size_t)audio->sample_rate * AUDIO_TARGET_MS / 1000u;

    for (unsigned i = 0u; i < AUDIO_MAX_BLOCKS && queued_frames < target; ++i) {
        size_t frames = target - queued_frames;
        PIXELICA_Status status;

        if (frames > AUDIO_BLOCK_FRAMES) {
            frames = AUDIO_BLOCK_FRAMES;
        }
        status = pixelica_audio_render(&audio->mixer, block, frames);
        if (status != PIXELICA_OK) {
            return status;
        }
        if (!SDL_PutAudioStreamData(stream, block, (int)(frames * AUDIO_FRAME_BYTES))) {
            return PIXELICA_ERR_PLATFORM;
        }
        queued_frames += frames;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_audio_sdl3_load_wav(PIXELICA_AudioWav *wav, const char *path)
{
    PIXELICA_AudioClip clip;
    PIXELICA_Status status;
    void *bytes;
    size_t size = 0u;

    if (wav == NULL || path == NULL || wav->bytes != NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    bytes = SDL_LoadFile(path, &size);
    if (bytes == NULL) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    memset(&clip, 0, sizeof(clip));
    status = pixelica_audio_clip_wav(&clip, bytes, size);
    if (status != PIXELICA_OK) {
        SDL_free(bytes);
        return status;
    }
    wav->bytes = bytes;
    wav->size = size;
    wav->clip = clip;
    return PIXELICA_OK;
}

void pixelica_audio_sdl3_unload_wav(PIXELICA_AudioSDL3 *audio, PIXELICA_AudioWav *wav)
{
    if (wav == NULL) {
        return;
    }
    if (wav->bytes != NULL) {
        if (audio != NULL && audio->open) {
            pixelica_audio_stop_clip(&audio->mixer, &wav->clip);
        }
        SDL_free(wav->bytes);
    }
    memset(wav, 0, sizeof(*wav));
}

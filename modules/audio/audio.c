/*
 * Pixelica - optional audio mixer. See pixelica/audio.h for the contract.
 * Allocation-free and renderer-independent: voices borrow caller PCM, advance
 * in 32.32 fixed point (linear interpolation, deterministic), and render()
 * overwrites an interleaved stereo float block. The WAV parser only reads bytes
 * (never casts to structs or int16 pointers) and points into the caller buffer.
 */
#include "pixelica/audio.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Handle = serial << 4 | slot, so 0 is never issued and stale IDs never match. */
#define AU_SLOT_BITS 4u
#define AU_SLOT_MASK ((uint64_t)PIXELICA_AUDIO_MAX_VOICES - 1u)
#define AU_MAX_SERIAL (UINT64_MAX >> AU_SLOT_BITS)
#define AU_MAX_STEP (((uint64_t)PIXELICA_AUDIO_MAX_RATE << 32) / PIXELICA_AUDIO_MIN_RATE)
/* render_s16 mixes through a small stack block (512 bytes). */
#define AU_S16_BLOCK 64u

/* Also false for NaN and infinities. */
static bool au_in_range(float v, float lo, float hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

static bool au_rate_valid(uint32_t rate)
{
    return rate >= PIXELICA_AUDIO_MIN_RATE && rate <= PIXELICA_AUDIO_MAX_RATE;
}

static bool au_clip_valid(const PIXELICA_AudioClip *clip)
{
    if (clip == NULL || clip->samples == NULL || clip->frame_count == 0u ||
        !au_rate_valid(clip->sample_rate)) {
        return false;
    }
    if (clip->channels != 1u && clip->channels != 2u) {
        return false;
    }
    if ((int)clip->format != (int)PIXELICA_AUDIO_U8 &&
        (int)clip->format != (int)PIXELICA_AUDIO_S16LE) {
        return false;
    }
    /* At most 4 bytes per frame: bounds the byte size and position math. */
    return clip->frame_count <= SIZE_MAX / 4u;
}

static bool au_mixer_ready(const PIXELICA_AudioMixer *mixer)
{
    return mixer != NULL && au_rate_valid(mixer->sample_rate);
}

static void au_free(PIXELICA_AudioVoiceSlot *v)
{
    memset(v, 0, sizeof *v);
    v->clip.samples = NULL;
    v->id = 0u;
}

static PIXELICA_AudioVoiceSlot *au_find(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice id)
{
    PIXELICA_AudioVoiceSlot *v;

    if (!au_mixer_ready(mixer) || id == 0u) {
        return NULL;
    }
    v = &mixer->voices[(size_t)(id & AU_SLOT_MASK)];
    return v->id == id ? v : NULL;
}

static size_t au_free_slot(const PIXELICA_AudioMixer *mixer)
{
    size_t i;

    for (i = 0; i < PIXELICA_AUDIO_MAX_VOICES; ++i) {
        if (mixer->voices[i].id == 0u) {
            return i;
        }
    }
    return PIXELICA_AUDIO_MAX_VOICES;
}

/* Arguments are validated by the caller. Returns the new handle. */
static PIXELICA_AudioVoice au_start(PIXELICA_AudioMixer *mixer, size_t slot,
    const PIXELICA_AudioClip *clip, PIXELICA_AudioBus bus, bool loop, float volume, float pan)
{
    PIXELICA_AudioClip copy = *clip; /* clip may alias the slot being reset */
    PIXELICA_AudioVoiceSlot *v = &mixer->voices[slot];
    uint64_t serial = mixer->next_serial;

    au_free(v);
    v->clip = copy;
    v->id = (serial << AU_SLOT_BITS) | (uint64_t)slot;
    v->step = ((uint64_t)copy.sample_rate << 32) / (uint64_t)mixer->sample_rate;
    v->volume = volume;
    v->pan = pan;
    v->fade_target = volume;
    v->bus = bus;
    v->loop = loop;
    mixer->next_serial = serial + 1u;
    return v->id;
}

PIXELICA_Status pixelica_audio_init(PIXELICA_AudioMixer *mixer, uint32_t sample_rate)
{
    if (mixer == NULL || !au_rate_valid(sample_rate)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* init accepts fresh stack storage: never read its previous contents. */
    memset(mixer, 0, sizeof *mixer);
    mixer->next_serial = 1u;
    mixer->music = 0u;
    mixer->sample_rate = sample_rate;
    mixer->master_volume = 1.0f;
    mixer->bus_volume[0] = 1.0f;
    mixer->bus_volume[1] = 1.0f;
    return PIXELICA_OK;
}

/* ---- WAV ---------------------------------------------------------------- */

static unsigned au_le16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint32_t au_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

PIXELICA_Status pixelica_audio_clip_wav(PIXELICA_AudioClip *out, const void *bytes, size_t size)
{
    const uint8_t *p = (const uint8_t *)bytes;
    const uint8_t *fmt = NULL;
    const uint8_t *data = NULL;
    size_t data_size = 0u;
    size_t pos;
    size_t end;
    uint32_t riff;
    unsigned channels, bits, block_align;
    uint32_t rate, byte_rate;
    PIXELICA_AudioClip clip;

    if (out == NULL || bytes == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (size < 12u || memcmp(p, "RIFF", 4u) != 0 || memcmp(p + 8, "WAVE", 4u) != 0) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    /* Honor the declared RIFF extent; bytes after it are ignored. */
    riff = au_le32(p + 4);
    if (riff < 4u || (uint64_t)riff > (uint64_t)size - 8u) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    end = (size_t)riff + 8u;
    pos = 12u;
    while (pos < end) {
        size_t chunk;

        if (end - pos < 8u) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        chunk = (size_t)au_le32(p + pos + 4u);
        if (chunk > end - pos - 8u) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        if (memcmp(p + pos, "fmt ", 4u) == 0) {
            if (fmt != NULL || chunk < 16u) {
                return PIXELICA_ERR_BAD_ASSET;
            }
            fmt = p + pos + 8u;
        } else if (memcmp(p + pos, "data", 4u) == 0) {
            if (data != NULL) {
                return PIXELICA_ERR_BAD_ASSET;
            }
            data = p + pos + 8u;
            data_size = chunk;
        }
        pos += 8u + chunk;
        /* Odd chunks are padded; a missing pad byte is tolerated at the end. */
        if ((chunk & 1u) != 0u && pos < end) {
            ++pos;
        }
    }
    if (fmt == NULL || data == NULL) {
        return PIXELICA_ERR_BAD_ASSET;
    }

    channels = au_le16(fmt + 2);
    rate = au_le32(fmt + 4);
    byte_rate = au_le32(fmt + 8);
    block_align = au_le16(fmt + 12);
    bits = au_le16(fmt + 14);
    if (au_le16(fmt) != 1u || (channels != 1u && channels != 2u) ||
        (bits != 8u && bits != 16u) || !au_rate_valid(rate)) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (block_align != channels * (bits / 8u) || byte_rate != rate * (uint32_t)block_align) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (data_size == 0u || data_size % block_align != 0u) {
        return PIXELICA_ERR_BAD_ASSET;
    }

    memset(&clip, 0, sizeof clip);
    clip.samples = data;
    clip.frame_count = data_size / block_align;
    clip.sample_rate = rate;
    clip.channels = (uint8_t)channels;
    clip.format = bits == 8u ? PIXELICA_AUDIO_U8 : PIXELICA_AUDIO_S16LE;
    if (!au_clip_valid(&clip)) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    *out = clip;
    return PIXELICA_OK;
}

/* ---- playback control ---------------------------------------------------- */

PIXELICA_AudioPlayOptions pixelica_audio_default_options(void)
{
    PIXELICA_AudioPlayOptions o;

    memset(&o, 0, sizeof o);
    o.bus = PIXELICA_AUDIO_SFX;
    o.loop = false;
    o.volume = 1.0f;
    o.pan = 0.0f;
    return o;
}

static bool au_bus_valid(PIXELICA_AudioBus bus)
{
    return (int)bus == (int)PIXELICA_AUDIO_SFX || (int)bus == (int)PIXELICA_AUDIO_MUSIC;
}

PIXELICA_Status pixelica_audio_play(PIXELICA_AudioMixer *mixer, const PIXELICA_AudioClip *clip,
    const PIXELICA_AudioPlayOptions *options, PIXELICA_AudioVoice *out)
{
    PIXELICA_AudioPlayOptions o =
        options != NULL ? *options : pixelica_audio_default_options();
    PIXELICA_AudioVoice id;
    size_t slot;

    if (out != NULL) {
        *out = 0u;
    }
    if (!au_mixer_ready(mixer) || !au_clip_valid(clip) || !au_bus_valid(o.bus) ||
        !au_in_range(o.volume, 0.0f, 1.0f) || !au_in_range(o.pan, -1.0f, 1.0f)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    slot = au_free_slot(mixer);
    if (slot >= PIXELICA_AUDIO_MAX_VOICES || mixer->next_serial == 0u ||
        mixer->next_serial > AU_MAX_SERIAL) {
        return PIXELICA_ERR_CAPACITY;
    }
    id = au_start(mixer, slot, clip, o.bus, o.loop, o.volume, o.pan);
    if (out != NULL) {
        *out = id;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_audio_play_music(PIXELICA_AudioMixer *mixer,
    const PIXELICA_AudioClip *clip, PIXELICA_AudioVoice *out)
{
    PIXELICA_AudioVoiceSlot *old;
    size_t slot;

    if (out != NULL) {
        *out = 0u;
    }
    if (!au_mixer_ready(mixer) || !au_clip_valid(clip)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    old = au_find(mixer, mixer->music);
    slot = old != NULL ? (size_t)(old - mixer->voices) : au_free_slot(mixer);
    if (slot >= PIXELICA_AUDIO_MAX_VOICES || mixer->next_serial == 0u ||
        mixer->next_serial > AU_MAX_SERIAL) {
        return PIXELICA_ERR_CAPACITY;
    }
    mixer->music = au_start(mixer, slot, clip, PIXELICA_AUDIO_MUSIC, true, 1.0f, 0.0f);
    if (out != NULL) {
        *out = mixer->music;
    }
    return PIXELICA_OK;
}

bool pixelica_audio_is_playing(const PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice)
{
    return au_find((PIXELICA_AudioMixer *)mixer, voice) != NULL;
}

void pixelica_audio_stop(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice)
{
    PIXELICA_AudioVoiceSlot *v = au_find(mixer, voice);

    if (v != NULL) {
        au_free(v);
    }
}

void pixelica_audio_stop_all(PIXELICA_AudioMixer *mixer)
{
    size_t i;

    if (mixer == NULL) {
        return;
    }
    for (i = 0; i < PIXELICA_AUDIO_MAX_VOICES; ++i) {
        au_free(&mixer->voices[i]);
    }
    mixer->music = 0u;
}

void pixelica_audio_stop_clip(PIXELICA_AudioMixer *mixer, const PIXELICA_AudioClip *clip)
{
    const uint8_t *samples;
    size_t i;

    if (mixer == NULL || clip == NULL || clip->samples == NULL) {
        return;
    }
    samples = clip->samples; /* clip may point into a slot cleared below */
    for (i = 0; i < PIXELICA_AUDIO_MAX_VOICES; ++i) {
        if (mixer->voices[i].id != 0u && mixer->voices[i].clip.samples == samples) {
            au_free(&mixer->voices[i]);
        }
    }
}

PIXELICA_Status pixelica_audio_pause(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice,
    bool paused)
{
    PIXELICA_AudioVoiceSlot *v;

    if (!au_mixer_ready(mixer)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    v = au_find(mixer, voice);
    if (v == NULL) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    v->paused = paused;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_audio_set_volume(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice,
    float volume)
{
    PIXELICA_AudioVoiceSlot *v;

    if (!au_mixer_ready(mixer) || !au_in_range(volume, 0.0f, 1.0f)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    v = au_find(mixer, voice);
    if (v == NULL) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    v->volume = volume;
    v->fade_target = volume;
    v->fade_frames = 0u;
    v->fade_stop = false;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_audio_set_pan(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice,
    float pan)
{
    PIXELICA_AudioVoiceSlot *v;

    if (!au_mixer_ready(mixer) || !au_in_range(pan, -1.0f, 1.0f)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    v = au_find(mixer, voice);
    if (v == NULL) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    v->pan = pan;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_audio_fade(PIXELICA_AudioMixer *mixer, PIXELICA_AudioVoice voice,
    float target_volume, float seconds, bool stop_when_done)
{
    PIXELICA_AudioVoiceSlot *v;
    uint32_t frames;

    if (!au_mixer_ready(mixer) || !au_in_range(target_volume, 0.0f, 1.0f) ||
        !au_in_range(seconds, 0.0f, PIXELICA_AUDIO_MAX_FADE_SECONDS)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    v = au_find(mixer, voice);
    if (v == NULL) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    /* Rounded (truncation of a non-negative value); <= 3600 s * 96 kHz fits. */
    frames = (uint32_t)((double)seconds * (double)mixer->sample_rate + 0.5);
    v->fade_target = target_volume;
    v->fade_start = v->volume;
    v->fade_stop = stop_when_done;
    if (frames == 0u) {
        v->volume = target_volume;
        v->fade_frames = 0u;
        if (stop_when_done) {
            au_free(v);
        }
        return PIXELICA_OK;
    }
    v->fade_frames = frames;
    v->fade_total = frames;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_audio_set_master_volume(PIXELICA_AudioMixer *mixer, float volume)
{
    if (!au_mixer_ready(mixer) || !au_in_range(volume, 0.0f, 1.0f)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    mixer->master_volume = volume;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_audio_set_bus_volume(PIXELICA_AudioMixer *mixer, PIXELICA_AudioBus bus,
    float volume)
{
    if (!au_mixer_ready(mixer) || !au_bus_valid(bus) || !au_in_range(volume, 0.0f, 1.0f)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    mixer->bus_volume[(int)bus == (int)PIXELICA_AUDIO_MUSIC ? 1 : 0] = volume;
    return PIXELICA_OK;
}

/* ---- mixing -------------------------------------------------------------- */

/* `sample` counts samples (frame * channels + channel); reads bytes only. */
static float au_decode(const PIXELICA_AudioClip *clip, size_t sample)
{
    if ((int)clip->format == (int)PIXELICA_AUDIO_U8) {
        return ((float)clip->samples[sample] - 128.0f) * (1.0f / 128.0f);
    } else {
        const uint8_t *p = clip->samples + sample * 2u;
        int32_t s = (int32_t)au_le16(p);

        if (s >= 32768) {
            s -= 65536;
        }
        return (float)s * (1.0f / 32768.0f);
    }
}

static void au_frame(const PIXELICA_AudioClip *clip, uint64_t frame, float *l, float *r)
{
    size_t base = (size_t)frame * (size_t)clip->channels;

    *l = au_decode(clip, base);
    *r = clip->channels == 2u ? au_decode(clip, base + 1u) : *l;
}

/* Rejects slots whose storage was modified to unsafe values. */
static bool au_voice_sane(const PIXELICA_AudioVoiceSlot *v)
{
    return au_clip_valid(&v->clip) && v->frame < (uint64_t)v->clip.frame_count &&
           v->step != 0u && v->step <= AU_MAX_STEP && au_in_range(v->volume, 0.0f, 1.0f) &&
           au_in_range(v->fade_target, 0.0f, 1.0f) && au_in_range(v->pan, -1.0f, 1.0f) &&
           (v->fade_frames == 0u || (v->fade_total >= v->fade_frames &&
                                    au_in_range(v->fade_start, 0.0f, 1.0f)));
}

static void au_mix_voice(PIXELICA_AudioVoiceSlot *v, float bus, float *out, size_t frames)
{
    const uint64_t count = (uint64_t)v->clip.frame_count;
    float lg = v->pan > 0.0f ? 1.0f - v->pan : 1.0f;
    float rg = v->pan < 0.0f ? 1.0f + v->pan : 1.0f;
    size_t i;

    for (i = 0; i < frames; ++i) {
        float l, r, gain;
        uint64_t pos;

        au_frame(&v->clip, v->frame, &l, &r);
        if (v->frac != 0u) {
            uint64_t next = v->frame + 1u;
            float t = (float)v->frac * (1.0f / 4294967296.0f);
            float l2, r2;

            if (next >= count) {
                next = v->loop ? 0u : v->frame;
            }
            au_frame(&v->clip, next, &l2, &r2);
            l += (l2 - l) * t;
            r += (r2 - r) * t;
        }
        gain = v->volume * bus;
        out[2u * i] += l * gain * lg;
        out[2u * i + 1u] += r * gain * rg;

        /* Derive from elapsed samples: tiny float increments must not accumulate drift. */
        if (v->fade_frames != 0u) {
            if (--v->fade_frames == 0u) {
                v->volume = v->fade_target;
                if (v->fade_stop) {
                    au_free(v);
                    return;
                }
            } else {
                float t = (float)(v->fade_total - v->fade_frames) / (float)v->fade_total;
                v->volume = v->fade_start + (v->fade_target - v->fade_start) * t;
            }
        }

        pos = (uint64_t)v->frac + v->step;
        v->frame += pos >> 32;
        v->frac = (uint32_t)(pos & 0xFFFFFFFFu);
        if (v->frame >= count) {
            if (!v->loop) {
                au_free(v);
                return;
            }
            v->frame %= count;
        }
    }
}

static float au_clamp(float x)
{
    if (isnan(x)) {
        return 0.0f;
    }
    return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
}

/* Out-of-range gains (only possible through direct field writes) mute. */
static float au_gain(float g)
{
    return au_in_range(g, 0.0f, 1.0f) ? g : 0.0f;
}

static void au_render_block(PIXELICA_AudioMixer *mixer, float *out, size_t frames)
{
    float master = au_gain(mixer->master_volume);
    float sfx = au_gain(mixer->bus_volume[0]);
    float music = au_gain(mixer->bus_volume[1]);
    size_t i;

    for (i = 0; i < 2u * frames; ++i) {
        out[i] = 0.0f;
    }
    for (i = 0; i < PIXELICA_AUDIO_MAX_VOICES; ++i) {
        PIXELICA_AudioVoiceSlot *v = &mixer->voices[i];

        if (v->id == 0u || v->paused) {
            continue;
        }
        if (!au_voice_sane(v)) {
            au_free(v);
            continue;
        }
        au_mix_voice(v, (int)v->bus == (int)PIXELICA_AUDIO_MUSIC ? music : sfx, out, frames);
    }
    for (i = 0; i < 2u * frames; ++i) {
        out[i] = au_clamp(out[i] * master);
    }
}

PIXELICA_Status pixelica_audio_render(PIXELICA_AudioMixer *mixer, float *stereo, size_t frames)
{
    if (!au_mixer_ready(mixer)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (frames == 0u) {
        return PIXELICA_OK;
    }
    if (stereo == NULL || frames > SIZE_MAX / (2u * sizeof(float))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    au_render_block(mixer, stereo, frames);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_audio_render_s16(PIXELICA_AudioMixer *mixer, int16_t *stereo,
    size_t frames)
{
    float block[2u * AU_S16_BLOCK];

    if (!au_mixer_ready(mixer)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (frames == 0u) {
        return PIXELICA_OK;
    }
    if (stereo == NULL || frames > SIZE_MAX / (2u * sizeof(int16_t))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    while (frames > 0u) {
        size_t n = frames < AU_S16_BLOCK ? frames : AU_S16_BLOCK;
        size_t i;

        au_render_block(mixer, block, n);
        for (i = 0; i < 2u * n; ++i) {
            float x = block[i] * 32767.0f;

            stereo[i] = (int16_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);
        }
        stereo += 2u * n;
        frames -= n;
    }
    return PIXELICA_OK;
}

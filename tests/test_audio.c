/*
 * Pixelica - optional audio mixer: init/NULL safety, clip validation, WAV
 * parsing (padding, unknown chunks, RIFF extent, truncation, malformed and
 * ambiguous chunks, bounded random mutations), PCM decoding, loop/end
 * boundaries, linear resampling, pan/balance, clipping, master/bus gains,
 * capacity and music replacement, stale handles, pause/fade timing and the
 * f32/s16 outputs.
 *
 * Uses only the common headers plus pixelica/audio.h, so it builds and runs
 * with every renderer disabled.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/audio.h"
#include "pixelica/status.h"

#include "test_util.h"

#define RATE 8000u

/* ---- helpers ------------------------------------------------------------ */

typedef struct Wav {
    uint8_t b[512];
    size_t n;
} Wav;

static void w_u8(Wav *w, unsigned v)
{
    if (w->n < sizeof w->b) {
        w->b[w->n] = (uint8_t)(v & 0xFFu);
    }
    w->n++;
}

static void w_u16(Wav *w, unsigned v)
{
    w_u8(w, v);
    w_u8(w, v >> 8);
}

static void w_u32(Wav *w, uint32_t v)
{
    w_u16(w, (unsigned)(v & 0xFFFFu));
    w_u16(w, (unsigned)(v >> 16));
}

static void w_tag(Wav *w, const char *tag)
{
    size_t i;

    for (i = 0; i < 4u; ++i) {
        w_u8(w, (unsigned char)tag[i]);
    }
}

static void w_begin(Wav *w)
{
    memset(w, 0, sizeof *w);
    w_tag(w, "RIFF");
    w_u32(w, 0u); /* patched by w_end */
    w_tag(w, "WAVE");
}

/* fmt chunk with explicit fields so tests can corrupt each one. */
static void w_fmt_raw(Wav *w, unsigned format, unsigned channels, uint32_t rate,
                      uint32_t byte_rate, unsigned block_align, unsigned bits)
{
    w_tag(w, "fmt ");
    w_u32(w, 16u);
    w_u16(w, format);
    w_u16(w, channels);
    w_u32(w, rate);
    w_u32(w, byte_rate);
    w_u16(w, block_align);
    w_u16(w, bits);
}

static void w_fmt(Wav *w, unsigned channels, unsigned bits, uint32_t rate)
{
    unsigned align = channels * (bits / 8u);

    w_fmt_raw(w, 1u, channels, rate, rate * align, align, bits);
}

/* Chunk with payload and the RIFF pad byte when odd. */
static void w_chunk(Wav *w, const char *tag, const uint8_t *data, size_t size, bool pad)
{
    size_t i;

    w_tag(w, tag);
    w_u32(w, (uint32_t)size);
    for (i = 0; i < size; ++i) {
        w_u8(w, data[i]);
    }
    if (pad && (size & 1u) != 0u) {
        w_u8(w, 0u);
    }
}

static void w_end(Wav *w)
{
    uint32_t riff = (uint32_t)(w->n - 8u);

    w->b[4] = (uint8_t)(riff & 0xFFu);
    w->b[5] = (uint8_t)((riff >> 8) & 0xFFu);
    w->b[6] = (uint8_t)((riff >> 16) & 0xFFu);
    w->b[7] = (uint8_t)(riff >> 24);
}

/* Little-endian int16 PCM bytes. */
static void s16_bytes(uint8_t *dst, const int16_t *src, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        uint16_t u = (uint16_t)src[i];

        dst[2u * i] = (uint8_t)(u & 0xFFu);
        dst[2u * i + 1u] = (uint8_t)(u >> 8);
    }
}

static PIXELICA_AudioClip mk_clip(const uint8_t *samples, size_t frames, uint32_t rate,
                                  uint8_t channels, PIXELICA_AudioFormat format)
{
    PIXELICA_AudioClip c;

    memset(&c, 0, sizeof c);
    c.samples = samples;
    c.frame_count = frames;
    c.sample_rate = rate;
    c.channels = channels;
    c.format = format;
    return c;
}

static PIXELICA_AudioPlayOptions mk_opts(PIXELICA_AudioBus bus, bool loop, float volume,
                                         float pan)
{
    PIXELICA_AudioPlayOptions o = pixelica_audio_default_options();

    o.bus = bus;
    o.loop = loop;
    o.volume = volume;
    o.pan = pan;
    return o;
}

/* Mono S16 DC clips: value/32768 is exact. */
static uint8_t g_half[8];         /* 16384 -> 0.5 */
static uint8_t g_three_q[8];      /* 24576 -> 0.75 */
static uint8_t g_neg_three_q[8];  /* -24576 -> -0.75 */
static uint8_t g_stereo_pm[16];   /* L 0.5, R -0.5 */

static void init_fixtures(void)
{
    int16_t v[8];
    size_t i;

    for (i = 0; i < 4u; ++i) {
        v[i] = 16384;
    }
    s16_bytes(g_half, v, 4u);
    for (i = 0; i < 4u; ++i) {
        v[i] = 24576;
    }
    s16_bytes(g_three_q, v, 4u);
    for (i = 0; i < 4u; ++i) {
        v[i] = -24576;
    }
    s16_bytes(g_neg_three_q, v, 4u);
    for (i = 0; i < 8u; i += 2u) {
        v[i] = 16384;
        v[i + 1u] = -16384;
    }
    s16_bytes(g_stereo_pm, v, 8u);
}

static PIXELICA_AudioClip dc_clip(const uint8_t *bytes)
{
    return mk_clip(bytes, 4u, RATE, 1u, PIXELICA_AUDIO_S16LE);
}

static void init_mixer(PIXELICA_AudioMixer *m)
{
    memset(m, 0, sizeof *m);
    CHECK(pixelica_audio_init(m, RATE) == PIXELICA_OK);
}

/* Renders one frame and returns the left/right values. */
static void render1(PIXELICA_AudioMixer *m, float *l, float *r)
{
    float f[2] = {9.0f, 9.0f};

    CHECK(pixelica_audio_render(m, f, 1u) == PIXELICA_OK);
    *l = f[0];
    *r = f[1];
}

static int frame_is(PIXELICA_AudioMixer *m, float l, float r)
{
    float a, b;

    render1(m, &a, &b);
    return fabs((double)a - (double)l) <= 1e-6 && fabs((double)b - (double)r) <= 1e-6;
}

static int16_t to_s16(float x)
{
    float y = x * 32767.0f;

    return (int16_t)(y >= 0.0f ? y + 0.5f : y - 0.5f);
}

/* +-1 LSB: the compiler may contract the scale-and-round differently here. */
static int s16_near(int16_t a, int16_t b)
{
    int d = (int)a - (int)b;

    return d >= -1 && d <= 1;
}

/* ---- tests -------------------------------------------------------------- */

static void test_null_and_init(void)
{
    PIXELICA_AudioMixer m;
    PIXELICA_AudioMixer before;
    PIXELICA_AudioClip clip = dc_clip(g_half);
    PIXELICA_AudioVoice id = 77u;
    float f[4];
    int16_t s[4];
    size_t i;

    CHECK(pixelica_audio_init(NULL, RATE) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_play(NULL, &clip, NULL, &id) == PIXELICA_ERR_INVALID_ARG);
    CHECK(id == 0u);
    CHECK(pixelica_audio_play_music(NULL, &clip, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(!pixelica_audio_is_playing(NULL, 16u));
    pixelica_audio_stop(NULL, 16u);
    pixelica_audio_stop_all(NULL);
    pixelica_audio_stop_clip(NULL, &clip);
    CHECK(pixelica_audio_pause(NULL, 16u, true) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_volume(NULL, 16u, 1.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_pan(NULL, 16u, 0.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_fade(NULL, 16u, 0.0f, 1.0f, false) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_master_volume(NULL, 1.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_bus_volume(NULL, PIXELICA_AUDIO_SFX, 1.0f) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_render(NULL, f, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_render_s16(NULL, s, 1u) == PIXELICA_ERR_INVALID_ARG);

    /* A zero-filled mixer is uninitialized and rejected. */
    memset(&m, 0, sizeof m);
    CHECK(pixelica_audio_play(&m, &clip, NULL, &id) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_render(&m, f, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_master_volume(&m, 1.0f) == PIXELICA_ERR_INVALID_ARG);

    /* Rate range; a failed init leaves the mixer untouched. */
    memset(&m, 0xA5, sizeof m);
    memcpy(&before, &m, sizeof m);
    CHECK(pixelica_audio_init(&m, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_init(&m, PIXELICA_AUDIO_MIN_RATE - 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_init(&m, PIXELICA_AUDIO_MAX_RATE + 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_init(&m, UINT32_MAX) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&m, &before, sizeof m) == 0);
    CHECK(pixelica_audio_init(&m, PIXELICA_AUDIO_MIN_RATE) == PIXELICA_OK);
    CHECK(pixelica_audio_init(&m, PIXELICA_AUDIO_MAX_RATE) == PIXELICA_OK);

    /* Garbage storage is fully reset: idle output is silence. */
    memset(&m, 0xA5, sizeof m);
    CHECK(pixelica_audio_init(&m, 44100u) == PIXELICA_OK);
    CHECK(m.next_serial == 1u);
    m.next_serial = 37u;
    CHECK(pixelica_audio_init(&m, 44100u) == PIXELICA_OK);
    CHECK(m.next_serial == 1u);
    for (i = 0; i < PIXELICA_AUDIO_MAX_VOICES; ++i) {
        CHECK(m.voices[i].id == 0u);
    }
    for (i = 0; i < 4u; ++i) {
        f[i] = 5.0f;
        s[i] = 1234;
    }
    CHECK(pixelica_audio_render(&m, f, 2u) == PIXELICA_OK);
    CHECK(pixelica_audio_render_s16(&m, s, 2u) == PIXELICA_OK);
    for (i = 0; i < 4u; ++i) {
        CHECK_NEAR(f[i], 0.0, 0.0);
        CHECK(s[i] == 0);
    }

    /* frames = 0 accepts NULL; frames > 0 needs a buffer; overflow rejected. */
    CHECK(pixelica_audio_render(&m, NULL, 0u) == PIXELICA_OK);
    CHECK(pixelica_audio_render_s16(&m, NULL, 0u) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, NULL, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_render_s16(&m, NULL, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_render(&m, f, SIZE_MAX) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_render(&m, f, SIZE_MAX / 2u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_render_s16(&m, s, SIZE_MAX) == PIXELICA_ERR_INVALID_ARG);

    /* Defaults. */
    {
        PIXELICA_AudioPlayOptions o = pixelica_audio_default_options();

        CHECK(o.bus == PIXELICA_AUDIO_SFX);
        CHECK(!o.loop);
        CHECK_NEAR(o.volume, 1.0, 0.0);
        CHECK_NEAR(o.pan, 0.0, 0.0);
    }
}

static void test_clip_and_option_validation(void)
{
    static const uint8_t bytes[8] = {0};
    PIXELICA_AudioMixer m;
    PIXELICA_AudioClip bad[9];
    PIXELICA_AudioPlayOptions opt[8];
    PIXELICA_AudioClip good = mk_clip(bytes, 4u, RATE, 1u, PIXELICA_AUDIO_U8);
    PIXELICA_AudioVoice id;
    size_t i;

    init_mixer(&m);
    bad[0] = mk_clip(NULL, 4u, RATE, 1u, PIXELICA_AUDIO_U8);
    bad[1] = mk_clip(bytes, 0u, RATE, 1u, PIXELICA_AUDIO_U8);
    bad[2] = mk_clip(bytes, 4u, PIXELICA_AUDIO_MIN_RATE - 1u, 1u, PIXELICA_AUDIO_U8);
    bad[3] = mk_clip(bytes, 4u, PIXELICA_AUDIO_MAX_RATE + 1u, 1u, PIXELICA_AUDIO_U8);
    bad[4] = mk_clip(bytes, 4u, RATE, 0u, PIXELICA_AUDIO_U8);
    bad[5] = mk_clip(bytes, 4u, RATE, 3u, PIXELICA_AUDIO_U8);
    bad[6] = mk_clip(bytes, 4u, RATE, 1u, (PIXELICA_AudioFormat)2);
    bad[7] = mk_clip(bytes, SIZE_MAX, RATE, 2u, PIXELICA_AUDIO_S16LE); /* overflow */
    bad[8] = mk_clip(bytes, SIZE_MAX / 4u + 1u, RATE, 1u, PIXELICA_AUDIO_U8);
    for (i = 0; i < 9u; ++i) {
        id = 99u;
        CHECK(pixelica_audio_play(&m, &bad[i], NULL, &id) == PIXELICA_ERR_INVALID_ARG);
        CHECK(id == 0u);
        id = 99u;
        CHECK(pixelica_audio_play_music(&m, &bad[i], &id) == PIXELICA_ERR_INVALID_ARG);
        CHECK(id == 0u);
    }
    CHECK(pixelica_audio_play(&m, NULL, NULL, &id) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_play_music(&m, NULL, &id) == PIXELICA_ERR_INVALID_ARG);

    opt[0] = mk_opts((PIXELICA_AudioBus)2, false, 1.0f, 0.0f);
    opt[1] = mk_opts((PIXELICA_AudioBus)-1, false, 1.0f, 0.0f);
    opt[2] = mk_opts(PIXELICA_AUDIO_SFX, false, pixelica_t_nan(), 0.0f);
    opt[3] = mk_opts(PIXELICA_AUDIO_SFX, false, -0.01f, 0.0f);
    opt[4] = mk_opts(PIXELICA_AUDIO_SFX, false, 1.01f, 0.0f);
    opt[5] = mk_opts(PIXELICA_AUDIO_SFX, false, 1.0f, pixelica_t_inf());
    opt[6] = mk_opts(PIXELICA_AUDIO_SFX, false, 1.0f, -1.01f);
    opt[7] = mk_opts(PIXELICA_AUDIO_SFX, false, 1.0f, pixelica_t_nan());
    for (i = 0; i < 8u; ++i) {
        CHECK(pixelica_audio_play(&m, &good, &opt[i], &id) == PIXELICA_ERR_INVALID_ARG);
    }
    for (i = 0; i < PIXELICA_AUDIO_MAX_VOICES; ++i) {
        CHECK(m.voices[i].id == 0u);
    }

    /* Boundary values are accepted; out may be NULL. */
    opt[0] = mk_opts(PIXELICA_AUDIO_MUSIC, true, 0.0f, -1.0f);
    CHECK(pixelica_audio_play(&m, &good, &opt[0], NULL) == PIXELICA_OK);
    good.sample_rate = PIXELICA_AUDIO_MAX_RATE;
    good.channels = 2u;
    good.frame_count = 2u;
    good.format = PIXELICA_AUDIO_S16LE;
    CHECK(pixelica_audio_play(&m, &good, NULL, &id) == PIXELICA_OK);
    CHECK(id != 0u && pixelica_audio_is_playing(&m, id));
}

static void test_wav_parse(void)
{
    static const uint8_t u8data[5] = {0u, 64u, 128u, 192u, 255u};
    static const uint8_t junk[3] = {1u, 2u, 3u};
    int16_t s16[4] = {-32768, 32767, 1, -1};
    uint8_t s16data[8];
    PIXELICA_AudioClip clip;
    PIXELICA_AudioMixer m;
    Wav w;
    float f[10];
    int16_t out[4];
    size_t i;

    /* 8-bit mono, odd data length padded, unknown odd chunk first, and extra
     * bytes after the declared RIFF extent. */
    w_begin(&w);
    w_chunk(&w, "LIST", junk, 3u, true);
    w_fmt(&w, 1u, 8u, RATE);
    w_chunk(&w, "data", u8data, 5u, true);
    w_end(&w);
    w_u32(&w, 0xDEADBEEFu); /* outside the extent: ignored */
    CHECK(w.n <= sizeof w.b);
    CHECK(pixelica_audio_clip_wav(&clip, w.b, w.n) == PIXELICA_OK);
    CHECK(clip.frame_count == 5u);
    CHECK(clip.channels == 1u);
    CHECK(clip.sample_rate == RATE);
    CHECK(clip.format == PIXELICA_AUDIO_U8);
    CHECK(clip.samples == w.b + 12u + 8u + 4u + 24u + 8u);
    CHECK(memcmp(clip.samples, u8data, 5u) == 0);

    init_mixer(&m);
    CHECK(pixelica_audio_play(&m, &clip, NULL, NULL) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 5u) == PIXELICA_OK);
    CHECK_NEAR(f[0], -1.0, 0.0);
    CHECK_NEAR(f[1], -1.0, 0.0);
    CHECK_NEAR(f[2], -0.5, 0.0);
    CHECK_NEAR(f[4], 0.0, 0.0);
    CHECK_NEAR(f[6], 0.5, 0.0);
    CHECK_NEAR(f[8], 127.0 / 128.0, 1e-7);
    CHECK_NEAR(f[9], 127.0 / 128.0, 1e-7);

    /* Missing pad byte on the final odd chunk is tolerated. */
    w_begin(&w);
    w_fmt(&w, 1u, 8u, RATE);
    w_chunk(&w, "data", u8data, 5u, false);
    w_end(&w);
    CHECK(pixelica_audio_clip_wav(&clip, w.b, w.n) == PIXELICA_OK);
    CHECK(clip.frame_count == 5u);

    /* 16-bit stereo with data before fmt, at an odd (unaligned) address. */
    s16_bytes(s16data, s16, 4u);
    memset(&w, 0, sizeof w);
    w_u8(&w, 0u); /* the WAV starts at b + 1, so samples are unaligned */
    w_tag(&w, "RIFF");
    w_u32(&w, 0u);
    w_tag(&w, "WAVE");
    w_chunk(&w, "data", s16data, 8u, true);
    w_fmt(&w, 2u, 16u, 22050u);
    {
        uint32_t riff = (uint32_t)(w.n - 9u);

        w.b[5] = (uint8_t)(riff & 0xFFu);
        w.b[6] = (uint8_t)(riff >> 8);
        w.b[7] = 0u;
        w.b[8] = 0u;
    }
    CHECK(pixelica_audio_clip_wav(&clip, w.b + 1, w.n - 1u) == PIXELICA_OK);
    CHECK(clip.frame_count == 2u);
    CHECK(clip.channels == 2u);
    CHECK(clip.sample_rate == 22050u);
    CHECK(clip.format == PIXELICA_AUDIO_S16LE);
    CHECK(((uintptr_t)clip.samples & 1u) == 1u);

    CHECK(pixelica_audio_init(&m, 22050u) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&m, &clip, NULL, NULL) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 3u) == PIXELICA_OK);
    CHECK_NEAR(f[0], -1.0, 0.0);
    CHECK_NEAR(f[1], 32767.0 / 32768.0, 0.0);
    CHECK_NEAR(f[2], 1.0 / 32768.0, 0.0);
    CHECK_NEAR(f[3], -1.0 / 32768.0, 0.0);
    CHECK_NEAR(f[4], 0.0, 0.0);
    CHECK_NEAR(f[5], 0.0, 0.0);
    CHECK(pixelica_audio_play(&m, &clip, NULL, NULL) == PIXELICA_OK);
    CHECK(pixelica_audio_render_s16(&m, out, 2u) == PIXELICA_OK);
    CHECK(out[0] == -32767);
    CHECK(out[1] == 32766);
    CHECK(out[2] == 1);
    CHECK(out[3] == -1);

    /* fmt with an 18-byte body (cbSize) is fine. */
    w_begin(&w);
    w_tag(&w, "fmt ");
    w_u32(&w, 18u);
    w_u16(&w, 1u);
    w_u16(&w, 1u);
    w_u32(&w, RATE);
    w_u32(&w, RATE);
    w_u16(&w, 1u);
    w_u16(&w, 8u);
    w_u16(&w, 0u);
    w_chunk(&w, "data", u8data, 4u, true);
    w_end(&w);
    CHECK(pixelica_audio_clip_wav(&clip, w.b, w.n) == PIXELICA_OK);
    CHECK(clip.frame_count == 4u);
    for (i = 0; i < 4u; ++i) {
        CHECK(clip.samples[i] == u8data[i]);
    }
}

/* Variant `which` of a malformed WAV; returns 0 when out of variants. */
static int bad_wav(int which, Wav *w)
{
    static const uint8_t d8[4] = {1u, 2u, 3u, 4u};
    static const uint8_t d3[3] = {1u, 2u, 3u};

    w_begin(w);
    switch (which) {
    case 0: w_fmt(w, 1u, 8u, RATE); break;                              /* no data */
    case 1: w_chunk(w, "data", d8, 4u, true); break;                    /* no fmt */
    case 2: w_fmt(w, 1u, 8u, RATE); w_fmt(w, 1u, 8u, RATE);
            w_chunk(w, "data", d8, 4u, true); break;                    /* dup fmt */
    case 3: w_fmt(w, 1u, 8u, RATE); w_chunk(w, "data", d8, 4u, true);
            w_chunk(w, "data", d8, 4u, true); break;                    /* dup data */
    case 4: w_fmt_raw(w, 3u, 1u, RATE, RATE * 4u, 4u, 32u);
            w_chunk(w, "data", d8, 4u, true); break;                    /* float */
    case 5: w_fmt_raw(w, 0xFFFEu, 1u, RATE, RATE, 1u, 8u);
            w_chunk(w, "data", d8, 4u, true); break;                    /* extensible */
    case 6: w_fmt(w, 3u, 8u, RATE); w_chunk(w, "data", d3, 3u, true); break;
    case 7: w_fmt_raw(w, 1u, 0u, RATE, 0u, 0u, 8u);
            w_chunk(w, "data", d8, 4u, true); break;
    case 8: w_fmt(w, 1u, 24u, RATE); w_chunk(w, "data", d3, 3u, true); break;
    case 9: w_fmt_raw(w, 1u, 1u, RATE, RATE, 1u, 0u);
            w_chunk(w, "data", d8, 4u, true); break;
    case 10: w_fmt(w, 1u, 8u, 7999u); w_chunk(w, "data", d8, 4u, true); break;
    case 11: w_fmt(w, 1u, 8u, 96001u); w_chunk(w, "data", d8, 4u, true); break;
    case 12: w_fmt_raw(w, 1u, 1u, RATE, RATE * 2u, 2u, 8u);
             w_chunk(w, "data", d8, 4u, true); break;                   /* block_align */
    case 13: w_fmt_raw(w, 1u, 2u, RATE, RATE * 2u, 2u, 16u);
             w_chunk(w, "data", d8, 4u, true); break;                   /* block_align */
    case 14: w_fmt_raw(w, 1u, 1u, RATE, RATE + 1u, 1u, 8u);
             w_chunk(w, "data", d8, 4u, true); break;                   /* byte_rate */
    case 15: w_fmt(w, 1u, 8u, RATE); w_chunk(w, "data", d8, 0u, true); break;
    case 16: w_fmt(w, 1u, 16u, RATE); w_chunk(w, "data", d3, 3u, true); break;
    case 17: w_fmt(w, 2u, 16u, RATE); w_chunk(w, "data", d8, 2u, true); break;
    case 18: /* fmt body too short */
        w_tag(w, "fmt ");
        w_u32(w, 14u);
        w_u16(w, 1u); w_u16(w, 1u); w_u32(w, RATE); w_u32(w, RATE); w_u16(w, 1u);
        w_chunk(w, "data", d8, 4u, true);
        break;
    case 19: /* trailing partial chunk header inside the extent */
        w_fmt(w, 1u, 8u, RATE); w_chunk(w, "data", d8, 4u, true);
        w_tag(w, "LIST");
        break;
    case 20: /* chunk size runs past the extent */
        w_fmt(w, 1u, 8u, RATE);
        w_tag(w, "data"); w_u32(w, 5u); w_u32(w, 0u);
        break;
    case 21: /* chunk size near 4 GiB */
        w_fmt(w, 1u, 8u, RATE);
        w_tag(w, "data"); w_u32(w, 0xFFFFFFFFu); w_u32(w, 0u);
        break;
    case 22: /* data empty first, real data later is still ambiguous */
        w_fmt(w, 1u, 8u, RATE); w_chunk(w, "data", d8, 0u, true);
        w_chunk(w, "data", d8, 4u, true);
        break;
    default:
        return 0;
    }
    w_end(w);
    return 1;
}

static void test_wav_malformed(void)
{
    static const uint8_t d8[4] = {1u, 2u, 3u, 4u};
    PIXELICA_AudioClip clip;
    PIXELICA_AudioClip sentinel;
    Wav w;
    Wav good;
    size_t n;
    int which;

    memset(&sentinel, 0x5A, sizeof sentinel);
    memcpy(&clip, &sentinel, sizeof clip);

    w_begin(&good);
    w_fmt(&good, 1u, 8u, RATE);
    w_chunk(&good, "data", d8, 4u, true);
    w_end(&good);
    CHECK(pixelica_audio_clip_wav(NULL, good.b, good.n) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_clip_wav(&clip, NULL, good.n) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_clip_wav(&clip, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&clip, &sentinel, sizeof clip) == 0);

    for (which = 0; bad_wav(which, &w); ++which) {
        CHECK(w.n <= sizeof w.b);
        if (pixelica_audio_clip_wav(&clip, w.b, w.n) != PIXELICA_ERR_BAD_ASSET) {
            fprintf(stderr, "bad_wav variant %d accepted\n", which);
            CHECK(0);
        }
        CHECK(memcmp(&clip, &sentinel, sizeof clip) == 0);
    }
    CHECK(which == 23);

    /* Every truncation of a valid file fails (declared extent > size). */
    for (n = 0; n < good.n; ++n) {
        CHECK(pixelica_audio_clip_wav(&clip, good.b, n) == PIXELICA_ERR_BAD_ASSET);
    }
    CHECK(memcmp(&clip, &sentinel, sizeof clip) == 0);

    /* Magic and RIFF size corruptions. */
    memcpy(&w, &good, sizeof w);
    w.b[0] = 'X';
    CHECK(pixelica_audio_clip_wav(&clip, w.b, w.n) == PIXELICA_ERR_BAD_ASSET);
    memcpy(&w, &good, sizeof w);
    w.b[11] = 'X';
    CHECK(pixelica_audio_clip_wav(&clip, w.b, w.n) == PIXELICA_ERR_BAD_ASSET);
    memcpy(&w, &good, sizeof w);
    w.b[4] = 3u; w.b[5] = 0u; w.b[6] = 0u; w.b[7] = 0u; /* < "WAVE" */
    CHECK(pixelica_audio_clip_wav(&clip, w.b, w.n) == PIXELICA_ERR_BAD_ASSET);
    memcpy(&w, &good, sizeof w);
    w.b[4] = 0xFFu; w.b[5] = 0xFFu; w.b[6] = 0xFFu; w.b[7] = 0xFFu;
    CHECK(pixelica_audio_clip_wav(&clip, w.b, w.n) == PIXELICA_ERR_BAD_ASSET);
    /* A shorter RIFF extent cuts the data chunk off. */
    memcpy(&w, &good, sizeof w);
    w.b[4] = (uint8_t)(w.b[4] - 1u);
    CHECK(pixelica_audio_clip_wav(&clip, w.b, w.n) == PIXELICA_ERR_BAD_ASSET);
    CHECK(memcmp(&clip, &sentinel, sizeof clip) == 0);

    CHECK(pixelica_audio_clip_wav(&clip, good.b, good.n) == PIXELICA_OK);
}

/* Bounded random byte mutations: never crash; on success the clip lies inside
 * the input; on failure the output is untouched. */
static void test_wav_fuzz(void)
{
    static const uint8_t d8[6] = {1u, 2u, 3u, 4u, 5u, 6u};
    uint32_t seed = 0x12345678u;
    PIXELICA_AudioClip clip;
    PIXELICA_AudioClip sentinel;
    Wav good;
    Wav w;
    int iter;
    int ok_count = 0;

    w_begin(&good);
    w_chunk(&good, "junk", d8, 3u, true);
    w_fmt(&good, 2u, 16u, RATE);
    w_chunk(&good, "data", d8, 4u, true);
    w_end(&good);
    memset(&sentinel, 0x5A, sizeof sentinel);

    for (iter = 0; iter < 4000; ++iter) {
        int k, flips;
        size_t n;

        memcpy(&w, &good, sizeof w);
        seed = seed * 1664525u + 1013904223u;
        flips = 1 + (int)((seed >> 24) % 4u);
        for (k = 0; k < flips; ++k) {
            seed = seed * 1664525u + 1013904223u;
            w.b[(seed >> 8) % good.n] = (uint8_t)(seed >> 24);
        }
        seed = seed * 1664525u + 1013904223u;
        n = good.n + 8u - (size_t)((seed >> 16) % 16u); /* also shorter/longer */
        memcpy(&clip, &sentinel, sizeof clip);
        if (pixelica_audio_clip_wav(&clip, w.b, n) == PIXELICA_OK) {
            size_t bytes = clip.frame_count * clip.channels *
                           (clip.format == PIXELICA_AUDIO_S16LE ? 2u : 1u);

            ++ok_count;
            CHECK(clip.samples >= w.b && clip.samples < w.b + n);
            CHECK(bytes > 0u && bytes <= (size_t)(w.b + n - clip.samples));
        } else {
            CHECK(memcmp(&clip, &sentinel, sizeof clip) == 0);
        }
    }
    CHECK(ok_count > 0);
}

static void test_loop_end_resample(void)
{
    int16_t v[3] = {8192, 16384, -16384}; /* 0.25, 0.5, -0.5 */
    uint8_t b[6];
    PIXELICA_AudioMixer m;
    PIXELICA_AudioClip clip;
    PIXELICA_AudioPlayOptions loop = mk_opts(PIXELICA_AUDIO_SFX, true, 1.0f, 0.0f);
    PIXELICA_AudioVoice id;
    float f[16];

    s16_bytes(b, v, 3u);
    clip = mk_clip(b, 3u, RATE, 1u, PIXELICA_AUDIO_S16LE);

    /* Same rate, no loop: exactly three frames then silence. */
    init_mixer(&m);
    CHECK(pixelica_audio_play(&m, &clip, NULL, &id) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.25f, 0.25f));
    CHECK(frame_is(&m, 0.5f, 0.5f));
    CHECK(pixelica_audio_is_playing(&m, id));
    CHECK(frame_is(&m, -0.5f, -0.5f));
    CHECK(!pixelica_audio_is_playing(&m, id));
    CHECK(frame_is(&m, 0.0f, 0.0f));

    /* Same rate, loop: wraps at the boundary without a gap. */
    CHECK(pixelica_audio_play(&m, &clip, &loop, &id) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 7u) == PIXELICA_OK);
    CHECK_NEAR(f[0], 0.25, 0.0);
    CHECK_NEAR(f[4], -0.5, 0.0);
    CHECK_NEAR(f[6], 0.25, 0.0);
    CHECK_NEAR(f[10], -0.5, 0.0);
    CHECK_NEAR(f[12], 0.25, 0.0);
    CHECK(pixelica_audio_is_playing(&m, id));
    pixelica_audio_stop(&m, id);

    /* Upsample 8k -> 16k: linear interpolation, holds the last frame. */
    CHECK(pixelica_audio_init(&m, 16000u) == PIXELICA_OK);
    clip.frame_count = 2u; /* 0.25, 0.5 */
    CHECK(pixelica_audio_play(&m, &clip, NULL, &id) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 5u) == PIXELICA_OK);
    CHECK_NEAR(f[0], 0.25, 1e-7);
    CHECK_NEAR(f[2], 0.375, 1e-7);
    CHECK_NEAR(f[4], 0.5, 1e-7);
    CHECK_NEAR(f[6], 0.5, 1e-7);
    CHECK_NEAR(f[8], 0.0, 0.0);
    CHECK(!pixelica_audio_is_playing(&m, id));

    /* Upsample with loop interpolates across the wrap toward frame 0. */
    CHECK(pixelica_audio_play(&m, &clip, &loop, &id) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 5u) == PIXELICA_OK);
    CHECK_NEAR(f[0], 0.25, 1e-7);
    CHECK_NEAR(f[2], 0.375, 1e-7);
    CHECK_NEAR(f[4], 0.5, 1e-7);
    CHECK_NEAR(f[6], 0.375, 1e-7);
    CHECK_NEAR(f[8], 0.25, 1e-7);
    pixelica_audio_stop(&m, id);

    /* Downsample 16k -> 8k with loop: frames 0,2,1,0,2. */
    CHECK(pixelica_audio_init(&m, RATE) == PIXELICA_OK);
    clip.frame_count = 3u;
    clip.sample_rate = 16000u;
    CHECK(pixelica_audio_play(&m, &clip, &loop, &id) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 5u) == PIXELICA_OK);
    CHECK_NEAR(f[0], 0.25, 0.0);
    CHECK_NEAR(f[2], -0.5, 0.0);
    CHECK_NEAR(f[4], 0.5, 0.0);
    CHECK_NEAR(f[6], 0.25, 0.0);
    CHECK_NEAR(f[8], -0.5, 0.0);

    /* Extreme ratio: 96k clip into an 8k mixer, single-frame loop. */
    CHECK(pixelica_audio_init(&m, PIXELICA_AUDIO_MIN_RATE) == PIXELICA_OK);
    clip.frame_count = 1u;
    clip.sample_rate = PIXELICA_AUDIO_MAX_RATE;
    CHECK(pixelica_audio_play(&m, &clip, &loop, &id) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 8u) == PIXELICA_OK);
    CHECK_NEAR(f[14], 0.25, 0.0);
    CHECK(pixelica_audio_is_playing(&m, id));
}

static void test_pan_stereo(void)
{
    PIXELICA_AudioMixer m;
    PIXELICA_AudioClip mono = dc_clip(g_half);
    PIXELICA_AudioClip stereo = mk_clip(g_stereo_pm, 4u, RATE, 2u, PIXELICA_AUDIO_S16LE);
    PIXELICA_AudioPlayOptions o = mk_opts(PIXELICA_AUDIO_SFX, true, 1.0f, 0.0f);
    PIXELICA_AudioVoice id;

    init_mixer(&m);
    CHECK(pixelica_audio_play(&m, &mono, &o, &id) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, 0.5f));      /* centered mono at full gain */
    CHECK(pixelica_audio_set_pan(&m, id, 1.0f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.0f, 0.5f));
    CHECK(pixelica_audio_set_pan(&m, id, -1.0f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, 0.0f));
    CHECK(pixelica_audio_set_pan(&m, id, -0.5f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, 0.25f));
    CHECK(pixelica_audio_set_pan(&m, id, pixelica_t_nan()) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_pan(&m, id, 1.5f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_pan(&m, id, -pixelica_t_inf()) == PIXELICA_ERR_INVALID_ARG);
    CHECK(frame_is(&m, 0.5f, 0.25f));
    pixelica_audio_stop(&m, id);

    /* Stereo keeps both channels at center; balance attenuates one side. */
    CHECK(pixelica_audio_play(&m, &stereo, &o, &id) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, -0.5f));
    CHECK(pixelica_audio_set_pan(&m, id, 0.5f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.25f, -0.5f));
    CHECK(pixelica_audio_set_pan(&m, id, -0.75f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, -0.125f));
    pixelica_audio_stop(&m, id);

    /* Pan from play options. */
    o.pan = 1.0f;
    CHECK(pixelica_audio_play(&m, &stereo, &o, &id) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.0f, -0.5f));
}

static void test_mix_and_gains(void)
{
    PIXELICA_AudioMixer m;
    PIXELICA_AudioClip half = dc_clip(g_half);
    PIXELICA_AudioClip tq = dc_clip(g_three_q);
    PIXELICA_AudioClip ntq = dc_clip(g_neg_three_q);
    PIXELICA_AudioPlayOptions sfx = mk_opts(PIXELICA_AUDIO_SFX, true, 1.0f, 0.0f);
    PIXELICA_AudioPlayOptions mus = mk_opts(PIXELICA_AUDIO_MUSIC, true, 1.0f, 0.0f);
    PIXELICA_AudioVoice a, b;

    /* Sum and clip. */
    init_mixer(&m);
    CHECK(pixelica_audio_play(&m, &tq, &sfx, &a) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&m, &tq, &sfx, &b) == PIXELICA_OK);
    CHECK(frame_is(&m, 1.0f, 1.0f));
    CHECK(pixelica_audio_set_master_volume(&m, 0.5f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.75f, 0.75f)); /* master before the clamp */
    CHECK(pixelica_audio_set_master_volume(&m, 1.0f) == PIXELICA_OK);
    CHECK(pixelica_audio_set_volume(&m, b, 0.0f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.75f, 0.75f));
    pixelica_audio_stop_all(&m);
    CHECK(pixelica_audio_play(&m, &ntq, &sfx, &a) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&m, &ntq, &sfx, &b) == PIXELICA_OK);
    CHECK(frame_is(&m, -1.0f, -1.0f));
    CHECK(pixelica_audio_play(&m, &tq, &sfx, NULL) == PIXELICA_OK);
    CHECK(frame_is(&m, -0.75f, -0.75f)); /* cancellation */
    pixelica_audio_stop_all(&m);

    /* Bus and voice gains multiply. */
    CHECK(pixelica_audio_play(&m, &half, &sfx, &a) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&m, &half, &mus, &b) == PIXELICA_OK);
    CHECK(frame_is(&m, 1.0f, 1.0f));
    CHECK(pixelica_audio_set_bus_volume(&m, PIXELICA_AUDIO_SFX, 0.5f) == PIXELICA_OK);
    CHECK(pixelica_audio_set_bus_volume(&m, PIXELICA_AUDIO_MUSIC, 0.25f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.375f, 0.375f));
    CHECK(pixelica_audio_set_volume(&m, a, 0.5f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.25f, 0.25f));
    CHECK(pixelica_audio_set_master_volume(&m, 0.5f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.125f, 0.125f));
    CHECK(pixelica_audio_set_bus_volume(&m, PIXELICA_AUDIO_MUSIC, 0.0f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.0625f, 0.0625f));
    CHECK(pixelica_audio_set_master_volume(&m, 0.0f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.0f, 0.0f));

    /* Invalid gains are rejected and change nothing. */
    CHECK(pixelica_audio_set_master_volume(&m, pixelica_t_nan()) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_master_volume(&m, 1.01f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_master_volume(&m, -0.01f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_bus_volume(&m, (PIXELICA_AudioBus)2, 1.0f) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_bus_volume(&m, PIXELICA_AUDIO_SFX, pixelica_t_inf()) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_volume(&m, a, pixelica_t_nan()) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_set_volume(&m, a, 2.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK_NEAR(m.master_volume, 0.0, 0.0);
    CHECK_NEAR(m.bus_volume[0], 0.5, 0.0);
    CHECK(pixelica_audio_set_master_volume(&m, 1.0f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.125f, 0.125f));
}

static void test_capacity_and_music(void)
{
    PIXELICA_AudioMixer m;
    PIXELICA_AudioClip half = dc_clip(g_half);
    PIXELICA_AudioClip tq = dc_clip(g_three_q);
    PIXELICA_AudioClip bad = dc_clip(NULL);
    PIXELICA_AudioPlayOptions loop = mk_opts(PIXELICA_AUDIO_SFX, true, 0.0f, 0.0f);
    PIXELICA_AudioVoice ids[PIXELICA_AUDIO_MAX_VOICES];
    PIXELICA_AudioVoice id = 1u;
    PIXELICA_AudioVoice music, music2;
    float f[2 * 64];
    size_t i, j;

    init_mixer(&m);
    for (i = 0; i < PIXELICA_AUDIO_MAX_VOICES; ++i) {
        CHECK(pixelica_audio_play(&m, &half, &loop, &ids[i]) == PIXELICA_OK);
        for (j = 0; j < i; ++j) {
            CHECK(ids[i] != ids[j]);
        }
    }
    CHECK(pixelica_audio_play(&m, &half, &loop, &id) == PIXELICA_ERR_CAPACITY);
    CHECK(id == 0u);
    CHECK(pixelica_audio_play_music(&m, &half, &id) == PIXELICA_ERR_CAPACITY);
    CHECK(id == 0u);
    for (i = 0; i < PIXELICA_AUDIO_MAX_VOICES; ++i) {
        CHECK(pixelica_audio_is_playing(&m, ids[i])); /* nothing stolen */
    }

    /* Free a slot for music; replacement reuses the music slot. */
    pixelica_audio_stop(&m, ids[5]);
    CHECK(pixelica_audio_play_music(&m, &half, &music) == PIXELICA_OK);
    CHECK(music != 0u && pixelica_audio_is_playing(&m, music));
    CHECK(pixelica_audio_play(&m, &half, &loop, NULL) == PIXELICA_ERR_CAPACITY);
    CHECK(pixelica_audio_play_music(&m, &tq, &music2) == PIXELICA_OK);
    CHECK(music2 != music);
    CHECK(!pixelica_audio_is_playing(&m, music));
    CHECK(pixelica_audio_is_playing(&m, music2));
    CHECK(frame_is(&m, 0.75f, 0.75f)); /* SFX voices are silent (volume 0) */

    /* Invalid clip: transactional, old music keeps playing. */
    CHECK(pixelica_audio_play_music(&m, &bad, &id) == PIXELICA_ERR_INVALID_ARG);
    CHECK(id == 0u);
    CHECK(pixelica_audio_is_playing(&m, music2));
    CHECK(frame_is(&m, 0.75f, 0.75f));

    /* Music loops, is on the MUSIC bus, and survives long renders. */
    for (i = 0; i < 20u; ++i) {
        CHECK(pixelica_audio_render(&m, f, 64u) == PIXELICA_OK);
    }
    CHECK(pixelica_audio_is_playing(&m, music2));
    CHECK(pixelica_audio_set_bus_volume(&m, PIXELICA_AUDIO_MUSIC, 0.0f) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.0f, 0.0f));
    CHECK(pixelica_audio_set_bus_volume(&m, PIXELICA_AUDIO_MUSIC, 1.0f) == PIXELICA_OK);

    /* Stopped music: the next play_music needs a free slot again. */
    pixelica_audio_stop(&m, music2);
    CHECK(pixelica_audio_play(&m, &half, &loop, NULL) == PIXELICA_OK);
    CHECK(pixelica_audio_play_music(&m, &half, &id) == PIXELICA_ERR_CAPACITY);
    pixelica_audio_stop_all(&m);
    CHECK(pixelica_audio_play_music(&m, &half, &id) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, 0.5f));
}

static void test_stale_handles(void)
{
    static const uint8_t other_bytes[8] = {200u, 200u, 200u, 200u};
    PIXELICA_AudioMixer m;
    PIXELICA_AudioClip half = dc_clip(g_half);
    PIXELICA_AudioClip other = mk_clip(other_bytes, 4u, RATE, 1u, PIXELICA_AUDIO_U8);
    PIXELICA_AudioPlayOptions loop = mk_opts(PIXELICA_AUDIO_SFX, true, 1.0f, 0.0f);
    PIXELICA_AudioVoice a, b, c, d;

    init_mixer(&m);
    CHECK(pixelica_audio_play(&m, &half, &loop, &a) == PIXELICA_OK);
    pixelica_audio_stop(&m, a);
    CHECK(!pixelica_audio_is_playing(&m, a));
    CHECK(pixelica_audio_play(&m, &half, &loop, &b) == PIXELICA_OK);
    CHECK(b != a);
    CHECK(m.voices[0].id == b); /* same slot, new generation */

    /* The stale handle never affects the new voice. */
    CHECK(!pixelica_audio_is_playing(&m, a));
    CHECK(pixelica_audio_pause(&m, a, true) == PIXELICA_ERR_NOT_FOUND);
    CHECK(pixelica_audio_set_volume(&m, a, 0.0f) == PIXELICA_ERR_NOT_FOUND);
    CHECK(pixelica_audio_set_pan(&m, a, 1.0f) == PIXELICA_ERR_NOT_FOUND);
    CHECK(pixelica_audio_fade(&m, a, 0.0f, 0.0f, true) == PIXELICA_ERR_NOT_FOUND);
    pixelica_audio_stop(&m, a);
    CHECK(pixelica_audio_is_playing(&m, b));
    CHECK(frame_is(&m, 0.5f, 0.5f));

    /* 0 and forged handles. */
    CHECK(!pixelica_audio_is_playing(&m, 0u));
    CHECK(pixelica_audio_pause(&m, 0u, true) == PIXELICA_ERR_NOT_FOUND);
    CHECK(!pixelica_audio_is_playing(&m, b + 16u));
    CHECK(!pixelica_audio_is_playing(&m, b + 1u));
    CHECK(!pixelica_audio_is_playing(&m, UINT64_MAX));
    pixelica_audio_stop(&m, 0u);
    pixelica_audio_stop(&m, UINT64_MAX);
    CHECK(pixelica_audio_is_playing(&m, b));

    /* Natural end makes the handle stale. */
    CHECK(pixelica_audio_play(&m, &half, NULL, &c) == PIXELICA_OK);
    {
        float f[8];

        CHECK(pixelica_audio_render(&m, f, 4u) == PIXELICA_OK);
    }
    CHECK(!pixelica_audio_is_playing(&m, c));
    CHECK(pixelica_audio_set_volume(&m, c, 1.0f) == PIXELICA_ERR_NOT_FOUND);

    /* stop_clip matches the backing samples pointer only. */
    CHECK(pixelica_audio_play(&m, &other, &loop, &c) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&m, &half, &loop, &d) == PIXELICA_OK);
    pixelica_audio_stop_clip(&m, &half);
    CHECK(!pixelica_audio_is_playing(&m, b));
    CHECK(!pixelica_audio_is_playing(&m, d));
    CHECK(pixelica_audio_is_playing(&m, c));
    pixelica_audio_stop_clip(&m, NULL);
    pixelica_audio_stop_clip(&m, &m.voices[(size_t)(c & 15u)].clip); /* aliasing */
    CHECK(!pixelica_audio_is_playing(&m, c));

    /* stop_all preserves handle generations; init starts a new lifetime. */
    CHECK(pixelica_audio_play(&m, &half, &loop, &a) == PIXELICA_OK);
    pixelica_audio_stop_all(&m);
    CHECK(!pixelica_audio_is_playing(&m, a));
    CHECK(pixelica_audio_play(&m, &half, &loop, &b) == PIXELICA_OK);
    CHECK(pixelica_audio_init(&m, RATE) == PIXELICA_OK);
    CHECK(!pixelica_audio_is_playing(&m, b));
    CHECK(pixelica_audio_play(&m, &half, &loop, &c) == PIXELICA_OK);
    CHECK(c != 0u);
    /* A mixer never wraps its serial and reuses a live-lifetime handle. */
    m.next_serial = (UINT64_MAX >> 4) + 1u;
    CHECK(pixelica_audio_play(&m, &half, &loop, &d) == PIXELICA_ERR_CAPACITY);
    CHECK(d == 0u);
    CHECK(pixelica_audio_play_music(&m, &half, &d) == PIXELICA_ERR_CAPACITY);
    CHECK(pixelica_audio_is_playing(&m, c));
}

static void test_pause_and_fade(void)
{
    int16_t ramp[6] = {1024, 2048, 3072, 4096, 5120, 6144};
    uint8_t rb[12];
    PIXELICA_AudioMixer m;
    PIXELICA_AudioClip half = dc_clip(g_half);
    PIXELICA_AudioClip rc;
    PIXELICA_AudioPlayOptions loop = mk_opts(PIXELICA_AUDIO_SFX, true, 1.0f, 0.0f);
    PIXELICA_AudioVoice id;
    float f[32];
    int k;

    s16_bytes(rb, ramp, 6u);
    rc = mk_clip(rb, 6u, RATE, 1u, PIXELICA_AUDIO_S16LE);

    /* Pause keeps the position and outputs silence. */
    init_mixer(&m);
    CHECK(pixelica_audio_play(&m, &rc, NULL, &id) == PIXELICA_OK);
    CHECK(frame_is(&m, 1024.0f / 32768.0f, 1024.0f / 32768.0f));
    CHECK(frame_is(&m, 2048.0f / 32768.0f, 2048.0f / 32768.0f));
    CHECK(pixelica_audio_pause(&m, id, true) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 16u) == PIXELICA_OK);
    for (k = 0; k < 32; ++k) {
        CHECK_NEAR(f[k], 0.0, 0.0);
    }
    CHECK(pixelica_audio_is_playing(&m, id)); /* paused voices are live */
    CHECK(pixelica_audio_pause(&m, id, true) == PIXELICA_OK);
    CHECK(pixelica_audio_pause(&m, id, false) == PIXELICA_OK);
    CHECK(frame_is(&m, 3072.0f / 32768.0f, 3072.0f / 32768.0f));
    pixelica_audio_stop(&m, id);

    /* 1 ms at 8 kHz = 8 frames, linear 1 -> 0, then stops. */
    CHECK(pixelica_audio_play(&m, &half, &loop, &id) == PIXELICA_OK);
    CHECK(pixelica_audio_fade(&m, id, 0.0f, 0.001f, true) == PIXELICA_OK);
    for (k = 0; k < 8; ++k) {
        float expect = 0.5f * (1.0f - (float)k / 8.0f);

        CHECK(pixelica_audio_is_playing(&m, id));
        CHECK(frame_is(&m, expect, expect));
    }
    CHECK(!pixelica_audio_is_playing(&m, id));
    CHECK(frame_is(&m, 0.0f, 0.0f));

    /* Paused fades freeze; resume continues on schedule. */
    CHECK(pixelica_audio_play(&m, &half, &loop, &id) == PIXELICA_OK);
    CHECK(pixelica_audio_fade(&m, id, 0.0f, 0.001f, false) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, 0.5f));
    CHECK(frame_is(&m, 0.4375f, 0.4375f));
    CHECK(pixelica_audio_pause(&m, id, true) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 16u) == PIXELICA_OK);
    CHECK(pixelica_audio_pause(&m, id, false) == PIXELICA_OK);
    for (k = 2; k < 8; ++k) {
        float expect = 0.5f * (1.0f - (float)k / 8.0f);

        CHECK(frame_is(&m, expect, expect));
    }
    /* Without stop_when_done the voice stays at the target. */
    CHECK(frame_is(&m, 0.0f, 0.0f));
    CHECK(pixelica_audio_is_playing(&m, id));

    /* Fade up, then set_volume cancels a fade in progress. */
    CHECK(pixelica_audio_fade(&m, id, 1.0f, 0.001f, true) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.0f, 0.0f));
    CHECK(frame_is(&m, 0.0625f, 0.0625f));
    CHECK(pixelica_audio_set_volume(&m, id, 0.5f) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&m, f, 16u) == PIXELICA_OK);
    CHECK_NEAR(f[30], 0.25, 1e-7);
    CHECK(pixelica_audio_is_playing(&m, id));

    /* seconds = 0 (or rounding to 0 frames) applies immediately. */
    CHECK(pixelica_audio_fade(&m, id, 1.0f, 0.0f, false) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, 0.5f));
    CHECK(pixelica_audio_fade(&m, id, 0.5f, 1e-6f, false) == PIXELICA_OK);
    CHECK(frame_is(&m, 0.25f, 0.25f));
    CHECK(pixelica_audio_fade(&m, id, 0.0f, 0.0f, true) == PIXELICA_OK);
    CHECK(!pixelica_audio_is_playing(&m, id));

    /* Invalid fade arguments. */
    CHECK(pixelica_audio_play(&m, &half, &loop, &id) == PIXELICA_OK);
    CHECK(pixelica_audio_fade(&m, id, pixelica_t_nan(), 1.0f, false) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_fade(&m, id, 1.5f, 1.0f, false) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_fade(&m, id, -0.1f, 1.0f, false) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_fade(&m, id, 0.0f, -1.0f, false) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_fade(&m, id, 0.0f, pixelica_t_nan(), false) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_fade(&m, id, 0.0f, pixelica_t_inf(), false) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_fade(&m, id, 0.0f, PIXELICA_AUDIO_MAX_FADE_SECONDS * 2.0f, false) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_audio_fade(&m, id, 0.0f, PIXELICA_AUDIO_MAX_FADE_SECONDS, true) ==
          PIXELICA_OK);
    CHECK(frame_is(&m, 0.5f, 0.5f));
    CHECK(pixelica_audio_is_playing(&m, id));
    /* A long fade must progress linearly even when each step is below a float ULP. */
    for (k = 0; k < 256; ++k) {
        CHECK(pixelica_audio_render(&m, f, 16u) == PIXELICA_OK);
    }
    CHECK_NEAR(f[30], 0.5 * (1.0 - 4096.0 /
               ((double)RATE * PIXELICA_AUDIO_MAX_FADE_SECONDS)), 1e-7);
}

static void test_s16_matches_f32(void)
{
    int16_t wave[50];
    uint8_t wb[100];
    PIXELICA_AudioMixer a, b;
    PIXELICA_AudioClip clip;
    PIXELICA_AudioClip tq = dc_clip(g_three_q);
    PIXELICA_AudioPlayOptions o = mk_opts(PIXELICA_AUDIO_SFX, true, 0.8f, 0.3f);
    PIXELICA_AudioVoice va, vb;
    float f[2 * 200];
    int16_t s[2 * 200];
    size_t i;
    int nonzero = 0;

    for (i = 0; i < 50u; ++i) {
        wave[i] = (int16_t)((int)(i * 1311u % 65536u) - 32768);
    }
    s16_bytes(wb, wave, 50u);
    clip = mk_clip(wb, 25u, 11025u, 2u, PIXELICA_AUDIO_S16LE);

    /* Two identical mixers; one renders f32 in one call, the other s16 in
     * blocks that straddle the internal 64-frame block. */
    CHECK(pixelica_audio_init(&a, 22050u) == PIXELICA_OK);
    CHECK(pixelica_audio_init(&b, 22050u) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&a, &clip, &o, &va) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&b, &clip, &o, &vb) == PIXELICA_OK);
    CHECK(pixelica_audio_fade(&a, va, 0.1f, 0.005f, false) == PIXELICA_OK);
    CHECK(pixelica_audio_fade(&b, vb, 0.1f, 0.005f, false) == PIXELICA_OK);
    CHECK(pixelica_audio_play_music(&a, &tq, NULL) == PIXELICA_OK);
    CHECK(pixelica_audio_play_music(&b, &tq, NULL) == PIXELICA_OK);
    CHECK(pixelica_audio_render(&a, f, 200u) == PIXELICA_OK);
    CHECK(pixelica_audio_render_s16(&b, s, 130u) == PIXELICA_OK);
    CHECK(pixelica_audio_render_s16(&b, s + 260, 70u) == PIXELICA_OK);
    for (i = 0; i < 400u; ++i) {
        CHECK(isfinite(f[i]) && f[i] >= -1.0f && f[i] <= 1.0f);
        CHECK(s16_near(s[i], to_s16(f[i])));
        if (s[i] != 0) {
            ++nonzero;
        }
    }
    CHECK(nonzero > 300);

    /* Full-scale clipping maps to +-32767. */
    pixelica_audio_stop_all(&b);
    CHECK(pixelica_audio_play_music(&b, &tq, NULL) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&b, &tq, &o, NULL) == PIXELICA_OK);
    CHECK(pixelica_audio_set_volume(&b, b.voices[1].id, 1.0f) == PIXELICA_OK);
    CHECK(pixelica_audio_set_pan(&b, b.voices[1].id, 0.0f) == PIXELICA_OK);
    CHECK(pixelica_audio_render_s16(&b, s, 1u) == PIXELICA_OK);
    CHECK(s[0] == 32767 && s[1] == 32767);
    pixelica_audio_stop_all(&b);
    {
        PIXELICA_AudioClip ntq = dc_clip(g_neg_three_q);

        CHECK(pixelica_audio_play_music(&b, &ntq, NULL) == PIXELICA_OK);
        CHECK(pixelica_audio_play(&b, &ntq, NULL, NULL) == PIXELICA_OK);
    }
    CHECK(pixelica_audio_render_s16(&b, s, 1u) == PIXELICA_OK);
    CHECK(s[0] == -32767 && s[1] == -32767);
}

static void test_corrupted_storage_guard(void)
{
    PIXELICA_AudioMixer m;
    PIXELICA_AudioClip half = dc_clip(g_half);
    PIXELICA_AudioPlayOptions loop = mk_opts(PIXELICA_AUDIO_SFX, true, 1.0f, 0.0f);
    PIXELICA_AudioVoice a, b, c;
    float f[4];

    init_mixer(&m);
    CHECK(pixelica_audio_play(&m, &half, &loop, &a) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&m, &half, &loop, &b) == PIXELICA_OK);
    CHECK(pixelica_audio_play(&m, &half, &loop, &c) == PIXELICA_OK);

    /* Direct writes outside the contract must not read out of bounds. */
    m.voices[0].frame = 1000000u;
    m.voices[1].clip.frame_count = SIZE_MAX;
    m.voices[2].volume = pixelica_t_nan();
    m.master_volume = 5.0f;
    CHECK(pixelica_audio_render(&m, f, 2u) == PIXELICA_OK);
    CHECK_NEAR(f[0], 0.0, 0.0); /* invalid master mutes */
    CHECK(!pixelica_audio_is_playing(&m, a));
    CHECK(!pixelica_audio_is_playing(&m, b));
    CHECK(!pixelica_audio_is_playing(&m, c));
}

int main(void)
{
    init_fixtures();
    test_null_and_init();
    test_clip_and_option_validation();
    test_wav_parse();
    test_wav_malformed();
    test_wav_fuzz();
    test_loop_end_resample();
    test_pan_stereo();
    test_mix_and_gains();
    test_capacity_and_music();
    test_stale_handles();
    test_pause_and_fade();
    test_s16_matches_f32();
    test_corrupted_storage_guard();
    return pixelica_t_finish("test_audio");
}

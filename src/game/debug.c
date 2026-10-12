/*
 * Pixelica - frame timing accumulator and bounded debug text formatting.
 * See pixelica/debug.h for the contract. No heap, no global state, no clock,
 * no UI or renderer dependency: only integer math and caller buffers.
 */
#include "pixelica/debug.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEBUG_MAGIC 0x50444247u /* "PDBG" */

/* ---- address ranges ---------------------------------------------------- */

static bool range_ok(const void *p, size_t bytes)
{
    return p != NULL && bytes <= UINTPTR_MAX - (uintptr_t)p;
}

static bool overlap(const void *a, size_t a_bytes, const void *b, size_t b_bytes)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;

    return x < y + b_bytes && y < x + a_bytes;
}

/* ---- timing ------------------------------------------------------------ */

PIXELICA_Status pixelica_debug_timing_init(PIXELICA_DebugTiming *out, uint32_t window_us)
{
    PIXELICA_DebugTiming t;

    if (out == NULL || window_us > PIXELICA_DEBUG_MAX_WINDOW_US) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(&t, 0, sizeof t);
    t.window_us = window_us == 0u ? PIXELICA_DEBUG_DEFAULT_WINDOW_US : window_us;
    t.magic = DEBUG_MAGIC;
    *out = t;
    return PIXELICA_OK;
}

/* The state invariants update maintains; also rejects zeroed/garbage memory. */
static bool timing_valid(const PIXELICA_DebugTiming *t)
{
    return t != NULL && t->magic == DEBUG_MAGIC && t->window_us >= 1u &&
        t->window_us <= PIXELICA_DEBUG_MAX_WINDOW_US && t->accum_us < t->window_us &&
        t->frames < PIXELICA_DEBUG_MAX_WINDOW_FRAMES &&
        t->window_max_us <= PIXELICA_DEBUG_MAX_FRAME_US;
}

PIXELICA_Status pixelica_debug_timing_update(PIXELICA_DebugTiming *timing, uint32_t frame_us)
{
    PIXELICA_DebugTiming t;

    if (!timing_valid(timing) || frame_us > PIXELICA_DEBUG_MAX_FRAME_US) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    t = *timing;
    /* accum < window <= 60 s and frame <= 10 s: the sum fits uint32_t. */
    t.accum_us += frame_us;
    t.frames += 1u;
    t.last_frame_us = frame_us;
    if (frame_us > t.window_max_us) {
        t.window_max_us = frame_us;
    }
    if (t.accum_us >= t.window_us || t.frames >= PIXELICA_DEBUG_MAX_WINDOW_FRAMES) {
        if (t.accum_us != 0u) {
            uint64_t fps = ((uint64_t)t.frames * 1000000000u + t.accum_us / 2u) / t.accum_us;

            t.fps_milli = fps > UINT32_MAX ? UINT32_MAX : (uint32_t)fps;
            t.avg_frame_us = (uint32_t)(((uint64_t)t.accum_us + t.frames / 2u) / t.frames);
            t.max_frame_us = t.window_max_us;
            t.has_report = true;
        }
        t.accum_us = 0u;
        t.frames = 0u;
        t.window_max_us = 0u;
    }
    *timing = t;
    return PIXELICA_OK;
}

/* ---- bounded text writer ----------------------------------------------- */

typedef struct Writer {
    char *buf;
    size_t cap; /* includes the NUL */
    size_t len;
    bool truncated;
} Writer;

static void put_char(Writer *w, char c)
{
    if (w->len + 1u < w->cap) {
        w->buf[w->len++] = c;
    } else {
        w->truncated = true;
    }
}

static void put_str(Writer *w, const char *s, size_t n)
{
    size_t i;

    for (i = 0u; i < n; ++i) {
        put_char(w, s[i]);
    }
}

static void put_lit(Writer *w, const char *s)
{
    put_str(w, s, strlen(s));
}

static void put_u64(Writer *w, uint64_t v)
{
    char digits[20];
    size_t n = 0u;

    do {
        digits[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
        put_char(w, digits[--n]);
    }
}

/* tenths -> "12.3" */
static void put_tenths(Writer *w, uint64_t tenths)
{
    put_u64(w, tenths / 10u);
    put_char(w, '.');
    put_char(w, (char)('0' + (int)(tenths % 10u)));
}

/* microseconds -> milliseconds with one rounded decimal */
static void put_ms(Writer *w, uint32_t us)
{
    put_tenths(w, ((uint64_t)us + 50u) / 100u);
    put_lit(w, " ms");
}

static bool output_ok(const void *in, size_t in_bytes, char *out, size_t capacity,
    const size_t *out_len, const bool *out_truncated)
{
    if (out == NULL || capacity == 0u || !range_ok(out, capacity)) {
        return false;
    }
    if (in != NULL && (!range_ok(in, in_bytes) || overlap(in, in_bytes, out, capacity))) {
        return false;
    }
    if (out_len != NULL && (!range_ok(out_len, sizeof *out_len) ||
            overlap(out_len, sizeof *out_len, out, capacity) ||
            overlap(out_len, sizeof *out_len, in, in_bytes))) {
        return false;
    }
    if (out_truncated != NULL && (!range_ok(out_truncated, sizeof *out_truncated) ||
            overlap(out_truncated, sizeof *out_truncated, out, capacity) ||
            overlap(out_truncated, sizeof *out_truncated, in, in_bytes) ||
            (out_len != NULL && overlap(out_truncated, sizeof *out_truncated,
                out_len, sizeof *out_len)))) {
        return false;
    }
    return true;
}

static void finish(Writer *w, size_t *out_len, bool *out_truncated)
{
    w->buf[w->len] = '\0';
    if (out_len != NULL) {
        *out_len = w->len;
    }
    if (out_truncated != NULL) {
        *out_truncated = w->truncated;
    }
}

PIXELICA_Status pixelica_debug_format_timing(const PIXELICA_DebugTiming *timing,
    char *out, size_t capacity, size_t *out_len, bool *out_truncated)
{
    Writer w;

    if (!timing_valid(timing) ||
        !output_ok(timing, sizeof *timing, out, capacity, out_len, out_truncated)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    w.buf = out;
    w.cap = capacity;
    w.len = 0u;
    w.truncated = false;
    put_lit(&w, "FPS ");
    if (timing->has_report) {
        put_tenths(&w, ((uint64_t)timing->fps_milli + 50u) / 100u);
        put_lit(&w, " | ");
        put_ms(&w, timing->avg_frame_us);
        put_lit(&w, " | max ");
        put_ms(&w, timing->max_frame_us);
    } else {
        put_lit(&w, "-- | -- ms");
    }
    finish(&w, out_len, out_truncated);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_debug_format_metric(const char *name, size_t name_bytes,
    int32_t value, char *out, size_t capacity, size_t *out_len, bool *out_truncated)
{
    Writer w;
    size_t i;

    if (name == NULL || name_bytes == 0u || name_bytes > PIXELICA_DEBUG_NAME_MAX ||
        !output_ok(name, name_bytes, out, capacity, out_len, out_truncated)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0u; i < name_bytes; ++i) {
        if ((unsigned char)name[i] < 0x20u || (unsigned char)name[i] > 0x7Eu) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    w.buf = out;
    w.cap = capacity;
    w.len = 0u;
    w.truncated = false;
    put_str(&w, name, name_bytes);
    put_lit(&w, ": ");
    if (value < 0) {
        put_char(&w, '-');
        put_u64(&w, (uint64_t)0 - (uint64_t)(int64_t)value);
    } else {
        put_u64(&w, (uint64_t)value);
    }
    finish(&w, out_len, out_truncated);
    return PIXELICA_OK;
}

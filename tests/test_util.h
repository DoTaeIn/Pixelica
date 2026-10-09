/*
 * Pixelica - tiny self-contained test helpers.
 *
 * CHECK() never uses assert(), so tests stay active in Release (NDEBUG).
 * Each test program returns non-zero if any check failed.
 *
 * This header includes only the COMMON public headers (status, input,
 * platform, math, image, assets_common, animation), so tests of the common
 * library, the 2D renderer and the optional modules build without any 3D
 * header. The 3D umbrella header and the helpers that need a PIXELICA_Context
 * (pixelica_t_config, pixelica_t_color_at, ...) exist only when the test target defines
 * PIXELICA_TEST_WITH_3D (the root CMakeLists.txt does that for every test that
 * links the 3D core). Code that uses them in a test which is also meant to
 * build without the 3D renderer must be wrapped in #ifdef PIXELICA_TEST_WITH_3D.
 */
#ifndef PIXELICA_TEST_UTIL_H
#define PIXELICA_TEST_UTIL_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pixelica/animation.h"
#include "pixelica/assets_common.h"
#include "pixelica/image.h"
#include "pixelica/input.h"
#include "pixelica/math.h"
#include "pixelica/platform.h"
#include "pixelica/status.h"

#ifdef PIXELICA_TEST_WITH_3D
#include "pixelica/pixelica.h"
#endif

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,         \
                    __LINE__, #cond);                                      \
        }                                                                  \
    } while (0)

#define CHECK_EQ_LONG(actual, expected)                                    \
    do {                                                                   \
        long long pixelica_t_a_ = (long long)(actual);                           \
        long long pixelica_t_e_ = (long long)(expected);                         \
        ++g_checks;                                                        \
        if (pixelica_t_a_ != pixelica_t_e_) {                                          \
            ++g_failures;                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s == %s (%lld vs %lld)\n", \
                    __FILE__, __LINE__, #actual, #expected, pixelica_t_a_,       \
                    pixelica_t_e_);                                              \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                  \
    do {                                                                   \
        double pixelica_t_a_ = (double)(actual);                                 \
        double pixelica_t_e_ = (double)(expected);                               \
        ++g_checks;                                                        \
        if (!(fabs(pixelica_t_a_ - pixelica_t_e_) <= (double)(tol))) {                 \
            ++g_failures;                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s ~= %s (%.9g vs %.9g)\n", \
                    __FILE__, __LINE__, #actual, #expected, pixelica_t_a_,       \
                    pixelica_t_e_);                                              \
        }                                                                  \
    } while (0)

static inline int pixelica_t_finish(const char *name)
{
    if (g_failures != 0) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, g_failures,
                g_checks);
        return 1;
    }
    printf("%s: all %d checks passed\n", name, g_checks);
    return 0;
}

/* Runtime-generated special values (avoids constant-folding warnings). */
static inline float pixelica_t_inf(void)
{
    volatile float zero = 0.0f;
    return 1.0f / zero;
}

static inline float pixelica_t_nan(void)
{
    volatile float zero = 0.0f;
    return zero / zero;
}

/* FNV-1a 64 over uint16 values in little-endian byte order (portable). */
static inline uint64_t pixelica_t_hash16(const uint16_t *data, size_t count)
{
    uint64_t h = 14695981039346656037ull;
    size_t i;

    for (i = 0; i < count; ++i) {
        h ^= (uint64_t)(data[i] & 0xFFu);
        h *= 1099511628211ull;
        h ^= (uint64_t)(data[i] >> 8);
        h *= 1099511628211ull;
    }
    return h;
}

#ifdef PIXELICA_TEST_WITH_3D
/* ---- 3D-context helpers (need the 3D core) ---------------------------- */

static inline PIXELICA_Config pixelica_t_config(uint16_t w, uint16_t h)
{
    PIXELICA_Config cfg;

    cfg.width = w;
    cfg.height = h;
    cfg.max_draw_commands = 64;
    cfg.max_vertices_per_mesh = 256;
    cfg.frame_arena_bytes = 256u * PIXELICA_ARENA_BYTES_PER_VERTEX;
    return cfg;
}

static inline size_t pixelica_t_pixels(const PIXELICA_Context *ctx, uint16_t w, uint16_t h)
{
    (void)ctx;
    return (size_t)w * (size_t)h;
}

/* Number of pixels in the color buffer equal to `color`. */
static inline uint32_t pixelica_t_count_color(const PIXELICA_Context *ctx, uint16_t w,
                                        uint16_t h, uint16_t color)
{
    const uint16_t *px = pixelica_color_buffer(ctx);
    size_t n = pixelica_t_pixels(ctx, w, h);
    size_t i;
    uint32_t count = 0;

    for (i = 0; i < n; ++i) {
        if (px[i] == color) {
            ++count;
        }
    }
    return count;
}

static inline uint16_t pixelica_t_color_at(const PIXELICA_Context *ctx, uint16_t w,
                                     int x, int y)
{
    return pixelica_color_buffer(ctx)[(size_t)y * (size_t)w + (size_t)x];
}

static inline uint16_t pixelica_t_depth_at(const PIXELICA_Context *ctx, uint16_t w,
                                     int x, int y)
{
    return pixelica_depth_buffer(ctx)[(size_t)y * (size_t)w + (size_t)x];
}
#endif /* PIXELICA_TEST_WITH_3D */

#endif /* PIXELICA_TEST_UTIL_H */

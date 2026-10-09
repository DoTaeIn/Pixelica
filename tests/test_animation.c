/*
 * Pixelica - animation module: easing, scalar tween, keyframe sampling,
 * deterministic player and TRS + pivot transform matrices.
 *
 * Every expectation is derived from the written contract (docs/animation.md),
 * not from the implementation: closed forms, hard-coded sequences, a unit-step
 * reference model and a bulk-versus-partition property. CHECK() stays active
 * under NDEBUG. The animation calls themselves allocate nothing; only the one
 * render test creates a context.
 */
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/animation.h"
#include "pixelica/image.h"
#include "pixelica/math.h"
#include "pixelica/status.h"
#ifdef PIXELICA_TEST_WITH_3D
#include "pixelica/pixelica.h"
#endif
#include "test_util.h"

#define PI_F 3.14159265358979323846f
#define EPS 1e-5

static const PIXELICA_Ease k_eases[6] = {
    PIXELICA_EASE_STEP,       PIXELICA_EASE_LINEAR,       PIXELICA_EASE_QUAD_IN,
    PIXELICA_EASE_QUAD_OUT,   PIXELICA_EASE_QUAD_IN_OUT,  PIXELICA_EASE_SMOOTHSTEP,
};

static const PIXELICA_AnimMode k_modes[3] = {PIXELICA_ANIM_ONCE, PIXELICA_ANIM_LOOP, PIXELICA_ANIM_PING_PONG};

#define BAD_EASE_HIGH ((PIXELICA_Ease)6)
#define BAD_EASE_FAR ((PIXELICA_Ease)99)
#define BAD_EASE_NEG ((PIXELICA_Ease)-1)
#define BAD_MODE_HIGH ((PIXELICA_AnimMode)3)
#define BAD_MODE_NEG ((PIXELICA_AnimMode)-1)

#define SENTINEL 123.456f

/* The non-finite sweep below views PIXELICA_Transform as 12 packed floats. */
_Static_assert(sizeof(PIXELICA_Transform) == 12u * sizeof(float), "PIXELICA_Transform is 12 floats");

/* ------------------------------------------------------------------ utils */

static uint32_t g_rng = 0x2468ACE1u;

static uint32_t rng_next(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static float rng_range(float lo, float hi)
{
    return lo + (hi - lo) * ((float)(rng_next() >> 8) / 16777216.0f);
}

static int bits_equal(float a, float b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

/* Independent closed forms of the documented easing curves. */
static double ref_ease(PIXELICA_Ease e, double t)
{
    if (t <= 0.0) {
        return 0.0;
    }
    if (t >= 1.0) {
        return 1.0;
    }
    switch (e) {
    case PIXELICA_EASE_STEP:
        return 0.0;
    case PIXELICA_EASE_LINEAR:
        return t;
    case PIXELICA_EASE_QUAD_IN:
        return t * t;
    case PIXELICA_EASE_QUAD_OUT:
        return t * (2.0 - t);
    case PIXELICA_EASE_QUAD_IN_OUT:
        return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
    case PIXELICA_EASE_SMOOTHSTEP:
        return t * t * (3.0 - 2.0 * t);
    }
    return -1.0;
}

static PIXELICA_Vec3 vec3(float x, float y, float z)
{
    PIXELICA_Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

/* ------------------------------------------------------------------- ease */

static void test_ease_endpoints_and_clamp(void)
{
    size_t i;

    for (i = 0; i < 6; ++i) {
        float out = SENTINEL;

        CHECK(pixelica_anim_ease(k_eases[i], 0.0f, &out) == PIXELICA_OK);
        CHECK(bits_equal(out, 0.0f));
        out = SENTINEL;
        CHECK(pixelica_anim_ease(k_eases[i], 1.0f, &out) == PIXELICA_OK);
        CHECK(out == 1.0f);

        /* Out-of-range finite t is clamped, including the extremes. */
        CHECK(pixelica_anim_ease(k_eases[i], -0.25f, &out) == PIXELICA_OK);
        CHECK(out == 0.0f);
        CHECK(pixelica_anim_ease(k_eases[i], 7.5f, &out) == PIXELICA_OK);
        CHECK(out == 1.0f);
        CHECK(pixelica_anim_ease(k_eases[i], -FLT_MAX, &out) == PIXELICA_OK);
        CHECK(out == 0.0f);
        CHECK(pixelica_anim_ease(k_eases[i], FLT_MAX, &out) == PIXELICA_OK);
        CHECK(out == 1.0f);
        CHECK(pixelica_anim_ease(k_eases[i], -0.0f, &out) == PIXELICA_OK);
        CHECK(out == 0.0f);
    }
}

static void test_ease_values(void)
{
    static const float ts[] = {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.9375f, 0.999f};
    size_t i;
    size_t k;
    float out = SENTINEL;

    for (i = 0; i < 6; ++i) {
        for (k = 0; k < sizeof ts / sizeof ts[0]; ++k) {
            out = SENTINEL;
            CHECK(pixelica_anim_ease(k_eases[i], ts[k], &out) == PIXELICA_OK);
            CHECK_NEAR(out, ref_ease(k_eases[i], (double)ts[k]), EPS);
        }
    }

    /* Hand-computed anchors. */
    CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_IN, 0.25f, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 0.0625, EPS);
    CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_OUT, 0.25f, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 0.4375, EPS);
    CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_IN_OUT, 0.25f, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 0.125, EPS);
    CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_IN_OUT, 0.75f, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 0.875, EPS);
    CHECK(pixelica_anim_ease(PIXELICA_EASE_SMOOTHSTEP, 0.25f, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 0.15625, EPS);
    CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_IN_OUT, 0.5f, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 0.5, EPS);
    CHECK(pixelica_anim_ease(PIXELICA_EASE_SMOOTHSTEP, 0.5f, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 0.5, EPS);

    /* STEP is 0 for every t < 1, including the float just below 1. */
    CHECK(pixelica_anim_ease(PIXELICA_EASE_STEP, 0.5f, &out) == PIXELICA_OK);
    CHECK(out == 0.0f);
    CHECK(pixelica_anim_ease(PIXELICA_EASE_STEP, nextafterf(1.0f, 0.0f), &out) == PIXELICA_OK);
    CHECK(out == 0.0f);
    CHECK(pixelica_anim_ease(PIXELICA_EASE_STEP, 1.0f, &out) == PIXELICA_OK);
    CHECK(out == 1.0f);
}

static void test_ease_properties(void)
{
    size_t i;
    int n;

    for (i = 0; i < 6; ++i) {
        float prev = 0.0f;

        for (n = 0; n <= 1024; ++n) {
            float t = (float)n / 1024.0f;
            float out = SENTINEL;

            CHECK(pixelica_anim_ease(k_eases[i], t, &out) == PIXELICA_OK);
            CHECK(out >= 0.0f && out <= 1.0f);
            CHECK(out >= prev); /* monotone non-decreasing */
            if (k_eases[i] == PIXELICA_EASE_STEP) {
                CHECK(out == 0.0f || out == 1.0f);
            }
            prev = out;
        }
    }

    /* Symmetries of the documented curves. */
    for (n = 0; n <= 64; ++n) {
        float t = (float)n / 64.0f;
        float a;
        float b;

        CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_OUT, t, &a) == PIXELICA_OK);
        CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_IN, 1.0f - t, &b) == PIXELICA_OK);
        CHECK_NEAR(a, 1.0 - (double)b, EPS);
        CHECK(pixelica_anim_ease(PIXELICA_EASE_SMOOTHSTEP, t, &a) == PIXELICA_OK);
        CHECK(pixelica_anim_ease(PIXELICA_EASE_SMOOTHSTEP, 1.0f - t, &b) == PIXELICA_OK);
        CHECK_NEAR((double)a + (double)b, 1.0, EPS);
        CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_IN_OUT, t, &a) == PIXELICA_OK);
        CHECK(pixelica_anim_ease(PIXELICA_EASE_QUAD_IN_OUT, 1.0f - t, &b) == PIXELICA_OK);
        CHECK_NEAR((double)a + (double)b, 1.0, EPS);
    }
}

static void test_ease_invalid(void)
{
    size_t i;
    float out;
    float bad_t[3];

    bad_t[0] = pixelica_t_nan();
    bad_t[1] = pixelica_t_inf();
    bad_t[2] = -pixelica_t_inf();

    for (i = 0; i < 3; ++i) {
        out = SENTINEL;
        CHECK(pixelica_anim_ease(PIXELICA_EASE_LINEAR, bad_t[i], &out) == PIXELICA_ERR_INVALID_ARG);
        CHECK(bits_equal(out, SENTINEL));
    }

    out = SENTINEL;
    CHECK(pixelica_anim_ease(BAD_EASE_HIGH, 0.5f, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(bits_equal(out, SENTINEL));
    CHECK(pixelica_anim_ease(BAD_EASE_FAR, 0.5f, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(bits_equal(out, SENTINEL));
    CHECK(pixelica_anim_ease(BAD_EASE_NEG, 0.5f, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(bits_equal(out, SENTINEL));
    /* An unknown enum is rejected even at the clamped endpoints. */
    CHECK(pixelica_anim_ease(BAD_EASE_HIGH, 0.0f, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_ease(BAD_EASE_HIGH, 1.0f, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(bits_equal(out, SENTINEL));

    CHECK(pixelica_anim_ease(PIXELICA_EASE_LINEAR, 0.5f, NULL) == PIXELICA_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------ tween */

static void test_tween_basic(void)
{
    size_t i;
    float out = SENTINEL;
    static const float pairs[][2] = {
        {0.0f, 1.0f},     {1.0f, 0.0f},      {-3.3f, 7.7f},  {0.1f, 0.3f},
        {1e-30f, 1e30f},  {-5.0f, -5.0f},    {123456.79f, -0.001f},
        {-FLT_MAX, FLT_MAX}, {FLT_MAX, -FLT_MAX}, {FLT_MAX, FLT_MAX},
    };
    size_t p;

    /* Endpoints are exact for every easing and every pair. */
    for (p = 0; p < sizeof pairs / sizeof pairs[0]; ++p) {
        for (i = 0; i < 6; ++i) {
            out = SENTINEL;
            CHECK(pixelica_anim_tween(pairs[p][0], pairs[p][1], 0.0f, k_eases[i], &out) == PIXELICA_OK);
            CHECK(out == pairs[p][0]);
            out = SENTINEL;
            CHECK(pixelica_anim_tween(pairs[p][0], pairs[p][1], 1.0f, k_eases[i], &out) == PIXELICA_OK);
            CHECK(out == pairs[p][1]);
        }
    }

    CHECK(pixelica_anim_tween(10.0f, 20.0f, 0.25f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 12.5, EPS);
    CHECK(pixelica_anim_tween(20.0f, 10.0f, 0.25f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 17.5, EPS);
    CHECK(pixelica_anim_tween(10.0f, 20.0f, 0.5f, PIXELICA_EASE_QUAD_IN, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 12.5, EPS);
    CHECK(pixelica_anim_tween(-4.0f, 4.0f, 0.5f, PIXELICA_EASE_SMOOTHSTEP, &out) == PIXELICA_OK);
    CHECK_NEAR(out, 0.0, EPS);
    CHECK(pixelica_anim_tween(10.0f, 20.0f, 0.999f, PIXELICA_EASE_STEP, &out) == PIXELICA_OK);
    CHECK(out == 10.0f);

    /* Clamped t. */
    CHECK(pixelica_anim_tween(0.0f, 10.0f, -5.0f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_OK);
    CHECK(out == 0.0f);
    CHECK(pixelica_anim_tween(0.0f, 10.0f, 5.0f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_OK);
    CHECK(out == 10.0f);

    /* Equal endpoints stay put for any t. */
    for (i = 0; i < 6; ++i) {
        int n;

        for (n = 0; n <= 16; ++n) {
            CHECK(pixelica_anim_tween(3.5f, 3.5f, (float)n / 16.0f, k_eases[i], &out) == PIXELICA_OK);
            CHECK_NEAR(out, 3.5, 1e-6);
        }
    }
}

static void test_tween_extremes(void)
{
    float out = SENTINEL;
    size_t i;
    int n;

    CHECK(pixelica_anim_tween(-FLT_MAX, FLT_MAX, 0.5f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_OK);
    CHECK(isfinite(out));
    CHECK(fabsf(out) <= FLT_MAX * 1e-6f);
    CHECK(pixelica_anim_tween(FLT_MAX, -FLT_MAX, 0.5f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_OK);
    CHECK(isfinite(out));
    CHECK(fabsf(out) <= FLT_MAX * 1e-6f);
    CHECK(pixelica_anim_tween(-FLT_MAX, FLT_MAX, 0.25f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_OK);
    CHECK_NEAR(out / FLT_MAX, -0.5, 1e-5);
    CHECK(pixelica_anim_tween(-FLT_MAX, FLT_MAX, 0.75f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_OK);
    CHECK_NEAR(out / FLT_MAX, 0.5, 1e-5);

    /* Never overflows and always stays inside the endpoint range. */
    for (i = 0; i < 6; ++i) {
        for (n = 0; n <= 64; ++n) {
            float t = (float)n / 64.0f;

            CHECK(pixelica_anim_tween(-FLT_MAX, FLT_MAX, t, k_eases[i], &out) == PIXELICA_OK);
            CHECK(isfinite(out));
            CHECK(pixelica_anim_tween(FLT_MAX, -FLT_MAX, t, k_eases[i], &out) == PIXELICA_OK);
            CHECK(isfinite(out));
            CHECK(pixelica_anim_tween(FLT_MAX, FLT_MAX, t, k_eases[i], &out) == PIXELICA_OK);
            CHECK(out == FLT_MAX);
        }
    }

    /* Bounded interpolation for ordinary random ranges. */
    for (n = 0; n < 2000; ++n) {
        float a = rng_range(-1000.0f, 1000.0f);
        float b = rng_range(-1000.0f, 1000.0f);
        float t = rng_range(-0.5f, 1.5f);
        float lo = a < b ? a : b;
        float hi = a < b ? b : a;

        i = (size_t)(rng_next() % 6u);
        CHECK(pixelica_anim_tween(a, b, t, k_eases[i], &out) == PIXELICA_OK);
        CHECK(out >= lo - 1e-3f && out <= hi + 1e-3f);
    }
}

static void test_tween_invalid(void)
{
    float out = SENTINEL;
    float not_a_number = pixelica_t_nan();
    float infinity = pixelica_t_inf();

    CHECK(pixelica_anim_tween(not_a_number, 1.0f, 0.5f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_tween(0.0f, not_a_number, 0.5f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_tween(0.0f, 1.0f, not_a_number, PIXELICA_EASE_LINEAR, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_tween(infinity, 1.0f, 0.5f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_tween(0.0f, -infinity, 0.5f, PIXELICA_EASE_LINEAR, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_tween(0.0f, 1.0f, infinity, PIXELICA_EASE_LINEAR, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_tween(0.0f, 1.0f, 0.5f, BAD_EASE_HIGH, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_tween(0.0f, 1.0f, 0.5f, BAD_EASE_NEG, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(bits_equal(out, SENTINEL));
    CHECK(pixelica_anim_tween(0.0f, 1.0f, 0.5f, PIXELICA_EASE_LINEAR, NULL) == PIXELICA_ERR_INVALID_ARG);
}

/* ----------------------------------------------------------------- sample */

static PIXELICA_AnimKeyframe key(uint32_t tick, float value, PIXELICA_Ease ease)
{
    PIXELICA_AnimKeyframe k;

    k.tick = tick;
    k.value = value;
    k.ease = ease;
    return k;
}

static float sample_ok(const PIXELICA_AnimKeyframe *keys, size_t count, uint32_t tick)
{
    float out = SENTINEL;

    CHECK(pixelica_anim_sample(keys, count, tick, &out) == PIXELICA_OK);
    return out;
}

static void test_sample_intervals_and_clamps(void)
{
    const PIXELICA_AnimKeyframe keys[4] = {
        {10u, 0.0f, PIXELICA_EASE_LINEAR},
        {20u, 10.0f, PIXELICA_EASE_QUAD_IN},
        {30u, 20.0f, PIXELICA_EASE_STEP},
        {40u, -5.0f, PIXELICA_EASE_SMOOTHSTEP}, /* final ease is unused but still validated */
    };

    /* The first tick need not be zero: clamp before it. */
    CHECK(sample_ok(keys, 4, 0u) == 0.0f);
    CHECK(sample_ok(keys, 4, 9u) == 0.0f);
    CHECK(sample_ok(keys, 4, 10u) == 0.0f);

    CHECK_NEAR(sample_ok(keys, 4, 15u), 5.0, EPS);  /* linear */
    CHECK(sample_ok(keys, 4, 20u) == 10.0f);        /* exact key */
    CHECK_NEAR(sample_ok(keys, 4, 25u), 12.5, EPS); /* quad-in: 10 + 10 * 0.25 */
    CHECK_NEAR(sample_ok(keys, 4, 22u), 10.0 + 10.0 * 0.04, EPS);
    CHECK(sample_ok(keys, 4, 30u) == 20.0f);

    /* LEFT key easing: STEP holds the left value until the next key tick. */
    CHECK(sample_ok(keys, 4, 31u) == 20.0f);
    CHECK(sample_ok(keys, 4, 39u) == 20.0f);
    CHECK(sample_ok(keys, 4, 40u) == -5.0f);

    /* Clamp after the last key. */
    CHECK(sample_ok(keys, 4, 41u) == -5.0f);
    CHECK(sample_ok(keys, 4, 1000000u) == -5.0f);
    CHECK(sample_ok(keys, 4, UINT32_MAX) == -5.0f);
}

static void test_sample_each_ease(void)
{
    size_t i;
    size_t j;
    static const uint32_t ticks[] = {0u, 1u, 25u, 50u, 75u, 99u, 100u};

    for (i = 0; i < 6; ++i) {
        for (j = 0; j < 6; ++j) {
            /* The RIGHT key's ease must have no influence on the interval. */
            PIXELICA_AnimKeyframe keys[2];
            size_t k;

            keys[0] = key(0u, 2.0f, k_eases[i]);
            keys[1] = key(100u, 6.0f, k_eases[j]);
            for (k = 0; k < sizeof ticks / sizeof ticks[0]; ++k) {
                double expect;

                if (ticks[k] == 0u) {
                    expect = 2.0;
                } else if (ticks[k] == 100u) {
                    expect = 6.0;
                } else {
                    expect = 2.0 + 4.0 * ref_ease(k_eases[i], (double)ticks[k] / 100.0);
                }
                CHECK_NEAR(sample_ok(keys, 2, ticks[k]), expect, 1e-4);
            }
        }
    }
}

static void test_sample_single_and_step_keys(void)
{
    PIXELICA_AnimKeyframe one = key(500u, 3.25f, PIXELICA_EASE_STEP);
    PIXELICA_AnimKeyframe step[2];
    PIXELICA_AnimKeyframe edge[2];

    CHECK(sample_ok(&one, 1, 0u) == 3.25f);
    CHECK(sample_ok(&one, 1, 499u) == 3.25f);
    CHECK(sample_ok(&one, 1, 500u) == 3.25f);
    CHECK(sample_ok(&one, 1, 501u) == 3.25f);
    CHECK(sample_ok(&one, 1, UINT32_MAX) == 3.25f);

    one = key(0u, -1.5f, PIXELICA_EASE_LINEAR);
    CHECK(sample_ok(&one, 1, 0u) == -1.5f);
    CHECK(sample_ok(&one, 1, UINT32_MAX) == -1.5f);
    one = key(UINT32_MAX, 8.0f, PIXELICA_EASE_QUAD_IN);
    CHECK(sample_ok(&one, 1, 0u) == 8.0f);
    CHECK(sample_ok(&one, 1, UINT32_MAX) == 8.0f);

    /* STEP: hold, then jump exactly at the next key. */
    step[0] = key(0u, 1.0f, PIXELICA_EASE_STEP);
    step[1] = key(10u, 2.0f, PIXELICA_EASE_STEP);
    CHECK(sample_ok(step, 2, 0u) == 1.0f);
    CHECK(sample_ok(step, 2, 9u) == 1.0f);
    CHECK(sample_ok(step, 2, 10u) == 2.0f);
    CHECK(sample_ok(step, 2, 11u) == 2.0f);

    /* Adjacent ticks at the very top of the range. */
    edge[0] = key(UINT32_MAX - 1u, 4.0f, PIXELICA_EASE_LINEAR);
    edge[1] = key(UINT32_MAX, 9.0f, PIXELICA_EASE_LINEAR);
    CHECK(sample_ok(edge, 2, UINT32_MAX - 2u) == 4.0f);
    CHECK(sample_ok(edge, 2, UINT32_MAX - 1u) == 4.0f);
    CHECK(sample_ok(edge, 2, UINT32_MAX) == 9.0f);
}

static void test_sample_uint32_spans(void)
{
    PIXELICA_AnimKeyframe full[2];
    PIXELICA_AnimKeyframe top[2];

    full[0] = key(0u, 0.0f, PIXELICA_EASE_LINEAR);
    full[1] = key(UINT32_MAX, 1.0f, PIXELICA_EASE_LINEAR);
    CHECK_NEAR(sample_ok(full, 2, 2147483647u), 0.5, 1e-6);
    CHECK_NEAR(sample_ok(full, 2, 2147483648u), 0.5, 1e-6);
    CHECK_NEAR(sample_ok(full, 2, 1u), 1.0 / 4294967295.0, 1e-14);
    CHECK_NEAR(sample_ok(full, 2, UINT32_MAX - 1u), 1.0, 1e-6);
    CHECK(sample_ok(full, 2, UINT32_MAX - 1u) <= 1.0f);
    CHECK(sample_ok(full, 2, UINT32_MAX) == 1.0f);

    /* Float-only normalization would collapse these near-2^32 ticks. */
    top[0] = key(4294967290u, 0.0f, PIXELICA_EASE_LINEAR);
    top[1] = key(UINT32_MAX, 5.0f, PIXELICA_EASE_LINEAR);
    CHECK_NEAR(sample_ok(top, 2, 4294967291u), 1.0, 1e-4);
    CHECK_NEAR(sample_ok(top, 2, 4294967292u), 2.0, 1e-4);
    CHECK_NEAR(sample_ok(top, 2, 4294967293u), 3.0, 1e-4);
    CHECK_NEAR(sample_ok(top, 2, 4294967294u), 4.0, 1e-4);

    /* Monotone across a huge linear span. */
    {
        float prev = -1.0f;
        uint32_t t;

        for (t = 0u; t < 16u; ++t) {
            float v = sample_ok(full, 2, t * 268435455u);

            CHECK(v >= prev);
            prev = v;
        }
    }

    /* No angle wrapping or shortest-arc: radians interpolate directly. */
    {
        PIXELICA_AnimKeyframe turn[2];

        turn[0] = key(0u, -3.0f, PIXELICA_EASE_LINEAR);
        turn[1] = key(100u, 3.0f, PIXELICA_EASE_LINEAR);
        CHECK_NEAR(sample_ok(turn, 2, 50u), 0.0, 1e-5);
        CHECK_NEAR(sample_ok(turn, 2, 25u), -1.5, 1e-5);
        turn[0] = key(0u, 0.0f, PIXELICA_EASE_LINEAR);
        turn[1] = key(100u, 2.0f * PI_F, PIXELICA_EASE_LINEAR);
        CHECK_NEAR(sample_ok(turn, 2, 50u), PI_F, 1e-5);
    }
}

static void test_sample_many_keys(void)
{
    static PIXELICA_AnimKeyframe keys[512];
    uint32_t i;
    uint32_t tick;

    for (i = 0; i < 512u; ++i) {
        keys[i] = key(i * 3u + 5u, (float)i * 2.0f, PIXELICA_EASE_LINEAR);
    }
    for (tick = 0u; tick <= 512u * 3u + 20u; ++tick) {
        double expect;
        float got = sample_ok(keys, 512, tick);

        if (tick <= 5u) {
            expect = 0.0;
        } else if (tick >= 511u * 3u + 5u) {
            expect = 2.0 * 511.0;
        } else {
            expect = 2.0 * ((double)(tick - 5u) / 3.0);
        }
        CHECK_NEAR(got, expect, 1e-3);
    }
}

static void test_sample_invalid_and_unchanged(void)
{
    PIXELICA_AnimKeyframe good[3];
    PIXELICA_AnimKeyframe bad[3];
    PIXELICA_AnimKeyframe snapshot[3];
    float out;
    size_t i;
    float not_a_number = pixelica_t_nan();
    float infinity = pixelica_t_inf();

    good[0] = key(0u, 0.0f, PIXELICA_EASE_LINEAR);
    good[1] = key(10u, 1.0f, PIXELICA_EASE_LINEAR);
    good[2] = key(20u, 2.0f, PIXELICA_EASE_LINEAR);

    out = SENTINEL;
    CHECK(pixelica_anim_sample(good, 0, 5u, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_sample(NULL, 3, 5u, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_sample(NULL, 0, 5u, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_sample(good, 3, 5u, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(bits_equal(out, SENTINEL));

    /* Equal and descending ticks. */
    memcpy(bad, good, sizeof bad);
    bad[1].tick = 0u;
    CHECK(pixelica_anim_sample(bad, 3, 5u, &out) == PIXELICA_ERR_INVALID_ARG);
    memcpy(bad, good, sizeof bad);
    bad[2].tick = 10u;
    CHECK(pixelica_anim_sample(bad, 3, 5u, &out) == PIXELICA_ERR_INVALID_ARG);
    memcpy(bad, good, sizeof bad);
    bad[1].tick = 30u;
    CHECK(pixelica_anim_sample(bad, 3, 5u, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(bits_equal(out, SENTINEL));

    /* Non-finite values at every position, sampled far away from them. */
    for (i = 0; i < 3; ++i) {
        float poison[3];
        size_t p;

        poison[0] = not_a_number;
        poison[1] = infinity;
        poison[2] = -infinity;
        for (p = 0; p < 3; ++p) {
            memcpy(bad, good, sizeof bad);
            bad[i].value = poison[p];
            CHECK(pixelica_anim_sample(bad, 3, 0u, &out) == PIXELICA_ERR_INVALID_ARG);
            CHECK(pixelica_anim_sample(bad, 3, 5u, &out) == PIXELICA_ERR_INVALID_ARG);
            CHECK(pixelica_anim_sample(bad, 3, UINT32_MAX, &out) == PIXELICA_ERR_INVALID_ARG);
            CHECK(bits_equal(out, SENTINEL));
        }
    }

    /* Invalid ease at every position, including the unused final key. */
    for (i = 0; i < 3; ++i) {
        memcpy(bad, good, sizeof bad);
        bad[i].ease = BAD_EASE_HIGH;
        CHECK(pixelica_anim_sample(bad, 3, 0u, &out) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_anim_sample(bad, 3, UINT32_MAX, &out) == PIXELICA_ERR_INVALID_ARG);
        memcpy(bad, good, sizeof bad);
        bad[i].ease = BAD_EASE_NEG;
        CHECK(pixelica_anim_sample(bad, 3, 15u, &out) == PIXELICA_ERR_INVALID_ARG);
        CHECK(bits_equal(out, SENTINEL));
    }

    /* A single-key track is validated too. */
    bad[0] = key(7u, infinity, PIXELICA_EASE_LINEAR);
    CHECK(pixelica_anim_sample(bad, 1, 7u, &out) == PIXELICA_ERR_INVALID_ARG);
    bad[0] = key(7u, 1.0f, BAD_EASE_FAR);
    CHECK(pixelica_anim_sample(bad, 1, 7u, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(bits_equal(out, SENTINEL));

    /* Sampling never modifies the key array. */
    memcpy(snapshot, good, sizeof snapshot);
    for (i = 0; i < 25; ++i) {
        (void)sample_ok(good, 3, (uint32_t)i);
    }
    CHECK(memcmp(snapshot, good, sizeof good) == 0);
}

static void test_sample_aliasing(void)
{
    PIXELICA_AnimKeyframe keys[2];
    PIXELICA_Status status;

    /* The output may be one of the keyframe values: reads finish first. */
    keys[0] = key(0u, 0.0f, PIXELICA_EASE_LINEAR);
    keys[1] = key(10u, 10.0f, PIXELICA_EASE_LINEAR);
    status = pixelica_anim_sample(keys, 2, 5u, &keys[1].value);
    CHECK(status == PIXELICA_OK);
    CHECK_NEAR(keys[1].value, 5.0, EPS);
    CHECK(keys[0].value == 0.0f && keys[0].tick == 0u && keys[1].tick == 10u);

    keys[0] = key(0u, 4.0f, PIXELICA_EASE_LINEAR);
    keys[1] = key(10u, 8.0f, PIXELICA_EASE_LINEAR);
    status = pixelica_anim_sample(keys, 2, 5u, &keys[0].value);
    CHECK(status == PIXELICA_OK);
    CHECK_NEAR(keys[0].value, 6.0, EPS);
    CHECK(keys[1].value == 8.0f);

    /* Failure with an aliased output leaves the key bit-identical. */
    keys[0] = key(0u, 1.0f, PIXELICA_EASE_LINEAR);
    keys[1] = key(0u, 2.0f, PIXELICA_EASE_LINEAR);
    status = pixelica_anim_sample(keys, 2, 0u, &keys[1].value);
    CHECK(status == PIXELICA_ERR_INVALID_ARG);
    CHECK(keys[1].value == 2.0f && keys[0].value == 1.0f);
}

/* ----------------------------------------------------------------- player */

static PIXELICA_AnimPlayer make_player(uint32_t duration, PIXELICA_AnimMode mode)
{
    PIXELICA_AnimPlayer player;

    memset(&player, 0, sizeof player);
    CHECK(pixelica_anim_player_init(&player, duration, mode) == PIXELICA_OK);
    return player;
}

static uint32_t player_tick(const PIXELICA_AnimPlayer *player)
{
    uint32_t tick = 0xDEADBEEFu;

    CHECK(pixelica_anim_player_tick(player, &tick) == PIXELICA_OK);
    return tick;
}

static void advance_ok(PIXELICA_AnimPlayer *player, uint32_t ticks)
{
    CHECK(pixelica_anim_player_advance(player, ticks) == PIXELICA_OK);
}

static void test_player_init(void)
{
    PIXELICA_AnimPlayer player;
    PIXELICA_AnimPlayer before;
    size_t m;

    for (m = 0; m < 3; ++m) {
        player = make_player(42u, k_modes[m]);
        CHECK(player.duration_ticks == 42u);
        CHECK(player.phase_ticks == 0u);
        CHECK(player.mode == k_modes[m]);
        CHECK(!player.paused);
        CHECK(!player.finished);
        CHECK(player_tick(&player) == 0u);
    }

    /* Invalid arguments leave the player untouched. */
    player = make_player(9u, PIXELICA_ANIM_LOOP);
    advance_ok(&player, 4u);
    memcpy(&before, &player, sizeof before);
    CHECK(pixelica_anim_player_init(&player, 0u, PIXELICA_ANIM_LOOP) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&before, &player, sizeof before) == 0);
    CHECK(pixelica_anim_player_init(&player, 5u, BAD_MODE_HIGH) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&before, &player, sizeof before) == 0);
    CHECK(pixelica_anim_player_init(&player, 5u, BAD_MODE_NEG) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&before, &player, sizeof before) == 0);
    CHECK(pixelica_anim_player_init(NULL, 5u, PIXELICA_ANIM_LOOP) == PIXELICA_ERR_INVALID_ARG);

    /* Re-init restarts everything, including paused and finished. */
    player = make_player(3u, PIXELICA_ANIM_ONCE);
    advance_ok(&player, 100u);
    player.paused = true;
    CHECK(player.finished);
    CHECK(pixelica_anim_player_init(&player, 8u, PIXELICA_ANIM_PING_PONG) == PIXELICA_OK);
    CHECK(player.duration_ticks == 8u && player.phase_ticks == 0u);
    CHECK(player.mode == PIXELICA_ANIM_PING_PONG && !player.paused && !player.finished);
}

static void test_player_once(void)
{
    PIXELICA_AnimPlayer player = make_player(10u, PIXELICA_ANIM_ONCE);
    int i;

    advance_ok(&player, 0u);
    CHECK(player_tick(&player) == 0u && !player.finished);
    advance_ok(&player, 3u);
    CHECK(player_tick(&player) == 3u && !player.finished);
    advance_ok(&player, 6u);
    CHECK(player_tick(&player) == 9u && !player.finished);
    advance_ok(&player, 1u);
    CHECK(player_tick(&player) == 10u);
    CHECK(player.finished && player.phase_ticks == 10u);
    /* Finished: further advances succeed without moving. */
    advance_ok(&player, 1u);
    advance_ok(&player, UINT32_MAX);
    advance_ok(&player, 0u);
    CHECK(player_tick(&player) == 10u && player.finished && player.phase_ticks == 10u);

    /* A single large advance clamps, never wraps. */
    player = make_player(10u, PIXELICA_ANIM_ONCE);
    advance_ok(&player, UINT32_MAX);
    CHECK(player_tick(&player) == 10u && player.finished);

    /* Unit duration. */
    player = make_player(1u, PIXELICA_ANIM_ONCE);
    CHECK(player_tick(&player) == 0u && !player.finished);
    advance_ok(&player, 1u);
    CHECK(player_tick(&player) == 1u && player.finished);

    /* Maximum duration. */
    player = make_player(UINT32_MAX, PIXELICA_ANIM_ONCE);
    advance_ok(&player, UINT32_MAX - 1u);
    CHECK(player_tick(&player) == UINT32_MAX - 1u && !player.finished);
    advance_ok(&player, 1u);
    CHECK(player_tick(&player) == UINT32_MAX && player.finished);
    advance_ok(&player, UINT32_MAX);
    CHECK(player_tick(&player) == UINT32_MAX && player.finished);

    player = make_player(UINT32_MAX, PIXELICA_ANIM_ONCE);
    advance_ok(&player, UINT32_MAX);
    CHECK(player_tick(&player) == UINT32_MAX && player.finished);

    /* The tick never decreases while a ONCE player advances. */
    player = make_player(1000u, PIXELICA_ANIM_ONCE);
    {
        uint32_t prev = 0u;

        for (i = 0; i < 400; ++i) {
            uint32_t now;

            advance_ok(&player, rng_next() % 9u);
            now = player_tick(&player);
            CHECK(now >= prev && now <= 1000u);
            prev = now;
        }
    }
}

static void test_player_loop(void)
{
    PIXELICA_AnimPlayer player = make_player(10u, PIXELICA_ANIM_LOOP);

    advance_ok(&player, 9u);
    CHECK(player_tick(&player) == 9u && !player.finished);
    advance_ok(&player, 1u); /* exactly the duration wraps to 0 */
    CHECK(player_tick(&player) == 0u && player.phase_ticks == 0u && !player.finished);
    advance_ok(&player, 25u);
    CHECK(player_tick(&player) == 5u && !player.finished);
    advance_ok(&player, 0u);
    CHECK(player_tick(&player) == 5u);

    player = make_player(1u, PIXELICA_ANIM_LOOP);
    advance_ok(&player, 1u);
    CHECK(player_tick(&player) == 0u && !player.finished);
    advance_ok(&player, UINT32_MAX);
    CHECK(player_tick(&player) == 0u && !player.finished);

    player = make_player(UINT32_MAX, PIXELICA_ANIM_LOOP);
    advance_ok(&player, UINT32_MAX - 1u);
    CHECK(player_tick(&player) == UINT32_MAX - 1u);
    advance_ok(&player, 1u);
    CHECK(player_tick(&player) == 0u);
    advance_ok(&player, UINT32_MAX);
    CHECK(player_tick(&player) == 0u);
    advance_ok(&player, 7u);
    advance_ok(&player, UINT32_MAX);
    CHECK(player_tick(&player) == 7u && !player.finished);

    /* Large advances accumulate modulo the duration. */
    player = make_player(7u, PIXELICA_ANIM_LOOP);
    {
        uint64_t total = 0u;
        int i;

        for (i = 0; i < 3; ++i) {
            advance_ok(&player, UINT32_MAX);
            total += UINT32_MAX;
        }
        CHECK_EQ_LONG(player_tick(&player), total % 7u);
    }
}

static void test_player_ping_pong(void)
{
    static const uint32_t d4[8] = {0u, 1u, 2u, 3u, 4u, 3u, 2u, 1u};
    static const uint32_t d1[2] = {0u, 1u};
    PIXELICA_AnimPlayer player = make_player(4u, PIXELICA_ANIM_PING_PONG);
    int i;

    /* Both endpoints are visited; the period is 2 * duration. */
    for (i = 0; i < 24; ++i) {
        CHECK_EQ_LONG(player_tick(&player), d4[i % 8]);
        CHECK(!player.finished);
        CHECK(player.phase_ticks < 8u);
        advance_ok(&player, 1u);
    }

    player = make_player(1u, PIXELICA_ANIM_PING_PONG);
    for (i = 0; i < 9; ++i) {
        CHECK_EQ_LONG(player_tick(&player), d1[i % 2]);
        advance_ok(&player, 1u);
    }

    /* Bulk advances land on the same phase as stepping. */
    player = make_player(4u, PIXELICA_ANIM_PING_PONG);
    advance_ok(&player, 4u);
    CHECK(player_tick(&player) == 4u);
    advance_ok(&player, 2u);
    CHECK(player_tick(&player) == 2u);
    advance_ok(&player, 6u); /* phase 12 mod 8 = 4 */
    CHECK(player_tick(&player) == 4u);
    advance_ok(&player, 8u);
    CHECK(player_tick(&player) == 4u);

    /* Maximum duration needs the 64-bit period 2 * UINT32_MAX. */
    player = make_player(UINT32_MAX, PIXELICA_ANIM_PING_PONG);
    advance_ok(&player, UINT32_MAX);
    CHECK(player_tick(&player) == UINT32_MAX && player.phase_ticks == UINT32_MAX);
    advance_ok(&player, 1u);
    CHECK(player_tick(&player) == UINT32_MAX - 1u);
    CHECK(player.phase_ticks == (uint64_t)UINT32_MAX + 1u);
    advance_ok(&player, UINT32_MAX - 1u); /* phase 2 * duration wraps to 0 */
    CHECK(player_tick(&player) == 0u && player.phase_ticks == 0u);
    advance_ok(&player, UINT32_MAX);
    advance_ok(&player, UINT32_MAX);
    advance_ok(&player, UINT32_MAX);
    CHECK(player_tick(&player) == UINT32_MAX && !player.finished);
    CHECK(player.phase_ticks < 2u * (uint64_t)UINT32_MAX);
}

static void test_player_pause_and_restart(void)
{
    size_t m;

    for (m = 0; m < 3; ++m) {
        PIXELICA_AnimPlayer player = make_player(10u, k_modes[m]);
        uint64_t phase;

        advance_ok(&player, 4u);
        phase = player.phase_ticks;
        player.paused = true;
        advance_ok(&player, 100u);
        advance_ok(&player, UINT32_MAX);
        CHECK(player.phase_ticks == phase);
        CHECK(player_tick(&player) == 4u);
        CHECK(player.paused);

        player.paused = false;
        advance_ok(&player, 1u);
        CHECK(player_tick(&player) == 5u);

        /* Re-init restarts from tick 0 and clears pause. */
        player.paused = true;
        CHECK(pixelica_anim_player_init(&player, 10u, k_modes[m]) == PIXELICA_OK);
        CHECK(player.phase_ticks == 0u && !player.paused && !player.finished);
        advance_ok(&player, 2u);
        CHECK(player_tick(&player) == 2u);
    }

    /* A finished ONCE player stays finished even if unpaused and paused again. */
    {
        PIXELICA_AnimPlayer player = make_player(5u, PIXELICA_ANIM_ONCE);

        advance_ok(&player, 5u);
        player.paused = true;
        advance_ok(&player, 3u);
        player.paused = false;
        advance_ok(&player, 3u);
        CHECK(player.finished && player_tick(&player) == 5u);
        CHECK(pixelica_anim_player_init(&player, 5u, PIXELICA_ANIM_ONCE) == PIXELICA_OK);
        CHECK(!player.finished && player_tick(&player) == 0u);
    }
}

static void test_player_invalid_state(void)
{
    PIXELICA_AnimPlayer base[3];
    PIXELICA_AnimPlayer bad;
    PIXELICA_AnimPlayer before;
    uint32_t tick;
    size_t m;
    size_t c;

    for (m = 0; m < 3; ++m) {
        base[m] = make_player(10u, k_modes[m]);
    }

    CHECK(pixelica_anim_player_advance(NULL, 1u) == PIXELICA_ERR_INVALID_ARG);
    tick = 0xDEADBEEFu;
    CHECK(pixelica_anim_player_tick(NULL, &tick) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_anim_player_tick(&base[0], NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(tick == 0xDEADBEEFu);

    /* Each corruption is invalid for advance AND tick, even when paused. */
    for (c = 0; c < 10; ++c) {
        int paused;

        for (paused = 0; paused < 2; ++paused) {
            switch (c) {
            case 0: /* zero duration */
                bad = base[1];
                bad.duration_ticks = 0u;
                break;
            case 1: /* unknown mode */
                bad = base[0];
                bad.mode = BAD_MODE_HIGH;
                break;
            case 2: /* ONCE phase beyond duration */
                bad = base[0];
                bad.phase_ticks = 11u;
                break;
            case 3: /* ONCE finished flag disagrees with phase */
                bad = base[0];
                bad.phase_ticks = 3u;
                bad.finished = true;
                break;
            case 4: /* ONCE at duration but not finished */
                bad = base[0];
                bad.phase_ticks = 10u;
                bad.finished = false;
                break;
            case 5: /* LOOP phase == duration is out of range */
                bad = base[1];
                bad.phase_ticks = 10u;
                break;
            case 6: /* LOOP never finishes */
                bad = base[1];
                bad.finished = true;
                break;
            case 7: /* PING_PONG phase == period is out of range */
                bad = base[2];
                bad.phase_ticks = 20u;
                break;
            case 8: /* PING_PONG never finishes */
                bad = base[2];
                bad.finished = true;
                break;
            default: /* astronomically large phase */
                bad = base[1];
                bad.phase_ticks = UINT64_MAX;
                break;
            }
            bad.paused = paused != 0;
            memcpy(&before, &bad, sizeof before);

            CHECK(pixelica_anim_player_advance(&bad, 1u) == PIXELICA_ERR_INVALID_ARG);
            CHECK(memcmp(&before, &bad, sizeof before) == 0);
            CHECK(pixelica_anim_player_advance(&bad, 0u) == PIXELICA_ERR_INVALID_ARG);
            CHECK(memcmp(&before, &bad, sizeof before) == 0);
            tick = 0xDEADBEEFu;
            CHECK(pixelica_anim_player_tick(&bad, &tick) == PIXELICA_ERR_INVALID_ARG);
            CHECK(tick == 0xDEADBEEFu);
        }
    }

    /* Boundary phases that ARE valid. */
    bad = base[0];
    bad.phase_ticks = 10u;
    bad.finished = true;
    CHECK(pixelica_anim_player_tick(&bad, &tick) == PIXELICA_OK && tick == 10u);
    bad = base[1];
    bad.phase_ticks = 9u;
    CHECK(pixelica_anim_player_tick(&bad, &tick) == PIXELICA_OK && tick == 9u);
    bad = base[2];
    bad.phase_ticks = 19u;
    CHECK(pixelica_anim_player_tick(&bad, &tick) == PIXELICA_OK && tick == 1u);
    bad.phase_ticks = 10u;
    CHECK(pixelica_anim_player_tick(&bad, &tick) == PIXELICA_OK && tick == 10u);
}

/* Closed-form reference for the folded tick after `total` accumulated ticks. */
static uint32_t ref_tick(PIXELICA_AnimMode mode, uint32_t duration, uint64_t total)
{
    uint64_t d = duration;

    switch (mode) {
    case PIXELICA_ANIM_ONCE:
        return (uint32_t)(total < d ? total : d);
    case PIXELICA_ANIM_LOOP:
        return (uint32_t)(total % d);
    case PIXELICA_ANIM_PING_PONG: {
        uint64_t p = total % (2u * d);

        return (uint32_t)(p <= d ? p : 2u * d - p);
    }
    }
    return 0xFFFFFFFFu;
}

static void test_player_partition_property(void)
{
    static const uint32_t durations[] = {1u, 2u, 3u, 7u, 100u, 65535u, UINT32_MAX - 1u, UINT32_MAX};
    size_t m;
    size_t di;

    for (m = 0; m < 3; ++m) {
        for (di = 0; di < sizeof durations / sizeof durations[0]; ++di) {
            uint32_t d = durations[di];
            PIXELICA_AnimPlayer whole = make_player(d, k_modes[m]);
            PIXELICA_AnimPlayer split = make_player(d, k_modes[m]);
            uint64_t total = 0u;
            int i;

            for (i = 0; i < 600; ++i) {
                uint32_t a;
                uint32_t half;

                switch (i % 4) {
                case 0:
                    a = rng_next() % 5u;
                    break;
                case 1:
                    a = rng_next() % 1000u;
                    break;
                case 2:
                    a = rng_next();
                    break;
                default:
                    a = UINT32_MAX - rng_next() % 3u;
                    break;
                }
                half = a / 2u;
                advance_ok(&whole, a);
                advance_ok(&split, half);
                advance_ok(&split, a - half);
                total += a;

                CHECK(whole.phase_ticks == split.phase_ticks);
                CHECK(whole.finished == split.finished);
                CHECK(player_tick(&whole) == player_tick(&split));
                CHECK(player_tick(&whole) == ref_tick(k_modes[m], d, total));
                CHECK(whole.finished == (k_modes[m] == PIXELICA_ANIM_ONCE && total >= d));
                CHECK(whole.duration_ticks == d && whole.mode == k_modes[m]);
            }
        }
    }
}

static void test_player_unit_step_model(void)
{
    uint32_t d;
    size_t m;

    /* A trivially simple unit-step model, independent of any bulk arithmetic. */
    for (m = 0; m < 3; ++m) {
        for (d = 1u; d <= 9u; ++d) {
            PIXELICA_AnimPlayer player = make_player(d, k_modes[m]);
            uint32_t ref_phase = 0u;
            int i;

            for (i = 0; i < 200; ++i) {
                uint32_t a = rng_next() % 40u;
                uint32_t k;

                for (k = 0u; k < a; ++k) {
                    if (k_modes[m] == PIXELICA_ANIM_ONCE) {
                        if (ref_phase < d) {
                            ++ref_phase;
                        }
                    } else if (k_modes[m] == PIXELICA_ANIM_LOOP) {
                        ref_phase = (ref_phase + 1u) % d;
                    } else {
                        ref_phase = (ref_phase + 1u) % (2u * d);
                    }
                }
                advance_ok(&player, a);
                CHECK_EQ_LONG(player.phase_ticks, ref_phase);
                if (k_modes[m] == PIXELICA_ANIM_PING_PONG) {
                    CHECK_EQ_LONG(player_tick(&player), ref_phase <= d ? ref_phase : 2u * d - ref_phase);
                } else {
                    CHECK_EQ_LONG(player_tick(&player), ref_phase);
                }
            }
        }
    }
}

static void test_player_drives_sample(void)
{
    /* A seamless LOOP: the last key equals the first, and the player wraps at
     * the duration so the final key tick is never sampled twice. */
    PIXELICA_AnimKeyframe keys[3];
    PIXELICA_AnimPlayer loop = make_player(20u, PIXELICA_ANIM_LOOP);
    PIXELICA_AnimPlayer once = make_player(20u, PIXELICA_ANIM_ONCE);
    int i;

    keys[0] = key(0u, 0.0f, PIXELICA_EASE_LINEAR);
    keys[1] = key(10u, 1.0f, PIXELICA_EASE_LINEAR);
    keys[2] = key(20u, 0.0f, PIXELICA_EASE_LINEAR);

    for (i = 0; i < 100; ++i) {
        float v = sample_ok(keys, 3, player_tick(&loop));

        CHECK(v >= 0.0f && v <= 1.0f);
        CHECK_NEAR(v, (double)(i % 20 <= 10 ? i % 20 : 20 - i % 20) / 10.0, EPS);
        advance_ok(&loop, 1u);
    }

    /* ONCE ends on the last key's value. */
    advance_ok(&once, 1000u);
    CHECK(once.finished);
    CHECK(sample_ok(keys, 3, player_tick(&once)) == 0.0f);
    advance_ok(&once, 0u);
}

/* -------------------------------------------------------------- transform */

static PIXELICA_Transform make_xf(float tx, float ty, float tz, float rx, float ry, float rz,
                             float sx, float sy, float sz, float px, float py, float pz)
{
    PIXELICA_Transform t;

    pixelica_transform_identity(&t);
    t.translation = vec3(tx, ty, tz);
    t.rotation = vec3(rx, ry, rz);
    t.scale = vec3(sx, sy, sz);
    t.pivot = vec3(px, py, pz);
    return t;
}

static void matrix_ok(float m[16], const PIXELICA_Transform *t)
{
    CHECK(pixelica_transform_matrix(m, t) == PIXELICA_OK);
}

static void map_point(const float m[16], float x, float y, float z, double out[3])
{
    float r[4];

    pixelica_mat4_transform_point(r, m, vec3(x, y, z));
    out[0] = (double)r[0];
    out[1] = (double)r[1];
    out[2] = (double)r[2];
}

static void check_point(const float m[16], float x, float y, float z, double ex, double ey,
                        double ez)
{
    double r[3];

    map_point(m, x, y, z, r);
    CHECK_NEAR(r[0], ex, 1e-4);
    CHECK_NEAR(r[1], ey, 1e-4);
    CHECK_NEAR(r[2], ez, 1e-4);
}

static void test_transform_identity(void)
{
    PIXELICA_Transform t;
    float m[16];
    float id[16];
    int i;

    memset(&t, 0xAB, sizeof t);
    pixelica_transform_identity(&t);
    CHECK(t.translation.x == 0.0f && t.translation.y == 0.0f && t.translation.z == 0.0f);
    CHECK(t.rotation.x == 0.0f && t.rotation.y == 0.0f && t.rotation.z == 0.0f);
    CHECK(t.scale.x == 1.0f && t.scale.y == 1.0f && t.scale.z == 1.0f);
    CHECK(t.pivot.x == 0.0f && t.pivot.y == 0.0f && t.pivot.z == 0.0f);

    pixelica_transform_identity(NULL); /* no-op, must not crash */

    matrix_ok(m, &t);
    pixelica_mat4_identity(id);
    for (i = 0; i < 16; ++i) {
        CHECK_NEAR(m[i], id[i], 1e-7);
    }
}

static void test_transform_trs(void)
{
    float m[16];
    PIXELICA_Transform t;

    /* Translation lives in elements 12..14 (column-major). */
    t = make_xf(1.0f, 2.0f, 3.0f, 0, 0, 0, 1, 1, 1, 0, 0, 0);
    matrix_ok(m, &t);
    CHECK_NEAR(m[12], 1.0, EPS);
    CHECK_NEAR(m[13], 2.0, EPS);
    CHECK_NEAR(m[14], 3.0, EPS);
    check_point(m, 4.0f, 5.0f, 6.0f, 5.0, 7.0, 9.0);

    t = make_xf(0, 0, 0, 0, 0, 0, 2.0f, 3.0f, 4.0f, 0, 0, 0);
    matrix_ok(m, &t);
    check_point(m, 1.0f, 1.0f, 1.0f, 2.0, 3.0, 4.0);

    /* Euler angles in radians, counter-clockwise about each axis. */
    t = make_xf(0, 0, 0, 0, 0, PI_F * 0.5f, 1, 1, 1, 0, 0, 0);
    matrix_ok(m, &t);
    check_point(m, 1.0f, 0.0f, 0.0f, 0.0, 1.0, 0.0);
    t = make_xf(0, 0, 0, 0, PI_F * 0.5f, 0, 1, 1, 1, 0, 0, 0);
    matrix_ok(m, &t);
    check_point(m, 0.0f, 0.0f, 1.0f, 1.0, 0.0, 0.0);
    t = make_xf(0, 0, 0, PI_F * 0.5f, 0, 0, 1, 1, 1, 0, 0, 0);
    matrix_ok(m, &t);
    check_point(m, 0.0f, 1.0f, 0.0f, 0.0, 0.0, 1.0);

    /* Rotation order: X first, then Y, then Z. */
    t = make_xf(0, 0, 0, PI_F * 0.5f, PI_F * 0.5f, 0, 1, 1, 1, 0, 0, 0);
    matrix_ok(m, &t);
    check_point(m, 0.0f, 1.0f, 0.0f, 1.0, 0.0, 0.0); /* Y-then-X would give (0,0,1) */
    t = make_xf(0, 0, 0, PI_F * 0.5f, 0, PI_F * 0.5f, 1, 1, 1, 0, 0, 0);
    matrix_ok(m, &t);
    check_point(m, 1.0f, 0.0f, 0.0f, 0.0, 1.0, 0.0); /* Z-then-X would give (0,0,1) */

    /* Scale is applied before rotation, translation last. */
    t = make_xf(0, 0, 0, 0, 0, PI_F * 0.5f, 2.0f, 1.0f, 1.0f, 0, 0, 0);
    matrix_ok(m, &t);
    check_point(m, 1.0f, 0.0f, 0.0f, 0.0, 2.0, 0.0);
    t = make_xf(10.0f, 0, 0, 0, 0, PI_F * 0.5f, 1, 1, 1, 0, 0, 0);
    matrix_ok(m, &t);
    check_point(m, 0.0f, 0.0f, 0.0f, 10.0, 0.0, 0.0);
    check_point(m, 1.0f, 0.0f, 0.0f, 10.0, 1.0, 0.0);

    /* Affine bottom row. */
    t = make_xf(1, -2, 3, 0.3f, -1.2f, 2.5f, 1.5f, -0.5f, 2.0f, 0.5f, 1.0f, -1.0f);
    matrix_ok(m, &t);
    CHECK(m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f);
    CHECK_NEAR(m[15], 1.0, 1e-7);
    CHECK(pixelica_mat4_is_finite(m) == 1);
}

static void test_transform_pivot(void)
{
    float m[16];
    PIXELICA_Transform t;
    size_t k;
    static const float angles[][3] = {
        {0.0f, 0.0f, 0.0f}, {0.4f, 0.0f, 0.0f}, {0.0f, -1.1f, 0.0f},
        {0.0f, 0.0f, 2.0f}, {0.7f, -0.3f, 1.9f}, {PI_F, PI_F * 0.5f, -PI_F},
    };
    static const float scales[][3] = {
        {1.0f, 1.0f, 1.0f}, {2.0f, 3.0f, 0.5f}, {0.0f, 1.0f, 1.0f},
        {-1.0f, 2.0f, -3.0f}, {0.0f, 0.0f, 0.0f},
    };
    size_t s;

    /* The pivot point itself ends at translation + pivot for ANY rotation/scale. */
    for (k = 0; k < sizeof angles / sizeof angles[0]; ++k) {
        for (s = 0; s < sizeof scales / sizeof scales[0]; ++s) {
            t = make_xf(5.0f, 6.0f, 7.0f, angles[k][0], angles[k][1], angles[k][2], scales[s][0],
                        scales[s][1], scales[s][2], 1.0f, 2.0f, 3.0f);
            matrix_ok(m, &t);
            check_point(m, 1.0f, 2.0f, 3.0f, 6.0, 8.0, 10.0);
        }
    }

    /* Rotate about a pivot: (2,0,0) is one unit right of the pivot (1,0,0). */
    t = make_xf(0, 0, 0, 0, 0, PI_F * 0.5f, 1, 1, 1, 1.0f, 0.0f, 0.0f);
    matrix_ok(m, &t);
    check_point(m, 2.0f, 0.0f, 0.0f, 1.0, 1.0, 0.0);
    check_point(m, 1.0f, 0.0f, 0.0f, 1.0, 0.0, 0.0);

    /* Pivot is in mesh-local coordinates BEFORE scale. */
    t = make_xf(0, 0, 0, 0, 0, 0, 2.0f, 2.0f, 2.0f, 1.0f, 0.0f, 0.0f);
    matrix_ok(m, &t);
    check_point(m, 2.0f, 0.0f, 0.0f, 3.0, 0.0, 0.0);
    check_point(m, 0.0f, 0.0f, 0.0f, -1.0, 0.0, 0.0);

    /* Translation is the displacement of the local origin when R and S are identity. */
    t = make_xf(5.0f, 0, 0, 0, 0, 0, 1, 1, 1, 1.0f, 0.0f, 0.0f);
    matrix_ok(m, &t);
    check_point(m, 0.0f, 0.0f, 0.0f, 5.0, 0.0, 0.0);
    check_point(m, 1.0f, 0.0f, 0.0f, 6.0, 0.0, 0.0);

    /* Zero scale collapses everything to the pivot but stays finite. */
    t = make_xf(0, 0, 0, 0, 0, 0, 0, 0, 0, 1.0f, 2.0f, 3.0f);
    matrix_ok(m, &t);
    check_point(m, 9.0f, -4.0f, 2.0f, 1.0, 2.0, 3.0);
}

static void test_transform_matches_documented_product(void)
{
    int n;

    for (n = 0; n < 200; ++n) {
        PIXELICA_Transform t = make_xf(
            rng_range(-5, 5), rng_range(-5, 5), rng_range(-5, 5), rng_range(-4, 4),
            rng_range(-4, 4), rng_range(-4, 4), rng_range(-2, 2), rng_range(-2, 2),
            rng_range(-2, 2), rng_range(-3, 3), rng_range(-3, 3), rng_range(-3, 3));
        float m[16];
        float expect[16];
        float a[16];
        float b[16];
        int i;

        matrix_ok(m, &t);

        /* out = T(translation) * T(pivot) * Rz * Ry * Rx * S * T(-pivot) */
        pixelica_mat4_translation(expect, t.translation.x, t.translation.y, t.translation.z);
        pixelica_mat4_translation(a, t.pivot.x, t.pivot.y, t.pivot.z);
        pixelica_mat4_multiply(expect, expect, a);
        pixelica_mat4_rotation_z(a, t.rotation.z);
        pixelica_mat4_multiply(expect, expect, a);
        pixelica_mat4_rotation_y(a, t.rotation.y);
        pixelica_mat4_multiply(expect, expect, a);
        pixelica_mat4_rotation_x(a, t.rotation.x);
        pixelica_mat4_multiply(expect, expect, a);
        pixelica_mat4_scale(a, t.scale.x, t.scale.y, t.scale.z);
        pixelica_mat4_multiply(expect, expect, a);
        pixelica_mat4_translation(b, -t.pivot.x, -t.pivot.y, -t.pivot.z);
        pixelica_mat4_multiply(expect, expect, b);

        for (i = 0; i < 16; ++i) {
            CHECK_NEAR(m[i], expect[i], 1e-3 * (1.0 + fabs((double)expect[i])));
        }
    }

    /* Pure rotation keeps unit columns (no hidden scaling). */
    {
        PIXELICA_Transform t = make_xf(0, 0, 0, 0.3f, -1.2f, 2.5f, 1, 1, 1, 0, 0, 0);
        float m[16];
        int c;

        matrix_ok(m, &t);
        for (c = 0; c < 3; ++c) {
            double len = sqrt((double)m[c * 4] * m[c * 4] + (double)m[c * 4 + 1] * m[c * 4 + 1] +
                              (double)m[c * 4 + 2] * m[c * 4 + 2]);

            CHECK_NEAR(len, 1.0, 1e-5);
        }
    }

    /* Deterministic: identical input gives bit-identical matrices. */
    {
        PIXELICA_Transform t = make_xf(1, 2, 3, 0.1f, 0.2f, 0.3f, 1.1f, 1.2f, 1.3f, 0.4f, 0.5f, 0.6f);
        float a[16];
        float b[16];

        matrix_ok(a, &t);
        matrix_ok(b, &t);
        CHECK(memcmp(a, b, sizeof a) == 0);
    }
}

static void test_transform_invalid(void)
{
    float m[16];
    float snapshot[16];
    PIXELICA_Transform t = make_xf(1, 2, 3, 0.1f, 0.2f, 0.3f, 1, 1, 1, 0.5f, 0.5f, 0.5f);
    size_t i;
    size_t p;
    float poison[3];
    int k;

    for (k = 0; k < 16; ++k) {
        m[k] = 777.0f + (float)k;
    }
    memcpy(snapshot, m, sizeof m);

    CHECK(pixelica_transform_matrix(NULL, &t) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_transform_matrix(m, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(snapshot, m, sizeof m) == 0);

    poison[0] = pixelica_t_nan();
    poison[1] = pixelica_t_inf();
    poison[2] = -pixelica_t_inf();

    /* Any non-finite component of the 12 floats is rejected. */
    for (i = 0; i < 12; ++i) {
        for (p = 0; p < 3; ++p) {
            float comps[12];
            PIXELICA_Transform bad;

            memcpy(comps, &t, sizeof comps);
            comps[i] = poison[p];
            memcpy(&bad, comps, sizeof bad);
            CHECK(pixelica_transform_matrix(m, &bad) == PIXELICA_ERR_INVALID_ARG);
            CHECK(memcmp(snapshot, m, sizeof m) == 0);
        }
    }

    /* Finite inputs whose matrix overflows are rejected, output untouched. */
    {
        PIXELICA_Transform big = make_xf(0, 0, 0, 0, 0, 0, FLT_MAX, FLT_MAX, FLT_MAX, 1e30f, 0, 0);

        CHECK(pixelica_transform_matrix(m, &big) == PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(snapshot, m, sizeof m) == 0);
        big = make_xf(0, 0, 0, 0, 0, 0, 1, FLT_MAX, 1, 0, 1e30f, 0);
        CHECK(pixelica_transform_matrix(m, &big) == PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(snapshot, m, sizeof m) == 0);
    }

    /* Representable extremes are fine and finite. */
    {
        PIXELICA_Transform ok = make_xf(FLT_MAX, -FLT_MAX, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0);

        CHECK(pixelica_transform_matrix(m, &ok) == PIXELICA_OK);
        CHECK(pixelica_mat4_is_finite(m) == 1);
        CHECK(m[12] == FLT_MAX && m[13] == -FLT_MAX);
        ok = make_xf(0, 0, 0, 1e30f, -1e30f, 1e30f, 1, 1, 1, 0, 0, 0);
        CHECK(pixelica_transform_matrix(m, &ok) == PIXELICA_OK);
        CHECK(pixelica_mat4_is_finite(m) == 1);
    }
}

static void test_transform_parent_child(void)
{
    float parent[16];
    float child[16];
    float world[16];
    PIXELICA_Transform tp;
    PIXELICA_Transform tc;
    int n;

    /* Child translated inside a rotated + moved parent. */
    tp = make_xf(10.0f, 0, 0, 0, 0, PI_F * 0.5f, 1, 1, 1, 0, 0, 0);
    tc = make_xf(2.0f, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0);
    matrix_ok(parent, &tp);
    matrix_ok(child, &tc);
    pixelica_mat4_multiply(world, parent, child);
    check_point(world, 0.0f, 0.0f, 0.0f, 10.0, 2.0, 0.0);

    /* Arm hanging from a shoulder pivot: swings a quarter turn about its top. */
    tp = make_xf(10.0f, 0, 0, 0, 0, 0, 2.0f, 2.0f, 2.0f, 0, 0, 0);
    tc = make_xf(0, 0, 0, 0, 0, PI_F * 0.5f, 1, 1, 1, 0.0f, 1.0f, 0.0f);
    matrix_ok(parent, &tp);
    matrix_ok(child, &tc);
    pixelica_mat4_multiply(world, parent, child);
    check_point(world, 0.0f, 1.0f, 0.0f, 10.0, 2.0, 0.0);  /* shoulder stays put */
    check_point(world, 0.0f, 0.0f, 0.0f, 12.0, 2.0, 0.0);  /* hand: (1,1,0) * 2 + parent */

    /* world * p == parent * (child * p) for arbitrary hierarchies. */
    for (n = 0; n < 100; ++n) {
        float px = rng_range(-3, 3);
        float py = rng_range(-3, 3);
        float pz = rng_range(-3, 3);
        float mid[4];
        float direct[4];
        float two_step[4];
        int i;

        tp = make_xf(rng_range(-5, 5), rng_range(-5, 5), rng_range(-5, 5), rng_range(-3, 3),
                     rng_range(-3, 3), rng_range(-3, 3), rng_range(0.5f, 2), rng_range(0.5f, 2),
                     rng_range(0.5f, 2), rng_range(-1, 1), rng_range(-1, 1), rng_range(-1, 1));
        tc = make_xf(rng_range(-5, 5), rng_range(-5, 5), rng_range(-5, 5), rng_range(-3, 3),
                     rng_range(-3, 3), rng_range(-3, 3), rng_range(0.5f, 2), rng_range(0.5f, 2),
                     rng_range(0.5f, 2), rng_range(-1, 1), rng_range(-1, 1), rng_range(-1, 1));
        matrix_ok(parent, &tp);
        matrix_ok(child, &tc);
        pixelica_mat4_multiply(world, parent, child);
        pixelica_mat4_transform_point(direct, world, vec3(px, py, pz));
        pixelica_mat4_transform_point(mid, child, vec3(px, py, pz));
        pixelica_mat4_transform_point(two_step, parent, vec3(mid[0], mid[1], mid[2]));
        for (i = 0; i < 3; ++i) {
            CHECK_NEAR(direct[i], two_step[i], 1e-3 * (1.0 + fabs((double)two_step[i])));
        }
    }
}

static void test_transform_animated_pivot_sweep(void)
{
    /* Sampled swing angle drives the arm: its shoulder never moves. */
    PIXELICA_AnimKeyframe keys[3];
    uint32_t tick;

    keys[0] = key(0u, -0.8f, PIXELICA_EASE_SMOOTHSTEP);
    keys[1] = key(30u, 0.9f, PIXELICA_EASE_QUAD_IN_OUT);
    keys[2] = key(60u, -0.8f, PIXELICA_EASE_LINEAR);

    for (tick = 0u; tick <= 60u; ++tick) {
        float angle = sample_ok(keys, 3, tick);
        PIXELICA_Transform t = make_xf(2.0f, 1.0f, 0.0f, 0.0f, 0.0f, angle, 1, 1, 1, 0.0f, 0.5f, 0.0f);
        float m[16];
        double hand[3];

        matrix_ok(m, &t);
        check_point(m, 0.0f, 0.5f, 0.0f, 2.0, 1.5, 0.0);
        map_point(m, 0.0f, -0.5f, 0.0f, hand);
        /* The hand stays exactly one unit below the shoulder in distance. */
        CHECK_NEAR(sqrt((hand[0] - 2.0) * (hand[0] - 2.0) + (hand[1] - 1.5) * (hand[1] - 1.5)), 1.0,
                   1e-4);
    }
}

#ifdef PIXELICA_TEST_WITH_3D
/* ----------------------------------------------------------------- render */

static void test_render_pivot_rotation(void)
{
    enum { W = 64, H = 64 };
    /* CCW with y up, centered on (0.3, 0.1). Its edges and diagonal avoid every
     * pixel center on a 64x64 target, so half-turn symmetry is exact in pixels. */
    static const PIXELICA_Vec3 square[4] = {
        {0.05f, -0.05f, 0.0f}, {0.55f, -0.05f, 0.0f}, {0.55f, 0.25f, 0.0f}, {0.05f, 0.25f, 0.0f},
    };
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    static const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    const uint16_t fill = PIXELICA_RGB565(240, 60, 60);
    PIXELICA_Mesh mesh = {square, 4u, idx, 6u, fill};
    PIXELICA_Config cfg = pixelica_t_config(W, H);
    PIXELICA_Context *ctx = NULL;
    uint64_t hash_plain;
    uint64_t hash_centered;
    uint64_t hash_corner;
    uint32_t area_plain;
    uint32_t area_corner;
    PIXELICA_Transform t;
    float m[16];

    if (pixelica_create(&cfg, NULL, &ctx) != PIXELICA_OK) {
        CHECK(0);
        return;
    }
    pixelica_set_view_projection(ctx, identity);

    pixelica_begin_frame(ctx, 0x0000u);
    CHECK(pixelica_draw_mesh(ctx, &mesh, identity) == PIXELICA_OK);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    hash_plain = pixelica_t_hash16(pixelica_color_buffer(ctx), (size_t)W * H);
    area_plain = pixelica_t_count_color(ctx, W, H, fill);
    CHECK(area_plain > 100u);

    /* Half turn about the rectangle's own center: the same pixels. */
    t = make_xf(0, 0, 0, 0, 0, PI_F, 1, 1, 1, 0.3f, 0.1f, 0.0f);
    matrix_ok(m, &t);
    pixelica_begin_frame(ctx, 0x0000u);
    CHECK(pixelica_draw_mesh(ctx, &mesh, m) == PIXELICA_OK);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    hash_centered = pixelica_t_hash16(pixelica_color_buffer(ctx), (size_t)W * H);
    CHECK(hash_centered == hash_plain);

    /* The same half turn about the origin sweeps it to the other side. */
    t = make_xf(0, 0, 0, 0, 0, PI_F, 1, 1, 1, 0, 0, 0);
    matrix_ok(m, &t);
    pixelica_begin_frame(ctx, 0x0000u);
    CHECK(pixelica_draw_mesh(ctx, &mesh, m) == PIXELICA_OK);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    hash_corner = pixelica_t_hash16(pixelica_color_buffer(ctx), (size_t)W * H);
    area_corner = pixelica_t_count_color(ctx, W, H, fill);
    CHECK(hash_corner != hash_plain);
    CHECK(area_corner > 100u);
    CHECK(pixelica_t_color_at(ctx, W, 41, 28) == 0x0000u); /* now empty, was inside the rectangle */

    /* A non-finite pose is rejected before it can reach the renderer. */
    t.rotation.z = pixelica_t_nan();
    CHECK(pixelica_transform_matrix(m, &t) == PIXELICA_ERR_INVALID_ARG);

    CHECK(pixelica_memory_guards_intact(ctx) == 1);
    pixelica_destroy(ctx);
}
#endif /* PIXELICA_TEST_WITH_3D */

int main(void)
{
    test_ease_endpoints_and_clamp();
    test_ease_values();
    test_ease_properties();
    test_ease_invalid();

    test_tween_basic();
    test_tween_extremes();
    test_tween_invalid();

    test_sample_intervals_and_clamps();
    test_sample_each_ease();
    test_sample_single_and_step_keys();
    test_sample_uint32_spans();
    test_sample_many_keys();
    test_sample_invalid_and_unchanged();
    test_sample_aliasing();

    test_player_init();
    test_player_once();
    test_player_loop();
    test_player_ping_pong();
    test_player_pause_and_restart();
    test_player_invalid_state();
    test_player_partition_property();
    test_player_unit_step_model();
    test_player_drives_sample();

    test_transform_identity();
    test_transform_trs();
    test_transform_pivot();
    test_transform_matches_documented_product();
    test_transform_invalid();
    test_transform_parent_child();
    test_transform_animated_pivot_sweep();

#ifdef PIXELICA_TEST_WITH_3D
    test_render_pivot_rotation();
#endif

    return pixelica_t_finish("test_animation");
}

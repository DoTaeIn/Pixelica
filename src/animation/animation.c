/*
 * Pixelica - easing, keyframe sampling, playback cursor and part transform.
 * See include/pixelica/animation.h.
 *
 * Interpolation and matrix composition run in double precision: a uint32_t
 * tick ratio needs more than float's 24 bits to stay below 1 inside an
 * interval, and opposite +-FLT_MAX endpoints or large pivot/scale products
 * would overflow float intermediates. Every function validates all of its
 * inputs into locals before writing a single output byte.
 */
#include <float.h>
#include <math.h>
#include <string.h>

#include "pixelica/animation.h"

#include "anim_internal.h"

int pixelica_anim_ease_valid(PIXELICA_Ease ease)
{
    switch (ease) {
    case PIXELICA_EASE_STEP:
    case PIXELICA_EASE_LINEAR:
    case PIXELICA_EASE_QUAD_IN:
    case PIXELICA_EASE_QUAD_OUT:
    case PIXELICA_EASE_QUAD_IN_OUT:
    case PIXELICA_EASE_SMOOTHSTEP:
        return 1;
    default:
        return 0;
    }
}

static int pixelica_mode_valid(PIXELICA_AnimMode mode)
{
    switch (mode) {
    case PIXELICA_ANIM_ONCE:
    case PIXELICA_ANIM_LOOP:
    case PIXELICA_ANIM_PING_PONG:
        return 1;
    default:
        return 0;
    }
}

double pixelica_anim_ease_eval(PIXELICA_Ease ease, double t)
{
    double r;

    if (t <= 0.0) {
        t = 0.0;
    } else if (t >= 1.0) {
        t = 1.0;
    }
    switch (ease) {
    case PIXELICA_EASE_STEP:
        r = t < 1.0 ? 0.0 : 1.0;
        break;
    case PIXELICA_EASE_QUAD_IN:
        r = t * t;
        break;
    case PIXELICA_EASE_QUAD_OUT:
        r = t * (2.0 - t);
        break;
    case PIXELICA_EASE_QUAD_IN_OUT:
        r = t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
        break;
    case PIXELICA_EASE_SMOOTHSTEP:
        r = t * t * (3.0 - 2.0 * t);
        break;
    case PIXELICA_EASE_LINEAR:
    default:
        r = t;
        break;
    }
    if (r < 0.0) {
        r = 0.0;
    } else if (r > 1.0) {
        r = 1.0;
    }
    return r;
}

float pixelica_anim_lerp(float from, float to, double e)
{
    double lo;
    double hi;
    double r;

    if (e <= 0.0) {
        return from;
    }
    if (e >= 1.0) {
        return to;
    }
    /* Floats are at most FLT_MAX in magnitude, so the double difference
     * (<= 2 * FLT_MAX) and the result cannot overflow. */
    r = (double)from + ((double)to - (double)from) * e;
    lo = from < to ? (double)from : (double)to;
    hi = from < to ? (double)to : (double)from;
    if (r < lo) {
        r = lo;
    } else if (r > hi) {
        r = hi;
    }
    /* r is within two floats, so the conversion is in range. */
    return (float)r;
}

PIXELICA_Status pixelica_anim_ease(PIXELICA_Ease ease, float t, float *out)
{
    if (out == NULL || !pixelica_anim_ease_valid(ease) || !isfinite(t)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = (float)pixelica_anim_ease_eval(ease, (double)t);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_anim_tween(float from, float to, float t, PIXELICA_Ease ease,
                          float *out)
{
    if (out == NULL || !pixelica_anim_ease_valid(ease) || !isfinite(from) ||
        !isfinite(to) || !isfinite(t)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = pixelica_anim_lerp(from, to, pixelica_anim_ease_eval(ease, (double)t));
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_anim_sample(const PIXELICA_AnimKeyframe *keys, size_t count,
                           uint32_t tick, float *out)
{
    size_t i;
    size_t lo;
    size_t hi;
    double t;
    float result;

    if (keys == NULL || count == 0 || out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* Validate the whole track every call, even when the sample clamps. */
    for (i = 0; i < count; ++i) {
        if (!isfinite(keys[i].value) || !pixelica_anim_ease_valid(keys[i].ease)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (i > 0 && keys[i].tick <= keys[i - 1].tick) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    if (tick <= keys[0].tick) {
        result = keys[0].value;
    } else if (tick >= keys[count - 1].tick) {
        result = keys[count - 1].value;
    } else {
        /* count >= 2 and keys[lo].tick <= tick < keys[hi].tick. */
        lo = 0;
        hi = count - 1;
        while (hi - lo > 1) {
            size_t mid = lo + (hi - lo) / 2;
            if (keys[mid].tick <= tick) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        if (keys[lo].tick == tick) {
            result = keys[lo].value;
        } else {
            /* Exact uint32_t spans in double: t stays strictly below 1 even
             * for a UINT32_MAX-tick interval. */
            t = (double)(tick - keys[lo].tick) /
                (double)(keys[hi].tick - keys[lo].tick);
            result = pixelica_anim_lerp(keys[lo].value, keys[hi].value,
                              pixelica_anim_ease_eval(keys[lo].ease, t));
        }
    }
    *out = result;
    return PIXELICA_OK;
}

/* 1 if the player's fields satisfy the invariants of its mode. */
static int pixelica_player_valid(const PIXELICA_AnimPlayer *player)
{
    uint64_t duration;
    bool at_end;

    if (player == NULL || player->duration_ticks == 0) {
        return 0;
    }
    duration = player->duration_ticks;
    switch (player->mode) {
    case PIXELICA_ANIM_ONCE:
        at_end = player->phase_ticks == duration;
        return player->phase_ticks <= duration && player->finished == at_end;
    case PIXELICA_ANIM_LOOP:
        return player->phase_ticks < duration && !player->finished;
    case PIXELICA_ANIM_PING_PONG:
        return player->phase_ticks < 2u * duration && !player->finished;
    default:
        return 0;
    }
}

PIXELICA_Status pixelica_anim_player_init(PIXELICA_AnimPlayer *player,
                                uint32_t duration_ticks, PIXELICA_AnimMode mode)
{
    if (player == NULL || duration_ticks == 0 || !pixelica_mode_valid(mode)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    player->duration_ticks = duration_ticks;
    player->phase_ticks = 0;
    player->mode = mode;
    player->paused = false;
    player->finished = false;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_anim_player_advance(PIXELICA_AnimPlayer *player, uint32_t ticks)
{
    uint64_t duration;
    uint64_t next;

    if (!pixelica_player_valid(player)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (player->paused || player->finished || ticks == 0) {
        return PIXELICA_OK;
    }
    duration = player->duration_ticks;
    /* phase < 2^33 and ticks < 2^32: the 64-bit sum cannot overflow. */
    next = player->phase_ticks + (uint64_t)ticks;
    switch (player->mode) {
    case PIXELICA_ANIM_ONCE:
        if (next >= duration) {
            next = duration;
            player->finished = true;
        }
        break;
    case PIXELICA_ANIM_LOOP:
        next %= duration;
        break;
    case PIXELICA_ANIM_PING_PONG:
    default:
        next %= 2u * duration;
        break;
    }
    player->phase_ticks = next;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_anim_player_tick(const PIXELICA_AnimPlayer *player,
                                uint32_t *out_tick)
{
    uint64_t duration;
    uint64_t phase;

    if (out_tick == NULL || !pixelica_player_valid(player)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    duration = player->duration_ticks;
    phase = player->phase_ticks;
    if (player->mode == PIXELICA_ANIM_PING_PONG && phase > duration) {
        phase = 2u * duration - phase;
    }
    /* phase <= duration <= UINT32_MAX for every mode after folding. */
    *out_tick = (uint32_t)phase;
    return PIXELICA_OK;
}

void pixelica_transform_identity(PIXELICA_Transform *transform)
{
    if (transform == NULL) {
        return;
    }
    memset(transform, 0, sizeof *transform);
    transform->scale.x = 1.0f;
    transform->scale.y = 1.0f;
    transform->scale.z = 1.0f;
}

static int pixelica_vec3_finite(PIXELICA_Vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

PIXELICA_Status pixelica_transform_matrix(float out[16], const PIXELICA_Transform *transform)
{
    double m[16];
    float r[16];
    double cx, sx, cy, sy, cz, sz;
    double px, py, pz;
    int i;

    if (out == NULL || transform == NULL ||
        !pixelica_vec3_finite(transform->translation) ||
        !pixelica_vec3_finite(transform->rotation) ||
        !pixelica_vec3_finite(transform->scale) ||
        !pixelica_vec3_finite(transform->pivot)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    /* Same float trig as pixelica_mat4_rotation_x/y/z. */
    cx = cosf(transform->rotation.x);
    sx = sinf(transform->rotation.x);
    cy = cosf(transform->rotation.y);
    sy = sinf(transform->rotation.y);
    cz = cosf(transform->rotation.z);
    sz = sinf(transform->rotation.z);

    /* Columns of Rz * Ry * Rx, each scaled by its scale component (R * S). */
    m[0] = cz * cy * transform->scale.x;
    m[1] = sz * cy * transform->scale.x;
    m[2] = -sy * transform->scale.x;
    m[3] = 0.0;
    m[4] = (cz * sy * sx - sz * cx) * transform->scale.y;
    m[5] = (sz * sy * sx + cz * cx) * transform->scale.y;
    m[6] = cy * sx * transform->scale.y;
    m[7] = 0.0;
    m[8] = (cz * sy * cx + sz * sx) * transform->scale.z;
    m[9] = (sz * sy * cx - cz * sx) * transform->scale.z;
    m[10] = cy * cx * transform->scale.z;
    m[11] = 0.0;

    /* Translation column: translation + pivot - (R * S) * pivot. */
    px = transform->pivot.x;
    py = transform->pivot.y;
    pz = transform->pivot.z;
    m[12] = (double)transform->translation.x +
            (px - (m[0] * px + m[4] * py + m[8] * pz));
    m[13] = (double)transform->translation.y +
            (py - (m[1] * px + m[5] * py + m[9] * pz));
    m[14] = (double)transform->translation.z +
            (pz - (m[2] * px + m[6] * py + m[10] * pz));
    m[15] = 1.0;

    /* Finite float inputs keep every double term far below DBL_MAX; reject
     * elements that do not fit a float before converting them. */
    for (i = 0; i < 16; ++i) {
        if (!(fabs(m[i]) <= (double)FLT_MAX)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        r[i] = (float)m[i];
    }
    memcpy(out, r, sizeof r);
    return PIXELICA_OK;
}

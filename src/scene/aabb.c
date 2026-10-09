/*
 * Pixelica - axis-aligned boxes: overlap test and swept move.
 *
 * Invariant kept by pixelica_aabb_move: the box never overlaps (open interiors)
 * any obstacle. Moving along one axis only changes the box's extent on that
 * axis, so only obstacles overlapping the box on the two other axes can be
 * hit; of those, the ones ahead (in the direction of motion) bound the
 * move. The clamp is computed so that the float result touches but never
 * passes the nearest such obstacle, accounting for rounding.
 */
#include <math.h>
#include <string.h>

#include "pixelica/scene.h"

static int pixelica_finite3(PIXELICA_Vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static float pixelica_axis(const PIXELICA_Vec3 *v, int axis)
{
    return axis == 0 ? v->x : (axis == 1 ? v->y : v->z);
}

static void pixelica_set_axis(PIXELICA_Vec3 *v, int axis, float value)
{
    if (axis == 0) {
        v->x = value;
    } else if (axis == 1) {
        v->y = value;
    } else {
        v->z = value;
    }
}

int pixelica_aabb_is_valid(const PIXELICA_AABB *box)
{
    if (box == NULL || !pixelica_finite3(box->min) || !pixelica_finite3(box->max)) {
        return 0;
    }
    return box->min.x <= box->max.x && box->min.y <= box->max.y &&
           box->min.z <= box->max.z;
}

/* Open-interval overlap on one axis. */
static int pixelica_axis_overlaps(const PIXELICA_AABB *a, const PIXELICA_AABB *b, int axis)
{
    return pixelica_axis(&a->min, axis) < pixelica_axis(&b->max, axis) &&
           pixelica_axis(&b->min, axis) < pixelica_axis(&a->max, axis);
}

bool pixelica_aabb_overlaps(const PIXELICA_AABB *a, const PIXELICA_AABB *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return pixelica_axis_overlaps(a, b, 0) && pixelica_axis_overlaps(a, b, 1) &&
           pixelica_axis_overlaps(a, b, 2);
}

/* Float sum rounded to float even with excess-precision evaluation. */
static float pixelica_add_f(float a, float b)
{
    const float r = a + b;
    return r;
}

/*
 * Largest displacement d' toward `d` (|d'| <= |d|, same sign or 0) such
 * that `edge + d'` (float) does not pass `limit`. `edge` is the box face
 * moving toward the obstacle face `limit`; the caller guarantees the
 * obstacle is ahead: limit >= edge for d > 0, limit <= edge for d < 0.
 */
static float pixelica_clamp_to_contact(float edge, float d, float limit)
{
    float allowed;
    int i;

    if (d == 0.0f) {
        return 0.0f;
    }
    if (d > 0.0f) {
        if ((double)edge + (double)d <= (double)limit &&
            pixelica_add_f(edge, d) <= limit) {
            return d;
        }
        allowed = (float)((double)limit - (double)edge);
        if (!(allowed >= 0.0f)) {
            allowed = 0.0f;
        }
        if (allowed > d) {
            allowed = d;
        }
        /* Rounding of edge + allowed may still pass limit by an ulp. */
        for (i = 0; i < 8 && pixelica_add_f(edge, allowed) > limit; ++i) {
            allowed = nextafterf(allowed, 0.0f);
        }
        return pixelica_add_f(edge, allowed) > limit ? 0.0f : allowed;
    }
    if ((double)edge + (double)d >= (double)limit &&
        pixelica_add_f(edge, d) >= limit) {
        return d;
    }
    allowed = (float)((double)limit - (double)edge);
    if (!(allowed <= 0.0f)) {
        allowed = 0.0f;
    }
    if (allowed < d) {
        allowed = d;
    }
    for (i = 0; i < 8 && pixelica_add_f(edge, allowed) < limit; ++i) {
        allowed = nextafterf(allowed, 0.0f);
    }
    return pixelica_add_f(edge, allowed) < limit ? 0.0f : allowed;
}

/* Moves `box` along `axis` by at most d. Returns the applied displacement. */
static float pixelica_sweep_axis(PIXELICA_AABB *box, int axis, float d,
                            const PIXELICA_AABB *obstacles, size_t count)
{
    const int o1 = (axis + 1) % 3;
    const int o2 = (axis + 2) % 3;
    float allowed = d;
    size_t i;

    if (d == 0.0f) {
        return 0.0f;
    }
    for (i = 0; i < count; ++i) {
        const PIXELICA_AABB *ob = &obstacles[i];

        if (!pixelica_axis_overlaps(box, ob, o1) ||
            !pixelica_axis_overlaps(box, ob, o2)) {
            continue; /* cannot be hit by motion along `axis` */
        }
        if (d > 0.0f) {
            const float edge = pixelica_axis(&box->max, axis);
            const float limit = pixelica_axis(&ob->min, axis);
            if (limit >= edge) {
                allowed = pixelica_clamp_to_contact(edge, allowed, limit);
            }
        } else {
            const float edge = pixelica_axis(&box->min, axis);
            const float limit = pixelica_axis(&ob->max, axis);
            if (limit <= edge) {
                allowed = pixelica_clamp_to_contact(edge, allowed, limit);
            }
        }
    }
    pixelica_set_axis(&box->min, axis,
                 pixelica_add_f(pixelica_axis(&box->min, axis), allowed));
    pixelica_set_axis(&box->max, axis,
                 pixelica_add_f(pixelica_axis(&box->max, axis), allowed));
    return allowed;
}

PIXELICA_Status pixelica_aabb_move(PIXELICA_AABB *box, PIXELICA_Vec3 delta,
                         const PIXELICA_AABB *obstacles, size_t count,
                         PIXELICA_Vec3 *out_applied)
{
    PIXELICA_AABB moved;
    PIXELICA_Vec3 applied;
    size_t i;
    int axis;

    if (out_applied != NULL) {
        memset(out_applied, 0, sizeof *out_applied);
    }
    if (box == NULL || (obstacles == NULL && count > 0) ||
        !pixelica_aabb_is_valid(box) || !pixelica_finite3(delta)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < count; ++i) {
        if (!pixelica_aabb_is_valid(&obstacles[i]) ||
            pixelica_aabb_overlaps(box, &obstacles[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    moved = *box;
    memset(&applied, 0, sizeof applied);
    for (axis = 0; axis < 3; ++axis) {
        pixelica_set_axis(&applied, axis,
                     pixelica_sweep_axis(&moved, axis, pixelica_axis(&delta, axis),
                                    obstacles, count));
    }
    if (!pixelica_aabb_is_valid(&moved)) {
        return PIXELICA_ERR_INVALID_ARG; /* the move overflowed the float range */
    }
    *box = moved;
    if (out_applied != NULL) {
        *out_applied = applied;
    }
    return PIXELICA_OK;
}

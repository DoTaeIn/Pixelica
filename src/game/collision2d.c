/*
 * Pixelica - 2D axis-aligned collision and platformer bodies.
 *
 * Invariant kept by the sweep: the moving box never overlaps (open
 * interiors) any obstacle. Moving along one axis only changes the box's
 * extent on that axis, so only obstacles overlapping it on the other axis can
 * be hit; of those, every one ahead bounds the move however far away it is
 * (no tunneling). The clamp makes the float result touch but never pass the
 * nearest face, accounting for rounding. Work happens on copies; callers'
 * memory is written only after everything succeeded.
 */
#include <float.h>
#include <math.h>
#include <string.h>

#include "pixelica/collision2d.h"

static int pixelica_c2_finite(PIXELICA_Vec2 v)
{
    return isfinite(v.x) && isfinite(v.y);
}

static float pixelica_c2_axis(const PIXELICA_Vec2 *v, int axis)
{
    return axis == 0 ? v->x : v->y;
}

static void pixelica_c2_set_axis(PIXELICA_Vec2 *v, int axis, float value)
{
    if (axis == 0) {
        v->x = value;
    } else {
        v->y = value;
    }
}

int pixelica_aabb2d_is_valid(const PIXELICA_AABB2D *box)
{
    if (box == NULL || !pixelica_c2_finite(box->min) || !pixelica_c2_finite(box->max)) {
        return 0;
    }
    return box->min.x < box->max.x && box->min.y < box->max.y;
}

/* Open-interval overlap on one axis. */
static int pixelica_c2_axis_overlaps(const PIXELICA_AABB2D *a, const PIXELICA_AABB2D *b, int axis)
{
    return pixelica_c2_axis(&a->min, axis) < pixelica_c2_axis(&b->max, axis) &&
           pixelica_c2_axis(&b->min, axis) < pixelica_c2_axis(&a->max, axis);
}

bool pixelica_aabb2d_overlaps(const PIXELICA_AABB2D *a, const PIXELICA_AABB2D *b)
{
    if (!pixelica_aabb2d_is_valid(a) || !pixelica_aabb2d_is_valid(b)) {
        return false;
    }
    return pixelica_c2_axis_overlaps(a, b, 0) && pixelica_c2_axis_overlaps(a, b, 1);
}

/* True if byte ranges [a, a+na) and [b, b+nb) share a byte. */
static int pixelica_c2_ranges_overlap(const void *a, size_t na, const void *b, size_t nb)
{
    const uintptr_t pa = (uintptr_t)a;
    const uintptr_t pb = (uintptr_t)b;
    if (a == NULL || b == NULL || na == 0u || nb == 0u) {
        return 0;
    }
    return pa <= pb ? pb - pa < na : pa - pb < nb;
}

/* Obstacle array is addressable, every obstacle valid, none penetrating box. */
static int pixelica_c2_obstacles_ok(const PIXELICA_AABB2D *box, const PIXELICA_AABB2D *obstacles,
                                    size_t count)
{
    size_t i;
    if (count == 0u) {
        return 1;
    }
    if (obstacles == NULL || count > (size_t)PIXELICA_COLLISION2D_MAX_OBSTACLES ||
        count > SIZE_MAX / sizeof(PIXELICA_AABB2D) ||
        (uintptr_t)obstacles > UINTPTR_MAX - count * sizeof(PIXELICA_AABB2D)) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (!pixelica_aabb2d_is_valid(&obstacles[i]) || pixelica_aabb2d_overlaps(box, &obstacles[i])) {
            return 0;
        }
    }
    return 1;
}

/* `face` lies ahead of `edge` by at most a few ulps (exact touch up to rounding). */
static int pixelica_c2_touching(float edge, float face)
{
    const double gap = (double)face - (double)edge;
    double scale = fabs((double)edge);
    if (fabs((double)face) > scale) {
        scale = fabs((double)face);
    }
    if (scale < 1.0) {
        scale = 1.0;
    }
    return gap >= 0.0 && gap <= 4.0 * (double)FLT_EPSILON * scale;
}

/* Float sum rounded to float even with excess-precision evaluation. */
static float pixelica_c2_add(float a, float b)
{
    const float r = a + b;
    return r;
}

/*
 * Largest displacement d' toward `d` (|d'| <= |d|, same sign or 0) such that
 * `edge + d'` (float) does not pass `limit`. The caller guarantees the
 * obstacle is ahead: limit >= edge for d > 0, limit <= edge for d < 0.
 */
static float pixelica_c2_clamp(float edge, float d, float limit)
{
    float allowed;
    int i;

    if (d > 0.0f) {
        if ((double)edge + (double)d <= (double)limit && pixelica_c2_add(edge, d) <= limit) {
            return d;
        }
        allowed = (float)((double)limit - (double)edge);
        if (!(allowed >= 0.0f)) {
            allowed = 0.0f;
        }
        if (allowed > d) {
            allowed = d;
        }
        for (i = 0; i < 8 && pixelica_c2_add(edge, allowed) > limit; ++i) {
            allowed = nextafterf(allowed, 0.0f);
        }
        return pixelica_c2_add(edge, allowed) > limit ? 0.0f : allowed;
    }
    if ((double)edge + (double)d >= (double)limit && pixelica_c2_add(edge, d) >= limit) {
        return d;
    }
    allowed = (float)((double)limit - (double)edge);
    if (!(allowed <= 0.0f)) {
        allowed = 0.0f;
    }
    if (allowed < d) {
        allowed = d;
    }
    for (i = 0; i < 8 && pixelica_c2_add(edge, allowed) < limit; ++i) {
        allowed = nextafterf(allowed, 0.0f);
    }
    return pixelica_c2_add(edge, allowed) < limit ? 0.0f : allowed;
}

/* Sweeps `box` along `axis` by at most d; returns the applied displacement
 * and sets *hit when the move was blocked or ends touching a face ahead. */
static float pixelica_c2_sweep(PIXELICA_AABB2D *box, int axis, float d,
                               const PIXELICA_AABB2D *obstacles, size_t count, int *hit)
{
    const int other = 1 - axis;
    float allowed = d;
    size_t i;

    *hit = 0;
    if (d == 0.0f) {
        return 0.0f;
    }
    for (i = 0; i < count; ++i) {
        const PIXELICA_AABB2D *ob = &obstacles[i];
        if (!pixelica_c2_axis_overlaps(box, ob, other)) {
            continue; /* cannot be hit by motion along `axis` */
        }
        if (d > 0.0f) {
            const float edge = pixelica_c2_axis(&box->max, axis);
            const float limit = pixelica_c2_axis(&ob->min, axis);
            if (limit >= edge) {
                allowed = pixelica_c2_clamp(edge, allowed, limit);
            }
        } else {
            const float edge = pixelica_c2_axis(&box->min, axis);
            const float limit = pixelica_c2_axis(&ob->max, axis);
            if (limit <= edge) {
                allowed = pixelica_c2_clamp(edge, allowed, limit);
            }
        }
    }
    if (allowed != d) {
        *hit = 1;
    }
    pixelica_c2_set_axis(&box->min, axis, pixelica_c2_add(pixelica_c2_axis(&box->min, axis), allowed));
    pixelica_c2_set_axis(&box->max, axis, pixelica_c2_add(pixelica_c2_axis(&box->max, axis), allowed));
    for (i = 0; i < count && !*hit; ++i) {
        const PIXELICA_AABB2D *ob = &obstacles[i];
        if (!pixelica_c2_axis_overlaps(box, ob, other)) {
            continue;
        }
        if (d > 0.0f) {
            *hit = pixelica_c2_touching(pixelica_c2_axis(&box->max, axis), pixelica_c2_axis(&ob->min, axis));
        } else {
            *hit = pixelica_c2_touching(-pixelica_c2_axis(&box->min, axis), -pixelica_c2_axis(&ob->max, axis));
        }
    }
    return allowed;
}

/* Inputs already validated. Writes only *moved, *applied and *contacts. */
static void pixelica_c2_move(PIXELICA_AABB2D *moved, PIXELICA_Vec2 delta, const PIXELICA_AABB2D *obstacles,
                             size_t count, PIXELICA_Vec2 *applied, uint32_t *contacts)
{
    int hit;
    *contacts = 0u;
    applied->x = pixelica_c2_sweep(moved, 0, delta.x, obstacles, count, &hit);
    if (hit) {
        *contacts |= delta.x > 0.0f ? PIXELICA_CONTACT2D_RIGHT : PIXELICA_CONTACT2D_LEFT;
    }
    applied->y = pixelica_c2_sweep(moved, 1, delta.y, obstacles, count, &hit);
    if (hit) {
        *contacts |= delta.y > 0.0f ? PIXELICA_CONTACT2D_BOTTOM : PIXELICA_CONTACT2D_TOP;
    }
}

/* The box's bottom rests on some obstacle's top (X interiors overlap). */
static int pixelica_c2_supported(const PIXELICA_AABB2D *box, const PIXELICA_AABB2D *obstacles, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (pixelica_c2_axis_overlaps(box, &obstacles[i], 0) &&
            pixelica_c2_touching(box->max.y, obstacles[i].min.y)) {
            return 1;
        }
    }
    return 0;
}

PIXELICA_Status pixelica_aabb2d_move(PIXELICA_AABB2D *box, PIXELICA_Vec2 delta,
                                     const PIXELICA_AABB2D *obstacles, size_t count,
                                     PIXELICA_Vec2 *out_applied, uint32_t *out_contacts)
{
    PIXELICA_AABB2D moved;
    PIXELICA_Vec2 applied;
    uint32_t contacts;
    const size_t obstacle_bytes = count * sizeof(PIXELICA_AABB2D);

    if (!pixelica_aabb2d_is_valid(box) || !pixelica_c2_finite(delta) ||
        !pixelica_c2_obstacles_ok(box, obstacles, count)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* obstacles_ok guarantees obstacle_bytes did not overflow. */
    if (pixelica_c2_ranges_overlap(box, sizeof *box, obstacles, obstacle_bytes) ||
        pixelica_c2_ranges_overlap(out_applied, sizeof *out_applied, box, sizeof *box) ||
        pixelica_c2_ranges_overlap(out_applied, sizeof *out_applied, obstacles, obstacle_bytes) ||
        pixelica_c2_ranges_overlap(out_contacts, sizeof *out_contacts, box, sizeof *box) ||
        pixelica_c2_ranges_overlap(out_contacts, sizeof *out_contacts, obstacles, obstacle_bytes) ||
        pixelica_c2_ranges_overlap(out_contacts, sizeof *out_contacts, out_applied, sizeof *out_applied)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    moved = *box;
    pixelica_c2_move(&moved, delta, obstacles, count, &applied, &contacts);
    if (!pixelica_aabb2d_is_valid(&moved)) {
        return PIXELICA_ERR_INVALID_ARG; /* overflowed or lost extent to rounding */
    }
    *box = moved;
    if (out_applied != NULL) {
        *out_applied = applied;
    }
    if (out_contacts != NULL) {
        *out_contacts = contacts;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_body2d_step(PIXELICA_Body2D *body, float dt, float gravity,
                                     float jump_speed, float max_fall_speed, bool jump,
                                     const PIXELICA_AABB2D *obstacles, size_t count)
{
    PIXELICA_AABB2D moved;
    PIXELICA_Vec2 delta;
    PIXELICA_Vec2 applied;
    uint32_t contacts;
    double vx;
    double vy;
    int supported;

    /* Negated range tests also reject NaN. */
    if (body == NULL || !(dt > 0.0f && dt <= PIXELICA_BODY2D_MAX_DT) ||
        !(gravity >= 0.0f && gravity <= PIXELICA_BODY2D_MAX_ACCEL) ||
        !(jump_speed >= 0.0f && jump_speed <= PIXELICA_BODY2D_MAX_ACCEL) ||
        !(max_fall_speed > 0.0f && max_fall_speed <= PIXELICA_BODY2D_MAX_ACCEL) ||
        !pixelica_aabb2d_is_valid(&body->bounds) || !pixelica_c2_finite(body->velocity) ||
        !pixelica_c2_obstacles_ok(&body->bounds, obstacles, count) ||
        pixelica_c2_ranges_overlap(body, sizeof *body, obstacles, count * sizeof(PIXELICA_AABB2D))) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    vx = (double)body->velocity.x;
    vy = (double)body->velocity.y;
    supported = vy >= 0.0 && pixelica_c2_supported(&body->bounds, obstacles, count);
    vy += (double)gravity * (double)dt;
    if (vy > (double)max_fall_speed) {
        vy = (double)max_fall_speed;
    }
    if (jump && supported && jump_speed > 0.0f) {
        vy = -(double)jump_speed;
    }
    delta.x = (float)(vx * (double)dt);
    delta.y = (float)(vy * (double)dt);

    moved = body->bounds;
    pixelica_c2_move(&moved, delta, obstacles, count, &applied, &contacts);
    if (!pixelica_aabb2d_is_valid(&moved)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if ((contacts & (PIXELICA_CONTACT2D_LEFT | PIXELICA_CONTACT2D_RIGHT)) != 0u) {
        vx = 0.0;
    }
    if ((contacts & (PIXELICA_CONTACT2D_TOP | PIXELICA_CONTACT2D_BOTTOM)) != 0u) {
        vy = 0.0;
    }
    if (vy >= 0.0 && pixelica_c2_supported(&moved, obstacles, count)) {
        contacts |= PIXELICA_CONTACT2D_BOTTOM;
        vy = 0.0;
    }

    body->bounds = moved;
    body->velocity.x = (float)vx;
    body->velocity.y = (float)vy;
    body->contacts = contacts;
    body->grounded = (contacts & PIXELICA_CONTACT2D_BOTTOM) != 0u ? 1u : 0u;
    return PIXELICA_OK;
}

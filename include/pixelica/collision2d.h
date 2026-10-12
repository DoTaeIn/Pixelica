/*
 * Pixelica - 2D axis-aligned collision and simple platformer bodies (common
 * library).
 *
 * No heap, no global state, no renderer dependency. Same screen convention
 * as renderer2d.h: +X right, +Y DOWN, so gravity is a positive Y
 * acceleration and a jump is a negative Y velocity.
 *
 * Boxes: an AABB is valid when every coordinate is finite and min < max on
 * both axes (positive extent). Overlap uses open interiors: boxes that only
 * touch (share an edge or a corner) do NOT overlap.
 *
 * Movement is swept, X first then Y, against static obstacles only. A move
 * stops exactly at (never past) the first obstacle face ahead regardless of
 * speed, so fast bodies cannot tunnel through thin obstacles. Sliding along
 * a wall falls out of the per-axis sweep. There are no slopes, impulses,
 * moving platforms or dynamic-vs-dynamic resolution.
 *
 * Errors: every function validates ALL inputs before writing anything. On a
 * non-PIXELICA_OK result every output and in/out argument is left exactly as
 * it was. PIXELICA_ERR_INVALID_ARG is returned for a NULL pointer, a
 * non-finite or out-of-range value, an invalid box, a box that already
 * overlaps an obstacle (starting penetration is never silently resolved),
 * an obstacle count whose byte size overflows, an output that aliases an
 * input, or a result that would not be a valid finite box.
 */
#ifndef PIXELICA_COLLISION2D_H
#define PIXELICA_COLLISION2D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PIXELICA_AABB2D {
    PIXELICA_Vec2 min, max;
} PIXELICA_AABB2D;

/* Contact flags name the side of the MOVING box that touches an obstacle. */
#define PIXELICA_CONTACT2D_LEFT 1u
#define PIXELICA_CONTACT2D_RIGHT 2u
#define PIXELICA_CONTACT2D_TOP 4u
#define PIXELICA_CONTACT2D_BOTTOM 8u

/* Largest obstacle count accepted by one call. */
#define PIXELICA_COLLISION2D_MAX_OBSTACLES 1048576u

/* Limits for pixelica_body2d_step parameters. */
#define PIXELICA_BODY2D_MAX_DT 0.25f
#define PIXELICA_BODY2D_MAX_ACCEL 10000.0f /* gravity, jump_speed, max_fall_speed */

/*
 * A body is a box plus a velocity in units per second. contacts and grounded
 * are outputs of pixelica_body2d_step (their input values are ignored):
 * contacts holds PIXELICA_CONTACT2D_* flags, grounded is 0 or 1.
 */
typedef struct PIXELICA_Body2D {
    PIXELICA_AABB2D bounds;
    PIXELICA_Vec2 velocity;
    uint32_t contacts;
    uint32_t grounded;
} PIXELICA_Body2D;

/* 1 if box is non-NULL, finite and min < max on both axes, else 0. */
int pixelica_aabb2d_is_valid(const PIXELICA_AABB2D *box);

/* Open-interior overlap. Touching, NULL or invalid boxes give false. */
bool pixelica_aabb2d_overlaps(const PIXELICA_AABB2D *a, const PIXELICA_AABB2D *b);

/*
 * Move *box by `delta`, X then Y, stopping at static obstacles. The box must
 * not start overlapping any obstacle. out_applied (may be NULL) receives the
 * displacement actually applied. out_contacts (may be NULL) receives the
 * flags of the sides that were blocked or end exactly touching an obstacle in
 * the direction they moved (an axis with zero delta reports nothing).
 * obstacles may be NULL only when count is 0. Outputs must not overlap *box,
 * each other or the obstacle array.
 */
PIXELICA_Status pixelica_aabb2d_move(PIXELICA_AABB2D *box, PIXELICA_Vec2 delta,
                                     const PIXELICA_AABB2D *obstacles, size_t count,
                                     PIXELICA_Vec2 *out_applied, uint32_t *out_contacts);

/*
 * Advance a platformer body by dt seconds:
 *   1. supported = the body's bottom touches an obstacle top (exactly, up to
 *      float rounding) and it is not moving up.
 *   2. velocity.y += gravity * dt (semi-implicit Euler), capped at
 *      max_fall_speed downward. If jump and supported, velocity.y becomes
 *      -jump_speed instead (no air jumps).
 *   3. Move by velocity * dt with pixelica_aabb2d_move. Blocked velocity
 *      components are zeroed.
 *   4. contacts = blocked sides, plus BOTTOM if the body now rests on an
 *      obstacle; grounded = 1 when BOTTOM is set. Support is recomputed every
 *      step, so walking off an edge clears grounded (no hovering).
 * Ranges (finite): dt in (0, 0.25], gravity and jump_speed in [0, 10000],
 * max_fall_speed in (0, 10000]. velocity must be finite. Grounding works with
 * gravity 0 and on the first step of a body placed exactly on a floor.
 * On failure *body is unchanged.
 */
PIXELICA_Status pixelica_body2d_step(PIXELICA_Body2D *body, float dt, float gravity,
                                     float jump_speed, float max_fall_speed, bool jump,
                                     const PIXELICA_AABB2D *obstacles, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_COLLISION2D_H */

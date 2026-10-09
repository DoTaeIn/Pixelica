/*
 * Pixelica - allocation-free animation helpers: easing, scalar keyframe
 * tracks, a tick-based playback cursor and a pivoted part transform.
 *
 * Nothing here allocates, reads a clock, keeps hidden/global state or needs a
 * PIXELICA_Context. Time is an integer tick count chosen by the caller (typically
 * one tick per fixed PIXELICA_UPDATE_HZ step). Every array and state object is
 * caller-owned; distinct objects share no mutable state, and each mutable
 * object is single-threaded.
 *
 * Errors: every PIXELICA_Status function returns PIXELICA_ERR_INVALID_ARG for a NULL
 * required pointer, an unknown enum value, a non-finite float input or an
 * invalid player state, and then leaves every output (and the player)
 * byte-for-byte unchanged.
 */
#ifndef PIXELICA_ANIMATION_H
#define PIXELICA_ANIMATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Easing curves over t in [0, 1] (finite t is clamped to that range first):
 *   STEP         0 for t < 1, else 1
 *   LINEAR       t
 *   QUAD_IN      t * t
 *   QUAD_OUT     t * (2 - t)
 *   QUAD_IN_OUT  2*t*t for t < 0.5, else 1 - 2*(1-t)*(1-t)
 *   SMOOTHSTEP   t * t * (3 - 2*t)
 * Every curve maps 0 to exactly 0 and 1 to exactly 1; results lie in [0, 1].
 */
typedef enum PIXELICA_Ease {
    PIXELICA_EASE_STEP = 0,
    PIXELICA_EASE_LINEAR,
    PIXELICA_EASE_QUAD_IN,
    PIXELICA_EASE_QUAD_OUT,
    PIXELICA_EASE_QUAD_IN_OUT,
    PIXELICA_EASE_SMOOTHSTEP
} PIXELICA_Ease;

/* *out = ease(clamp(t, 0, 1)). */
PIXELICA_Status pixelica_anim_ease(PIXELICA_Ease ease, float t, float *out);

/*
 * *out = from + (to - from) * ease(clamp(t, 0, 1)), evaluated without
 * overflow even for opposite +-FLT_MAX endpoints, and always within
 * [min(from, to), max(from, to)]. An eased value of exactly 0 gives exactly
 * `from`, exactly 1 gives exactly `to`.
 */
PIXELICA_Status pixelica_anim_tween(float from, float to, float t, PIXELICA_Ease ease,
                          float *out);

/* One key of a scalar track. `ease` shapes the interval that STARTS at this
 * key (the last key's ease is unused but must still be valid). */
typedef struct PIXELICA_AnimKeyframe {
    uint32_t tick;
    float value;
    PIXELICA_Ease ease;
} PIXELICA_AnimKeyframe;

/*
 * Samples a track of `count` keys at `tick`.
 * Requires (checked on every call, O(count)): keys != NULL, count >= 1,
 * every value finite, every ease valid, ticks strictly increasing. The
 * first tick need not be 0; one key is a constant track.
 *   tick <= first key tick  -> first value
 *   tick >= last key tick   -> last value
 *   tick == a key's tick    -> that key's value (also for STEP)
 *   otherwise               -> tween(left.value, right.value, t, left.ease)
 *                              with t = (tick - left.tick) /
 *                              (right.tick - left.tick) in double precision.
 * Values are interpolated directly (no angle wrapping or shortest arc).
 * `out` may point at a key's `value`; all reads finish before the write.
 */
PIXELICA_Status pixelica_anim_sample(const PIXELICA_AnimKeyframe *keys, size_t count,
                           uint32_t tick, float *out);

/*
 * Playback modes for PIXELICA_AnimPlayer (D = duration_ticks):
 *   ONCE       phase runs 0..D, then stays at D with finished = true.
 *   LOOP       phase = elapsed mod D (0..D-1); tick D wraps to 0, so a
 *              track's key at tick D is never shown (use ONCE/PING_PONG or a
 *              seamless loop whose last key equals its first).
 *   PING_PONG  phase = elapsed mod 2D (0..2D-1); the sampled tick is
 *              phase for phase <= D, else 2D - phase, so 0 and D are both
 *              visited each cycle.
 * LOOP and PING_PONG never finish.
 */
typedef enum PIXELICA_AnimMode {
    PIXELICA_ANIM_ONCE = 0,
    PIXELICA_ANIM_LOOP,
    PIXELICA_ANIM_PING_PONG
} PIXELICA_AnimMode;

/*
 * Caller-owned playback cursor. duration_ticks, phase_ticks, mode and
 * finished are read-only for callers (change them only through init);
 * `paused` may be toggled directly. A player whose fields break the mode's
 * invariants is rejected by advance/tick.
 */
typedef struct PIXELICA_AnimPlayer {
    uint32_t duration_ticks;
    uint64_t phase_ticks;
    PIXELICA_AnimMode mode;
    bool paused;
    bool finished;
} PIXELICA_AnimPlayer;

/* Requires duration_ticks >= 1 and a known mode. Sets phase 0, paused and
 * finished false; calling it again restarts the player. */
PIXELICA_Status pixelica_anim_player_init(PIXELICA_AnimPlayer *player,
                                uint32_t duration_ticks, PIXELICA_AnimMode mode);

/* Moves the phase forward by `ticks` in O(1) (any uint32_t, 0 allowed). A
 * paused or finished player succeeds without moving. */
PIXELICA_Status pixelica_anim_player_advance(PIXELICA_AnimPlayer *player, uint32_t ticks);

/* Track tick for the current phase (folded per mode, always in
 * 0..duration_ticks); pass it to pixelica_anim_sample. */
PIXELICA_Status pixelica_anim_player_tick(const PIXELICA_AnimPlayer *player,
                                uint32_t *out_tick);

/*
 * Local transform of a rigid part. Rotation is Euler XYZ in radians, applied
 * X then Y then Z (not quaternions). `pivot` is in mesh-local coordinates
 * before scaling; rotation and scale happen around it. `translation` moves
 * the local origin, so the pivot ends up at translation + pivot.
 */
typedef struct PIXELICA_Transform {
    PIXELICA_Vec3 translation;
    PIXELICA_Vec3 rotation;
    PIXELICA_Vec3 scale;
    PIXELICA_Vec3 pivot;
} PIXELICA_Transform;

/* translation, rotation, pivot = 0; scale = 1. NULL is a no-op. */
void pixelica_transform_identity(PIXELICA_Transform *transform);

/*
 * Column-major, column-vector (see math.h) local matrix:
 *   out = T(translation) * T(pivot) * Rz * Ry * Rx * S(scale) * T(-pivot)
 * Scale may be zero or negative. Requires every component finite, and
 * fails (out unchanged) if any resulting element overflows the float range.
 * For a child part, compose with the parent's WORLD matrix:
 *   pixelica_mat4_multiply(child_world, parent_world, child_local);
 * Nothing updates scene nodes or their bounds automatically.
 */
PIXELICA_Status pixelica_transform_matrix(float out[16], const PIXELICA_Transform *transform);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_ANIMATION_H */

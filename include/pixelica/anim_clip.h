/*
 * Pixelica - skeletal animation CLIPS: per-bone transform tracks, playback,
 * crossfade and timed events.
 *
 * This header belongs to the common library (Pixelica::common). It is pure
 * data and arithmetic: it knows nothing about the 3D renderer, the 2D
 * renderer or the optional skeleton module. A clip produces one
 * PIXELICA_TRS per bone INDEX; what a bone index means (and how those
 * transforms become a matrix palette) is the caller's business - with the
 * skeleton module, turn each PIXELICA_TRS into a local matrix with
 * pixelica_trs_matrix() and feed the array to pixelica_skeleton_pose().
 *
 * Like animation.h: nothing here allocates, reads a clock or keeps hidden
 * global state. Time is an integer TICK count chosen by the caller (one tick
 * per fixed PIXELICA_UPDATE_HZ step is the intended use). Every clip, key
 * array, player and output array is caller-owned; a clip is only ever read.
 *
 * Errors: every function returns PIXELICA_ERR_INVALID_ARG for a NULL
 * required pointer, an unknown enum value, a non-finite float, a malformed
 * clip or an invalid player, and then leaves every output (and the player)
 * byte-for-byte unchanged. PIXELICA_ERR_NO_MEMORY is only returned for a
 * caller workspace that is too small.
 *
 * Validation cost: a clip is validated IN FULL (O(total keys)) by
 * pixelica_clip_validate, pixelica_clip_sample, pixelica_clip_player_init,
 * pixelica_clip_player_crossfade and pixelica_clip_player_sample, so bad
 * data can never be read as if it were valid.
 * pixelica_clip_player_advance only re-checks the event list (O(events)),
 * because it reads no keys.
 */
#ifndef PIXELICA_ANIM_CLIP_H
#define PIXELICA_ANIM_CLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/animation.h"
#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest accepted clip bone_count (every bone addressable by a uint16_t,
 * matching the optional skeleton module). */
#define PIXELICA_CLIP_MAX_BONES 65536u
/* Largest accepted clip event_count. */
#define PIXELICA_CLIP_MAX_EVENTS 256u
/*
 * Largest number of clip REPETITIONS whose events a single
 * pixelica_clip_player_advance reports. One advance may legally span
 * billions of repetitions (ticks is a uint32_t and a clip may be one tick
 * long); reporting them all would stall. The OLDEST repetitions beyond this
 * bound are dropped, the most recent ones are reported, and the number
 * dropped is added to the player's skipped_repetitions. The final playback
 * position is always exact, whether or not anything was dropped.
 */
#define PIXELICA_CLIP_MAX_EVENT_REPETITIONS 16u

/*
 * Translation / rotation / scale of one bone, relative to its parent. This
 * is the clip's unit of output and the counterpart of PIXELICA_Transform
 * (animation.h) for rigs: it carries a QUATERNION instead of Euler angles,
 * so interpolation is well defined and gimbal-free, and it has no pivot
 * (the bind pose already places the bone's origin).
 *
 * Rotation policy: see PIXELICA_Quat in math.h. Quaternions need not be
 * unit length on input, q and -q are the same rotation and every blend
 * takes the shortest arc.
 */
typedef struct PIXELICA_TRS {
    PIXELICA_Vec3 translation;
    PIXELICA_Quat rotation;
    PIXELICA_Vec3 scale;
} PIXELICA_TRS;

/* translation 0, rotation identity, scale 1. NULL is a no-op. */
void pixelica_trs_identity(PIXELICA_TRS *out);

/*
 * Column-major local matrix of a bone (math.h conventions):
 *   out = T(translation) * R(rotation) * S(scale)
 * Requires finite translation/scale and a finite non-zero rotation; fails
 * (out unchanged) if an element does not fit a float. Scale may be zero or
 * negative, but a zero scale gives a singular matrix, which lighting's
 * normal matrix rejects.
 */
PIXELICA_Status pixelica_trs_matrix(float out[16], const PIXELICA_TRS *trs);

/*
 * *out = a blended toward b by t (clamped to [0, 1]): translation and scale
 * are linearly interpolated per component, rotation by
 * pixelica_quat_slerp (shortest arc). t == 0 reproduces `a` and t == 1
 * reproduces `b` exactly, except that both rotations come out normalized.
 * `out` may alias `a` or `b`. Requires valid finite inputs, else
 * PIXELICA_ERR_INVALID_ARG with *out unchanged.
 */
PIXELICA_Status pixelica_trs_blend(PIXELICA_TRS *out, const PIXELICA_TRS *a,
                                   const PIXELICA_TRS *b, float t);

/*
 * One key of a vector / rotation track. As in PIXELICA_AnimKeyframe, `ease`
 * shapes the interval that STARTS at this key (the last key's ease is
 * unused but must still be valid), and a track's ticks must be STRICTLY
 * INCREASING. All three components of a vector key move together with the
 * same eased parameter.
 */
typedef struct PIXELICA_ClipVec3Key {
    uint32_t tick;
    PIXELICA_Vec3 value;
    PIXELICA_Ease ease;
} PIXELICA_ClipVec3Key;

typedef struct PIXELICA_ClipQuatKey {
    uint32_t tick;
    PIXELICA_Quat value; /* finite, non-zero; need not be unit length */
    PIXELICA_Ease ease;
} PIXELICA_ClipQuatKey;

/*
 * The three independent tracks of one bone. Each may be EMPTY (NULL pointer
 * with count 0), and an empty track samples to that component's identity
 * (translation 0, rotation identity, scale 1) - not to a bind pose, which
 * the clip does not know. Give a bone one constant key to hold a non-identity
 * value for the whole clip.
 *
 * Sampling a non-empty track at tick T follows pixelica_anim_sample:
 * T at or before the first key gives the first value, T at or after the last
 * key gives the last value, T exactly on a key gives that key's value (also
 * for STEP), and otherwise the two surrounding keys are interpolated with
 * the left key's ease at t = (T - left.tick) / (right.tick - left.tick),
 * computed in double.
 */
typedef struct PIXELICA_ClipBoneTrack {
    const PIXELICA_ClipVec3Key *translation;
    size_t translation_count;
    const PIXELICA_ClipQuatKey *rotation;
    size_t rotation_count;
    const PIXELICA_ClipVec3Key *scale;
    size_t scale_count;
} PIXELICA_ClipBoneTrack;

/*
 * A timed marker inside a clip. `id` is opaque to the engine (a footstep,
 * a hit frame, a sound cue). Events must be listed in NON-DECREASING tick
 * order and every tick must be <= duration_ticks; several events may share
 * a tick and are then reported in array order.
 */
typedef struct PIXELICA_ClipEvent {
    uint32_t tick;
    uint32_t id;
} PIXELICA_ClipEvent;

/*
 * An immutable clip. `tracks` holds bone_count entries (bone i uses
 * tracks[i]); key ticks outside [0, duration_ticks] are allowed and simply
 * never reached. duration_ticks >= 1 is the clip's length: a looping clip
 * plays ticks 0 .. duration_ticks - 1 and then wraps (tick duration_ticks
 * IS tick 0 of the next repetition and is never sampled), while a
 * non-looping clip ends exactly ON tick duration_ticks.
 */
typedef struct PIXELICA_AnimClip {
    const PIXELICA_ClipBoneTrack *tracks;
    uint32_t bone_count;     /* 1..PIXELICA_CLIP_MAX_BONES */
    uint32_t duration_ticks; /* >= 1 */
    const PIXELICA_ClipEvent *events; /* NULL if event_count == 0 */
    size_t event_count;               /* 0..PIXELICA_CLIP_MAX_EVENTS */
} PIXELICA_AnimClip;

/* Full structural and numeric check of a clip (see the header comment).
 * PIXELICA_OK or PIXELICA_ERR_INVALID_ARG; nothing is written. */
PIXELICA_Status pixelica_clip_validate(const PIXELICA_AnimClip *clip);

/*
 * Samples every bone of `clip` at `tick` into out[0 .. clip->bone_count).
 * out_count must be >= clip->bone_count (else PIXELICA_ERR_NO_MEMORY);
 * entries beyond bone_count are not touched. Ticks outside the clip are
 * clamped per track (see PIXELICA_ClipBoneTrack); `tick` itself is not
 * folded by any playback mode. Rotations come out unit length.
 *
 * Aliasing: the clip is READ while `out` is written, so out[0 ..
 * clip->bone_count) must not overlap the clip object, its track array, its
 * event array or any of its key arrays. Overlaps are detected and rejected
 * with PIXELICA_ERR_INVALID_ARG.
 *
 * A failure leaves `out` byte-for-byte unchanged: everything that can fail
 * (structure, finiteness, ordering, sizes, aliasing) is checked up front,
 * and sampling a clip that passed those checks cannot fail. Interpolation
 * is clamped to the two surrounding key values on every component (an
 * easing curve can never overshoot them), so even +-FLT_MAX keys stay
 * finite and in range.
 */
PIXELICA_Status pixelica_clip_sample(const PIXELICA_AnimClip *clip, uint32_t tick,
                                     PIXELICA_TRS *out, size_t out_count);

/* What pixelica_clip_player_advance reports to the event callback. */
typedef enum PIXELICA_ClipEventKind {
    PIXELICA_CLIP_EVENT_USER = 0, /* a PIXELICA_ClipEvent at its tick */
    PIXELICA_CLIP_EVENT_LOOP = 1, /* a looping clip wrapped to tick 0 */
    PIXELICA_CLIP_EVENT_END = 2   /* a non-looping clip reached its end */
} PIXELICA_ClipEventKind;

typedef struct PIXELICA_ClipEventInfo {
    const PIXELICA_AnimClip *clip; /* the clip that fired; borrowed */
    PIXELICA_ClipEventKind kind;
    uint32_t id;         /* PIXELICA_ClipEvent.id; 0 for LOOP and END */
    uint32_t tick;       /* the clip tick; duration_ticks for LOOP/END */
    uint64_t repetition; /* 0-based repetition this instant belongs to */
} PIXELICA_ClipEventInfo;

/*
 * Event callback. Lifetime and reentrancy policy, guaranteed:
 *   - It is called SYNCHRONOUSLY from pixelica_clip_player_advance, always
 *     before that call returns, and never from any other function.
 *   - `info` and everything it points at are borrowed for the duration of
 *     the call only; copy anything you keep.
 *   - The player has ALREADY been moved to its final position for the whole
 *     advance when any callback runs, so every callback of one advance sees
 *     the same, consistent player state - not the state at its own instant,
 *     which is in `info`. Sampling the player from the callback is allowed
 *     and gives that final pose.
 *   - The callback must NOT re-drive the player. ALL THREE mutators -
 *     pixelica_clip_player_init, _crossfade and _advance - return
 *     PIXELICA_ERR_INVALID_ARG and change nothing when called on the same
 *     player while one of its callbacks is running. Other players, other
 *     clips and read-only calls (_sample, _weight) are fine.
 *   - Callbacks of one advance run in increasing time order (see
 *     pixelica_clip_player_advance).
 */
typedef void (*PIXELICA_ClipEventFn)(void *userdata,
                                     const PIXELICA_ClipEventInfo *info);

/*
 * Caller-owned playback cursor over one clip, with an optional crossfade
 * from the clip it replaced. Every field is READ-ONLY for callers except
 * `paused`, which may be toggled directly; change the rest only through the
 * functions below. A player whose fields break these invariants is rejected
 * with PIXELICA_ERR_INVALID_ARG.
 *
 *   clip         the clip being played (never NULL in a live player)
 *   time_ticks   current position: 0..duration_ticks - 1 when looping,
 *                0..duration_ticks when not
 *   repetition   completed repetitions so far (0 on the first pass)
 *   skipped_repetitions  repetitions whose events were dropped because one
 *                advance spanned more than
 *                PIXELICA_CLIP_MAX_EVENT_REPETITIONS of them
 *   finished     a non-looping clip has reached duration_ticks
 *   started      the player has been moved at least once (it decides
 *                whether the STARTING pose still owes its events; see
 *                pixelica_clip_player_advance)
 *   fade_from    the outgoing clip of an active crossfade, else NULL
 */
typedef struct PIXELICA_ClipPlayer {
    const PIXELICA_AnimClip *clip;
    uint64_t time_ticks;
    uint64_t repetition;
    uint64_t skipped_repetitions;
    bool loop;
    bool paused;
    bool finished;
    bool started;
    /* crossfade */
    const PIXELICA_AnimClip *fade_from;
    uint64_t fade_from_time;
    bool fade_from_loop;
    uint32_t fade_ticks;   /* 0 when no fade is active */
    uint32_t fade_elapsed; /* <= fade_ticks */
    PIXELICA_Ease fade_ease;
    /* internal: set while an event callback is running */
    bool in_event;
} PIXELICA_ClipPlayer;

/*
 * Starts `clip` at tick 0, not paused, not finished, not started, with no
 * crossfade. Validates the clip in full. Calling it again restarts the
 * player and drops any crossfade.
 *
 * ZERO-INITIALIZATION CONTRACT. So that init can also refuse reentrant use
 * (see PIXELICA_ClipEventFn), the object it is given must already be a
 * readable PIXELICA_ClipPlayer: either zero-initialized -
 *     PIXELICA_ClipPlayer p = {0};        / memset(&p, 0, sizeof p);
 * - or a player a previous pixelica_clip_player_init returned
 * PIXELICA_OK for. Handing it indeterminate storage is a programming error
 * (it may then read an indeterminate in_event flag and refuse). Zeroed
 * storage is NOT a usable player by itself: init must still succeed before
 * any other call.
 */
PIXELICA_Status pixelica_clip_player_init(PIXELICA_ClipPlayer *player,
                                          const PIXELICA_AnimClip *clip,
                                          bool loop);

/*
 * Switches to `clip` (from tick 0, with its own `loop` flag) over
 * `fade_ticks` ticks, keeping the current clip and position as the outgoing
 * pose. fade_ticks == 0 switches immediately (identical to
 * pixelica_clip_player_init, including resetting the counters).
 *
 *   - The weight is w = ease(fade_elapsed / fade_ticks), so at the moment
 *     of the call w is 0 and the pose is exactly the outgoing one.
 *   - BOTH clips keep advancing during the fade (each with its own loop
 *     flag), so neither freezes, but only the INCOMING clip reports events.
 *   - The fade is driven by pixelica_clip_player_advance and runs on
 *     elapsed ticks alone. It therefore still completes after a
 *     non-looping incoming clip has FINISHED: that clip's pose is then
 *     held at its last tick while the weight keeps rising to 1.
 *   - INTERRUPTED-FADE POLICY (this is a real discontinuity, not a
 *     guarantee): starting a crossfade while one is still running drops
 *     the older outgoing clip and uses the clip that was fading IN, at its
 *     current position, as the new outgoing pose. Fades never chain, so
 *     cost and memory stay constant - but because the dropped clip still
 *     owed (1 - w) of the visible pose, the rendered pose JUMPS by that
 *     much at the moment of the interruption. Only a crossfade started
 *     when no fade is active is jump-free. To avoid the jump, keep fades
 *     short, or wait for fade_from to become NULL before starting the next
 *     one.
 *   - `clip` must have the same bone_count as the current clip.
 *   - Like init, this restarts the cursor: time_ticks, repetition,
 *     skipped_repetitions and `started` are reset, so the incoming clip's
 *     tick 0 events are reported on the next advance that moves it.
 * Requires a live player (init first) and a valid clip and ease.
 */
PIXELICA_Status pixelica_clip_player_crossfade(PIXELICA_ClipPlayer *player,
                                               const PIXELICA_AnimClip *clip,
                                               bool loop, uint32_t fade_ticks,
                                               PIXELICA_Ease ease);

/*
 * Moves the player forward by `ticks` (any uint32_t, 0 allowed) and reports
 * the events it REACHED. A paused player succeeds without doing anything.
 * A finished (non-looping) clip no longer moves and reports nothing, but an
 * active crossfade still advances, so a fade always runs to completion.
 * `on_event` may be NULL to move without reporting.
 *
 * WHICH EVENTS FIRE. The player occupies one integer tick position at a
 * time. An advance of N ticks VISITS the next N positions in order,
 * wrapping to 0 after duration_ticks - 1 when looping, or stopping on
 * duration_ticks when not. An event fires exactly when its tick is one of
 * the positions visited - the conventional swept, left-open window
 * (previous, current] per step - so a marker always fires on the step that
 * reaches its pose, never one step late and never twice:
 *   - advancing from tick 0 by 2 visits ticks 1 and 2 and therefore fires
 *     an event at tick 2;
 *   - several events on one tick fire in array order;
 *   - a LOOPING clip wrapping out of duration_ticks - 1 lands on tick 0 of
 *     the next repetition: PIXELICA_CLIP_EVENT_LOOP fires first, then that
 *     repetition's tick 0 events;
 *   - a NON-LOOPING clip lands on duration_ticks: its tick duration_ticks
 *     events fire, then PIXELICA_CLIP_EVENT_END, once. Neither fires again
 *     until the player is re-initialized.
 * THE STARTING POSE. A freshly initialized player already occupies tick 0
 * without having visited it. Its tick 0 events are therefore reported once,
 * on the FIRST advance that moves the player (together with that step's own
 * events, in tick order); `started` then stays true and tick 0 only fires
 * again on a loop wrap. An advance of 0 ticks, or one on a paused player,
 * does not consume this.
 * Consequences worth knowing:
 *   - PARTITION EQUIVALENCE: splitting an advance changes nothing. One step
 *     of 100 ticks over a 10-tick looping clip reports exactly the same
 *     events, in the same order, as 100 steps of 1 tick (including the 10
 *     LOOP markers), as long as no more than
 *     PIXELICA_CLIP_MAX_EVENT_REPETITIONS repetitions are crossed at once.
 *   - In a LOOPING clip the position duration_ticks does not exist (it is
 *     tick 0 of the next repetition), so a user event placed there never
 *     fires: put it on tick 0, or listen for PIXELICA_CLIP_EVENT_LOOP. A
 *     non-looping clip does fire events on duration_ticks.
 *   - An event on the tick the player currently sits on has already been
 *     reported (or was skipped by a jump) and does not fire again when the
 *     player leaves it.
 * A crossfade's outgoing clip advances too but reports nothing.
 * See PIXELICA_ClipEventFn for the callback's lifetime and reentrancy
 * rules; calling this from a callback of the same player is rejected.
 */
PIXELICA_Status pixelica_clip_player_advance(PIXELICA_ClipPlayer *player,
                                             uint32_t ticks,
                                             PIXELICA_ClipEventFn on_event,
                                             void *userdata);

/* Current crossfade weight in [0, 1]: 0 = the outgoing pose, 1 = the
 * incoming clip alone (also the value when no fade is active). */
PIXELICA_Status pixelica_clip_player_weight(const PIXELICA_ClipPlayer *player,
                                            float *out);

/*
 * Samples the player's current pose into out[0 .. bone_count), blending the
 * crossfade when one is active. out_count must be >= bone_count.
 *
 * `scratch` holds the outgoing pose while it is blended: it may be NULL
 * with scratch_count 0 when NO crossfade is active, but while one is it
 * must hold at least bone_count entries, else PIXELICA_ERR_NO_MEMORY. Its
 * contents are unspecified afterwards.
 *
 * Aliasing: `out` and the used part of `scratch` must not overlap each
 * other, nor either clip's object, track array, event array or key arrays
 * (PIXELICA_ERR_INVALID_ARG). Both clips are validated and all of that is
 * checked before anything is written, and sampling a validated clip cannot
 * fail, so a failure leaves `out` byte-for-byte unchanged.
 */
PIXELICA_Status pixelica_clip_player_sample(const PIXELICA_ClipPlayer *player,
                                            PIXELICA_TRS *out, size_t out_count,
                                            PIXELICA_TRS *scratch,
                                            size_t scratch_count);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_ANIM_CLIP_H */

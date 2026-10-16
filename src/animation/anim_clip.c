/*
 * Pixelica - skeletal animation clips. See include/pixelica/anim_clip.h.
 *
 * Shape of every entry point, as elsewhere in the common library: validate
 * everything into locals first, then compute, then write. A clip that has
 * passed pixelica_clip_validate_impl() can be SAMPLED without any further
 * failure mode (finite values, valid eases, strictly increasing ticks and
 * non-zero quaternions are exactly what sampling needs), which is what lets
 * pixelica_clip_sample write its output in place and still promise that a
 * failed call changes nothing.
 *
 * Playback keeps the clip-local phase (0..duration) plus a repetition
 * counter instead of one ever-growing time, so nothing can overflow however
 * long a clip runs, and event reporting walks repetition by repetition.
 */
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "pixelica/anim_clip.h"

#include "anim_internal.h"

/* ---- PIXELICA_TRS ------------------------------------------------------ */

static int pixelica_clip_vec3_finite(PIXELICA_Vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* Finite and non-zero: exactly what the quaternion helpers accept. */
static int pixelica_clip_quat_ok(PIXELICA_Quat q)
{
    if (!isfinite(q.x) || !isfinite(q.y) || !isfinite(q.z) || !isfinite(q.w)) {
        return 0;
    }
    return !(q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 0.0f);
}

static int pixelica_clip_trs_ok(const PIXELICA_TRS *t)
{
    return t != NULL && pixelica_clip_vec3_finite(t->translation) &&
           pixelica_clip_vec3_finite(t->scale) &&
           pixelica_clip_quat_ok(t->rotation);
}

void pixelica_trs_identity(PIXELICA_TRS *out)
{
    if (out == NULL) {
        return;
    }
    out->translation.x = 0.0f;
    out->translation.y = 0.0f;
    out->translation.z = 0.0f;
    pixelica_quat_identity(&out->rotation);
    out->scale.x = 1.0f;
    out->scale.y = 1.0f;
    out->scale.z = 1.0f;
}

PIXELICA_Status pixelica_trs_matrix(float out[16], const PIXELICA_TRS *trs)
{
    float rot[16];
    float m[16];
    float scale[3];
    int c;
    int row;

    if (out == NULL || !pixelica_clip_trs_ok(trs)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (pixelica_quat_to_mat4(rot, trs->rotation) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    scale[0] = trs->scale.x;
    scale[1] = trs->scale.y;
    scale[2] = trs->scale.z;
    /* R's elements are in [-1, 1], so R * S cannot leave the float range. */
    for (c = 0; c < 3; ++c) {
        for (row = 0; row < 3; ++row) {
            m[c * 4 + row] = rot[c * 4 + row] * scale[c];
        }
        m[c * 4 + 3] = 0.0f;
    }
    m[12] = trs->translation.x;
    m[13] = trs->translation.y;
    m[14] = trs->translation.z;
    m[15] = 1.0f;
    memcpy(out, m, sizeof m);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_trs_blend(PIXELICA_TRS *out, const PIXELICA_TRS *a,
                                   const PIXELICA_TRS *b, float t)
{
    PIXELICA_TRS r;
    double e;

    if (out == NULL || !pixelica_clip_trs_ok(a) || !pixelica_clip_trs_ok(b) ||
        !isfinite(t)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    e = (double)t;
    if (e < 0.0) {
        e = 0.0;
    } else if (e > 1.0) {
        e = 1.0;
    }
    r.translation.x = pixelica_anim_lerp(a->translation.x, b->translation.x, e);
    r.translation.y = pixelica_anim_lerp(a->translation.y, b->translation.y, e);
    r.translation.z = pixelica_anim_lerp(a->translation.z, b->translation.z, e);
    r.scale.x = pixelica_anim_lerp(a->scale.x, b->scale.x, e);
    r.scale.y = pixelica_anim_lerp(a->scale.y, b->scale.y, e);
    r.scale.z = pixelica_anim_lerp(a->scale.z, b->scale.z, e);
    if (pixelica_quat_slerp(&r.rotation, a->rotation, b->rotation,
                            (float)e) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = r; /* written last: out may alias a or b */
    return PIXELICA_OK;
}

/* ---- clip validation --------------------------------------------------- */

/* tick of key i, read without assuming anything beyond "tick is first". */
static uint32_t pixelica_clip_key_tick(const void *keys, size_t stride, size_t i)
{
    uint32_t t;

    memcpy(&t, (const unsigned char *)keys + i * stride, sizeof t);
    return t;
}

/* Strictly increasing ticks over `count` keys of `stride` bytes. */
static int pixelica_clip_ticks_increasing(const void *keys, size_t count,
                                          size_t stride)
{
    size_t i;

    for (i = 1; i < count; ++i) {
        if (pixelica_clip_key_tick(keys, stride, i) <=
            pixelica_clip_key_tick(keys, stride, i - 1)) {
            return 0;
        }
    }
    return 1;
}

static int pixelica_clip_vec3_track_ok(const PIXELICA_ClipVec3Key *keys,
                                       size_t count)
{
    size_t i;

    if (count == 0) {
        return keys == NULL;
    }
    if (keys == NULL || count > SIZE_MAX / sizeof keys[0]) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (!pixelica_clip_vec3_finite(keys[i].value) ||
            !pixelica_anim_ease_valid(keys[i].ease)) {
            return 0;
        }
    }
    return pixelica_clip_ticks_increasing(keys, count, sizeof keys[0]);
}

static int pixelica_clip_quat_track_ok(const PIXELICA_ClipQuatKey *keys,
                                       size_t count)
{
    size_t i;

    if (count == 0) {
        return keys == NULL;
    }
    if (keys == NULL || count > SIZE_MAX / sizeof keys[0]) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (!pixelica_clip_quat_ok(keys[i].value) ||
            !pixelica_anim_ease_valid(keys[i].ease)) {
            return 0;
        }
    }
    return pixelica_clip_ticks_increasing(keys, count, sizeof keys[0]);
}

/* Structure plus the event list: everything pixelica_clip_player_advance
 * needs, without walking the (potentially large) key arrays. */
static int pixelica_clip_shape_ok(const PIXELICA_AnimClip *clip)
{
    size_t i;

    if (clip == NULL || clip->tracks == NULL || clip->bone_count == 0 ||
        clip->bone_count > PIXELICA_CLIP_MAX_BONES ||
        clip->duration_ticks == 0 ||
        clip->event_count > PIXELICA_CLIP_MAX_EVENTS ||
        (clip->event_count > 0 && clip->events == NULL) ||
        (clip->event_count == 0 && clip->events != NULL)) {
        return 0;
    }
    for (i = 0; i < clip->event_count; ++i) {
        if (clip->events[i].tick > clip->duration_ticks) {
            return 0;
        }
        if (i > 0 && clip->events[i].tick < clip->events[i - 1].tick) {
            return 0; /* must be non-decreasing */
        }
    }
    return 1;
}

static int pixelica_clip_full_ok(const PIXELICA_AnimClip *clip)
{
    uint32_t b;

    if (!pixelica_clip_shape_ok(clip)) {
        return 0;
    }
    for (b = 0; b < clip->bone_count; ++b) {
        const PIXELICA_ClipBoneTrack *tr = &clip->tracks[b];

        if (!pixelica_clip_vec3_track_ok(tr->translation, tr->translation_count) ||
            !pixelica_clip_quat_track_ok(tr->rotation, tr->rotation_count) ||
            !pixelica_clip_vec3_track_ok(tr->scale, tr->scale_count)) {
            return 0;
        }
    }
    return 1;
}

PIXELICA_Status pixelica_clip_validate(const PIXELICA_AnimClip *clip)
{
    return pixelica_clip_full_ok(clip) ? PIXELICA_OK : PIXELICA_ERR_INVALID_ARG;
}

/* ---- output aliasing --------------------------------------------------- */

/*
 * 1 if the byte ranges [a, a + abytes) and [b, b + bbytes) share a byte. A
 * range whose end is not representable counts as overlapping, like the
 * renderer's own alias checks.
 */
static int pixelica_clip_overlap(const void *a, size_t abytes, const void *b,
                                 size_t bbytes)
{
    const uintptr_t a0 = (uintptr_t)a;
    const uintptr_t b0 = (uintptr_t)b;

    if (a == NULL || b == NULL || abytes == 0 || bbytes == 0) {
        return 0;
    }
    if (abytes > UINTPTR_MAX - a0 || bbytes > UINTPTR_MAX - b0) {
        return 1;
    }
    return a0 < b0 + (uintptr_t)bbytes && b0 < a0 + (uintptr_t)abytes;
}

/*
 * 1 if [out, out + bytes) overlaps anything a validated clip is READ from
 * while that output is written. O(bone_count), the same order as sampling.
 * Every size below was range-checked by pixelica_clip_full_ok.
 */
static int pixelica_clip_reads_overlap(const PIXELICA_AnimClip *clip,
                                       const void *out, size_t bytes)
{
    uint32_t b;

    if (pixelica_clip_overlap(out, bytes, clip, sizeof *clip) ||
        pixelica_clip_overlap(out, bytes, clip->tracks,
                              (size_t)clip->bone_count * sizeof clip->tracks[0]) ||
        pixelica_clip_overlap(out, bytes, clip->events,
                              clip->event_count * sizeof clip->events[0])) {
        return 1;
    }
    for (b = 0; b < clip->bone_count; ++b) {
        const PIXELICA_ClipBoneTrack *tr = &clip->tracks[b];

        if (pixelica_clip_overlap(out, bytes, tr->translation,
                                  tr->translation_count *
                                      sizeof tr->translation[0]) ||
            pixelica_clip_overlap(out, bytes, tr->rotation,
                                  tr->rotation_count * sizeof tr->rotation[0]) ||
            pixelica_clip_overlap(out, bytes, tr->scale,
                                  tr->scale_count * sizeof tr->scale[0])) {
            return 1;
        }
    }
    return 0;
}

/* ---- sampling ---------------------------------------------------------- */

/*
 * Locates `tick` in a validated track of `count` keys. *out_lo is the key
 * to use; when *out_interp is 0 that key's value is the answer, otherwise
 * the answer interpolates keys *out_lo and *out_lo + 1 with parameter
 * *out_t (in [0, 1)) and keys[*out_lo].ease.
 */
static void pixelica_clip_locate(const void *keys, size_t count, size_t stride,
                                 uint32_t tick, size_t *out_lo, int *out_interp,
                                 double *out_t)
{
    size_t lo;
    size_t hi;

    *out_interp = 0;
    *out_t = 0.0;
    if (tick <= pixelica_clip_key_tick(keys, stride, 0)) {
        *out_lo = 0;
        return;
    }
    if (tick >= pixelica_clip_key_tick(keys, stride, count - 1)) {
        *out_lo = count - 1;
        return;
    }
    /* count >= 2 and keys[lo].tick <= tick < keys[hi].tick. */
    lo = 0;
    hi = count - 1;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;

        if (pixelica_clip_key_tick(keys, stride, mid) <= tick) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    *out_lo = lo;
    if (pixelica_clip_key_tick(keys, stride, lo) == tick) {
        return;
    }
    /* Exact uint32_t spans in double: t stays strictly below 1. */
    *out_t = (double)(tick - pixelica_clip_key_tick(keys, stride, lo)) /
             (double)(pixelica_clip_key_tick(keys, stride, hi) -
                      pixelica_clip_key_tick(keys, stride, lo));
    *out_interp = 1;
}

/* Samples a validated vector track; an empty track gives `fallback`. */
static PIXELICA_Vec3 pixelica_clip_sample_vec3(const PIXELICA_ClipVec3Key *keys,
                                               size_t count, uint32_t tick,
                                               PIXELICA_Vec3 fallback)
{
    size_t lo;
    int interp;
    double t;
    double e;
    PIXELICA_Vec3 r;

    if (count == 0) {
        return fallback;
    }
    pixelica_clip_locate(keys, count, sizeof keys[0], tick, &lo, &interp, &t);
    if (!interp) {
        return keys[lo].value;
    }
    e = pixelica_anim_ease_eval(keys[lo].ease, t);
    r.x = pixelica_anim_lerp(keys[lo].value.x, keys[lo + 1].value.x, e);
    r.y = pixelica_anim_lerp(keys[lo].value.y, keys[lo + 1].value.y, e);
    r.z = pixelica_anim_lerp(keys[lo].value.z, keys[lo + 1].value.z, e);
    return r;
}

/* Samples a validated rotation track; an empty track gives identity. The
 * result is always unit length. Cannot fail for validated keys. */
static PIXELICA_Quat pixelica_clip_sample_quat(const PIXELICA_ClipQuatKey *keys,
                                               size_t count, uint32_t tick)
{
    PIXELICA_Quat r;
    size_t lo;
    int interp;
    double t;

    pixelica_quat_identity(&r);
    if (count == 0) {
        return r;
    }
    pixelica_clip_locate(keys, count, sizeof keys[0], tick, &lo, &interp, &t);
    if (!interp) {
        (void)pixelica_quat_normalize(&r, keys[lo].value);
        return r;
    }
    (void)pixelica_quat_slerp(
        &r, keys[lo].value, keys[lo + 1].value,
        (float)pixelica_anim_ease_eval(keys[lo].ease, t));
    return r;
}

/* One bone of a validated clip. */
static void pixelica_clip_sample_bone(const PIXELICA_ClipBoneTrack *tr,
                                      uint32_t tick, PIXELICA_TRS *out)
{
    PIXELICA_TRS identity;

    pixelica_trs_identity(&identity);
    out->translation = pixelica_clip_sample_vec3(
        tr->translation, tr->translation_count, tick, identity.translation);
    out->rotation = pixelica_clip_sample_quat(tr->rotation, tr->rotation_count,
                                              tick);
    out->scale = pixelica_clip_sample_vec3(tr->scale, tr->scale_count, tick,
                                           identity.scale);
}

/* Fills out[0..clip->bone_count) from a validated clip. */
static void pixelica_clip_sample_all(const PIXELICA_AnimClip *clip, uint32_t tick,
                                     PIXELICA_TRS *out)
{
    uint32_t b;

    for (b = 0; b < clip->bone_count; ++b) {
        pixelica_clip_sample_bone(&clip->tracks[b], tick, &out[b]);
    }
}

PIXELICA_Status pixelica_clip_sample(const PIXELICA_AnimClip *clip, uint32_t tick,
                                     PIXELICA_TRS *out, size_t out_count)
{
    if (out == NULL || !pixelica_clip_full_ok(clip)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (out_count < (size_t)clip->bone_count) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    if (pixelica_clip_reads_overlap(clip, out,
                                    (size_t)clip->bone_count *
                                        sizeof(PIXELICA_TRS))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    pixelica_clip_sample_all(clip, tick, out);
    return PIXELICA_OK;
}

/* ---- player ------------------------------------------------------------ */

/* Every invariant of a live player (see PIXELICA_ClipPlayer). The event
 * callback guard is NOT part of it: read-only calls stay legal inside a
 * callback. */
static int pixelica_clip_player_ok(const PIXELICA_ClipPlayer *p)
{
    uint64_t duration;

    if (p == NULL || !pixelica_clip_shape_ok(p->clip) ||
        !pixelica_anim_ease_valid(p->fade_ease)) {
        return 0;
    }
    duration = (uint64_t)p->clip->duration_ticks;
    if (p->loop) {
        if (p->time_ticks >= duration || p->finished) {
            return 0;
        }
    } else {
        if (p->time_ticks > duration ||
            p->finished != (p->time_ticks == duration)) {
            return 0;
        }
    }
    if (p->fade_from == NULL) {
        return p->fade_ticks == 0 && p->fade_elapsed == 0;
    }
    if (!pixelica_clip_shape_ok(p->fade_from) ||
        p->fade_from->bone_count != p->clip->bone_count ||
        p->fade_ticks == 0 || p->fade_elapsed >= p->fade_ticks) {
        return 0;
    }
    duration = (uint64_t)p->fade_from->duration_ticks;
    return p->fade_from_loop ? p->fade_from_time < duration
                             : p->fade_from_time <= duration;
}

static void pixelica_clip_player_reset(PIXELICA_ClipPlayer *player,
                                       const PIXELICA_AnimClip *clip, bool loop)
{
    player->clip = clip;
    player->time_ticks = 0;
    player->repetition = 0;
    player->skipped_repetitions = 0;
    player->loop = loop;
    player->paused = false;
    player->finished = false;
    player->started = false;
    player->fade_from = NULL;
    player->fade_from_time = 0;
    player->fade_from_loop = false;
    player->fade_ticks = 0;
    player->fade_elapsed = 0;
    player->fade_ease = PIXELICA_EASE_LINEAR;
    player->in_event = false;
}

PIXELICA_Status pixelica_clip_player_init(PIXELICA_ClipPlayer *player,
                                          const PIXELICA_AnimClip *clip,
                                          bool loop)
{
    /* in_event is readable because of the zero-initialization contract in
     * anim_clip.h, so even the constructor refuses reentrant use. */
    if (player == NULL || player->in_event || !pixelica_clip_full_ok(clip)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    pixelica_clip_player_reset(player, clip, loop);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_clip_player_crossfade(PIXELICA_ClipPlayer *player,
                                               const PIXELICA_AnimClip *clip,
                                               bool loop, uint32_t fade_ticks,
                                               PIXELICA_Ease ease)
{
    const PIXELICA_AnimClip *from;
    uint64_t from_time;
    bool from_loop;

    if (!pixelica_clip_player_ok(player) || player->in_event ||
        !pixelica_anim_ease_valid(ease) || !pixelica_clip_full_ok(clip) ||
        clip->bone_count != player->clip->bone_count) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* The clip that was fading IN becomes the outgoing one; fades never
     * chain, so an older outgoing clip is dropped here. */
    from = player->clip;
    from_time = player->time_ticks;
    from_loop = player->loop;
    pixelica_clip_player_reset(player, clip, loop);
    if (fade_ticks == 0) {
        return PIXELICA_OK; /* immediate switch */
    }
    player->fade_from = from;
    player->fade_from_time = from_time;
    player->fade_from_loop = from_loop;
    player->fade_ticks = fade_ticks;
    player->fade_elapsed = 0;
    player->fade_ease = ease;
    return PIXELICA_OK;
}

/*
 * Reports the user events of one repetition whose tick is one of the
 * VISITED positions lo..hi (both inclusive; an empty range has hi < lo).
 * Event ticks are non-decreasing, so the scan can stop at the first tick
 * past the window.
 */
static void pixelica_clip_emit_user(const PIXELICA_AnimClip *clip, uint64_t rep,
                                    uint64_t lo, uint64_t hi,
                                    PIXELICA_ClipEventFn fn, void *userdata)
{
    size_t i;

    for (i = 0; i < clip->event_count; ++i) {
        const uint64_t t = (uint64_t)clip->events[i].tick;
        PIXELICA_ClipEventInfo info;

        if (t < lo) {
            continue;
        }
        if (t > hi) {
            break;
        }
        info.clip = clip;
        info.kind = PIXELICA_CLIP_EVENT_USER;
        info.id = clip->events[i].id;
        info.tick = clip->events[i].tick;
        info.repetition = rep;
        fn(userdata, &info);
    }
}

static void pixelica_clip_emit_mark(const PIXELICA_AnimClip *clip, uint64_t rep,
                                    PIXELICA_ClipEventKind kind,
                                    PIXELICA_ClipEventFn fn, void *userdata)
{
    PIXELICA_ClipEventInfo info;

    info.clip = clip;
    info.kind = kind;
    info.id = 0;
    info.tick = clip->duration_ticks;
    info.repetition = rep;
    fn(userdata, &info);
}

/*
 * Moves an active crossfade forward by `ticks`. The fade runs on elapsed
 * ticks only, so it completes even when the incoming clip has finished.
 * The outgoing clip keeps playing (with its own loop flag) but never
 * reports events.
 */
static void pixelica_clip_advance_fade(PIXELICA_ClipPlayer *player,
                                       uint32_t ticks)
{
    uint64_t fd;
    uint64_t ft;

    if (player->fade_from == NULL) {
        return;
    }
    fd = (uint64_t)player->fade_from->duration_ticks;
    ft = player->fade_from_time + (uint64_t)ticks;
    player->fade_from_time =
        player->fade_from_loop ? ft % fd : (ft >= fd ? fd : ft);
    if ((uint64_t)player->fade_elapsed + (uint64_t)ticks >=
        (uint64_t)player->fade_ticks) {
        player->fade_from = NULL;
        player->fade_from_time = 0;
        player->fade_from_loop = false;
        player->fade_ticks = 0;
        player->fade_elapsed = 0;
    } else {
        player->fade_elapsed += ticks;
    }
}

PIXELICA_Status pixelica_clip_player_advance(PIXELICA_ClipPlayer *player,
                                             uint32_t ticks,
                                             PIXELICA_ClipEventFn on_event,
                                             void *userdata)
{
    const PIXELICA_AnimClip *clip;
    uint64_t duration;
    uint64_t total;
    uint64_t wraps;
    uint64_t phase;
    uint64_t from_tick;
    uint64_t rep0;
    uint64_t segments;
    uint64_t first;
    uint64_t s;
    int finishing = 0;
    int include_start;

    if (!pixelica_clip_player_ok(player) || player->in_event) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (player->paused || ticks == 0) {
        return PIXELICA_OK;
    }
    /* Before the finished check: a fade must always run to completion. */
    pixelica_clip_advance_fade(player, ticks);
    if (player->finished) {
        return PIXELICA_OK; /* the incoming pose is held at its last tick */
    }

    clip = player->clip;
    duration = (uint64_t)clip->duration_ticks;
    from_tick = player->time_ticks;
    rep0 = player->repetition;
    include_start = !player->started;
    /* time_ticks <= UINT32_MAX and ticks <= UINT32_MAX: no overflow. */
    total = from_tick + (uint64_t)ticks;
    if (player->loop) {
        wraps = total / duration;
        phase = total - wraps * duration;
    } else {
        wraps = 0;
        if (total >= duration) {
            phase = duration;
            finishing = 1;
        } else {
            phase = total;
        }
    }

    /* Final position first: every callback sees one consistent state. */
    player->time_ticks = phase;
    player->repetition = rep0 + wraps;
    player->started = true;
    if (finishing) {
        player->finished = true;
    }
    if (on_event == NULL) {
        return PIXELICA_OK;
    }

    segments = wraps + 1u;
    first = 0;
    if (segments > (uint64_t)PIXELICA_CLIP_MAX_EVENT_REPETITIONS) {
        first = segments - (uint64_t)PIXELICA_CLIP_MAX_EVENT_REPETITIONS;
        player->skipped_repetitions += first;
    }

    player->in_event = true;
    for (s = first; s < segments; ++s) {
        /* Positions VISITED in this repetition, both ends inclusive. The
         * tick the player stood on is excluded (it was reported when it
         * was reached), except for the very first move of a fresh player,
         * which still owes its starting pose. */
        uint64_t lo;
        uint64_t hi;

        if (s == 0) {
            lo = include_start ? from_tick : from_tick + 1u;
        } else {
            /* Wrapping out of the previous repetition LANDS on tick 0 of
             * this one: the marker fires first, then tick 0's events. */
            pixelica_clip_emit_mark(clip, rep0 + s - 1u,
                                    PIXELICA_CLIP_EVENT_LOOP, on_event,
                                    userdata);
            lo = 0;
        }
        /* A looping clip never occupies `duration` (that is tick 0 of the
         * next repetition); a non-looping one stops exactly on it. */
        hi = s == wraps ? phase : duration - 1u;
        pixelica_clip_emit_user(clip, rep0 + s, lo, hi, on_event, userdata);
    }
    if (finishing) {
        pixelica_clip_emit_mark(clip, rep0 + wraps, PIXELICA_CLIP_EVENT_END,
                                on_event, userdata);
    }
    player->in_event = false;
    return PIXELICA_OK;
}

/* Fade weight of a validated player, in [0, 1]. */
static double pixelica_clip_fade_weight(const PIXELICA_ClipPlayer *player)
{
    if (player->fade_from == NULL) {
        return 1.0;
    }
    return pixelica_anim_ease_eval(player->fade_ease,
                                   (double)player->fade_elapsed /
                                       (double)player->fade_ticks);
}

PIXELICA_Status pixelica_clip_player_weight(const PIXELICA_ClipPlayer *player,
                                            float *out)
{
    if (out == NULL || !pixelica_clip_player_ok(player)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = (float)pixelica_clip_fade_weight(player);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_clip_player_sample(const PIXELICA_ClipPlayer *player,
                                            PIXELICA_TRS *out, size_t out_count,
                                            PIXELICA_TRS *scratch,
                                            size_t scratch_count)
{
    const PIXELICA_AnimClip *clip;
    size_t n;
    size_t bytes;
    size_t i;
    double w;

    if (out == NULL || !pixelica_clip_player_ok(player)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    clip = player->clip;
    n = (size_t)clip->bone_count;
    bytes = n * sizeof(PIXELICA_TRS); /* n <= 65536: no overflow */
    if (!pixelica_clip_full_ok(clip) ||
        (player->fade_from != NULL && !pixelica_clip_full_ok(player->fade_from))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (out_count < n) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    if (pixelica_clip_reads_overlap(clip, out, bytes)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (player->fade_from != NULL) {
        if (scratch == NULL || scratch_count < n) {
            return PIXELICA_ERR_NO_MEMORY;
        }
        if (pixelica_clip_overlap(out, bytes, scratch, bytes) ||
            pixelica_clip_reads_overlap(player->fade_from, out, bytes) ||
            pixelica_clip_reads_overlap(clip, scratch, bytes) ||
            pixelica_clip_reads_overlap(player->fade_from, scratch, bytes)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    pixelica_clip_sample_all(clip, (uint32_t)player->time_ticks, out);
    if (player->fade_from == NULL) {
        return PIXELICA_OK;
    }
    pixelica_clip_sample_all(player->fade_from,
                             (uint32_t)player->fade_from_time, scratch);
    w = pixelica_clip_fade_weight(player);
    for (i = 0; i < n; ++i) {
        /* Both poses come from validated clips, so this cannot fail. */
        (void)pixelica_trs_blend(&out[i], &scratch[i], &out[i], (float)w);
    }
    return PIXELICA_OK;
}

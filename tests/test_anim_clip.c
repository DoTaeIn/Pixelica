/*
 * Skeletal animation clips (include/pixelica/anim_clip.h): TRS helpers,
 * track sampling, playback, crossfade and the event contract.
 *
 * Common-library test: it never includes a renderer header. CHECK never
 * uses assert(), so the test stays active in Release (NDEBUG).
 *
 * "unchanged" means byte-for-byte: outputs are filled with a poison value
 * first and compared with memcmp over the WHOLE array after the call.
 */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test_util.h"

#include "pixelica/anim_clip.h"

#define BAD PIXELICA_ERR_INVALID_ARG
#define NOMEM PIXELICA_ERR_NO_MEMORY

/* ---- small builders ---------------------------------------------------- */

static PIXELICA_Vec3 v3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static PIXELICA_Quat qz(float radians)
{
    PIXELICA_Quat q;

    CHECK(pixelica_quat_from_axis_angle(&q, v3(0.0f, 0.0f, 1.0f), radians) ==
          PIXELICA_OK);
    return q;
}

static PIXELICA_ClipVec3Key vkey(uint32_t tick, PIXELICA_Vec3 value,
                                 PIXELICA_Ease ease)
{
    PIXELICA_ClipVec3Key k;

    k.tick = tick;
    k.value = value;
    k.ease = ease;
    return k;
}

static PIXELICA_ClipQuatKey qkey(uint32_t tick, PIXELICA_Quat value,
                                 PIXELICA_Ease ease)
{
    PIXELICA_ClipQuatKey k;

    k.tick = tick;
    k.value = value;
    k.ease = ease;
    return k;
}

/* Fills a pose array with a recognizable pattern. */
static void poison(PIXELICA_TRS *p, size_t n)
{
    memset(p, 0xA5, n * sizeof *p);
}

static double quat_dot(PIXELICA_Quat a, PIXELICA_Quat b)
{
    return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z +
           (double)a.w * b.w;
}

/* One-bone clip: translation x goes 0 -> 10 over ticks 0..10 (LINEAR),
 * rotation 0 -> pi/2 over the same span, no scale track. */
typedef struct Simple {
    PIXELICA_ClipVec3Key t[2];
    PIXELICA_ClipQuatKey r[2];
    PIXELICA_ClipBoneTrack track;
    PIXELICA_AnimClip clip;
} Simple;

static void simple_init(Simple *s, PIXELICA_Ease ease)
{
    memset(s, 0, sizeof *s);
    s->t[0] = vkey(0, v3(0.0f, 0.0f, 0.0f), ease);
    s->t[1] = vkey(10, v3(10.0f, -4.0f, 2.0f), ease);
    s->r[0] = qkey(0, qz(0.0f), ease);
    s->r[1] = qkey(10, qz(1.5707963f), ease);
    s->track.translation = s->t;
    s->track.translation_count = 2;
    s->track.rotation = s->r;
    s->track.rotation_count = 2;
    s->clip.tracks = &s->track;
    s->clip.bone_count = 1;
    s->clip.duration_ticks = 10;
}

/* ---- TRS helpers ------------------------------------------------------- */

static void test_trs_identity_and_matrix(void)
{
    PIXELICA_TRS trs;
    float m[16];
    float r[16];
    float keep[16];
    int i;

    pixelica_trs_identity(NULL); /* no crash */
    pixelica_trs_identity(&trs);
    CHECK(trs.translation.x == 0.0f && trs.translation.y == 0.0f &&
          trs.translation.z == 0.0f);
    CHECK(trs.scale.x == 1.0f && trs.scale.y == 1.0f && trs.scale.z == 1.0f);
    CHECK(trs.rotation.w == 1.0f && trs.rotation.x == 0.0f);

    CHECK(pixelica_trs_matrix(m, &trs) == PIXELICA_OK);
    pixelica_mat4_identity(r);
    for (i = 0; i < 16; ++i) {
        CHECK_NEAR(m[i], r[i], 0.0);
    }

    /* T * R * S, in that order. */
    trs.translation = v3(1.0f, 2.0f, 3.0f);
    trs.rotation = qz(1.5707963f);
    trs.scale = v3(2.0f, 4.0f, 1.0f);
    CHECK(pixelica_trs_matrix(m, &trs) == PIXELICA_OK);
    {
        float t[16], rot[16], sc[16], expect[16];

        pixelica_mat4_translation(t, 1.0f, 2.0f, 3.0f);
        pixelica_mat4_rotation_z(rot, 1.5707963f);
        pixelica_mat4_scale(sc, 2.0f, 4.0f, 1.0f);
        pixelica_mat4_multiply(expect, t, rot);
        pixelica_mat4_multiply(expect, expect, sc);
        for (i = 0; i < 16; ++i) {
            CHECK_NEAR(m[i], expect[i], 1e-5);
        }
    }

    /* Invalid input leaves `out` untouched. */
    for (i = 0; i < 16; ++i) {
        m[i] = 11.0f;
        keep[i] = 11.0f;
    }
    trs.scale.y = pixelica_t_nan();
    CHECK(pixelica_trs_matrix(m, &trs) == BAD);
    pixelica_trs_identity(&trs);
    trs.rotation.w = 0.0f; /* all-zero quaternion */
    CHECK(pixelica_trs_matrix(m, &trs) == BAD);
    CHECK(pixelica_trs_matrix(NULL, &trs) == BAD);
    CHECK(pixelica_trs_matrix(m, NULL) == BAD);
    CHECK(memcmp(m, keep, sizeof m) == 0);
}

static void test_trs_blend(void)
{
    PIXELICA_TRS a, b, got, keep;

    pixelica_trs_identity(&a);
    pixelica_trs_identity(&b);
    a.translation = v3(0.0f, 0.0f, 0.0f);
    b.translation = v3(4.0f, 8.0f, -2.0f);
    a.scale = v3(1.0f, 1.0f, 1.0f);
    b.scale = v3(3.0f, 1.0f, 1.0f);
    a.rotation = qz(0.0f);
    b.rotation = qz(1.0f);

    CHECK(pixelica_trs_blend(&got, &a, &b, 0.5f) == PIXELICA_OK);
    CHECK_NEAR(got.translation.x, 2.0, 1e-6);
    CHECK_NEAR(got.translation.y, 4.0, 1e-6);
    CHECK_NEAR(got.translation.z, -1.0, 1e-6);
    CHECK_NEAR(got.scale.x, 2.0, 1e-6);
    CHECK_NEAR(fabs(quat_dot(got.rotation, qz(0.5f))), 1.0, 1e-6);

    /* Endpoints are exact and t is clamped. */
    CHECK(pixelica_trs_blend(&got, &a, &b, 0.0f) == PIXELICA_OK);
    CHECK(got.translation.x == a.translation.x);
    CHECK(pixelica_trs_blend(&got, &a, &b, -3.0f) == PIXELICA_OK);
    CHECK(got.translation.x == a.translation.x);
    CHECK(pixelica_trs_blend(&got, &a, &b, 1.0f) == PIXELICA_OK);
    CHECK(got.translation.x == b.translation.x);
    CHECK(pixelica_trs_blend(&got, &a, &b, 9.0f) == PIXELICA_OK);
    CHECK(got.translation.x == b.translation.x);

    /* In-place on either side. */
    got = a;
    CHECK(pixelica_trs_blend(&got, &got, &b, 1.0f) == PIXELICA_OK);
    CHECK(got.translation.x == b.translation.x);
    got = b;
    CHECK(pixelica_trs_blend(&got, &a, &got, 0.0f) == PIXELICA_OK);
    CHECK(got.translation.x == a.translation.x);

    /* Opposite extremes cannot overflow. */
    a.translation = v3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    b.translation = v3(FLT_MAX, FLT_MAX, FLT_MAX);
    CHECK(pixelica_trs_blend(&got, &a, &b, 0.5f) == PIXELICA_OK);
    CHECK(isfinite(got.translation.x) && fabs(got.translation.x) <= FLT_MAX);

    /* Invalid input changes nothing. */
    pixelica_trs_identity(&keep);
    got = keep;
    a.translation.x = pixelica_t_inf();
    CHECK(pixelica_trs_blend(&got, &a, &b, 0.5f) == BAD);
    CHECK(pixelica_trs_blend(&got, &keep, &b, pixelica_t_nan()) == BAD);
    CHECK(pixelica_trs_blend(NULL, &keep, &b, 0.5f) == BAD);
    CHECK(pixelica_trs_blend(&got, NULL, &b, 0.5f) == BAD);
    CHECK(memcmp(&got, &keep, sizeof got) == 0);
}

/* ---- clip validation --------------------------------------------------- */

static void test_clip_validate(void)
{
    Simple s;
    PIXELICA_ClipEvent ev[2];

    simple_init(&s, PIXELICA_EASE_LINEAR);
    CHECK(pixelica_clip_validate(&s.clip) == PIXELICA_OK);
    CHECK(pixelica_clip_validate(NULL) == BAD);

    /* An empty clip with no tracks at all is still valid (identity pose). */
    {
        PIXELICA_ClipBoneTrack empty;
        PIXELICA_AnimClip c;

        memset(&empty, 0, sizeof empty);
        memset(&c, 0, sizeof c);
        c.tracks = &empty;
        c.bone_count = 1;
        c.duration_ticks = 1;
        CHECK(pixelica_clip_validate(&c) == PIXELICA_OK);
    }

    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.clip.duration_ticks = 0;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);

    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.clip.bone_count = 0;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);

    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.clip.bone_count = PIXELICA_CLIP_MAX_BONES + 1u;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);

    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.clip.tracks = NULL;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);

    /* Track pointer / count must agree. */
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.track.translation = NULL;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.track.scale_count = 1; /* count without keys */
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.track.scale = s.t;
    s.track.scale_count = 0; /* keys without count */
    CHECK(pixelica_clip_validate(&s.clip) == BAD);

    /* Ticks must be strictly increasing. */
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.t[1].tick = 0;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.r[1].tick = s.r[0].tick;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);

    /* Values must be finite, quaternions non-zero, eases known. */
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.t[1].value.y = pixelica_t_nan();
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.r[0].value.x = pixelica_t_inf();
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.r[0].value.x = 0.0f;
    s.r[0].value.y = 0.0f;
    s.r[0].value.z = 0.0f;
    s.r[0].value.w = 0.0f;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    simple_init(&s, PIXELICA_EASE_LINEAR);
    s.t[0].ease = (PIXELICA_Ease)99;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);

    /* Events: pointer/count agreement, range and ordering. */
    simple_init(&s, PIXELICA_EASE_LINEAR);
    ev[0].tick = 2;
    ev[0].id = 1;
    ev[1].tick = 5;
    ev[1].id = 2;
    s.clip.events = ev;
    s.clip.event_count = 2;
    CHECK(pixelica_clip_validate(&s.clip) == PIXELICA_OK);
    s.clip.event_count = 0;
    CHECK(pixelica_clip_validate(&s.clip) == BAD); /* keys without count */
    s.clip.events = NULL;
    s.clip.event_count = 1;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    s.clip.events = ev;
    s.clip.event_count = PIXELICA_CLIP_MAX_EVENTS + 1u;
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    s.clip.event_count = 2;
    ev[1].tick = 1; /* decreasing */
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    ev[0].tick = 0;
    ev[1].tick = s.clip.duration_ticks + 1u; /* past the end */
    CHECK(pixelica_clip_validate(&s.clip) == BAD);
    ev[1].tick = s.clip.duration_ticks; /* exactly the end is allowed */
    CHECK(pixelica_clip_validate(&s.clip) == PIXELICA_OK);
}

/* ---- sampling ---------------------------------------------------------- */

static void test_clip_sample(void)
{
    Simple s;
    PIXELICA_TRS pose[2];
    PIXELICA_TRS keep[2];

    simple_init(&s, PIXELICA_EASE_LINEAR);

    /* Exact keys, interior interpolation and clamping outside the range. */
    CHECK(pixelica_clip_sample(&s.clip, 0, pose, 2) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 0.0, 0.0);
    CHECK(pixelica_clip_sample(&s.clip, 10, pose, 2) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 10.0, 0.0);
    CHECK_NEAR(pose[0].translation.y, -4.0, 0.0);
    CHECK(pixelica_clip_sample(&s.clip, 5, pose, 2) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 5.0, 1e-5);
    CHECK_NEAR(pose[0].translation.y, -2.0, 1e-5);
    CHECK_NEAR(fabs(quat_dot(pose[0].rotation, qz(0.78539816f))), 1.0, 1e-6);
    /* Rotations always come out unit length. */
    CHECK_NEAR(quat_dot(pose[0].rotation, pose[0].rotation), 1.0, 1e-6);
    CHECK(pixelica_clip_sample(&s.clip, 999, pose, 2) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 10.0, 0.0);

    /* A missing track samples to its identity, not to garbage. */
    CHECK(pose[0].scale.x == 1.0f && pose[0].scale.y == 1.0f &&
          pose[0].scale.z == 1.0f);

    /* STEP holds the left value until the next key is reached. */
    simple_init(&s, PIXELICA_EASE_STEP);
    CHECK(pixelica_clip_sample(&s.clip, 9, pose, 1) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 0.0, 0.0);
    CHECK(pixelica_clip_sample(&s.clip, 10, pose, 1) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 10.0, 0.0);

    /* Every easing stays inside the two key values, even at the extremes. */
    {
        const PIXELICA_Ease eases[6] = {
            PIXELICA_EASE_STEP,     PIXELICA_EASE_LINEAR,
            PIXELICA_EASE_QUAD_IN,  PIXELICA_EASE_QUAD_OUT,
            PIXELICA_EASE_QUAD_IN_OUT, PIXELICA_EASE_SMOOTHSTEP};
        int e;
        uint32_t t;

        for (e = 0; e < 6; ++e) {
            simple_init(&s, eases[e]);
            s.t[0].value = v3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            s.t[1].value = v3(FLT_MAX, FLT_MAX, FLT_MAX);
            CHECK(pixelica_clip_validate(&s.clip) == PIXELICA_OK);
            for (t = 0; t <= 11u; ++t) {
                CHECK(pixelica_clip_sample(&s.clip, t, pose, 1) == PIXELICA_OK);
                CHECK(isfinite(pose[0].translation.x));
                CHECK(pose[0].translation.x >= -FLT_MAX &&
                      pose[0].translation.x <= FLT_MAX);
                CHECK(isfinite(pose[0].rotation.w));
            }
        }
    }

    /* Failures leave the output byte-for-byte unchanged. */
    simple_init(&s, PIXELICA_EASE_LINEAR);
    poison(pose, 2);
    poison(keep, 2);
    CHECK(pixelica_clip_sample(&s.clip, 5, pose, 0) == NOMEM);
    CHECK(pixelica_clip_sample(NULL, 5, pose, 2) == BAD);
    CHECK(pixelica_clip_sample(&s.clip, 5, NULL, 2) == BAD);
    s.clip.duration_ticks = 0;
    CHECK(pixelica_clip_sample(&s.clip, 5, pose, 2) == BAD);
    CHECK(memcmp(pose, keep, sizeof pose) == 0);

    /* An output that overlaps what the clip reads is rejected. */
    simple_init(&s, PIXELICA_EASE_LINEAR);
    CHECK(pixelica_clip_sample(&s.clip, 5,
                               (PIXELICA_TRS *)(void *)s.t, 1) == BAD);
    CHECK(pixelica_clip_sample(&s.clip, 5,
                               (PIXELICA_TRS *)(void *)s.r, 1) == BAD);
    CHECK(pixelica_clip_sample(&s.clip, 5,
                               (PIXELICA_TRS *)(void *)&s.track, 1) == BAD);
    CHECK(pixelica_clip_sample(&s.clip, 5,
                               (PIXELICA_TRS *)(void *)&s.clip, 1) == BAD);
}

/* ---- playback positions ------------------------------------------------ */

static void test_player_positions(void)
{
    Simple s;
    PIXELICA_ClipPlayer p;

    simple_init(&s, PIXELICA_EASE_LINEAR); /* duration 10 */
    memset(&p, 0, sizeof p);

    /* ONCE stops exactly on duration_ticks and reports finished. */
    CHECK(pixelica_clip_player_init(&p, &s.clip, false) == PIXELICA_OK);
    CHECK(p.time_ticks == 0 && !p.finished && !p.started);
    CHECK(pixelica_clip_player_advance(&p, 4, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.time_ticks, 4);
    CHECK(p.started && !p.finished);
    CHECK(pixelica_clip_player_advance(&p, 6, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.time_ticks, 10);
    CHECK(p.finished);
    CHECK(pixelica_clip_player_advance(&p, 1000, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.time_ticks, 10); /* stays put */
    CHECK_EQ_LONG(p.repetition, 0);

    /* A huge single step also stops on the end. */
    CHECK(pixelica_clip_player_init(&p, &s.clip, false) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 0xFFFFFFFFu, NULL, NULL) ==
          PIXELICA_OK);
    CHECK_EQ_LONG(p.time_ticks, 10);
    CHECK(p.finished);

    /* LOOP wraps, never finishes and counts repetitions. */
    CHECK(pixelica_clip_player_init(&p, &s.clip, true) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 10, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.time_ticks, 0);
    CHECK_EQ_LONG(p.repetition, 1);
    CHECK(!p.finished);
    CHECK(pixelica_clip_player_advance(&p, 25, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.time_ticks, 5);
    CHECK_EQ_LONG(p.repetition, 3);

    /* paused and zero steps do nothing at all. */
    p.paused = true;
    CHECK(pixelica_clip_player_advance(&p, 7, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.time_ticks, 5);
    p.paused = false;
    CHECK(pixelica_clip_player_advance(&p, 0, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.time_ticks, 5);

    /* Invalid players and clips are refused. */
    CHECK(pixelica_clip_player_init(NULL, &s.clip, false) == BAD);
    CHECK(pixelica_clip_player_init(&p, NULL, false) == BAD);
    CHECK(pixelica_clip_player_advance(NULL, 1, NULL, NULL) == BAD);
    {
        PIXELICA_ClipPlayer bad;

        memset(&bad, 0, sizeof bad);
        CHECK(pixelica_clip_player_advance(&bad, 1, NULL, NULL) == BAD);
        CHECK(pixelica_clip_player_sample(&bad, NULL, 0, NULL, 0) == BAD);
    }
    /* A player whose fields were tampered with is rejected. */
    CHECK(pixelica_clip_player_init(&p, &s.clip, true) == PIXELICA_OK);
    p.time_ticks = s.clip.duration_ticks; /* illegal while looping */
    CHECK(pixelica_clip_player_advance(&p, 1, NULL, NULL) == BAD);
}

/* ---- events ------------------------------------------------------------ */

#define LOGMAX 256

typedef struct Log {
    int n;
    int kind[LOGMAX];
    uint32_t id[LOGMAX];
    uint32_t tick[LOGMAX];
    uint64_t rep[LOGMAX];
    int overflow;
} Log;

static void log_event(void *userdata, const PIXELICA_ClipEventInfo *info)
{
    Log *l = (Log *)userdata;

    if (l->n >= LOGMAX) {
        l->overflow = 1;
        return;
    }
    l->kind[l->n] = (int)info->kind;
    l->id[l->n] = info->id;
    l->tick[l->n] = info->tick;
    l->rep[l->n] = info->repetition;
    ++l->n;
}

static int log_same(const Log *a, const Log *b)
{
    int i;

    if (a->n != b->n || a->overflow || b->overflow) {
        return 0;
    }
    for (i = 0; i < a->n; ++i) {
        if (a->kind[i] != b->kind[i] || a->id[i] != b->id[i] ||
            a->tick[i] != b->tick[i] || a->rep[i] != b->rep[i]) {
            return 0;
        }
    }
    return 1;
}

/* Clip of duration 4 with events on ticks 0, 2 and 4. */
typedef struct EvClip {
    PIXELICA_ClipBoneTrack track;
    PIXELICA_ClipEvent ev[3];
    PIXELICA_AnimClip clip;
} EvClip;

static void evclip_init(EvClip *e)
{
    memset(e, 0, sizeof *e);
    e->ev[0].tick = 0;
    e->ev[0].id = 100;
    e->ev[1].tick = 2;
    e->ev[1].id = 102;
    e->ev[2].tick = 4;
    e->ev[2].id = 104;
    e->clip.tracks = &e->track;
    e->clip.bone_count = 1;
    e->clip.duration_ticks = 4;
    e->clip.events = e->ev;
    e->clip.event_count = 3;
    CHECK(pixelica_clip_validate(&e->clip) == PIXELICA_OK);
}

static void test_events_swept_window(void)
{
    EvClip e;
    PIXELICA_ClipPlayer p;
    Log l;

    evclip_init(&e);
    memset(&p, 0, sizeof p);

    /* The reached pose fires on the step that reaches it: advancing by 2
     * from tick 0 must report the tick 2 event NOW, not one step later.
     * The starting pose (tick 0) is reported on this first move too. */
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_init(&p, &e.clip, false) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 2, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 2);
    CHECK_EQ_LONG(l.id[0], 100);
    CHECK_EQ_LONG(l.tick[0], 0);
    CHECK_EQ_LONG(l.id[1], 102);
    CHECK_EQ_LONG(l.tick[1], 2);
    CHECK(l.kind[1] == PIXELICA_CLIP_EVENT_USER);

    /* Leaving a tick does not re-fire its event. */
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_advance(&p, 1, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 0);

    /* Landing on duration_ticks fires its event, then END, once. */
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_advance(&p, 1, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 2);
    CHECK_EQ_LONG(l.id[0], 104);
    CHECK(l.kind[1] == PIXELICA_CLIP_EVENT_END);
    CHECK_EQ_LONG(l.tick[1], 4);
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_advance(&p, 50, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 0); /* no duplicate END */

    /* The starting pose is only owed once: a 0-tick advance does not
     * consume it, and it is not reported again after the first move. */
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_init(&p, &e.clip, false) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 0, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 0);
    CHECK(!p.started);
    CHECK(pixelica_clip_player_advance(&p, 1, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 1);
    CHECK_EQ_LONG(l.id[0], 100);
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_advance(&p, 1, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 1);
    CHECK_EQ_LONG(l.id[0], 102);
}

static void test_events_loop_boundaries(void)
{
    EvClip e;
    PIXELICA_ClipPlayer p;
    Log l;

    evclip_init(&e);
    memset(&p, 0, sizeof p);
    CHECK(pixelica_clip_player_init(&p, &e.clip, true) == PIXELICA_OK);

    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_advance(&p, 2, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 2); /* tick 0 (start) and tick 2 */

    /* Wrapping lands on tick 0 of the next repetition: LOOP first, then
     * that repetition's tick 0 event. The tick-4 event never fires in a
     * looping clip, because position 4 does not exist. */
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_advance(&p, 2, log_event, &l) == PIXELICA_OK);
    CHECK_EQ_LONG(l.n, 2);
    CHECK(l.kind[0] == PIXELICA_CLIP_EVENT_LOOP);
    CHECK_EQ_LONG(l.tick[0], 4);
    CHECK_EQ_LONG(l.rep[0], 0);
    CHECK(l.kind[1] == PIXELICA_CLIP_EVENT_USER);
    CHECK_EQ_LONG(l.id[1], 100);
    CHECK_EQ_LONG(l.rep[1], 1);
    CHECK_EQ_LONG(p.time_ticks, 0);
    CHECK_EQ_LONG(p.repetition, 1);
}

static void test_events_partition_equivalence(void)
{
    EvClip e;
    PIXELICA_ClipPlayer p;
    Log whole;
    Log split;
    uint32_t step;
    int i;

    evclip_init(&e);

    /* One long step must report exactly what many short steps report. */
    for (step = 1; step <= 5u; ++step) {
        uint32_t total = 20;
        uint32_t done;

        memset(&whole, 0, sizeof whole);
        memset(&p, 0, sizeof p);
        CHECK(pixelica_clip_player_init(&p, &e.clip, true) == PIXELICA_OK);
        CHECK(pixelica_clip_player_advance(&p, total, log_event, &whole) ==
              PIXELICA_OK);

        memset(&split, 0, sizeof split);
        memset(&p, 0, sizeof p);
        CHECK(pixelica_clip_player_init(&p, &e.clip, true) == PIXELICA_OK);
        for (done = 0; done < total; done += step) {
            uint32_t n = total - done < step ? total - done : step;

            CHECK(pixelica_clip_player_advance(&p, n, log_event, &split) ==
                  PIXELICA_OK);
        }
        CHECK(log_same(&whole, &split));
        CHECK(whole.n > 0);
    }

    /* Same for a non-looping clip, including the END marker. */
    memset(&whole, 0, sizeof whole);
    memset(&p, 0, sizeof p);
    CHECK(pixelica_clip_player_init(&p, &e.clip, false) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 4, log_event, &whole) == PIXELICA_OK);
    memset(&split, 0, sizeof split);
    memset(&p, 0, sizeof p);
    CHECK(pixelica_clip_player_init(&p, &e.clip, false) == PIXELICA_OK);
    for (i = 0; i < 4; ++i) {
        CHECK(pixelica_clip_player_advance(&p, 1, log_event, &split) ==
              PIXELICA_OK);
    }
    CHECK(log_same(&whole, &split));
    CHECK_EQ_LONG(whole.n, 4); /* 100, 102, 104, END */
    CHECK(whole.kind[whole.n - 1] == PIXELICA_CLIP_EVENT_END);
}

static void test_events_large_step_cap(void)
{
    PIXELICA_ClipBoneTrack track;
    PIXELICA_ClipEvent ev;
    PIXELICA_AnimClip clip;
    PIXELICA_ClipPlayer p;
    Log l;
    int i;
    int loops = 0;
    int users = 0;

    memset(&track, 0, sizeof track);
    memset(&clip, 0, sizeof clip);
    ev.tick = 0;
    ev.id = 7;
    clip.tracks = &track;
    clip.bone_count = 1;
    clip.duration_ticks = 1; /* every tick is a wrap */
    clip.events = &ev;
    clip.event_count = 1;
    CHECK(pixelica_clip_validate(&clip) == PIXELICA_OK);

    memset(&p, 0, sizeof p);
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_init(&p, &clip, true) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 100, log_event, &l) == PIXELICA_OK);

    /* The position is still exact; only the oldest events were dropped. */
    CHECK_EQ_LONG(p.time_ticks, 0);
    CHECK_EQ_LONG(p.repetition, 100);
    CHECK_EQ_LONG(p.skipped_repetitions,
                  101 - PIXELICA_CLIP_MAX_EVENT_REPETITIONS);
    for (i = 0; i < l.n; ++i) {
        if (l.kind[i] == PIXELICA_CLIP_EVENT_LOOP) {
            ++loops;
        } else if (l.kind[i] == PIXELICA_CLIP_EVENT_USER) {
            ++users;
        }
    }
    CHECK_EQ_LONG(loops, PIXELICA_CLIP_MAX_EVENT_REPETITIONS);
    CHECK_EQ_LONG(users, PIXELICA_CLIP_MAX_EVENT_REPETITIONS);
    CHECK(!l.overflow);
    /* The last reported repetition is the one the player ended in. */
    CHECK_EQ_LONG(l.rep[l.n - 1], 100);

    /* Without a callback nothing is reported and nothing is counted as
     * skipped, but the position is still exact. */
    memset(&p, 0, sizeof p);
    CHECK(pixelica_clip_player_init(&p, &clip, true) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 100, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.repetition, 100);
    CHECK_EQ_LONG(p.skipped_repetitions, 0);
}

/* ---- callback reentrancy ----------------------------------------------- */

typedef struct Reentry {
    PIXELICA_ClipPlayer *player;
    const PIXELICA_AnimClip *other;
    int calls;
    PIXELICA_Status advance_rc;
    PIXELICA_Status crossfade_rc;
    PIXELICA_Status init_rc;
    PIXELICA_Status sample_rc;
    uint64_t seen_time;
} Reentry;

static void reentry_cb(void *userdata, const PIXELICA_ClipEventInfo *info)
{
    Reentry *r = (Reentry *)userdata;
    PIXELICA_TRS pose[1];

    (void)info;
    ++r->calls;
    r->seen_time = r->player->time_ticks;
    r->advance_rc = pixelica_clip_player_advance(r->player, 1, NULL, NULL);
    r->crossfade_rc = pixelica_clip_player_crossfade(r->player, r->other, false,
                                                     2, PIXELICA_EASE_LINEAR);
    r->init_rc = pixelica_clip_player_init(r->player, r->other, false);
    /* Read-only calls stay legal inside a callback. */
    r->sample_rc = pixelica_clip_player_sample(r->player, pose, 1, NULL, 0);
}

static void test_event_reentrancy(void)
{
    EvClip e;
    Simple other;
    PIXELICA_ClipPlayer p;
    Reentry r;

    evclip_init(&e);
    simple_init(&other, PIXELICA_EASE_LINEAR);
    memset(&p, 0, sizeof p);
    memset(&r, 0, sizeof r);
    r.player = &p;
    r.other = &other.clip;

    CHECK(pixelica_clip_player_init(&p, &e.clip, false) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 2, reentry_cb, &r) == PIXELICA_OK);
    CHECK(r.calls > 0);
    CHECK(r.advance_rc == BAD);
    CHECK(r.crossfade_rc == BAD);
    CHECK(r.init_rc == BAD); /* the constructor refuses reentry too */
    CHECK(r.sample_rc == PIXELICA_OK);
    /* Nothing the callback tried changed the player: it is still on the
     * original clip at the final position of this advance. */
    CHECK(p.clip == &e.clip);
    CHECK_EQ_LONG(p.time_ticks, 2);
    CHECK_EQ_LONG(r.seen_time, 2); /* callbacks see the final position */
    CHECK(p.fade_from == NULL);
    CHECK(!p.in_event);

    /* Once the advance has returned, the same calls work again. */
    CHECK(pixelica_clip_player_advance(&p, 1, NULL, NULL) == PIXELICA_OK);
    CHECK(pixelica_clip_player_init(&p, &e.clip, false) == PIXELICA_OK);
}

/* ---- crossfade --------------------------------------------------------- */

/* Two one-bone clips with constant, distinguishable translations. */
typedef struct Const1 {
    PIXELICA_ClipVec3Key key;
    PIXELICA_ClipBoneTrack track;
    PIXELICA_AnimClip clip;
} Const1;

static void const1_init(Const1 *c, float x, uint32_t duration)
{
    memset(c, 0, sizeof *c);
    c->key = vkey(0, v3(x, 0.0f, 0.0f), PIXELICA_EASE_LINEAR);
    c->track.translation = &c->key;
    c->track.translation_count = 1;
    c->clip.tracks = &c->track;
    c->clip.bone_count = 1;
    c->clip.duration_ticks = duration;
    CHECK(pixelica_clip_validate(&c->clip) == PIXELICA_OK);
}

static void test_crossfade_basics(void)
{
    Const1 a, b;
    PIXELICA_ClipPlayer p;
    PIXELICA_TRS pose[1];
    PIXELICA_TRS scratch[1];
    float w = -1.0f;

    const1_init(&a, 0.0f, 10);
    const1_init(&b, 100.0f, 10);
    memset(&p, 0, sizeof p);
    CHECK(pixelica_clip_player_init(&p, &a.clip, true) == PIXELICA_OK);
    CHECK(pixelica_clip_player_weight(&p, &w) == PIXELICA_OK);
    CHECK_NEAR(w, 1.0, 0.0); /* no fade: the incoming clip alone */

    CHECK(pixelica_clip_player_crossfade(&p, &b.clip, true, 4,
                                         PIXELICA_EASE_LINEAR) == PIXELICA_OK);
    CHECK(p.clip == &b.clip);
    CHECK(p.fade_from == &a.clip);

    /* At the moment of the call the pose is still entirely the old one. */
    CHECK(pixelica_clip_player_weight(&p, &w) == PIXELICA_OK);
    CHECK_NEAR(w, 0.0, 0.0);
    CHECK(pixelica_clip_player_sample(&p, pose, 1, scratch, 1) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 0.0, 1e-6);

    /* Halfway through, the pose is halfway between the two clips. */
    CHECK(pixelica_clip_player_advance(&p, 2, NULL, NULL) == PIXELICA_OK);
    CHECK(pixelica_clip_player_weight(&p, &w) == PIXELICA_OK);
    CHECK_NEAR(w, 0.5, 1e-6);
    CHECK(pixelica_clip_player_sample(&p, pose, 1, scratch, 1) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 50.0, 1e-4);

    /* When it completes the outgoing clip is dropped entirely. */
    CHECK(pixelica_clip_player_advance(&p, 2, NULL, NULL) == PIXELICA_OK);
    CHECK(p.fade_from == NULL);
    CHECK(pixelica_clip_player_weight(&p, &w) == PIXELICA_OK);
    CHECK_NEAR(w, 1.0, 0.0);
    CHECK(pixelica_clip_player_sample(&p, pose, 1, NULL, 0) == PIXELICA_OK);
    CHECK_NEAR(pose[0].translation.x, 100.0, 1e-6);

    /* fade_ticks == 0 switches immediately. */
    CHECK(pixelica_clip_player_crossfade(&p, &a.clip, false, 0,
                                         PIXELICA_EASE_LINEAR) == PIXELICA_OK);
    CHECK(p.fade_from == NULL);
    CHECK(p.clip == &a.clip);
    CHECK_EQ_LONG(p.time_ticks, 0);

    /* Rejections: mismatched rigs, bad ease, bad clip, dead player. */
    {
        PIXELICA_ClipBoneTrack two[2];
        PIXELICA_AnimClip big;
        PIXELICA_ClipPlayer dead;

        memset(two, 0, sizeof two);
        memset(&big, 0, sizeof big);
        big.tracks = two;
        big.bone_count = 2;
        big.duration_ticks = 5;
        CHECK(pixelica_clip_validate(&big) == PIXELICA_OK);
        CHECK(pixelica_clip_player_crossfade(&p, &big, false, 3,
                                             PIXELICA_EASE_LINEAR) == BAD);
        CHECK(pixelica_clip_player_crossfade(&p, &b.clip, false, 3,
                                             (PIXELICA_Ease)42) == BAD);
        CHECK(pixelica_clip_player_crossfade(&p, NULL, false, 3,
                                             PIXELICA_EASE_LINEAR) == BAD);
        CHECK(p.clip == &a.clip && p.fade_from == NULL);
        memset(&dead, 0, sizeof dead);
        CHECK(pixelica_clip_player_crossfade(&dead, &b.clip, false, 3,
                                             PIXELICA_EASE_LINEAR) == BAD);
    }
}

/* Review case: a crossfade into a short ONCE clip must still finish. */
static void test_crossfade_outlives_finished_clip(void)
{
    Const1 a, b;
    PIXELICA_ClipPlayer p;
    Log l;
    float w = -1.0f;
    int ends = 0;
    int i;

    const1_init(&a, 0.0f, 10);
    const1_init(&b, 100.0f, 3); /* the incoming clip ends after 3 ticks */
    memset(&p, 0, sizeof p);
    memset(&l, 0, sizeof l);
    CHECK(pixelica_clip_player_init(&p, &a.clip, true) == PIXELICA_OK);
    CHECK(pixelica_clip_player_crossfade(&p, &b.clip, false, 10,
                                         PIXELICA_EASE_LINEAR) == PIXELICA_OK);

    CHECK(pixelica_clip_player_advance(&p, 3, log_event, &l) == PIXELICA_OK);
    CHECK(p.finished);
    CHECK(pixelica_clip_player_weight(&p, &w) == PIXELICA_OK);
    CHECK_NEAR(w, 0.3, 1e-6);

    /* The fade keeps running on elapsed ticks even though the incoming
     * clip is finished, and it reaches 1 exactly when it should. */
    CHECK(pixelica_clip_player_advance(&p, 6, log_event, &l) == PIXELICA_OK);
    CHECK(p.fade_from == &a.clip);
    CHECK(pixelica_clip_player_weight(&p, &w) == PIXELICA_OK);
    CHECK_NEAR(w, 0.9, 1e-6);
    CHECK(pixelica_clip_player_advance(&p, 1, log_event, &l) == PIXELICA_OK);
    CHECK(p.fade_from == NULL);
    CHECK(pixelica_clip_player_weight(&p, &w) == PIXELICA_OK);
    CHECK_NEAR(w, 1.0, 0.0);
    CHECK_EQ_LONG(p.time_ticks, 3); /* held at its last tick throughout */

    /* END fired exactly once over all of that. */
    for (i = 0; i < l.n; ++i) {
        if (l.kind[i] == PIXELICA_CLIP_EVENT_END) {
            ++ends;
        }
    }
    CHECK_EQ_LONG(ends, 1);

    /* A paused player holds the fade too. */
    const1_init(&b, 100.0f, 3);
    memset(&p, 0, sizeof p);
    CHECK(pixelica_clip_player_init(&p, &a.clip, true) == PIXELICA_OK);
    CHECK(pixelica_clip_player_crossfade(&p, &b.clip, false, 10,
                                         PIXELICA_EASE_LINEAR) == PIXELICA_OK);
    p.paused = true;
    CHECK(pixelica_clip_player_advance(&p, 5, NULL, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(p.fade_elapsed, 0);
}

/* The documented interrupted-fade policy: the older outgoing clip is
 * dropped, which is a visible discontinuity, not a jump-free handover. */
static void test_crossfade_interrupted_policy(void)
{
    Const1 a, b, c;
    PIXELICA_ClipPlayer p;
    PIXELICA_TRS before[1];
    PIXELICA_TRS after[1];
    PIXELICA_TRS scratch[1];
    float w = -1.0f;

    const1_init(&a, 0.0f, 10);
    const1_init(&b, 100.0f, 10);
    const1_init(&c, 200.0f, 10);
    memset(&p, 0, sizeof p);
    CHECK(pixelica_clip_player_init(&p, &a.clip, true) == PIXELICA_OK);
    CHECK(pixelica_clip_player_crossfade(&p, &b.clip, true, 10,
                                         PIXELICA_EASE_LINEAR) == PIXELICA_OK);
    CHECK(pixelica_clip_player_advance(&p, 5, NULL, NULL) == PIXELICA_OK);
    CHECK(pixelica_clip_player_sample(&p, before, 1, scratch, 1) == PIXELICA_OK);
    CHECK_NEAR(before[0].translation.x, 50.0, 1e-4);

    /* Interrupting: the clip that was fading in (b, at its current tick)
     * becomes the outgoing pose; a's remaining half is dropped. */
    CHECK(pixelica_clip_player_crossfade(&p, &c.clip, true, 10,
                                         PIXELICA_EASE_LINEAR) == PIXELICA_OK);
    CHECK(p.fade_from == &b.clip);
    CHECK_EQ_LONG(p.fade_from_time, 5);
    CHECK(pixelica_clip_player_weight(&p, &w) == PIXELICA_OK);
    CHECK_NEAR(w, 0.0, 0.0);
    CHECK(pixelica_clip_player_sample(&p, after, 1, scratch, 1) == PIXELICA_OK);
    CHECK_NEAR(after[0].translation.x, 100.0, 1e-4);
    /* Documented discontinuity: the pose really does move. */
    CHECK(fabs((double)after[0].translation.x - before[0].translation.x) > 1.0);
}

static void test_player_sample_errors(void)
{
    Const1 a, b;
    PIXELICA_ClipPlayer p;
    PIXELICA_TRS pose[2];
    PIXELICA_TRS keep[2];
    PIXELICA_TRS scratch[2];

    const1_init(&a, 0.0f, 10);
    const1_init(&b, 100.0f, 10);
    memset(&p, 0, sizeof p);
    CHECK(pixelica_clip_player_init(&p, &a.clip, true) == PIXELICA_OK);

    poison(pose, 2);
    poison(keep, 2);
    CHECK(pixelica_clip_player_sample(&p, pose, 0, NULL, 0) == NOMEM);
    CHECK(pixelica_clip_player_sample(&p, NULL, 2, NULL, 0) == BAD);
    CHECK(pixelica_clip_player_sample(NULL, pose, 2, NULL, 0) == BAD);
    CHECK(memcmp(pose, keep, sizeof pose) == 0);

    /* With a fade the workspace becomes mandatory. */
    CHECK(pixelica_clip_player_crossfade(&p, &b.clip, true, 4,
                                         PIXELICA_EASE_LINEAR) == PIXELICA_OK);
    CHECK(pixelica_clip_player_sample(&p, pose, 2, NULL, 0) == NOMEM);
    CHECK(pixelica_clip_player_sample(&p, pose, 2, scratch, 0) == NOMEM);
    CHECK(pixelica_clip_player_sample(&p, pose, 2, pose, 2) == BAD); /* alias */
    CHECK(memcmp(pose, keep, sizeof pose) == 0);
    CHECK(pixelica_clip_player_sample(&p, pose, 2, scratch, 2) == PIXELICA_OK);

    /* An output overlapping either clip's data is rejected. */
    CHECK(pixelica_clip_player_sample(&p, (PIXELICA_TRS *)(void *)&a.key, 1,
                                      scratch, 2) == BAD);
    CHECK(pixelica_clip_player_sample(&p, (PIXELICA_TRS *)(void *)&b.key, 1,
                                      scratch, 2) == BAD);
    CHECK(pixelica_clip_player_sample(&p, pose, 2,
                                      (PIXELICA_TRS *)(void *)&a.key, 1) == BAD);
}

int main(void)
{
    test_trs_identity_and_matrix();
    test_trs_blend();
    test_clip_validate();
    test_clip_sample();
    test_player_positions();
    test_events_swept_window();
    test_events_loop_boundaries();
    test_events_partition_equivalence();
    test_events_large_step_cap();
    test_event_reentrancy();
    test_crossfade_basics();
    test_crossfade_outlives_finished_clip();
    test_crossfade_interrupted_policy();
    test_player_sample_errors();
    return pixelica_t_finish("test_anim_clip");
}

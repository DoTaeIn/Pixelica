/*
 * Pixelica - optional particle simulation core: default config, exact preset
 * values and Y mirroring, init/config validation (NaN, infinities, bounds,
 * inverted ranges) with no mutation on error, uninitialized/corrupted
 * emitters, dt validation, seeded determinism (including seed 0), random
 * ranges, pool saturation and the saturating `dropped` counter, analytic
 * gravity, expiration with swap removal, end-of-tick emission with carried
 * fractions, size/color/opacity/rotation interpolation, move and clear.
 *
 * Includes pixelica/particles.h FIRST and fails to compile if it pulls in any
 * renderer or adapter header; links only Pixelica::particles (+ common).
 */
#include "pixelica/particles.h"

#if defined(PIXELICA_RENDERER2D_H) || defined(PIXELICA_HYBRID_H) || \
    defined(PIXELICA_PIXELICA_H) || defined(PIXELICA_RENDERER_H) || \
    defined(PIXELICA_SCENE_H) || defined(PIXELICA_ASSETS_H) || \
    defined(PIXELICA_PARTICLES2D_H) || defined(PIXELICA_PARTICLES25D_H) || \
    defined(PIXELICA_AUDIO_H)
#error "pixelica/particles.h must not include renderer, adapter or audio headers"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/status.h"

#include "test_util.h"

#define CAP 64u

static PIXELICA_Particle g_big_a[PIXELICA_PARTICLES_MAX_CAPACITY];
static PIXELICA_Particle g_big_b[PIXELICA_PARTICLES_MAX_CAPACITY];

/* ---- helpers ------------------------------------------------------------ */

static PIXELICA_Vec3 v3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static bool vec_eq(PIXELICA_Vec3 a, PIXELICA_Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* No continuous emission, every random range degenerate, long lifetime. */
static PIXELICA_ParticleConfig fixed_config(void)
{
    PIXELICA_ParticleConfig c;

    pixelica_particles_config_default(&c);
    c.rate = 0.0f;
    c.lifetime_min = 100.0f;
    c.lifetime_max = 100.0f;
    return c;
}

static bool same_particle(const PIXELICA_Particle *a, const PIXELICA_Particle *b)
{
    return vec_eq(a->position, b->position) && vec_eq(a->velocity, b->velocity) &&
        a->age == b->age && a->lifetime == b->lifetime && a->size == b->size &&
        a->rotation == b->rotation && a->color == b->color && a->opacity == b->opacity;
}

static bool same_emitter_state(const PIXELICA_ParticleEmitter *a,
                               const PIXELICA_ParticleEmitter *b)
{
    uint32_t i;

    if (a->count != b->count || a->dropped != b->dropped) {
        return false;
    }
    for (i = 0; i < a->count; ++i) {
        if (!same_particle(&a->particles[i], &b->particles[i])) {
            return false;
        }
    }
    return true;
}

static bool same_config(const PIXELICA_ParticleConfig *a, const PIXELICA_ParticleConfig *b)
{
    return vec_eq(a->position, b->position) && vec_eq(a->velocity_min, b->velocity_min) &&
        vec_eq(a->velocity_max, b->velocity_max) && vec_eq(a->gravity, b->gravity) &&
        a->rate == b->rate && a->lifetime_min == b->lifetime_min &&
        a->lifetime_max == b->lifetime_max && a->size_start == b->size_start &&
        a->size_end == b->size_end && a->rotation_min == b->rotation_min &&
        a->rotation_max == b->rotation_max && a->angular_velocity == b->angular_velocity &&
        a->color_start == b->color_start && a->color_end == b->color_end &&
        a->opacity_start == b->opacity_start && a->opacity_end == b->opacity_end;
}

/* Byte snapshot of emitter + live storage to prove "no mutation on error". */
typedef struct Snapshot {
    PIXELICA_ParticleEmitter emitter;
    PIXELICA_Particle particles[CAP];
} Snapshot;

static void snap(Snapshot *s, const PIXELICA_ParticleEmitter *e, const PIXELICA_Particle *storage)
{
    memcpy(&s->emitter, e, sizeof *e);
    memcpy(s->particles, storage, sizeof s->particles);
}

static bool snap_same(const Snapshot *s, const PIXELICA_ParticleEmitter *e,
                      const PIXELICA_Particle *storage)
{
    return memcmp(&s->emitter, e, sizeof *e) == 0 &&
        memcmp(s->particles, storage, sizeof s->particles) == 0;
}

/* ---- config_default / presets ------------------------------------------ */

static void test_default_config(void)
{
    PIXELICA_ParticleConfig c;
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];

    pixelica_particles_config_default(NULL); /* must not crash */
    memset(&c, 0xA5, sizeof c);
    pixelica_particles_config_default(&c);
    CHECK(vec_eq(c.position, v3(0.0f, 0.0f, 0.0f)));
    CHECK(vec_eq(c.velocity_min, v3(0.0f, 0.0f, 0.0f)));
    CHECK(vec_eq(c.velocity_max, v3(0.0f, 0.0f, 0.0f)));
    CHECK(vec_eq(c.gravity, v3(0.0f, 0.0f, 0.0f)));
    CHECK(c.rate == 10.0f);
    CHECK(c.lifetime_min == 1.0f);
    CHECK(c.lifetime_max == 1.0f);
    CHECK(c.size_start == 1.0f);
    CHECK(c.size_end == 1.0f);
    CHECK(c.rotation_min == 0.0f);
    CHECK(c.rotation_max == 0.0f);
    CHECK(c.angular_velocity == 0.0f);
    CHECK_EQ_LONG(c.color_start, 0xFFFF);
    CHECK_EQ_LONG(c.color_end, 0xFFFF);
    CHECK_EQ_LONG(c.opacity_start, 255);
    CHECK_EQ_LONG(c.opacity_end, 255);
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
}

static PIXELICA_ParticleConfig expected_preset(PIXELICA_ParticlePreset p)
{
    PIXELICA_ParticleConfig c;

    pixelica_particles_config_default(&c);
    if (p == PIXELICA_PARTICLES_SPARKS) {
        c.velocity_min = v3(-80.0f, 20.0f, -80.0f);
        c.velocity_max = v3(80.0f, 120.0f, 80.0f);
        c.gravity = v3(0.0f, -240.0f, 0.0f);
        c.rate = 60.0f;
        c.lifetime_min = 0.4f;
        c.lifetime_max = 0.9f;
        c.size_start = 2.0f;
        c.size_end = 1.0f;
        c.color_start = 0xFFE0u;
        c.color_end = 0xF800u;
        c.opacity_start = 255u;
        c.opacity_end = 0u;
    } else if (p == PIXELICA_PARTICLES_SMOKE) {
        c.velocity_min = v3(-10.0f, 15.0f, -10.0f);
        c.velocity_max = v3(10.0f, 35.0f, 10.0f);
        c.gravity = v3(0.0f, 6.0f, 0.0f);
        c.rate = 12.0f;
        c.lifetime_min = 1.5f;
        c.lifetime_max = 3.0f;
        c.size_start = 4.0f;
        c.size_end = 14.0f;
        c.rotation_min = 0.0f;
        c.rotation_max = 6.2831855f;
        c.angular_velocity = 0.5f;
        c.color_start = 0x8410u;
        c.color_end = 0xC618u;
        c.opacity_start = 160u;
        c.opacity_end = 0u;
    } else {
        c.velocity_min = v3(-30.0f, -260.0f, -30.0f);
        c.velocity_max = v3(30.0f, -200.0f, 30.0f);
        c.gravity = v3(0.0f, -60.0f, 0.0f);
        c.rate = 120.0f;
        c.lifetime_min = 0.6f;
        c.lifetime_max = 1.0f;
        c.size_start = 1.0f;
        c.size_end = 1.0f;
        c.color_start = 0x9DFFu;
        c.color_end = 0x5C1Fu;
        c.opacity_start = 200u;
        c.opacity_end = 120u;
    }
    return c;
}

static void test_presets(void)
{
    static const PIXELICA_ParticlePreset presets[3] = {
        PIXELICA_PARTICLES_SPARKS, PIXELICA_PARTICLES_SMOKE, PIXELICA_PARTICLES_RAIN
    };
    PIXELICA_ParticleConfig up;
    PIXELICA_ParticleConfig down;
    PIXELICA_ParticleConfig want;
    PIXELICA_ParticleConfig sentinel;
    PIXELICA_ParticleConfig out;
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];
    size_t i;

    for (i = 0; i < 3u; ++i) {
        memset(&up, 0xA5, sizeof up);
        memset(&down, 0xA5, sizeof down);
        CHECK(pixelica_particles_preset(presets[i], false, &up) == PIXELICA_OK);
        CHECK(pixelica_particles_preset(presets[i], true, &down) == PIXELICA_OK);
        want = expected_preset(presets[i]);
        CHECK(same_config(&up, &want));

        /* y_down mirrors exactly the Y velocity range and gravity.y. */
        CHECK(down.velocity_min.y == -up.velocity_max.y);
        CHECK(down.velocity_max.y == -up.velocity_min.y);
        CHECK(down.gravity.y == -up.gravity.y);
        CHECK(down.velocity_min.y <= down.velocity_max.y);
        want.velocity_min.y = -up.velocity_max.y;
        want.velocity_max.y = -up.velocity_min.y;
        want.gravity.y = -up.gravity.y;
        CHECK(same_config(&down, &want));

        CHECK(pixelica_particles_init(&e, storage, CAP, &up, 7u) == PIXELICA_OK);
        CHECK(pixelica_particles_init(&e, storage, CAP, &down, 7u) == PIXELICA_OK);
    }
    /* Direction sanity: sparks fly up and fall, rain falls, smoke rises. */
    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_SPARKS, false, &up) == PIXELICA_OK);
    CHECK(up.velocity_min.y > 0.0f && up.gravity.y < 0.0f);
    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_SPARKS, true, &down) == PIXELICA_OK);
    CHECK(down.velocity_max.y < 0.0f && down.gravity.y > 0.0f);
    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_RAIN, true, &down) == PIXELICA_OK);
    CHECK(down.velocity_min.y > 0.0f && down.gravity.y > 0.0f);
    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_SMOKE, true, &down) == PIXELICA_OK);
    CHECK(down.velocity_max.y < 0.0f && down.gravity.y < 0.0f);

    /* Unknown preset / NULL out: error, out untouched. */
    memset(&sentinel, 0x3C, sizeof sentinel);
    memcpy(&out, &sentinel, sizeof out);
    CHECK(pixelica_particles_preset((PIXELICA_ParticlePreset)3, false, &out) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_preset((PIXELICA_ParticlePreset)-1, true, &out) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_preset((PIXELICA_ParticlePreset)1000, true, &out) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_SPARKS, false, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
}

/* ---- init validation ---------------------------------------------------- */

/* Applies invalid mutation `which`; false when there are no more. */
static bool make_invalid(PIXELICA_ParticleConfig *c, int which)
{
    float nan_v = pixelica_t_nan();
    float inf_v = pixelica_t_inf();

    switch (which) {
    case 0: c->position.x = nan_v; break;
    case 1: c->position.y = inf_v; break;
    case 2: c->position.z = -inf_v; break;
    case 3: c->position.x = 2.0e6f; break;
    case 4: c->velocity_min.x = nan_v; break;
    case 5: c->velocity_max.y = inf_v; break;
    case 6: c->velocity_min.z = 1.0f; c->velocity_max.z = 0.0f; break;
    case 7: c->velocity_min.x = -2.0e6f; break;
    case 8: c->gravity.y = nan_v; break;
    case 9: c->gravity.x = 1.5e6f; break;
    case 10: c->rate = -1.0f; break;
    case 11: c->rate = 10001.0f; break;
    case 12: c->rate = nan_v; break;
    case 13: c->lifetime_min = 0.0f; break;
    case 14: c->lifetime_min = -1.0f; break;
    case 15: c->lifetime_min = 2.0f; c->lifetime_max = 1.0f; break;
    case 16: c->lifetime_max = 3601.0f; break;
    case 17: c->lifetime_min = nan_v; break;
    case 18: c->lifetime_max = inf_v; break;
    case 19: c->size_start = -0.5f; break;
    case 20: c->size_end = 2.0e6f; break;
    case 21: c->size_start = nan_v; break;
    case 22: c->rotation_min = 1.0f; c->rotation_max = 0.0f; break;
    case 23: c->rotation_max = inf_v; break;
    case 24: c->angular_velocity = nan_v; break;
    case 25: c->angular_velocity = 2.0e6f; break;
    case 26: c->rotation_min = -2.0e6f; break;
    case 27: c->velocity_min.y = 1.0f; c->velocity_max.y = -1.0f; break;
    case 28: c->velocity_min.x = 1.0f; c->velocity_max.x = 0.5f; break;
    case 29: c->lifetime_min = -0.0f; c->lifetime_max = 1.0f; break;
    case 30: c->gravity.z = -inf_v; break;
    case 31: c->size_end = -inf_v; break;
    default: return false;
    }
    return true;
}

static void test_init_validation(void)
{
    PIXELICA_ParticleConfig good = fixed_config();
    PIXELICA_ParticleConfig bad;
    PIXELICA_ParticleConfig edge;
    PIXELICA_ParticleEmitter e;
    PIXELICA_ParticleEmitter sentinel;
    PIXELICA_Particle storage[CAP];
    PIXELICA_Particle pattern[CAP];
    int which;
    int rejected = 0;

    memset(&sentinel, 0x5A, sizeof sentinel);
    memcpy(&e, &sentinel, sizeof e);
    CHECK(pixelica_particles_init(NULL, storage, CAP, &good, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_init(&e, NULL, CAP, &good, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_init(&e, storage, CAP, NULL, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_init(&e, storage, 0u, &good, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_init(&e, g_big_a, PIXELICA_PARTICLES_MAX_CAPACITY + 1u, &good, 1u) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_init(&e, g_big_a, UINT32_MAX, &good, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&e, &sentinel, sizeof e) == 0);
    {
        union {
            PIXELICA_ParticleEmitter emitter;
            PIXELICA_Particle particles[4];
        } overlap;
        unsigned char before[sizeof overlap];
        memset(&overlap, 0x5A, sizeof overlap);
        memcpy(before, &overlap, sizeof overlap);
        CHECK(pixelica_particles_init(&overlap.emitter, overlap.particles, 4u, &good, 1u) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(before, &overlap, sizeof overlap) == 0);
    }

    for (which = 0; ; ++which) {
        bad = good;
        if (!make_invalid(&bad, which)) {
            break;
        }
        if (pixelica_particles_init(&e, storage, CAP, &bad, 1u) == PIXELICA_ERR_INVALID_ARG &&
            memcmp(&e, &sentinel, sizeof e) == 0) {
            ++rejected;
        } else {
            fprintf(stderr, "invalid config mutation %d was accepted or mutated\n", which);
        }
    }
    CHECK_EQ_LONG(rejected, 32);

    /* Exact bounds are accepted. */
    edge = good;
    edge.position = v3(1.0e6f, -1.0e6f, 1.0e6f);
    edge.velocity_min = v3(-1.0e6f, -1.0e6f, -1.0e6f);
    edge.velocity_max = v3(1.0e6f, 1.0e6f, 1.0e6f);
    edge.gravity = v3(-1.0e6f, 1.0e6f, -1.0e6f);
    edge.rate = PIXELICA_PARTICLES_MAX_RATE;
    edge.lifetime_min = PIXELICA_PARTICLES_MAX_LIFETIME;
    edge.lifetime_max = PIXELICA_PARTICLES_MAX_LIFETIME;
    edge.size_start = 0.0f;
    edge.size_end = 1.0e6f;
    edge.rotation_min = -1.0e6f;
    edge.rotation_max = 1.0e6f;
    edge.angular_velocity = -1.0e6f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &edge, 1u) == PIXELICA_OK);
    /* Worst-case magnitudes stay finite over a full lifetime. */
    CHECK(pixelica_particles_burst(&e, CAP, NULL) == PIXELICA_OK);
    {
        int step;
        bool all_finite = true;
        uint32_t i;

        e.emitting = false;
        for (step = 0; step < 3599; ++step) {
            CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK);
        }
        CHECK_EQ_LONG(e.count, CAP);
        for (i = 0; i < e.count; ++i) {
            const PIXELICA_Particle *p = &e.particles[i];

            all_finite = all_finite && isfinite(p->position.x) && isfinite(p->position.y) &&
                isfinite(p->position.z) && isfinite(p->velocity.x) &&
                isfinite(p->velocity.y) && isfinite(p->velocity.z) &&
                isfinite(p->rotation) && isfinite(p->size);
        }
        CHECK(all_finite);
        CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK);
        CHECK_EQ_LONG(e.count, 0);
    }

    /* Capacity limits 1 and MAX are accepted. */
    CHECK(pixelica_particles_init(&e, storage, 1u, &good, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_init(&e, g_big_a, PIXELICA_PARTICLES_MAX_CAPACITY, &good, 1u) ==
          PIXELICA_OK);

    /* Fresh garbage-filled emitter is accepted; storage is not written. */
    memset(&e, 0xA5, sizeof e);
    memset(pattern, 0x6B, sizeof pattern);
    memcpy(storage, pattern, sizeof storage);
    good.position = v3(1.0f, 2.0f, 3.0f);
    CHECK(pixelica_particles_init(&e, storage, CAP, &good, 99u) == PIXELICA_OK);
    CHECK(e.particles == storage);
    CHECK_EQ_LONG(e.capacity, CAP);
    CHECK_EQ_LONG(e.count, 0);
    CHECK(e.emitting == true);
    CHECK_EQ_LONG(e.dropped, 0);
    CHECK(same_config(&e.config, &good));
    CHECK(memcmp(storage, pattern, sizeof storage) == 0);

    /* Re-init with the emitter's own config (aliasing) resets the state. */
    CHECK(pixelica_particles_burst(&e, 5u, NULL) == PIXELICA_OK);
    e.emitting = false;
    CHECK(pixelica_particles_init(&e, storage, CAP, &e.config, 99u) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 0);
    CHECK(e.emitting == true);
    CHECK(same_config(&e.config, &good));
}

/* ---- uninitialized / corrupted emitters --------------------------------- */

static void test_uninitialized(void)
{
    PIXELICA_ParticleConfig good = fixed_config();
    PIXELICA_ParticleEmitter zero;
    PIXELICA_ParticleEmitter copy;
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];
    uint32_t spawned = 77u;

    memset(&zero, 0, sizeof zero);
    memcpy(&copy, &zero, sizeof copy);
    CHECK(pixelica_particles_update(&zero, 0.1f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_burst(&zero, 3u, &spawned) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_move(&zero, v3(1.0f, 1.0f, 1.0f)) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_clear(&zero) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&zero, &copy, sizeof zero) == 0);
    CHECK_EQ_LONG(spawned, 77);

    CHECK(pixelica_particles_update(NULL, 0.1f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_burst(NULL, 3u, &spawned) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_move(NULL, v3(0.0f, 0.0f, 0.0f)) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_clear(NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(spawned, 77);

    /* Corrupted public fields are detected instead of overflowing. */
    CHECK(pixelica_particles_init(&e, storage, CAP, &good, 1u) == PIXELICA_OK);
    e.count = CAP + 1u;
    CHECK(pixelica_particles_burst(&e, 1u, &spawned) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_update(&e, 0.1f) == PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(e.count, CAP + 1u);

    CHECK(pixelica_particles_init(&e, storage, CAP, &good, 1u) == PIXELICA_OK);
    e.config.gravity.x = pixelica_t_nan();
    CHECK(pixelica_particles_update(&e, 0.1f) == PIXELICA_ERR_INVALID_ARG);

    CHECK(pixelica_particles_init(&e, storage, CAP, &good, 1u) == PIXELICA_OK);
    e.config.rate = 1.0e9f;
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(e.count, 0);

    CHECK(pixelica_particles_init(&e, storage, CAP, &good, 1u) == PIXELICA_OK);
    e.particles = NULL;
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_ERR_INVALID_ARG);

    CHECK(pixelica_particles_init(&e, storage, CAP, &good, 1u) == PIXELICA_OK);
    e.capacity = PIXELICA_PARTICLES_MAX_CAPACITY + 1u;
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(spawned, 77);
}

/* ---- update dt validation ----------------------------------------------- */

static void test_update_dt(void)
{
    static Snapshot s;
    PIXELICA_ParticleConfig c = fixed_config();
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];
    PIXELICA_Particle before;

    memset(storage, 0, sizeof storage);
    c.velocity_min = v3(1.0f, 2.0f, 3.0f);
    c.velocity_max = c.velocity_min;
    c.rate = 4.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 5u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 3u, NULL) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.125f) == PIXELICA_OK); /* carry 0.5 */
    snap(&s, &e, storage);

    CHECK(pixelica_particles_update(&e, pixelica_t_nan()) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_update(&e, pixelica_t_inf()) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_update(&e, -pixelica_t_inf()) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_update(&e, -0.001f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_update(&e, 1.001f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_update(&e, 2.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(snap_same(&s, &e, storage));

    /* dt = 0 moves nothing and births nothing. */
    before = storage[0];
    CHECK(pixelica_particles_update(&e, 0.0f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 3);
    CHECK(same_particle(&storage[0], &before));

    /* dt = MAX_DT is accepted: carry 0.5 + 4 births. */
    CHECK(pixelica_particles_update(&e, PIXELICA_PARTICLES_MAX_DT) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 7);
}

/* ---- determinism and ranges --------------------------------------------- */

static void run_script(PIXELICA_ParticleEmitter *e)
{
    int i;

    CHECK(pixelica_particles_burst(e, 5u, NULL) == PIXELICA_OK);
    for (i = 0; i < 30; ++i) {
        CHECK(pixelica_particles_update(e, 1.0f / 60.0f) == PIXELICA_OK);
    }
    CHECK(pixelica_particles_move(e, v3(10.0f, -4.0f, 2.5f)) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(e, 7u, NULL) == PIXELICA_OK);
    for (i = 0; i < 45; ++i) {
        CHECK(pixelica_particles_update(e, 1.0f / 30.0f) == PIXELICA_OK);
    }
}

static void test_determinism(void)
{
    PIXELICA_ParticleConfig c;
    PIXELICA_ParticleEmitter a;
    PIXELICA_ParticleEmitter b;
    uint32_t seeds_differ = 0u;
    uint32_t i;

    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_SPARKS, true, &c) == PIXELICA_OK);

    /* Same seed + same calls -> identical, regardless of storage garbage. */
    memset(g_big_a, 0x11, sizeof g_big_a);
    memset(g_big_b, 0xEE, sizeof g_big_b);
    CHECK(pixelica_particles_init(&a, g_big_a, 256u, &c, 1234u) == PIXELICA_OK);
    CHECK(pixelica_particles_init(&b, g_big_b, 256u, &c, 1234u) == PIXELICA_OK);
    run_script(&a);
    run_script(&b);
    CHECK(a.count > 0u);
    CHECK(same_emitter_state(&a, &b));

    /* Seed 0 is valid and deterministic. */
    CHECK(pixelica_particles_init(&a, g_big_a, 256u, &c, 0u) == PIXELICA_OK);
    CHECK(pixelica_particles_init(&b, g_big_b, 256u, &c, 0u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&a, 16u, NULL) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&b, 16u, NULL) == PIXELICA_OK);
    CHECK(same_emitter_state(&a, &b));
    for (i = 1; i < 16u; ++i) { /* draws are not all identical */
        if (!vec_eq(a.particles[i].velocity, a.particles[0].velocity)) {
            ++seeds_differ;
        }
    }
    CHECK(seeds_differ > 0u);

    /* Different seeds (0 vs 1, 1234 vs 1235) give different particles. */
    CHECK(pixelica_particles_init(&b, g_big_b, 256u, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&b, 16u, NULL) == PIXELICA_OK);
    CHECK(!same_emitter_state(&a, &b));
    CHECK(pixelica_particles_init(&a, g_big_a, 256u, &c, 1234u) == PIXELICA_OK);
    CHECK(pixelica_particles_init(&b, g_big_b, 256u, &c, 1235u) == PIXELICA_OK);
    run_script(&a);
    run_script(&b);
    CHECK(!same_emitter_state(&a, &b));

    /* Re-init with the same seed replays the same sequence. */
    CHECK(pixelica_particles_init(&a, g_big_a, 256u, &c, 42u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&a, 8u, NULL) == PIXELICA_OK);
    memcpy(g_big_b, g_big_a, 8u * sizeof g_big_a[0]);
    CHECK(pixelica_particles_init(&a, g_big_a, 256u, &c, 42u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&a, 8u, NULL) == PIXELICA_OK);
    for (i = 0; i < 8u; ++i) {
        CHECK(same_particle(&g_big_a[i], &g_big_b[i]));
    }
}

static void test_ranges(void)
{
    PIXELICA_ParticleConfig c = fixed_config();
    PIXELICA_ParticleEmitter e;
    float lo_vx = 1.0e9f, hi_vx = -1.0e9f;
    float lo_life = 1.0e9f, hi_life = -1.0e9f;
    float lo_rot = 1.0e9f, hi_rot = -1.0e9f;
    bool inside = true;
    uint32_t spawned = 0u;
    uint32_t i;

    c.position = v3(-3.0f, 4.0f, 5.0f);
    c.velocity_min = v3(-2.0f, 10.0f, 7.0f);
    c.velocity_max = v3(2.0f, 20.0f, 7.0f); /* z degenerate: exactly 7 */
    c.lifetime_min = 0.5f;
    c.lifetime_max = 2.0f;
    c.rotation_min = -3.0f;
    c.rotation_max = 3.0f;
    c.size_start = 6.0f;
    c.color_start = 0x1234u;
    c.opacity_start = 99u;
    CHECK(pixelica_particles_init(&e, g_big_a, PIXELICA_PARTICLES_MAX_CAPACITY, &c, 3u) ==
          PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, PIXELICA_PARTICLES_MAX_CAPACITY, &spawned) == PIXELICA_OK);
    CHECK_EQ_LONG(spawned, PIXELICA_PARTICLES_MAX_CAPACITY);
    for (i = 0; i < e.count; ++i) {
        const PIXELICA_Particle *p = &e.particles[i];

        inside = inside && vec_eq(p->position, c.position) &&
            p->velocity.x >= -2.0f && p->velocity.x <= 2.0f &&
            p->velocity.y >= 10.0f && p->velocity.y <= 20.0f && p->velocity.z == 7.0f &&
            p->lifetime >= 0.5f && p->lifetime <= 2.0f &&
            p->rotation >= -3.0f && p->rotation <= 3.0f && p->age == 0.0f &&
            p->size == 6.0f && p->color == 0x1234u && p->opacity == 99u;
        lo_vx = p->velocity.x < lo_vx ? p->velocity.x : lo_vx;
        hi_vx = p->velocity.x > hi_vx ? p->velocity.x : hi_vx;
        lo_life = p->lifetime < lo_life ? p->lifetime : lo_life;
        hi_life = p->lifetime > hi_life ? p->lifetime : hi_life;
        lo_rot = p->rotation < lo_rot ? p->rotation : lo_rot;
        hi_rot = p->rotation > hi_rot ? p->rotation : hi_rot;
    }
    CHECK(inside);
    /* 4096 uniform draws cover the range (fixed seed, so not flaky). */
    CHECK(lo_vx < -1.9f && hi_vx > 1.9f);
    CHECK(lo_life < 0.55f && hi_life > 1.95f);
    CHECK(lo_rot < -2.9f && hi_rot > 2.9f);
}

/* ---- saturation --------------------------------------------------------- */

static void test_saturation(void)
{
    PIXELICA_ParticleConfig c = fixed_config();
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[4];
    PIXELICA_Particle saved[4];
    uint32_t spawned = 0u;
    uint32_t i;

    c.velocity_min = v3(-1.0f, -1.0f, -1.0f);
    c.velocity_max = v3(1.0f, 1.0f, 1.0f);
    CHECK(pixelica_particles_init(&e, storage, 4u, &c, 9u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 10u, &spawned) == PIXELICA_OK);
    CHECK_EQ_LONG(spawned, 4);
    CHECK_EQ_LONG(e.count, 4);
    CHECK_EQ_LONG(e.dropped, 6);

    /* A full pool never overwrites live particles. */
    memcpy(saved, storage, sizeof saved);
    spawned = 55u;
    CHECK(pixelica_particles_burst(&e, 3u, &spawned) == PIXELICA_OK);
    CHECK_EQ_LONG(spawned, 0);
    CHECK_EQ_LONG(e.dropped, 9);
    for (i = 0; i < 4u; ++i) {
        CHECK(same_particle(&storage[i], &saved[i]));
    }
    spawned = 55u;
    CHECK(pixelica_particles_burst(&e, 0u, &spawned) == PIXELICA_OK);
    CHECK_EQ_LONG(spawned, 0);
    CHECK_EQ_LONG(e.dropped, 9);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(e.dropped, 10);

    /* Partial fit. */
    CHECK(pixelica_particles_init(&e, storage, 4u, &c, 9u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 3u, &spawned) == PIXELICA_OK);
    CHECK_EQ_LONG(spawned, 3);
    CHECK(pixelica_particles_burst(&e, 3u, &spawned) == PIXELICA_OK);
    CHECK_EQ_LONG(spawned, 1);
    CHECK_EQ_LONG(e.count, 4);
    CHECK_EQ_LONG(e.dropped, 2);

    /* dropped saturates at UINT32_MAX. */
    CHECK(pixelica_particles_init(&e, storage, 1u, &c, 9u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, UINT32_MAX, &spawned) == PIXELICA_OK);
    CHECK_EQ_LONG(spawned, 1);
    CHECK(e.dropped == UINT32_MAX - 1u);
    CHECK(pixelica_particles_burst(&e, 5u, &spawned) == PIXELICA_OK);
    CHECK(e.dropped == UINT32_MAX);
    CHECK(pixelica_particles_burst(&e, UINT32_MAX, &spawned) == PIXELICA_OK);
    CHECK(e.dropped == UINT32_MAX);
    CHECK_EQ_LONG(e.count, 1);

    /* Continuous emission also drops into the counter. */
    c.rate = PIXELICA_PARTICLES_MAX_RATE;
    CHECK(pixelica_particles_init(&e, g_big_a, PIXELICA_PARTICLES_MAX_CAPACITY, &c, 9u) ==
          PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, PIXELICA_PARTICLES_MAX_CAPACITY);
    CHECK_EQ_LONG(e.dropped, 10000 - (long)PIXELICA_PARTICLES_MAX_CAPACITY);
    CHECK(pixelica_particles_update(&e, 0.5f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, PIXELICA_PARTICLES_MAX_CAPACITY);
    CHECK_EQ_LONG(e.dropped, 15000 - (long)PIXELICA_PARTICLES_MAX_CAPACITY);
}

/* ---- motion ------------------------------------------------------------- */

static void test_gravity_exact(void)
{
    PIXELICA_ParticleConfig c = fixed_config();
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];
    const PIXELICA_Particle *p;
    float t;
    int i;

    /* Dyadic values: analytic integration is bit-exact for dt = 0.25. */
    c.position = v3(1.0f, 2.0f, 3.0f);
    c.velocity_min = v3(2.0f, 4.0f, -8.0f);
    c.velocity_max = c.velocity_min;
    c.gravity = v3(1.0f, -16.0f, 2.0f);
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    p = &e.particles[0];
    for (i = 1; i <= 8; ++i) {
        CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
        t = 0.25f * (float)i;
        CHECK(p->age == t);
        CHECK(p->position.x == 1.0f + 2.0f * t + 0.5f * 1.0f * t * t);
        CHECK(p->position.y == 2.0f + 4.0f * t - 0.5f * 16.0f * t * t);
        CHECK(p->position.z == 3.0f - 8.0f * t + 0.5f * 2.0f * t * t);
        CHECK(p->velocity.x == 2.0f + 1.0f * t);
        CHECK(p->velocity.y == 4.0f - 16.0f * t);
        CHECK(p->velocity.z == -8.0f + 2.0f * t);
    }
    /* t = 2: closed-form values spelled out. */
    CHECK(vec_eq(p->position, v3(7.0f, -22.0f, -9.0f)));
    CHECK(vec_eq(p->velocity, v3(4.0f, -28.0f, -4.0f)));

    /* Same end time with a different dt split lands on the same point. */
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.5f) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.5f) == PIXELICA_OK);
    CHECK(vec_eq(e.particles[0].position, v3(7.0f, -22.0f, -9.0f)));
    CHECK(vec_eq(e.particles[0].velocity, v3(4.0f, -28.0f, -4.0f)));

    /* Non-dyadic dt: matches the closed form; (semi-)Euler would be off by
     * about g*dt*t/2 = 0.83 here. */
    c.position = v3(0.0f, 0.0f, 0.0f);
    c.velocity_min = v3(10.0f, 20.0f, 0.0f);
    c.velocity_max = c.velocity_min;
    c.gravity = v3(0.0f, -100.0f, 0.0f);
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    for (i = 0; i < 60; ++i) {
        CHECK(pixelica_particles_update(&e, 1.0f / 60.0f) == PIXELICA_OK);
    }
    t = e.particles[0].age;
    CHECK_NEAR(t, 1.0, 1e-5);
    CHECK_NEAR(e.particles[0].position.x, 10.0 * t, 1e-3);
    CHECK_NEAR(e.particles[0].position.y, 20.0 * t - 50.0 * t * t, 1e-3);
    CHECK_NEAR(e.particles[0].velocity.y, 20.0 - 100.0 * t, 1e-3);
    CHECK(e.particles[0].position.z == 0.0f);

    /* Live particles keep moving after the emitter moves. */
    CHECK(pixelica_particles_move(&e, v3(500.0f, 500.0f, 500.0f)) == PIXELICA_OK);
    CHECK_NEAR(e.particles[0].position.x, 10.0 * t, 1e-3);
}

/* ---- expiration --------------------------------------------------------- */

static void test_expiration(void)
{
    PIXELICA_ParticleConfig c = fixed_config();
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];
    float lifetimes[CAP];
    bool used[CAP];
    float elapsed = 0.0f;
    uint32_t i;
    uint32_t j;
    int step;

    /* Exactly lifetime seconds -> removed (age >= lifetime). */
    c.lifetime_min = 1.0f;
    c.lifetime_max = 1.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.5f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 1);
    CHECK(e.particles[0].age == 0.5f);
    CHECK(pixelica_particles_update(&e, 0.5f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 0);

    /* Whole pool expiring in one tick. */
    CHECK(pixelica_particles_burst(&e, CAP, NULL) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 0);

    /* Random lifetimes: swap removal keeps exactly the survivors, packed. */
    c.lifetime_min = 0.5f;
    c.lifetime_max = 2.0f;
    c.velocity_min = v3(-1.0f, -1.0f, -1.0f);
    c.velocity_max = v3(1.0f, 1.0f, 1.0f);
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 77u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, CAP, NULL) == PIXELICA_OK);
    for (i = 0; i < CAP; ++i) {
        lifetimes[i] = storage[i].lifetime;
    }
    for (step = 0; step < 9; ++step) {
        uint32_t expected = 0u;
        uint32_t matched = 0u;
        bool ages_ok = true;

        CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
        elapsed += 0.25f;
        for (i = 0; i < CAP; ++i) {
            used[i] = false;
            if (lifetimes[i] > elapsed) {
                ++expected;
            }
        }
        CHECK_EQ_LONG(e.count, expected);
        for (i = 0; i < e.count; ++i) {
            const PIXELICA_Particle *p = &e.particles[i];

            ages_ok = ages_ok && p->age == elapsed && p->age < p->lifetime;
            for (j = 0; j < CAP; ++j) {
                if (!used[j] && lifetimes[j] == p->lifetime) {
                    used[j] = true;
                    ++matched;
                    break;
                }
            }
        }
        CHECK(ages_ok);
        CHECK_EQ_LONG(matched, e.count);
    }
    CHECK_EQ_LONG(e.count, 0); /* elapsed 2.25 > lifetime_max */
}

/* ---- emission ----------------------------------------------------------- */

static void test_emission(void)
{
    PIXELICA_ParticleConfig c = fixed_config();
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];
    int i;

    /* rate 6, dt 0.25 -> 1.5 per tick: births 1, 2, 1, 2. */
    c.rate = 6.0f;
    c.position = v3(5.0f, 6.0f, 7.0f);
    c.velocity_min = v3(1.0f, 0.0f, 0.0f);
    c.velocity_max = c.velocity_min;
    c.size_start = 3.0f;
    c.size_end = 9.0f;
    c.color_start = 0x07E0u;
    c.opacity_start = 10u;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 1);
    /* Born at the END of the tick: not integrated yet, start style. */
    CHECK(e.particles[0].age == 0.0f);
    CHECK(vec_eq(e.particles[0].position, v3(5.0f, 6.0f, 7.0f)));
    CHECK(e.particles[0].size == 3.0f);
    CHECK_EQ_LONG(e.particles[0].color, 0x07E0);
    CHECK_EQ_LONG(e.particles[0].opacity, 10);
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 3);
    CHECK(e.particles[0].age == 0.25f);
    CHECK(e.particles[0].position.x == 5.25f);
    CHECK(e.particles[1].age == 0.0f && e.particles[2].age == 0.0f);
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 4);
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 6);
    CHECK_EQ_LONG(e.dropped, 0);

    /* emitting = false keeps (does not advance) the carried fraction. */
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK); /* 1, carry 0.5 */
    e.emitting = false;
    for (i = 0; i < 3; ++i) {
        CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
    }
    CHECK_EQ_LONG(e.count, 1);
    e.emitting = true;
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK); /* 0.5 + 1.5 */
    CHECK_EQ_LONG(e.count, 3);

    /* dt = 0 keeps the carry and births nothing. */
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK); /* carry 0.5 */
    CHECK(pixelica_particles_update(&e, 0.0f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 1);
    CHECK(pixelica_particles_update(&e, 0.125f) == PIXELICA_OK); /* 0.5 + 0.75 */
    CHECK_EQ_LONG(e.count, 2);

    /* Fractions carry across many small ticks: 0.1f * 3 per tick. */
    c.rate = 3.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    for (i = 0; i < 3; ++i) {
        CHECK(pixelica_particles_update(&e, 0.1f) == PIXELICA_OK);
    }
    CHECK_EQ_LONG(e.count, 0); /* 0.9 */
    CHECK(pixelica_particles_update(&e, 0.1f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 1); /* 1.2 */
    for (i = 0; i < 6; ++i) {
        CHECK(pixelica_particles_update(&e, 0.1f) == PIXELICA_OK);
    }
    CHECK_EQ_LONG(e.count, 3); /* 3.0000000447 after 10 ticks */
    c.rate = 10.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    for (i = 0; i < 6; ++i) {
        CHECK(pixelica_particles_update(&e, 0.1f) == PIXELICA_OK);
        CHECK_EQ_LONG(e.count, i + 1);
    }

    /* Rate 0 never emits. */
    c.rate = 0.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 0);

    /* Expired particles free their slots BEFORE the end-of-tick births. */
    c.rate = 8.0f;
    c.lifetime_min = 0.25f;
    c.lifetime_max = 0.25f;
    CHECK(pixelica_particles_init(&e, storage, 2u, &c, 1u) == PIXELICA_OK);
    e.emitting = false;
    CHECK(pixelica_particles_burst(&e, 2u, NULL) == PIXELICA_OK);
    e.emitting = true;
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 2);
    CHECK_EQ_LONG(e.dropped, 0);
    CHECK(e.particles[0].age == 0.0f && e.particles[1].age == 0.0f);
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 2);
    CHECK_EQ_LONG(e.dropped, 0);

    /* Burst ignores `emitting` and rate. */
    c.rate = 0.0f;
    c.lifetime_min = 100.0f;
    c.lifetime_max = 100.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    e.emitting = false;
    CHECK(pixelica_particles_burst(&e, 4u, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 4);
}

/* ---- interpolation ------------------------------------------------------ */

static void test_interpolation(void)
{
    PIXELICA_ParticleConfig c = fixed_config();
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];
    const PIXELICA_Particle *p;

    c.lifetime_min = 4.0f;
    c.lifetime_max = 4.0f;
    c.size_start = 2.0f;
    c.size_end = 10.0f;
    c.rotation_min = 1.0f;
    c.rotation_max = 1.0f;
    c.angular_velocity = 2.0f;
    c.color_start = 0x0000u;
    c.color_end = 0xFFFFu;
    c.opacity_start = 0u;
    c.opacity_end = 255u;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    p = &e.particles[0];
    CHECK(p->size == 2.0f);
    CHECK(p->rotation == 1.0f);
    CHECK_EQ_LONG(p->color, 0x0000);
    CHECK_EQ_LONG(p->opacity, 0);

    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK); /* t = 0.25 */
    CHECK(p->size == 4.0f);
    CHECK(p->rotation == 3.0f);
    CHECK_EQ_LONG(p->color, (8 << 11) | (16 << 5) | 8); /* 7.75, 15.75, 7.75 */
    CHECK_EQ_LONG(p->opacity, 64);                       /* 63.75 */
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK); /* t = 0.5 */
    CHECK(p->size == 6.0f);
    CHECK(p->rotation == 5.0f);
    CHECK_EQ_LONG(p->color, (16 << 11) | (32 << 5) | 16); /* halves round up */
    CHECK_EQ_LONG(p->opacity, 128);
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK); /* t = 0.75 */
    CHECK(p->size == 8.0f);
    CHECK_EQ_LONG(p->color, (23 << 11) | (47 << 5) | 23);
    CHECK_EQ_LONG(p->opacity, 191);
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK); /* t = 1: expired */
    CHECK_EQ_LONG(e.count, 0);

    /* Decreasing, per-channel independent: red -> blue, opaque -> clear. */
    c.color_start = 0xF800u;
    c.color_end = 0x001Fu;
    c.opacity_start = 255u;
    c.opacity_end = 0u;
    c.size_start = 10.0f;
    c.size_end = 0.0f;
    c.angular_velocity = -1.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK); /* t = 0.25 */
    CHECK_EQ_LONG(p->color, (23 << 11) | 8); /* 23.25, 0, 7.75 */
    CHECK_EQ_LONG(p->opacity, 191);          /* 191.25 */
    CHECK(p->size == 7.5f);
    CHECK(p->rotation == 0.0f);
    CHECK(pixelica_particles_update(&e, 1.0f) == PIXELICA_OK); /* t = 0.5 */
    CHECK_EQ_LONG(p->color, (16 << 11) | 16);
    CHECK_EQ_LONG(p->opacity, 128);
    CHECK(p->size == 5.0f);
    CHECK(p->rotation == -1.0f);

    /* Constant style stays exact. */
    c.color_start = 0x1234u;
    c.color_end = 0x1234u;
    c.opacity_start = 77u;
    c.opacity_end = 77u;
    c.size_start = 3.0f;
    c.size_end = 3.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.3f) == PIXELICA_OK);
    CHECK_EQ_LONG(p->color, 0x1234);
    CHECK_EQ_LONG(p->opacity, 77);
    CHECK(p->size == 3.0f);
}

/* ---- move / clear ------------------------------------------------------- */

static void test_move_clear(void)
{
    static Snapshot s;
    PIXELICA_ParticleConfig c = fixed_config();
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle storage[CAP];

    memset(storage, 0, sizeof storage);
    c.rate = 6.0f;
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    CHECK(pixelica_particles_move(&e, v3(1.0f, 2.0f, 3.0f)) == PIXELICA_OK);
    CHECK(vec_eq(e.config.position, v3(1.0f, 2.0f, 3.0f)));
    CHECK(vec_eq(e.particles[0].position, v3(0.0f, 0.0f, 0.0f))); /* not moved */
    CHECK(pixelica_particles_burst(&e, 1u, NULL) == PIXELICA_OK);
    CHECK(vec_eq(e.particles[1].position, v3(1.0f, 2.0f, 3.0f)));

    CHECK(pixelica_particles_move(&e, v3(1.0e6f, -1.0e6f, 0.0f)) == PIXELICA_OK);
    snap(&s, &e, storage);
    CHECK(pixelica_particles_move(&e, v3(pixelica_t_nan(), 0.0f, 0.0f)) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_move(&e, v3(0.0f, pixelica_t_inf(), 0.0f)) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_move(&e, v3(0.0f, 0.0f, -1.5e6f)) == PIXELICA_ERR_INVALID_ARG);
    CHECK(snap_same(&s, &e, storage));

    /* clear: no particles, no carried fraction; dropped and RNG kept. */
    CHECK(pixelica_particles_init(&e, storage, 1u, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&e, 3u, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(e.dropped, 2);
    CHECK(pixelica_particles_init(&e, storage, CAP, &c, 1u) == PIXELICA_OK);
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK); /* 1, carry 0.5 */
    CHECK_EQ_LONG(e.count, 1);
    e.dropped = 5u;
    e.emitting = false;
    CHECK(pixelica_particles_clear(&e) == PIXELICA_OK);
    CHECK_EQ_LONG(e.count, 0);
    CHECK_EQ_LONG(e.dropped, 5);
    CHECK(e.emitting == false);
    e.emitting = true;
    CHECK(pixelica_particles_update(&e, 0.25f) == PIXELICA_OK); /* 0 + 1.5 */
    CHECK_EQ_LONG(e.count, 1);
}

int main(void)
{
    test_default_config();
    test_presets();
    test_init_validation();
    test_uninitialized();
    test_update_dt();
    test_determinism();
    test_ranges();
    test_saturation();
    test_gravity_exact();
    test_expiration();
    test_emission();
    test_interpolation();
    test_move_clear();
    return pixelica_t_finish("test_particles");
}

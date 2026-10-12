/*
 * Pixelica - optional particle simulation core. See pixelica/particles.h for
 * the contract. Allocation-free and renderer-independent: particles live in a
 * caller pool, randomness comes from a per-emitter splitmix64 generator, motion
 * under constant gravity is integrated analytically per tick, and emission
 * carries its fractional births in double precision.
 */
#include "pixelica/particles.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Also false for NaN and infinities. */
static bool pt_in_range(float v, float lo, float hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

static bool pt_vec_valid(PIXELICA_Vec3 v)
{
    const float m = PIXELICA_PARTICLES_MAX_VALUE;

    return pt_in_range(v.x, -m, m) && pt_in_range(v.y, -m, m) && pt_in_range(v.z, -m, m);
}

static bool pt_config_valid(const PIXELICA_ParticleConfig *c)
{
    const float m = PIXELICA_PARTICLES_MAX_VALUE;

    if (!pt_vec_valid(c->position) || !pt_vec_valid(c->velocity_min) ||
        !pt_vec_valid(c->velocity_max) || !pt_vec_valid(c->gravity)) {
        return false;
    }
    if (!(c->velocity_min.x <= c->velocity_max.x) || !(c->velocity_min.y <= c->velocity_max.y) ||
        !(c->velocity_min.z <= c->velocity_max.z)) {
        return false;
    }
    if (!pt_in_range(c->rate, 0.0f, PIXELICA_PARTICLES_MAX_RATE)) {
        return false;
    }
    if (!pt_in_range(c->lifetime_min, 0.0f, PIXELICA_PARTICLES_MAX_LIFETIME) ||
        !(c->lifetime_min > 0.0f) ||
        !pt_in_range(c->lifetime_max, c->lifetime_min, PIXELICA_PARTICLES_MAX_LIFETIME)) {
        return false;
    }
    if (!pt_in_range(c->size_start, 0.0f, m) || !pt_in_range(c->size_end, 0.0f, m)) {
        return false;
    }
    return pt_in_range(c->rotation_min, -m, m) && pt_in_range(c->rotation_max, c->rotation_min, m) &&
        pt_in_range(c->angular_velocity, -m, m);
}

static bool pt_capacity_valid(uint32_t capacity)
{
    return capacity >= PIXELICA_PARTICLES_MIN_CAPACITY &&
        capacity <= PIXELICA_PARTICLES_MAX_CAPACITY;
}

static bool pt_overlap(const void *a, size_t a_bytes, const void *b, size_t b_bytes)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (a_bytes > UINTPTR_MAX - x || b_bytes > UINTPTR_MAX - y) return true;
    return x < y + b_bytes && y < x + a_bytes;
}

/* Rejects NULL, zero-filled and corrupted emitters before anything is touched. */
static bool pt_ready(const PIXELICA_ParticleEmitter *e)
{
    return e != NULL && e->particles != NULL && pt_capacity_valid(e->capacity) &&
        e->count <= e->capacity && pt_config_valid(&e->config) && isfinite(e->emit_carry) &&
        e->emit_carry >= 0.0 && e->emit_carry < 1.0;
}

/* splitmix64: every state (including 0) is valid, so any seed works. */
static uint64_t pt_next(uint64_t *state)
{
    uint64_t z;

    *state += 0x9E3779B97F4A7C15ull;
    z = *state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Uniform in [0, 1) with 24 bits, exactly representable as float. */
static float pt_unit(uint64_t *state)
{
    return (float)(uint32_t)(pt_next(state) >> 40) * (1.0f / 16777216.0f);
}

/* Always consumes one draw, so degenerate ranges keep the sequence aligned. */
static float pt_range(uint64_t *state, float lo, float hi)
{
    float u = pt_unit(state);
    float v;

    if (!(hi > lo)) {
        return lo;
    }
    v = lo + (hi - lo) * u;
    if (v < lo) {
        return lo;
    }
    return v > hi ? hi : v;
}

/* Rounded lerp of an integer channel in [0, max_value]. */
static unsigned pt_lerp_channel(unsigned a, unsigned b, float t, unsigned max_value)
{
    float v = (float)a + ((float)b - (float)a) * t;

    if (!(v > 0.0f)) {
        return 0u;
    }
    if (v >= (float)max_value) {
        return max_value;
    }
    return (unsigned)(v + 0.5f);
}

static uint16_t pt_lerp_565(uint16_t a, uint16_t b, float t)
{
    unsigned r = pt_lerp_channel((unsigned)(a >> 11) & 31u, (unsigned)(b >> 11) & 31u, t, 31u);
    unsigned g = pt_lerp_channel((unsigned)(a >> 5) & 63u, (unsigned)(b >> 5) & 63u, t, 63u);
    unsigned bl = pt_lerp_channel((unsigned)a & 31u, (unsigned)b & 31u, t, 31u);

    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* size/color/opacity at t = age / lifetime. */
static void pt_style(PIXELICA_Particle *p, const PIXELICA_ParticleConfig *c)
{
    float t = p->age / p->lifetime;

    if (!(t > 0.0f)) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    p->size = c->size_start + (c->size_end - c->size_start) * t;
    p->color = pt_lerp_565(c->color_start, c->color_end, t);
    p->opacity = (uint8_t)pt_lerp_channel(c->opacity_start, c->opacity_end, t, 255u);
}

static void pt_add_dropped(PIXELICA_ParticleEmitter *e, uint32_t n)
{
    e->dropped = n > UINT32_MAX - e->dropped ? UINT32_MAX : e->dropped + n;
}

/* Spawns min(n, free slots) newborns; the rest is counted as dropped. */
static uint32_t pt_spawn(PIXELICA_ParticleEmitter *e, uint32_t n)
{
    const PIXELICA_ParticleConfig *c = &e->config;
    uint32_t space = e->capacity - e->count;
    uint32_t spawned = n < space ? n : space;
    uint32_t i;

    for (i = 0; i < spawned; ++i) {
        PIXELICA_Particle *p = &e->particles[e->count];

        memset(p, 0, sizeof *p);
        p->position = c->position;
        /* Fixed draw order: vx, vy, vz, lifetime, rotation. */
        p->velocity.x = pt_range(&e->rng_state, c->velocity_min.x, c->velocity_max.x);
        p->velocity.y = pt_range(&e->rng_state, c->velocity_min.y, c->velocity_max.y);
        p->velocity.z = pt_range(&e->rng_state, c->velocity_min.z, c->velocity_max.z);
        p->lifetime = pt_range(&e->rng_state, c->lifetime_min, c->lifetime_max);
        p->rotation = pt_range(&e->rng_state, c->rotation_min, c->rotation_max);
        p->age = 0.0f;
        p->size = c->size_start;
        p->color = c->color_start;
        p->opacity = c->opacity_start;
        e->count++;
    }
    pt_add_dropped(e, n - spawned);
    return spawned;
}

void pixelica_particles_config_default(PIXELICA_ParticleConfig *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->rate = 10.0f;
    out->lifetime_min = 1.0f;
    out->lifetime_max = 1.0f;
    out->size_start = 1.0f;
    out->size_end = 1.0f;
    out->color_start = 0xFFFFu;
    out->color_end = 0xFFFFu;
    out->opacity_start = 255u;
    out->opacity_end = 255u;
}

static void pt_vec(PIXELICA_Vec3 *v, float x, float y, float z)
{
    v->x = x;
    v->y = y;
    v->z = z;
}

PIXELICA_Status pixelica_particles_preset(PIXELICA_ParticlePreset preset, bool y_down,
    PIXELICA_ParticleConfig *out)
{
    PIXELICA_ParticleConfig c;
    float lo;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    pixelica_particles_config_default(&c);
    switch (preset) {
    case PIXELICA_PARTICLES_SPARKS:
        pt_vec(&c.velocity_min, -80.0f, 20.0f, -80.0f);
        pt_vec(&c.velocity_max, 80.0f, 120.0f, 80.0f);
        pt_vec(&c.gravity, 0.0f, -240.0f, 0.0f);
        c.rate = 60.0f;
        c.lifetime_min = 0.4f;
        c.lifetime_max = 0.9f;
        c.size_start = 2.0f;
        c.size_end = 1.0f;
        c.color_start = 0xFFE0u;
        c.color_end = 0xF800u;
        c.opacity_start = 255u;
        c.opacity_end = 0u;
        break;
    case PIXELICA_PARTICLES_SMOKE:
        pt_vec(&c.velocity_min, -10.0f, 15.0f, -10.0f);
        pt_vec(&c.velocity_max, 10.0f, 35.0f, 10.0f);
        pt_vec(&c.gravity, 0.0f, 6.0f, 0.0f);
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
        break;
    case PIXELICA_PARTICLES_RAIN:
        pt_vec(&c.velocity_min, -30.0f, -260.0f, -30.0f);
        pt_vec(&c.velocity_max, 30.0f, -200.0f, 30.0f);
        pt_vec(&c.gravity, 0.0f, -60.0f, 0.0f);
        c.rate = 120.0f;
        c.lifetime_min = 0.6f;
        c.lifetime_max = 1.0f;
        c.size_start = 1.0f;
        c.size_end = 1.0f;
        c.color_start = 0x9DFFu;
        c.color_end = 0x5C1Fu;
        c.opacity_start = 200u;
        c.opacity_end = 120u;
        break;
    default:
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (y_down) {
        lo = c.velocity_min.y;
        c.velocity_min.y = -c.velocity_max.y;
        c.velocity_max.y = -lo;
        c.gravity.y = -c.gravity.y;
    }
    *out = c;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_particles_init(PIXELICA_ParticleEmitter *out,
    PIXELICA_Particle *storage, uint32_t capacity, const PIXELICA_ParticleConfig *config,
    uint32_t seed)
{
    PIXELICA_ParticleConfig copy;

    if (out == NULL || storage == NULL || config == NULL || !pt_capacity_valid(capacity) ||
        !pt_config_valid(config) ||
        pt_overlap(out, sizeof *out, storage, (size_t)capacity * sizeof *storage)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    copy = *config; /* config may alias out->config on re-init */
    /* init accepts fresh stack storage: never read its previous contents. */
    memset(out, 0, sizeof *out);
    out->particles = storage;
    out->capacity = capacity;
    out->count = 0u;
    out->config = copy;
    out->emitting = true;
    out->dropped = 0u;
    out->rng_state = (uint64_t)seed;
    out->emit_carry = 0.0;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_particles_move(PIXELICA_ParticleEmitter *emitter,
    PIXELICA_Vec3 position)
{
    if (!pt_ready(emitter) || !pt_vec_valid(position)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    emitter->config.position = position;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_particles_burst(PIXELICA_ParticleEmitter *emitter, uint32_t requested,
    uint32_t *spawned)
{
    uint32_t n;

    if (!pt_ready(emitter)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    n = pt_spawn(emitter, requested);
    if (spawned != NULL) {
        *spawned = n;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_particles_update(PIXELICA_ParticleEmitter *emitter, float dt)
{
    const PIXELICA_ParticleConfig *c;
    float half_dt2;
    uint32_t i;

    if (!pt_ready(emitter) || !pt_in_range(dt, 0.0f, PIXELICA_PARTICLES_MAX_DT)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    c = &emitter->config;
    half_dt2 = 0.5f * dt * dt;

    i = 0u;
    while (i < emitter->count) {
        PIXELICA_Particle *p = &emitter->particles[i];
        float age = p->age + dt;

        /* Expired (or corrupted): swap the last live particle into slot i and
         * process it in this same iteration. */
        if (!(age < p->lifetime)) {
            emitter->count--;
            if (i != emitter->count) {
                *p = emitter->particles[emitter->count];
            }
            continue;
        }
        /* Exact for constant acceleration: x += v dt + g dt^2 / 2, v += g dt. */
        p->position.x += p->velocity.x * dt + c->gravity.x * half_dt2;
        p->position.y += p->velocity.y * dt + c->gravity.y * half_dt2;
        p->position.z += p->velocity.z * dt + c->gravity.z * half_dt2;
        p->velocity.x += c->gravity.x * dt;
        p->velocity.y += c->gravity.y * dt;
        p->velocity.z += c->gravity.z * dt;
        p->rotation += c->angular_velocity * dt;
        p->age = age;
        pt_style(p, c);
        ++i;
    }

    if (emitter->emitting) {
        /* <= 1 + MAX_RATE * MAX_DT births, so the uint32_t cast is safe. */
        double total = emitter->emit_carry + (double)c->rate * (double)dt;
        double whole = floor(total);

        emitter->emit_carry = total - whole;
        (void)pt_spawn(emitter, (uint32_t)whole);
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_particles_clear(PIXELICA_ParticleEmitter *emitter)
{
    if (!pt_ready(emitter)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    emitter->count = 0u;
    emitter->emit_carry = 0.0;
    return PIXELICA_OK;
}

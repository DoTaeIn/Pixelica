/*
 * Pixelica - optional, allocation-free particle simulation (portable core).
 *
 * Renderer-independent: this header needs only the common library
 * (pixelica/status.h, pixelica/math.h). Drawing lives in the separate
 * particles2d / particles25d adapter components.
 *
 * Model: one emitter owns a caller-supplied fixed pool of particles. Live
 * particles are packed in particles[0 .. count). Expired particles are removed
 * by swap removal (the last live particle moves into the hole), so the order
 * of live particles is not stable. A full pool never overwrites live
 * particles: extra births are dropped and counted in `dropped`.
 *
 * Units are whatever the caller's world uses (2D world pixels or 3D world
 * units); time is seconds. Coordinates are plain XYZ: 2D adapters use X/Y only.
 *
 * Determinism: the emitter carries its own seeded generator (no global rand).
 * The same seed, config and sequence of calls with the same dt values gives
 * identical results with the same compiler, flags and platform. Emission is
 * tick-based (see pixelica_particles_update), so it is NOT frame-rate
 * independent: replaying with different dt splits gives different particles.
 *
 * Single-threaded: an emitter must not be used from two threads at once.
 */
#ifndef PIXELICA_PARTICLES_H
#define PIXELICA_PARTICLES_H

#include <stdbool.h>
#include <stdint.h>
#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_PARTICLES_MIN_CAPACITY 1u
#define PIXELICA_PARTICLES_MAX_CAPACITY 4096u
/* Bound on |x|,|y|,|z| of position, velocity_min/max and gravity, on
 * |rotation_min|, |rotation_max|, |angular_velocity| and on size_start/end. */
#define PIXELICA_PARTICLES_MAX_VALUE 1.0e6f
#define PIXELICA_PARTICLES_MAX_LIFETIME 3600.0f /* seconds */
#define PIXELICA_PARTICLES_MAX_RATE 10000.0f    /* particles per second */
#define PIXELICA_PARTICLES_MAX_DT 1.0f          /* seconds per update call */

/* One live particle. Read-only for callers (renderers read it). */
typedef struct PIXELICA_Particle {
    PIXELICA_Vec3 position;
    PIXELICA_Vec3 velocity;
    float age;      /* seconds since birth, always < lifetime while live */
    float lifetime; /* seconds, > 0 */
    float size;     /* lerp(size_start, size_end, age / lifetime) */
    float rotation; /* radians, birth value + angular_velocity * age */
    uint16_t color; /* RGB565, per-channel lerp(color_start, color_end) */
    uint8_t opacity; /* lerp(opacity_start, opacity_end), 255 = opaque */
} PIXELICA_Particle;

/*
 * Emitter configuration. Valid when every float is finite and:
 *   - |position|, |velocity_min/max|, |gravity| components <= MAX_VALUE,
 *     velocity_min.c <= velocity_max.c for each component c;
 *   - 0 <= rate <= MAX_RATE (particles per second while emitting);
 *   - 0 < lifetime_min <= lifetime_max <= MAX_LIFETIME;
 *   - 0 <= size_start, size_end <= MAX_VALUE;
 *   - rotation_min <= rotation_max, |rotation_*|, |angular_velocity| <= MAX_VALUE.
 * Each birth draws velocity (per component), lifetime and rotation uniformly
 * from [min, max]; equal bounds give exactly that value. Births start at
 * `position` with size_start / color_start / opacity_start.
 */
typedef struct PIXELICA_ParticleConfig {
    PIXELICA_Vec3 position;
    PIXELICA_Vec3 velocity_min;
    PIXELICA_Vec3 velocity_max;
    PIXELICA_Vec3 gravity; /* constant acceleration, units / s^2 */
    float rate;
    float lifetime_min;
    float lifetime_max;
    float size_start;
    float size_end;
    float rotation_min;
    float rotation_max;
    float angular_velocity; /* radians / s, shared by all particles */
    uint16_t color_start;   /* RGB565 */
    uint16_t color_end;
    uint8_t opacity_start;
    uint8_t opacity_end;
} PIXELICA_ParticleConfig;

typedef enum PIXELICA_ParticlePreset {
    PIXELICA_PARTICLES_SPARKS = 0,
    PIXELICA_PARTICLES_SMOKE = 1,
    PIXELICA_PARTICLES_RAIN = 2
} PIXELICA_ParticlePreset;

/*
 * Caller-owned emitter; call pixelica_particles_init before use. Public fields are
 * READ-ONLY except `emitting`, which may be toggled directly at any time.
 * `config` is fixed after init except config.position, changed only through
 * pixelica_particles_move. Fields after `dropped` are private.
 */
typedef struct PIXELICA_ParticleEmitter {
    PIXELICA_Particle *particles; /* caller storage, live particles [0, count) */
    uint32_t capacity;
    uint32_t count;
    PIXELICA_ParticleConfig config;
    bool emitting;    /* continuous emission on/off (true after init) */
    uint32_t dropped; /* births lost to a full pool, saturates at UINT32_MAX */
    /* private */
    uint64_t rng_state;
    double emit_carry; /* fractional births carried to the next update, [0,1) */
} PIXELICA_ParticleEmitter;

/*
 * Neutral config: position/velocities/gravity 0, rate 10, lifetime 1..1,
 * size 1 -> 1, rotation 0..0, angular_velocity 0, color 0xFFFF -> 0xFFFF,
 * opacity 255 -> 255. NULL does nothing.
 */
void pixelica_particles_config_default(PIXELICA_ParticleConfig *out);

/*
 * Ready-made looks tuned for 2D world-pixel scale (scale velocity, gravity and
 * size for 3D worlds). Values below are for y_down = false (+Y up, the 3D
 * convention); y_down = true (2D screen convention) mirrors Y: the velocity
 * Y range becomes [-max.y, -min.y] and gravity.y is negated. Position is 0.
 *
 *   SPARKS: velocity (-80,20,-80)..(80,120,80), gravity (0,-240,0), rate 60,
 *           lifetime 0.4..0.9, size 2 -> 1, rotation 0, color 0xFFE0 (yellow)
 *           -> 0xF800 (red), opacity 255 -> 0.
 *   SMOKE:  velocity (-10,15,-10)..(10,35,10), gravity (0,6,0), rate 12,
 *           lifetime 1.5..3, size 4 -> 14, rotation 0..6.2831855,
 *           angular_velocity 0.5, color 0x8410 (gray) -> 0xC618 (light gray),
 *           opacity 160 -> 0.
 *   RAIN:   velocity (-30,-260,-30)..(30,-200,30), gravity (0,-60,0),
 *           rate 120, lifetime 0.6..1, size 1 -> 1, rotation 0,
 *           color 0x9DFF -> 0x5C1F (blues), opacity 200 -> 120.
 *
 * Unknown preset or NULL out -> PIXELICA_ERR_INVALID_ARG, *out untouched.
 */
PIXELICA_Status pixelica_particles_preset(PIXELICA_ParticlePreset preset, bool y_down,
    PIXELICA_ParticleConfig *out);

/*
 * Binds `storage` (capacity elements, PIXELICA_PARTICLES_MIN_CAPACITY ..
 * PIXELICA_PARTICLES_MAX_CAPACITY) and copies a valid *config. Starts with
 * count 0, emitting true, dropped 0 and no carried emission. Any seed is
 * valid, including 0. `out` may be fresh, uninitialized storage; storage
 * contents are never read. On error returns PIXELICA_ERR_INVALID_ARG and
 * leaves *out untouched. The emitter and particle storage must be disjoint
 * (overlap is rejected). Keep both alive while using the emitter; neither is
 * owned or freed by the library.
 */
PIXELICA_Status pixelica_particles_init(PIXELICA_ParticleEmitter *out,
    PIXELICA_Particle *storage, uint32_t capacity, const PIXELICA_ParticleConfig *config,
    uint32_t seed);

/* Sets the birth position for future births; live particles do not move.
 * Components must be finite with |c| <= PIXELICA_PARTICLES_MAX_VALUE. */
PIXELICA_Status pixelica_particles_move(PIXELICA_ParticleEmitter *emitter,
    PIXELICA_Vec3 position);

/*
 * Immediately spawns up to `requested` particles at the emitter position
 * (independent of `emitting` and rate). Saturation is success: only free
 * slots are used, the rest is added to `dropped`. `spawned` may be NULL;
 * otherwise it receives the number actually spawned. On error nothing is
 * modified (including *spawned).
 */
PIXELICA_Status pixelica_particles_burst(PIXELICA_ParticleEmitter *emitter, uint32_t requested,
    uint32_t *spawned);

/*
 * Advances one tick of dt seconds, dt finite in [0, PIXELICA_PARTICLES_MAX_DT]
 * (split longer gaps into several calls). For each live particle:
 *   age += dt; age >= lifetime -> removed (swap removal);
 *   otherwise position += velocity*dt + gravity*dt*dt/2, velocity += gravity*dt,
 *   rotation += angular_velocity*dt, and size/color/opacity are re-interpolated
 *   at t = age / lifetime.
 * Then, at the END of the tick and only while `emitting`, rate*dt births (plus
 * the carried fraction) are spawned with age 0; the fraction below one carries
 * to the next update (it is kept, not advanced, while emitting is false).
 * Births that do not fit are added to `dropped`. Newborns are not integrated
 * in the tick that creates them. On error nothing is modified.
 */
PIXELICA_Status pixelica_particles_update(PIXELICA_ParticleEmitter *emitter, float dt);

/* Removes every live particle and the carried emission fraction. Keeps the
 * generator state, `dropped`, `emitting` and the config. */
PIXELICA_Status pixelica_particles_clear(PIXELICA_ParticleEmitter *emitter);

#ifdef __cplusplus
}
#endif
#endif

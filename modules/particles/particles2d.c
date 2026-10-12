/*
 * Pixelica - 2D particle adapter (see pixelica/particles2d.h).
 *
 * Every particle becomes one pixelica2d_draw_sprite call. All inputs are
 * validated first, so a rejected call writes nothing.
 */
#include "pixelica/particles2d.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* (a * b) / 255 rounded to nearest: the same 8-bit scale the renderer blends with. */
static uint8_t scale_opacity(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a * (uint32_t)b + 127u) / 255u);
}

static bool emitter_valid(const PIXELICA_ParticleEmitter *emitter)
{
    return emitter != NULL &&
           emitter->capacity >= PIXELICA_PARTICLES_MIN_CAPACITY &&
           emitter->capacity <= PIXELICA_PARTICLES_MAX_CAPACITY &&
           emitter->count <= emitter->capacity &&
           (emitter->count == 0u || emitter->particles != NULL);
}

static bool particle_valid(const PIXELICA_Particle *p)
{
    return isfinite(p->position.x) && isfinite(p->position.y) && isfinite(p->rotation) &&
           isfinite(p->size) && p->size >= 0.0f && p->size <= PIXELICA_PARTICLES_MAX_VALUE;
}

/* Mirrors what pixelica2d_draw_sprite accepts for the fields the adapter keeps. */
static bool appearance_valid(const PIXELICA2D_Sprite *a)
{
    const PIXELICA_Texture *t = a->texture;

    if (t == NULL || t->pixels == NULL || t->width == 0u || t->height == 0u ||
        (uint32_t)t->width > PIXELICA_MAX_TEXTURE_DIMENSION ||
        (uint32_t)t->height > PIXELICA_MAX_TEXTURE_DIMENSION) {
        return false;
    }
    if (a->source.x < 0 || a->source.y < 0 || a->source.width <= 0 || a->source.height <= 0 ||
        (int64_t)a->source.x + (int64_t)a->source.width > (int64_t)t->width ||
        (int64_t)a->source.y + (int64_t)a->source.height > (int64_t)t->height) {
        return false;
    }
    if (!isfinite(a->scale.x) || !isfinite(a->scale.y) || !(a->scale.x < 0.0f || a->scale.x > 0.0f) ||
        !(a->scale.y < 0.0f || a->scale.y > 0.0f) || !isfinite(a->pivot.x) || !isfinite(a->pivot.y) ||
        !isfinite(a->rotation)) {
        return false;
    }
    if ((a->depth_test || a->depth_write) && a->depth > PIXELICA2D_DEPTH_MAX) {
        return false;
    }
    return true;
}

PIXELICA_Status pixelica2d_particles_draw(PIXELICA2D_Context *ctx,
    const PIXELICA_ParticleEmitter *emitter, const PIXELICA2D_Sprite *appearance)
{
    uint16_t texel = 0u;
    const PIXELICA_Texture square = {&texel, 1u, 1u};
    PIXELICA2D_Sprite base;
    uint32_t i;

    if (ctx == NULL || !emitter_valid(emitter)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (appearance != NULL) {
        if (!appearance_valid(appearance) ||
            ((appearance->depth_test || appearance->depth_write) && ctx->surface.depth == NULL)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        base = *appearance;
    } else {
        PIXELICA_Status status = pixelica2d_sprite_init(&base, &square);

        if (status != PIXELICA_OK) {
            return status;
        }
        base.pivot.x = 0.5f;
        base.pivot.y = 0.5f;
    }
    for (i = 0u; i < emitter->count; ++i) {
        const PIXELICA_Particle *p = &emitter->particles[i];
        PIXELICA2D_Sprite probe = base;
        PIXELICA2D_Context check = *ctx;
        float sx, sy;
        if (!particle_valid(p)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        sx = p->size / (float)base.source.width;
        sy = p->size / (float)base.source.height;
        if (p->opacity == 0u || (appearance != NULL && base.opacity == 0u) ||
            !(sx > 0.0f) || !(sy > 0.0f)) continue;
        probe.position.x = p->position.x;
        probe.position.y = p->position.y;
        probe.scale.x = base.scale.x < 0.0f ? -sx : sx;
        probe.scale.y = base.scale.y < 0.0f ? -sy : sy;
        probe.rotation = base.rotation + p->rotation;
        /* Reuse the renderer's complete validation. Zero alpha writes no
         * pixels/depth, and only the temporary context receives statistics. */
        probe.opacity = 0u;
        if (pixelica2d_draw_sprite(&check, &probe) != PIXELICA_OK)
            return PIXELICA_ERR_INVALID_ARG;
    }

    for (i = 0u; i < emitter->count; ++i) {
        const PIXELICA_Particle *p = &emitter->particles[i];
        const uint8_t opacity = appearance != NULL ? scale_opacity(p->opacity, base.opacity) : p->opacity;
        PIXELICA2D_Sprite sprite = base;
        float sx;
        float sy;
        PIXELICA_Status status;

        sx = p->size / (float)base.source.width;
        sy = p->size / (float)base.source.height;
        /* Zero size, zero opacity or a scale that underflows to 0 is invisible. */
        if (opacity == 0u || !(sx > 0.0f) || !(sy > 0.0f)) {
            continue;
        }
        sprite.position.x = p->position.x;
        sprite.position.y = p->position.y;
        sprite.scale.x = base.scale.x < 0.0f ? -sx : sx;
        sprite.scale.y = base.scale.y < 0.0f ? -sy : sy;
        sprite.rotation = base.rotation + p->rotation;
        sprite.opacity = opacity;
        if (appearance == NULL) {
            texel = p->color;
        }
        status = pixelica2d_draw_sprite(ctx, &sprite);
        if (status != PIXELICA_OK) {
            return status;
        }
    }
    return PIXELICA_OK;
}

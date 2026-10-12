/*
 * Pixelica - 2.5D particle adapter (see pixelica/particles25d.h).
 *
 * Pass 1 validates and projects every particle into the caller's scratch;
 * only then are the particles drawn, far to near, through
 * pixelica25d_draw_billboard. A rejected call therefore writes nothing.
 */
#include "pixelica/particles25d.h"

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
    return isfinite(p->position.x) && isfinite(p->position.y) && isfinite(p->position.z) &&
           isfinite(p->rotation) && isfinite(p->size) && p->size >= 0.0f &&
           p->size <= PIXELICA_PARTICLES_MAX_VALUE;
}

/*
 * The fields pixelica25d_draw_billboard reads from an appearance. Position,
 * depth and depth_test are ignored by the billboard, so they are not checked.
 */
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
    return isfinite(a->scale.x) && isfinite(a->scale.y) && (a->scale.x < 0.0f || a->scale.x > 0.0f) &&
           (a->scale.y < 0.0f || a->scale.y > 0.0f) && isfinite(a->pivot.x) && isfinite(a->pivot.y) &&
           isfinite(a->rotation);
}

static bool ranges_overlap(const void *a, size_t a_bytes, const void *b, size_t b_bytes)
{
    const uintptr_t a_start = (uintptr_t)a;
    const uintptr_t b_start = (uintptr_t)b;

    if (a_bytes == 0u || b_bytes == 0u) {
        return false;
    }
    if (a_start + a_bytes < a_start || b_start + b_bytes < b_start) {
        return true; /* wraps the address space: cannot be a real object */
    }
    return a_start < b_start + b_bytes && b_start < a_start + a_bytes;
}

static size_t plane_bytes(uint16_t width, uint16_t height, uint32_t stride)
{
    size_t rows;
    if (height == 0u) {
        return 0u;
    }
    rows = (size_t)(height - 1u);
    if (stride != 0u && rows > (SIZE_MAX / sizeof(uint16_t) - width) / stride)
        return SIZE_MAX;
    return (rows * (size_t)stride + width) * sizeof(uint16_t);
}

/* True when `a` is drawn before `b`: farther first, then pool order. */
static bool draws_before(const PIXELICA25D_ParticleSortItem *a, const PIXELICA25D_ParticleSortItem *b)
{
    return a->depth > b->depth || (a->depth == b->depth && a->index < b->index);
}

static void swap_items(PIXELICA25D_ParticleSortItem *a, PIXELICA25D_ParticleSortItem *b)
{
    const PIXELICA25D_ParticleSortItem tmp = *a;

    *a = *b;
    *b = tmp;
}

/* Max-heap whose root is the item drawn LAST among items[0 .. end). */
static void sift_down(PIXELICA25D_ParticleSortItem *items, size_t root, size_t end)
{
    for (;;) {
        size_t child = 2u * root + 1u;

        if (child >= end) {
            return;
        }
        if (child + 1u < end && draws_before(&items[child], &items[child + 1u])) {
            ++child;
        }
        if (!draws_before(&items[root], &items[child])) {
            return;
        }
        swap_items(&items[root], &items[child]);
        root = child;
    }
}

/* In-place heap sort: no allocation, O(n log n), and the order is total, so deterministic. */
static void sort_far_to_near(PIXELICA25D_ParticleSortItem *items, size_t count)
{
    size_t start;
    size_t end;

    for (start = count / 2u; start > 0u; --start) {
        sift_down(items, start - 1u, count);
    }
    for (end = count; end > 1u; --end) {
        swap_items(&items[0], &items[end - 1u]);
        sift_down(items, 0u, end - 1u);
    }
}

PIXELICA_Status pixelica25d_particles_draw(PIXELICA2D_Context *canvas, const float view_projection[16],
    const PIXELICA_ParticleEmitter *emitter, const PIXELICA25D_ParticleOptions *options,
    PIXELICA25D_ParticleSortItem *scratch, size_t scratch_count)
{
    const PIXELICA2D_Sprite *appearance = options != NULL ? options->appearance : NULL;
    const bool depth_write = options != NULL && options->depth_write;
    uint16_t texel = 0u;
    const PIXELICA_Texture square = {&texel, 1u, 1u};
    PIXELICA2D_Sprite base;
    size_t used = 0u;
    size_t i;

    if (canvas == NULL || view_projection == NULL || !pixelica_mat4_is_finite(view_projection) ||
        canvas->surface.depth == NULL || !emitter_valid(emitter) ||
        (appearance != NULL && !appearance_valid(appearance))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (emitter->count == 0u) {
        return PIXELICA_OK;
    }
    if (scratch == NULL || scratch_count < (size_t)emitter->count) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    {
        const size_t scratch_bytes = (size_t)emitter->count * sizeof(PIXELICA25D_ParticleSortItem);

        /* The scratch is written while the particles and canvas are still read. */
        if (ranges_overlap(scratch, scratch_bytes, emitter, sizeof *emitter) ||
            ranges_overlap(scratch, scratch_bytes, canvas, sizeof *canvas) ||
            ranges_overlap(scratch, scratch_bytes, view_projection, 16u * sizeof(float)) ||
            (options != NULL && ranges_overlap(scratch, scratch_bytes, options, sizeof *options)) ||
            (appearance != NULL &&
             (ranges_overlap(scratch, scratch_bytes, appearance, sizeof *appearance) ||
              ranges_overlap(scratch, scratch_bytes, appearance->texture, sizeof *appearance->texture) ||
              ranges_overlap(scratch, scratch_bytes, appearance->texture->pixels,
                (size_t)appearance->texture->width * appearance->texture->height * sizeof(uint16_t)))) ||
            ranges_overlap(scratch, scratch_bytes, emitter->particles,
                           (size_t)emitter->capacity * sizeof(PIXELICA_Particle)) ||
            ranges_overlap(scratch, scratch_bytes, canvas->surface.pixels,
                           plane_bytes(canvas->surface.width, canvas->surface.height, canvas->surface.stride)) ||
            ranges_overlap(scratch, scratch_bytes, canvas->surface.depth,
                           plane_bytes(canvas->surface.width, canvas->surface.height,
                                       canvas->surface.depth_stride))) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    if (appearance != NULL) {
        base = *appearance;
    } else {
        const PIXELICA_Status status = pixelica2d_sprite_init(&base, &square);

        if (status != PIXELICA_OK) {
            return status;
        }
        base.pivot.x = 0.5f;
        base.pivot.y = 0.5f;
    }

    /* Pass 1: validate, project and collect the visible particles. Nothing is drawn. */
    for (i = 0u; i < (size_t)emitter->count; ++i) {
        const PIXELICA_Particle *p = &emitter->particles[i];
        const uint8_t opacity = appearance != NULL ? scale_opacity(p->opacity, base.opacity) : p->opacity;
        PIXELICA25D_Projection projection;

        if (!particle_valid(p)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (pixelica25d_project(view_projection, p->position, canvas->surface.width, canvas->surface.height,
                                &projection) != PIXELICA_OK) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (!projection.drawable) {
            continue;
        }
        if ((double)p->size * (double)projection.pixels_per_unit_x > (double)PIXELICA25D_PARTICLES_MAX_PROJECTED_SIZE ||
            (double)p->size * (double)projection.pixels_per_unit_y > (double)PIXELICA25D_PARTICLES_MAX_PROJECTED_SIZE) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (opacity == 0u || !(p->size > 0.0f)) {
            continue;
        }
        {
            PIXELICA2D_Context check = *canvas;
            PIXELICA2D_Sprite probe = base;
            PIXELICA_Vec2 size = {p->size, p->size};
            probe.rotation = base.rotation + p->rotation;
            probe.opacity = 0u;
            probe.depth_write = depth_write;
            if (pixelica25d_draw_billboard(&check, view_projection, &probe, p->position, size) != PIXELICA_OK)
                return PIXELICA_ERR_INVALID_ARG;
        }
        scratch[used].index = (uint32_t)i;
        scratch[used].depth = projection.depth;
        ++used;
    }
    sort_far_to_near(scratch, used);

    /* Pass 2: far to near. */
    for (i = 0u; i < used; ++i) {
        const PIXELICA_Particle *p = &emitter->particles[scratch[i].index];
        PIXELICA2D_Sprite sprite = base;
        PIXELICA_Vec2 world_size;
        PIXELICA_Status status;

        world_size.x = p->size;
        world_size.y = p->size;
        sprite.rotation = base.rotation + p->rotation;
        sprite.opacity = appearance != NULL ? scale_opacity(p->opacity, base.opacity) : p->opacity;
        sprite.depth_write = depth_write;
        if (appearance == NULL) {
            texel = p->color;
        }
        status = pixelica25d_draw_billboard(canvas, view_projection, &sprite, p->position, world_size);
        if (status != PIXELICA_OK) {
            return status;
        }
    }
    return PIXELICA_OK;
}

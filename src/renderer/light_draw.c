/*
 * Pixelica - light patches, contact shadows and the emissive-mask hook
 * (3D core; see include/pixelica/renderer.h and docs/pixel_light.md).
 *
 * These are deliberately thin: a patch is ONE quad drawn through the ordinary
 * pixelica_draw_mesh_effects path, with the shape mask handed to the existing
 * texture_alpha plane. No new rasterizer code, no new per-frame memory and no
 * change to any existing draw entry point.
 */
#include "pixelica/pixelica.h"

#include <math.h>
#include <string.h>

/* Quad vertices/UVs/indices are copied to the stack before drawing: the
 * pipeline refuses mesh arrays that alias context-owned memory, and a local
 * can never alias live storage. */
static PIXELICA_Status ld_draw_quad(PIXELICA_Context *ctx, const PIXELICA_LightQuad *quad,
                                    const PIXELICA_Texture *tint,
                                    const PIXELICA_LightMask *mask,
                                    PIXELICA_AlphaMode mode, uint8_t opacity,
                                    const PIXELICA_MeshEffects *effects)
{
    PIXELICA_LightQuad local;
    PIXELICA_Texture texture;
    PIXELICA_Mesh mesh;
    PIXELICA_MaterialLayers layers;
    PIXELICA_DrawOptions options;
    float model[16];
    size_t texels;
    int i;

    if (ctx == NULL || quad == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (mode != PIXELICA_ALPHA_ADD && mode != PIXELICA_ALPHA_BLEND) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (tint->pixels == NULL || tint->width < 1u || tint->height < 1u ||
        tint->width > PIXELICA_MAX_TEXTURE_DIMENSION ||
        tint->height > PIXELICA_MAX_TEXTURE_DIMENSION) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* The renderer samples colour and alpha at the same texel, so the mask
     * must be the tint's size and tightly packed. */
    if (mask->alpha == NULL || mask->width != tint->width ||
        mask->height != tint->height || mask->stride != (size_t)mask->width) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < 4; ++i) {
        if (!isfinite(quad->vertices[i].x) || !isfinite(quad->vertices[i].y) ||
            !isfinite(quad->vertices[i].z) || !isfinite(quad->uvs[i].x) ||
            !isfinite(quad->uvs[i].y)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    for (i = 0; i < 6; ++i) {
        if (quad->indices[i] > 3u) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    local = *quad;
    texture = *tint;
    texels = (size_t)texture.width * (size_t)texture.height;

    memset(&layers, 0, sizeof layers);
    layers.base.uvs = local.uvs;
    layers.base.triangle_colors = NULL;
    layers.base.texture = &texture;

    mesh.vertices = local.vertices;
    mesh.vertex_count = 4u;
    mesh.indices = local.indices;
    mesh.index_count = 6u;
    mesh.color = 0u;

    options = pixelica_draw_options_default();
    options.alpha_mode = mode;
    options.opacity = opacity;
    options.texture_alpha = mask->alpha;
    options.texture_alpha_bytes = texels;

    pixelica_mat4_identity(model);
    return pixelica_draw_mesh_effects(ctx, &mesh, model, &layers, &options, effects);
}

PIXELICA_Status pixelica_draw_light(PIXELICA_Context *ctx, const PIXELICA_LightDraw *draw,
                                    const PIXELICA_MeshEffects *effects)
{
    if (draw == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return ld_draw_quad(ctx, &draw->quad, &draw->tint, &draw->mask, draw->mode,
                        draw->opacity, effects);
}

PIXELICA_Status pixelica_draw_contact_shadow(PIXELICA_Context *ctx,
                                             const PIXELICA_ContactShadow *shadow,
                                             const PIXELICA_MeshEffects *effects)
{
    PIXELICA_LightQuad quad;
    PIXELICA_Status status;

    if (shadow == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* pixelica_light_quad_centered checks the placement (finite fields,
     * positive radii, a usable normal and u_axis) before anything else. */
    status = pixelica_light_quad_centered(shadow->center, shadow->normal, shadow->u_axis,
                                          shadow->radius_u, shadow->radius_v,
                                          shadow->bias, &quad);
    if (status != PIXELICA_OK) {
        return status;
    }
    return ld_draw_quad(ctx, &quad, &shadow->tint, &shadow->mask, PIXELICA_ALPHA_BLEND,
                        shadow->opacity, effects);
}

/* ---- emissive mask ------------------------------------------------------- */

/* Half-open byte range [lo, hi); unusable (0) for NULL or an address wrap. */
typedef struct LdRange {
    uintptr_t lo, hi;
} LdRange;

static int ld_range(const void *p, size_t bytes, LdRange *r)
{
    uintptr_t lo = (uintptr_t)p;

    if (p == NULL || bytes == 0u || (uintptr_t)bytes > UINTPTR_MAX - lo) {
        return 0;
    }
    r->lo = lo;
    r->hi = lo + (uintptr_t)bytes;
    return 1;
}

static int ld_overlap(const LdRange *a, const LdRange *b)
{
    return a->lo < b->hi && b->lo < a->hi;
}

/*
 * Structural check only: dimensions, stride and the element count of one
 * plane ((height - 1) * stride + width). This is what the per-fragment hook
 * can afford; the overlap rules are checked by
 * pixelica_emissive_mask_clear() and _resolve().
 */
static int ld_mask_usable(const PIXELICA_EmissiveMask *mask, size_t *out_elements)
{
    size_t rows, elements;

    if (mask == NULL || mask->alpha == NULL || mask->width < 1u || mask->height < 1u ||
        mask->width > PIXELICA_MAX_DIMENSION || mask->height > PIXELICA_MAX_DIMENSION ||
        mask->stride < (size_t)mask->width) {
        return 0;
    }
    rows = (size_t)mask->height - 1u;
    if (rows != 0u && mask->stride > (SIZE_MAX - (size_t)mask->width) / rows) {
        return 0;
    }
    elements = rows * mask->stride + (size_t)mask->width;
    if (elements > SIZE_MAX / sizeof(uint16_t)) {
        return 0; /* the optional depth plane must be addressable too */
    }
    *out_elements = elements;
    return 1;
}

/*
 * Full check, used before anything is written: the structure above plus the
 * address ranges of both planes and of the descriptor itself. The planes are
 * WRITTEN, so neither may contain the descriptor they are described by, and
 * they may not overlap each other. Every range must also be addressable
 * without wrapping.
 */
static int ld_mask_planes(const PIXELICA_EmissiveMask *mask, size_t *out_elements,
                          LdRange *out_alpha, LdRange *out_depth, int *out_has_depth)
{
    LdRange alpha, depth, descriptor;
    size_t elements = 0u;

    if (!ld_mask_usable(mask, &elements) ||
        !ld_range(mask->alpha, elements, &alpha) ||
        !ld_range(mask, sizeof *mask, &descriptor) || ld_overlap(&alpha, &descriptor)) {
        return 0;
    }
    *out_has_depth = mask->depth != NULL;
    if (*out_has_depth) {
        if (!ld_range(mask->depth, elements * sizeof(uint16_t), &depth) ||
            ld_overlap(&depth, &descriptor) || ld_overlap(&alpha, &depth)) {
            return 0;
        }
        *out_depth = depth;
    }
    *out_elements = elements;
    *out_alpha = alpha;
    return 1;
}

bool pixelica_emissive_mask_pixel(void *userdata, const PIXELICA_Fragment *fragment,
                                  uint16_t *out_color, uint8_t *out_alpha)
{
    PIXELICA_EmissiveMask *mask = (PIXELICA_EmissiveMask *)userdata;
    size_t elements = 0u, index;
    uint8_t value;
    uint8_t *slot;

    (void)out_color;
    (void)out_alpha;
    /* The hook must never change the image, so an unusable mask (or a
     * fragment that will not be visible) records nothing and keeps the
     * pixel. Alpha 0 is skipped in every mode; `min_alpha` lets a
     * PIXELICA_ALPHA_TEST draw skip what its cutoff will discard, which the
     * hook itself cannot see (it runs before the test). */
    if (fragment == NULL || !ld_mask_usable(mask, &elements) ||
        fragment->x >= mask->width || fragment->y >= mask->height ||
        fragment->alpha == 0u || fragment->alpha < mask->min_alpha) {
        return true;
    }
    value = mask->value != 0u ? mask->value : fragment->alpha;
    index = (size_t)fragment->y * mask->stride + (size_t)fragment->x;
    slot = &mask->alpha[index];
    if (value > *slot) {
        *slot = value;
    }
    if (mask->depth != NULL && fragment->depth < mask->depth[index]) {
        mask->depth[index] = fragment->depth; /* keep the nearest emitter */
    }
    if (mask->written != UINT32_MAX) {
        ++mask->written;
    }
    return true;
}

PIXELICA_Status pixelica_emissive_mask_clear(PIXELICA_EmissiveMask *mask)
{
    LdRange alpha, depth;
    size_t elements = 0u, i;
    int has_depth = 0;

    if (!ld_mask_planes(mask, &elements, &alpha, &depth, &has_depth)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(mask->alpha, 0, elements);
    if (has_depth) {
        for (i = 0; i < elements; ++i) {
            mask->depth[i] = PIXELICA_DEPTH_CLEAR;
        }
    }
    mask->written = 0u;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_emissive_mask_resolve(PIXELICA_EmissiveMask *mask,
                                               const PIXELICA_FrameView *frame,
                                               uint32_t *out_removed)
{
    LdRange written, counter, depth, read_only[4];
    size_t elements = 0u, frame_elements, rows;
    uint32_t removed = 0u;
    size_t y, x;
    int read_count = 0, i, has_depth = 0;

    if (frame == NULL || !ld_mask_planes(mask, &elements, &written, &depth, &has_depth) ||
        !has_depth || frame->depth == NULL || frame->width != mask->width ||
        frame->height != mask->height || frame->stride < (size_t)frame->width) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    rows = (size_t)frame->height - 1u;
    if (rows != 0u && frame->stride > (SIZE_MAX - (size_t)frame->width) / rows) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    frame_elements = rows * frame->stride + (size_t)frame->width;
    if (frame_elements > SIZE_MAX / sizeof(uint16_t)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /*
     * mask->alpha is written throughout, *out_removed once at the end;
     * everything else is read and must stay intact, including the two
     * descriptors. ld_mask_planes already rejected alpha overlapping the mask
     * descriptor or its own depth plane.
     */
    read_only[read_count++] = depth;
    if (!ld_range(frame->depth, frame_elements * sizeof(uint16_t),
                  &read_only[read_count++]) ||
        !ld_range(mask, sizeof *mask, &read_only[read_count++]) ||
        !ld_range(frame, sizeof *frame, &read_only[read_count++])) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < read_count; ++i) {
        if (ld_overlap(&written, &read_only[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    if (out_removed != NULL) {
        if (!ld_range(out_removed, sizeof *out_removed, &counter) ||
            ld_overlap(&counter, &written)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        for (i = 0; i < read_count; ++i) {
            if (ld_overlap(&counter, &read_only[i])) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
    }

    for (y = 0; y < (size_t)mask->height; ++y) {
        for (x = 0; x < (size_t)mask->width; ++x) {
            size_t index = y * mask->stride + x;

            if (mask->alpha[index] == 0u) {
                continue;
            }
            /* The renderer's test is strictly LESS, so a later fragment won
             * the pixel only if its depth is smaller than ours. */
            if (frame->depth[y * frame->stride + x] < mask->depth[index]) {
                mask->alpha[index] = 0u;
                if (removed != UINT32_MAX) {
                    ++removed;
                }
            }
        }
    }
    if (out_removed != NULL) {
        *out_removed = removed;
    }
    return PIXELICA_OK;
}

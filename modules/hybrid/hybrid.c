/*
 * Pixelica - 2.5D hybrid glue (see include/pixelica/hybrid.h for the contract).
 *
 * The only private access is reading the color/depth pointers and the extent
 * of the engine-owned block of a live PIXELICA_Context in pixelica2d_bind_3d (to keep the
 * output view disjoint from it); everything else uses public 2D and math APIs.
 * Nothing here allocates, clears, presents or keeps state.
 */
#include "pixelica/hybrid.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "../../src/core/context_internal.h"

/* True when `value` is finite and fits a float; stores the narrowed value. */
static bool narrow_to_float(double value, float *out)
{
    if (!isfinite(value) || value > (double)FLT_MAX || value < -(double)FLT_MAX) {
        return false;
    }
    *out = (float)value;
    return true;
}

static bool vec3_is_finite(PIXELICA_Vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static bool vec2_is_positive_finite(PIXELICA_Vec2 v)
{
    return isfinite(v.x) && isfinite(v.y) && v.x > 0.0f && v.y > 0.0f;
}

static bool scale_is_valid(PIXELICA_Vec2 s)
{
    return isfinite(s.x) && isfinite(s.y) && s.x != 0.0f && s.y != 0.0f;
}

/*
 * True when [out, out + out_bytes) intersects the engine-owned block of `ctx`.
 * That block is contiguous from the PIXELICA_Context struct itself to the end of
 * the last guard (struct, guard0, color, guard1, depth, guard2, arena, guard3)
 * for heap and caller-owned contexts alike. All arithmetic is on uintptr_t
 * with the wrap checked before every addition; a range that cannot be
 * represented is reported as overlapping. `out` is never dereferenced.
 */
static bool overlaps_engine_block(const void *out, size_t out_bytes, const PIXELICA_Context *ctx)
{
    const uintptr_t block_lo = (uintptr_t)ctx;
    const uintptr_t out_lo = (uintptr_t)out;
    uintptr_t block_hi = (uintptr_t)ctx->guards[3];

    if (block_hi < block_lo || block_hi > UINTPTR_MAX - PIXELICA_GUARD_BYTES ||
        out_lo > UINTPTR_MAX - (uintptr_t)out_bytes) {
        return true;
    }
    block_hi += PIXELICA_GUARD_BYTES;
    return out_lo < block_hi && block_lo < out_lo + (uintptr_t)out_bytes;
}

PIXELICA_Status pixelica2d_bind_3d(PIXELICA2D_Context *out, PIXELICA_Context *ctx)
{
    PIXELICA2D_Surface surface;
    PIXELICA2D_Context view;
    PIXELICA_Status status;

    if (out == NULL || !pixelica_context_valid(ctx) || !ctx->in_frame ||
        ctx->color == NULL || ctx->depth == NULL ||
        overlaps_engine_block(out, sizeof *out, ctx)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    surface.pixels = ctx->color;
    surface.width = ctx->cfg.width;
    surface.height = ctx->cfg.height;
    surface.stride = (uint32_t)ctx->cfg.width;
    surface.depth = ctx->depth;
    surface.depth_stride = (uint32_t)ctx->cfg.width;

    /* Build into a local first so a failure leaves *out untouched. */
    status = pixelica2d_init(&view, &surface);
    if (status != PIXELICA_OK) {
        return status;
    }
    *out = view;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica25d_project(const float view_projection[16], PIXELICA_Vec3 position,
                         uint16_t width, uint16_t height, PIXELICA25D_Projection *out)
{
    const float *m = view_projection;
    double x, y, z, cx, cy, cz, cw;
    double norm_x, norm_y, depth, scale_x, scale_y;
    PIXELICA25D_Projection result;

    if (out == NULL || m == NULL || !pixelica_mat4_is_finite(m) || !vec3_is_finite(position) ||
        width == 0u || height == 0u || (uint32_t)width > PIXELICA2D_MAX_DIMENSION ||
        (uint32_t)height > PIXELICA2D_MAX_DIMENSION) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    /* Rows 0 and 1 of a column-major matrix: elements (r, c) = m[c * 4 + r]. */
    norm_x = sqrt((double)m[0] * (double)m[0] + (double)m[4] * (double)m[4] +
                  (double)m[8] * (double)m[8]);
    norm_y = sqrt((double)m[1] * (double)m[1] + (double)m[5] * (double)m[5] +
                  (double)m[9] * (double)m[9]);
    if (!isfinite(norm_x) || !isfinite(norm_y) || !(norm_x > 0.0) || !(norm_y > 0.0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    x = (double)position.x;
    y = (double)position.y;
    z = (double)position.z;
    cx = (double)m[0] * x + (double)m[4] * y + (double)m[8] * z + (double)m[12];
    cy = (double)m[1] * x + (double)m[5] * y + (double)m[9] * z + (double)m[13];
    cz = (double)m[2] * x + (double)m[6] * y + (double)m[10] * z + (double)m[14];
    cw = (double)m[3] * x + (double)m[7] * y + (double)m[11] * z + (double)m[15];
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz) || !isfinite(cw)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    memset(&result, 0, sizeof result);
    if (cw < (double)PIXELICA_CLIP_W_EPSILON || cz < -cw || cz > cw) {
        *out = result; /* drawable == false, everything else zero */
        return PIXELICA_OK;
    }

    depth = floor((cz / cw + 1.0) * 0.5 * (double)PIXELICA2D_DEPTH_MAX + 0.5);
    if (depth < 0.0) {
        depth = 0.0;
    } else if (depth > (double)PIXELICA2D_DEPTH_MAX) {
        depth = (double)PIXELICA2D_DEPTH_MAX;
    }
    scale_x = norm_x * (double)width / (2.0 * cw);
    scale_y = norm_y * (double)height / (2.0 * cw);

    if (!narrow_to_float((cx / cw * 0.5 + 0.5) * (double)width, &result.screen.x) ||
        !narrow_to_float((0.5 - cy / cw * 0.5) * (double)height, &result.screen.y) ||
        !narrow_to_float(scale_x, &result.pixels_per_unit_x) ||
        !narrow_to_float(scale_y, &result.pixels_per_unit_y) ||
        !(result.pixels_per_unit_x > 0.0f) || !(result.pixels_per_unit_y > 0.0f)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    result.depth = (uint16_t)depth;
    result.drawable = true;
    *out = result;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica25d_draw_billboard(PIXELICA2D_Context *canvas, const float view_projection[16],
                                const PIXELICA2D_Sprite *appearance, PIXELICA_Vec3 position, PIXELICA_Vec2 world_size)
{
    static const PIXELICA2D_Rect empty_clip = {0, 0, 0, 0};
    PIXELICA2D_Context local;
    PIXELICA2D_Context probe;
    PIXELICA2D_Sprite sprite;
    PIXELICA25D_Projection projection;
    float scale_x, scale_y;
    PIXELICA_Status status;

    if (canvas == NULL || view_projection == NULL || appearance == NULL ||
        !vec2_is_positive_finite(world_size) || canvas->surface.depth == NULL ||
        !scale_is_valid(appearance->scale)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    /*
     * Rebuild a trusted local canvas from public setters: init validates the
     * surface and gives the identity camera (billboards are placed in screen
     * pixels, so the caller's scrolling camera must not move them); the
     * caller's clip is re-validated and kept. Stats start from the caller's
     * counters and are copied back only after a successful draw.
     */
    status = pixelica2d_init(&local, &canvas->surface);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = pixelica2d_set_clip(&local, &canvas->clip);
    if (status != PIXELICA_OK) {
        return status;
    }
    local.stats = canvas->stats;

    /*
     * Validate the appearance even when the point ends up behind the camera:
     * draw it into a throw-away copy whose clip is empty. The 2D engine checks
     * everything before it touches a pixel, so this writes nothing and the
     * copy's stats are discarded. Position and depth are overridden below, so
     * neutral values stand in for them here; the scale was checked above (only
     * its sign is used) and a neutral +1 stands in for it too.
     */
    probe = local;
    status = pixelica2d_set_clip(&probe, &empty_clip);
    if (status != PIXELICA_OK) {
        return status;
    }
    sprite = *appearance;
    sprite.position.x = 0.0f;
    sprite.position.y = 0.0f;
    sprite.scale.x = 1.0f;
    sprite.scale.y = 1.0f;
    sprite.depth = 0u;
    sprite.depth_test = true;
    status = pixelica2d_draw_sprite(&probe, &sprite);
    if (status != PIXELICA_OK) {
        return status;
    }

    status = pixelica25d_project(view_projection, position, local.surface.width,
                            local.surface.height, &projection);
    if (status != PIXELICA_OK) {
        return status;
    }
    if (!projection.drawable) {
        return PIXELICA_OK;
    }

    /* source.width/height are > 0 here: the probe draw accepted the sprite. */
    if (!narrow_to_float((double)world_size.x * (double)projection.pixels_per_unit_x /
                             (double)appearance->source.width, &scale_x) ||
        !narrow_to_float((double)world_size.y * (double)projection.pixels_per_unit_y /
                             (double)appearance->source.height, &scale_y)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (scale_x == 0.0f || scale_y == 0.0f) {
        return PIXELICA_OK; /* smaller than any representable size: nothing to draw */
    }

    /* The magnitude comes from the world size; the appearance only picks the mirroring. */
    if (signbit(appearance->scale.x)) {
        scale_x = -scale_x;
    }
    if (signbit(appearance->scale.y)) {
        scale_y = -scale_y;
    }

    sprite = *appearance;
    sprite.position = projection.screen;
    sprite.scale.x = scale_x;
    sprite.scale.y = scale_y;
    sprite.depth = projection.depth;
    sprite.depth_test = true;
    status = pixelica2d_draw_sprite(&local, &sprite);
    if (status != PIXELICA_OK) {
        return status;
    }
    canvas->stats = local.stats;
    return PIXELICA_OK;
}

/* Pixelica - 3D context adapters for the common framebuffer API
 * (include/pixelica/framebuffer.h, docs/framebuffer.md). */
#include "pixelica/framebuffer.h"

#include "context_internal.h"

/* 1 if [out, out + bytes) is clear of every context-owned region. */
static int fb_out_is_disjoint(const PIXELICA_Context *ctx, const void *out,
                              size_t bytes)
{
    const uintptr_t a = (uintptr_t)out;
    const uintptr_t lo = (uintptr_t)(const void *)ctx;
    const uintptr_t hi =
        (uintptr_t)(const void *)ctx->guards[PIXELICA_GUARD_COUNT - 1u] +
        PIXELICA_GUARD_BYTES;

    return bytes <= UINTPTR_MAX - a && !(a < hi && lo < a + bytes);
}

PIXELICA_Status pixelica_frame_view(const PIXELICA_Context *ctx,
                                    PIXELICA_FrameView *out)
{
    if (out == NULL || !pixelica_context_valid(ctx) ||
        !fb_out_is_disjoint(ctx, out, sizeof *out)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    out->color = ctx->color;
    out->depth = ctx->depth;
    out->width = ctx->cfg.width;
    out->height = ctx->cfg.height;
    out->stride = (size_t)ctx->cfg.width;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_frame_texture(const PIXELICA_Context *ctx,
                                       PIXELICA_Texture *out)
{
    if (out == NULL || !pixelica_context_valid(ctx) ||
        !fb_out_is_disjoint(ctx, out, sizeof *out)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    out->pixels = ctx->color;
    out->width = ctx->cfg.width;
    out->height = ctx->cfg.height;
    return PIXELICA_OK;
}

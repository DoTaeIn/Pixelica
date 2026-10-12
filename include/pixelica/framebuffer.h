/*
 * Pixelica - framebuffer snapshots and sampling (common library).
 *
 * No heap, no global state, no renderer dependency. PIXELICA_FrameView is a
 * borrowed, read-only description of an RGB565 color plane and/or a raw uint16
 * depth plane. The 3D context adapters (pixelica_frame_view,
 * pixelica_frame_texture) live in the 3D core; see docs/framebuffer.md.
 *
 * Every function validates ALL inputs before writing anything; on a
 * non-PIXELICA_OK result every output is left exactly as it was.
 */
#ifndef PIXELICA_FRAMEBUFFER_H
#define PIXELICA_FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/draw_options.h"
#include "pixelica/image.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Row-major, row 0 = TOP row. Pixel (x, y) is plane[y * stride + x]; stride is
 * in pixels. Valid when at least one of color/depth is non-NULL,
 * 1 <= width, height <= PIXELICA_MAX_TEXTURE_DIMENSION and stride >= width.
 * A plane must hold (height - 1) * stride + width elements. Depth is the raw
 * depth16 of the 3D renderer (NDC, nonlinear distance for perspective,
 * cleared to 65535), not a distance.
 */
typedef struct PIXELICA_FrameView {
    const uint16_t *color;
    const uint16_t *depth;
    uint16_t width, height;
    size_t stride;
} PIXELICA_FrameView;

/*
 * Copies the source into caller buffers as an independent snapshot and fills
 * *out with a tightly packed view (stride == width) over them.
 *   color_out / depth_out: NULL = plane not requested (its capacity must be
 *     0); otherwise the source must have that plane and capacity (in pixels)
 *     must be >= width * height. At least one output is required.
 *   *out->color / ->depth are the supplied buffers or NULL when omitted.
 * Errors (nothing written): PIXELICA_ERR_INVALID_ARG for NULL/invalid view,
 * no output, a supplied buffer without the matching source plane, an omitted buffer with non-zero capacity, or any overlap
 * between outputs (out, color_out, depth_out) and inputs (source, its
 * planes) or among the outputs; PIXELICA_ERR_CAPACITY when a capacity is
 * smaller than width * height.
 */
PIXELICA_Status pixelica_frame_copy(const PIXELICA_FrameView *source,
                                    uint16_t *color_out, size_t color_capacity,
                                    uint16_t *depth_out, size_t depth_capacity,
                                    PIXELICA_FrameView *out);

/*
 * Samples the color plane at normalized (u, v); (0,0) = top-left corner.
 * NEAREST: texel floor(u * width); CLAMP clamps to the edge, REPEAT wraps.
 * BILINEAR: texel centers at (i + 0.5) / width; each native 5/6/5 channel is
 * blended in double and rounded to nearest. Any finite u, v is accepted.
 * PIXELICA_ERR_INVALID_ARG (and *out untouched) for a NULL pointer, a view
 * without color, non-finite u/v, an enum out of range or *out overlapping
 * the view.
 */
PIXELICA_Status pixelica_frame_sample_color(const PIXELICA_FrameView *view,
                                            float u, float v,
                                            PIXELICA_WrapMode wrap_u,
                                            PIXELICA_WrapMode wrap_v,
                                            PIXELICA_FilterMode filter,
                                            uint16_t *out);

/* Nearest raw depth16 at (u, v); depth is never interpolated. Same wrap and
 * error rules as pixelica_frame_sample_color; needs a depth plane. */
PIXELICA_Status pixelica_frame_sample_depth(const PIXELICA_FrameView *view,
                                            float u, float v,
                                            PIXELICA_WrapMode wrap_u,
                                            PIXELICA_WrapMode wrap_v,
                                            uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_FRAMEBUFFER_H */

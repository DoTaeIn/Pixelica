/* Pixelica - framebuffer snapshots and sampling (common library, see
 * include/pixelica/framebuffer.h and docs/framebuffer.md). */
#include "pixelica/framebuffer.h"

#include <math.h>
#include <string.h>

/* Half-open byte range [lo, hi). */
typedef struct FbRange {
    uintptr_t lo, hi;
} FbRange;

static int fb_range(const void *p, size_t bytes, FbRange *r)
{
    uintptr_t lo = (uintptr_t)p;

    if (p == NULL || bytes == 0u || bytes > (size_t)UINTPTR_MAX - lo) {
        return 0;
    }
    r->lo = lo;
    r->hi = lo + (uintptr_t)bytes;
    return 1;
}

static int fb_overlap(const FbRange *a, const FbRange *b)
{
    return a->lo < b->hi && b->lo < a->hi;
}

/* Elements spanned by a plane: (height - 1) * stride + width. 0 on overflow. */
static size_t fb_plane_elements(const PIXELICA_FrameView *v)
{
    size_t rows = (size_t)v->height - 1u;
    size_t n;

    if (rows != 0u && v->stride > (SIZE_MAX - (size_t)v->width) / rows) {
        return 0u;
    }
    n = rows * v->stride + (size_t)v->width;
    return n > SIZE_MAX / sizeof(uint16_t) ? 0u : n;
}

/* Validates the view and returns the byte ranges of its present planes. */
static int fb_view_valid(const PIXELICA_FrameView *v, FbRange *color,
                         FbRange *depth, int *has_color, int *has_depth)
{
    size_t bytes;

    if (v == NULL || (v->color == NULL && v->depth == NULL) ||
        v->width < 1u || v->height < 1u ||
        v->width > PIXELICA_MAX_TEXTURE_DIMENSION ||
        v->height > PIXELICA_MAX_TEXTURE_DIMENSION ||
        v->stride < (size_t)v->width) {
        return 0;
    }
    bytes = fb_plane_elements(v) * sizeof(uint16_t);
    if (bytes == 0u) {
        return 0;
    }
    *has_color = v->color != NULL;
    *has_depth = v->depth != NULL;
    if ((*has_color && !fb_range(v->color, bytes, color)) ||
        (*has_depth && !fb_range(v->depth, bytes, depth))) {
        return 0;
    }
    return 1;
}

PIXELICA_Status pixelica_frame_copy(const PIXELICA_FrameView *source,
                                    uint16_t *color_out, size_t color_capacity,
                                    uint16_t *depth_out, size_t depth_capacity,
                                    PIXELICA_FrameView *out)
{
    FbRange in[3], outs[3];
    int has_color = 0, has_depth = 0;
    int n_in = 0, n_out = 0, i, j;
    FbRange src_color = {0u, 0u}, src_depth = {0u, 0u};
    size_t pixels, row;
    size_t y;
    PIXELICA_FrameView result;

    if (source == NULL || out == NULL ||
        !fb_view_valid(source, &src_color, &src_depth, &has_color, &has_depth)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if ((depth_out == NULL && depth_capacity != 0u) ||
        (color_out == NULL && color_capacity != 0u) ||
        (color_out == NULL && depth_out == NULL) ||
        (color_out != NULL && !has_color) ||
        (depth_out != NULL && !has_depth)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    pixels = (size_t)source->width * (size_t)source->height;
    if ((color_out != NULL && color_capacity < pixels) ||
        (depth_out != NULL && depth_capacity < pixels)) {
        return PIXELICA_ERR_CAPACITY;
    }

    /* Inputs: descriptor and planes. Outputs: descriptor and buffers. */
    if (!fb_range(source, sizeof *source, &in[n_in++])) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (has_color) {
        in[n_in++] = src_color;
    }
    if (has_depth) {
        in[n_in++] = src_depth;
    }
    if (!fb_range(out, sizeof *out, &outs[n_out++])) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (color_out != NULL &&
        !fb_range(color_out, pixels * sizeof(uint16_t), &outs[n_out++])) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (depth_out != NULL &&
        !fb_range(depth_out, pixels * sizeof(uint16_t), &outs[n_out++])) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < n_out; ++i) {
        for (j = 0; j < n_in; ++j) {
            if (fb_overlap(&outs[i], &in[j])) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
        for (j = i + 1; j < n_out; ++j) {
            if (fb_overlap(&outs[i], &outs[j])) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
    }

    row = (size_t)source->width;
    for (y = 0; y < (size_t)source->height; ++y) {
        if (color_out != NULL) {
            memcpy(color_out + y * row, source->color + y * source->stride,
                   row * sizeof(uint16_t));
        }
        if (depth_out != NULL) {
            memcpy(depth_out + y * row, source->depth + y * source->stride,
                   row * sizeof(uint16_t));
        }
    }
    result.color = color_out;
    result.depth = depth_out;
    result.width = source->width;
    result.height = source->height;
    result.stride = row;
    *out = result;
    return PIXELICA_OK;
}

/* Texel index of integer-valued coordinate `x` (double) on an axis of n. */
static size_t fb_axis_index(double x, uint32_t n, PIXELICA_WrapMode wrap)
{
    double dn = (double)n;

    if (wrap == PIXELICA_WRAP_REPEAT) {
        double t = fmod(x, dn);

        if (t < 0.0) {
            t += dn;
        }
        if (t >= dn) {
            t = 0.0;
        }
        return (size_t)t;
    }
    if (x <= 0.0) {
        return 0u;
    }
    if (x >= dn - 1.0) {
        return (size_t)(n - 1u);
    }
    return (size_t)x;
}

/* Common validation for both samplers. */
static int fb_sample_args(const PIXELICA_FrameView *view, float u, float v,
                          PIXELICA_WrapMode wrap_u, PIXELICA_WrapMode wrap_v,
                          const uint16_t *out, int need_depth)
{
    FbRange color = {0u, 0u}, depth = {0u, 0u}, o, d;
    int has_color = 0, has_depth = 0;

    if (view == NULL || out == NULL || !isfinite(u) || !isfinite(v) ||
        (wrap_u != PIXELICA_WRAP_CLAMP && wrap_u != PIXELICA_WRAP_REPEAT) ||
        (wrap_v != PIXELICA_WRAP_CLAMP && wrap_v != PIXELICA_WRAP_REPEAT) ||
        !fb_view_valid(view, &color, &depth, &has_color, &has_depth) ||
        (need_depth ? !has_depth : !has_color)) {
        return 0;
    }
    if (!fb_range(out, sizeof *out, &o)) {
        return 0;
    }
    if ((has_color && fb_overlap(&o, &color)) ||
        (has_depth && fb_overlap(&o, &depth))) {
        return 0;
    }
    if (!fb_range(view, sizeof *view, &d) || fb_overlap(&o, &d)) {
        return 0;
    }
    return 1;
}

static double fb_channel(double c00, double c10, double c01, double c11,
                         double fx, double fy, double max)
{
    double top = c00 * (1.0 - fx) + c10 * fx;
    double bot = c01 * (1.0 - fx) + c11 * fx;
    double r = floor(top * (1.0 - fy) + bot * fy + 0.5);

    return r < 0.0 ? 0.0 : (r > max ? max : r);
}

PIXELICA_Status pixelica_frame_sample_color(const PIXELICA_FrameView *view,
                                            float u, float v,
                                            PIXELICA_WrapMode wrap_u,
                                            PIXELICA_WrapMode wrap_v,
                                            PIXELICA_FilterMode filter,
                                            uint16_t *out)
{
    double sx, sy;

    if ((filter != PIXELICA_FILTER_NEAREST && filter != PIXELICA_FILTER_BILINEAR) ||
        !fb_sample_args(view, u, v, wrap_u, wrap_v, out, 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    sx = (double)u * (double)view->width;
    sy = (double)v * (double)view->height;
    /* Reduce before subtracting half a texel: huge repeated coordinates
     * otherwise lose the half and both neighboring taps to double precision. */
    if (wrap_u == PIXELICA_WRAP_REPEAT) {
        sx = fmod(sx, (double)view->width);
    }
    if (wrap_v == PIXELICA_WRAP_REPEAT) {
        sy = fmod(sy, (double)view->height);
    }

    if (filter == PIXELICA_FILTER_NEAREST) {
        size_t x = fb_axis_index(floor(sx), view->width, wrap_u);
        size_t y = fb_axis_index(floor(sy), view->height, wrap_v);

        *out = view->color[y * view->stride + x];
    } else {
        double px = sx - 0.5, py = sy - 0.5;
        double x0 = floor(px), y0 = floor(py);
        double fx = px - x0, fy = py - y0;
        size_t xa = fb_axis_index(x0, view->width, wrap_u);
        size_t xb = fb_axis_index(x0 + 1.0, view->width, wrap_u);
        size_t ya = fb_axis_index(y0, view->height, wrap_v);
        size_t yb = fb_axis_index(y0 + 1.0, view->height, wrap_v);
        uint16_t t00 = view->color[ya * view->stride + xa];
        uint16_t t10 = view->color[ya * view->stride + xb];
        uint16_t t01 = view->color[yb * view->stride + xa];
        uint16_t t11 = view->color[yb * view->stride + xb];
        uint32_t r = (uint32_t)fb_channel((t00 >> 11) & 31u, (t10 >> 11) & 31u,
                                          (t01 >> 11) & 31u, (t11 >> 11) & 31u,
                                          fx, fy, 31.0);
        uint32_t g = (uint32_t)fb_channel((t00 >> 5) & 63u, (t10 >> 5) & 63u,
                                          (t01 >> 5) & 63u, (t11 >> 5) & 63u,
                                          fx, fy, 63.0);
        uint32_t b = (uint32_t)fb_channel(t00 & 31u, t10 & 31u, t01 & 31u,
                                          t11 & 31u, fx, fy, 31.0);

        *out = (uint16_t)((r << 11) | (g << 5) | b);
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_frame_sample_depth(const PIXELICA_FrameView *view,
                                            float u, float v,
                                            PIXELICA_WrapMode wrap_u,
                                            PIXELICA_WrapMode wrap_v,
                                            uint16_t *out)
{
    size_t x, y;

    if (!fb_sample_args(view, u, v, wrap_u, wrap_v, out, 1)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    x = fb_axis_index(floor((double)u * (double)view->width), view->width, wrap_u);
    y = fb_axis_index(floor((double)v * (double)view->height), view->height, wrap_v);
    *out = view->depth[y * view->stride + x];
    return PIXELICA_OK;
}

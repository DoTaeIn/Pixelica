/*
 * Pixelica - selective bloom over caller-owned RGB565 surfaces (common
 * library; see include/pixelica/light.h and docs/pixel_light.md).
 *
 * What glows is selected EXPLICITLY (an 8-bit emissive mask and/or a
 * separately rendered emissive image), never by thresholding the finished
 * frame. The pipeline is
 *
 *   select + box downsample  ->  N separable box blur passes  ->  bilinear
 *   upsample + ADDITIVE composite
 *
 * so every pixel the glow does not reach keeps its exact value: the frame is
 * never blurred, only added to. The downsampled planes keep 8 bits PER
 * CHANNEL (three uint16 per pixel) instead of being re-packed to RGB565
 * between passes, because a dim glow would otherwise be quantized away in the
 * middle of the chain. Work is O(W*H + dw*dh*radius*passes) and the only
 * memory is the caller's two ping-pong planes.
 */
#include "pixelica/light.h"

#include <math.h>

#include "light_internal.h"

/* ---- RGB565 <-> RGB8 (bit replication, as in modules/shaders) ----------- */

typedef struct BlRgb {
    unsigned int r, g, b;
} BlRgb;

static BlRgb bl_unpack(uint16_t c)
{
    unsigned int r5 = ((unsigned int)c >> 11) & 31u;
    unsigned int g6 = ((unsigned int)c >> 5) & 63u;
    unsigned int b5 = (unsigned int)c & 31u;
    BlRgb out;

    out.r = (r5 << 3) | (r5 >> 2);
    out.g = (g6 << 2) | (g6 >> 4);
    out.b = (b5 << 3) | (b5 >> 2);
    return out;
}

/* (77 R + 150 G + 29 B + 128) / 256 on RGB8, as documented for `threshold`. */
static unsigned int bl_luma(BlRgb c)
{
    return (77u * c.r + 150u * c.g + 29u * c.b + 128u) / 256u;
}

/* round(value * scale / 255) for 8-bit scales. */
static unsigned int bl_scale8(unsigned int value, unsigned int scale)
{
    return (value * scale + 127u) / 255u;
}

/* ---- validation ---------------------------------------------------------- */

/* Elements spanned by a uint16 plane of `height` rows of `stride`; 0 on
 * overflow of the element count or of the byte size. */
static size_t bl_elements(uint16_t width, uint16_t height, size_t stride)
{
    size_t rows = (size_t)height - 1u;
    size_t n;

    if (rows != 0u && stride > (SIZE_MAX - (size_t)width) / rows) {
        return 0u;
    }
    n = rows * stride + (size_t)width;
    return n > SIZE_MAX / sizeof(uint16_t) ? 0u : n;
}

static int bl_dims_ok(uint16_t width, uint16_t height)
{
    return width >= 1u && height >= 1u && width <= PIXELICA_MAX_TEXTURE_DIMENSION &&
           height <= PIXELICA_MAX_TEXTURE_DIMENSION;
}

/* Downsampled size of one axis: ceil(n / factor). */
static size_t bl_down(uint16_t n, uint8_t factor)
{
    return ((size_t)n + (size_t)factor - 1u) / (size_t)factor;
}

/*
 * Everything pixelica_bloom_apply needs to know before it may touch memory,
 * so pixelica_bloom_scratch_pixels() and pixelica_bloom_apply() agree exactly
 * on what "valid" means.
 *
 *   written   the target pixels, the only range this call WRITES
 *   descs     the descriptor objects, which stay live for the whole call
 *   sources   read-only pixel/byte data (mask, emissive image). These MAY
 *             overlap `written`: every source pixel is consumed by the
 *             downsample pass, which writes only scratch, long before the
 *             composite writes the target.
 */
typedef struct BlPlan {
    LtRange written;
    LtRange descs[4];
    LtRange sources[2];
    int desc_count, source_count;
    size_t dw, dh, plane, needed;
} BlPlan;

static int bl_plan(const PIXELICA_LightSurface *target, const PIXELICA_BloomDesc *desc,
                   BlPlan *plan)
{
    size_t elements;

    plan->desc_count = 0;
    plan->source_count = 0;

    if (target == NULL || desc == NULL || target->pixels == NULL ||
        !bl_dims_ok(target->width, target->height) ||
        target->stride < (size_t)target->width) {
        return 0;
    }
    if ((desc->mask == NULL && desc->emissive == NULL) ||
        desc->downsample < 1u || desc->downsample > PIXELICA_BLOOM_MAX_DOWNSAMPLE ||
        desc->radius < 1u || desc->radius > PIXELICA_BLOOM_MAX_RADIUS ||
        desc->passes < 1u || desc->passes > PIXELICA_BLOOM_MAX_PASSES) {
        return 0;
    }
    elements = bl_elements(target->width, target->height, target->stride);
    if (elements == 0u ||
        !lt_range(target->pixels, elements * sizeof(uint16_t), &plan->written)) {
        return 0;
    }
    if (!lt_range(target, sizeof *target, &plan->descs[plan->desc_count++]) ||
        !lt_range(desc, sizeof *desc, &plan->descs[plan->desc_count++])) {
        return 0;
    }

    if (desc->mask != NULL) {
        const PIXELICA_LightMask *m = desc->mask;
        size_t rows = (size_t)target->height - 1u;
        size_t bytes;

        if (m->alpha == NULL || m->width != target->width || m->height != target->height ||
            m->stride < (size_t)target->width) {
            return 0;
        }
        if (rows != 0u && m->stride > (SIZE_MAX - (size_t)m->width) / rows) {
            return 0;
        }
        bytes = rows * m->stride + (size_t)m->width;
        if (!lt_range(m->alpha, bytes, &plan->sources[plan->source_count++]) ||
            !lt_range(m, sizeof *m, &plan->descs[plan->desc_count++])) {
            return 0;
        }
    }
    if (desc->emissive != NULL) {
        const PIXELICA_FrameView *v = desc->emissive;
        size_t view_elements;

        if (v->color == NULL || v->width != target->width || v->height != target->height ||
            v->stride < (size_t)target->width) {
            return 0;
        }
        view_elements = bl_elements(v->width, v->height, v->stride);
        if (view_elements == 0u ||
            !lt_range(v->color, view_elements * sizeof(uint16_t),
                      &plan->sources[plan->source_count++]) ||
            !lt_range(v, sizeof *v, &plan->descs[plan->desc_count++])) {
            return 0;
        }
    }

    /* The composite writes the target while still reading every descriptor,
     * so a descriptor living inside the target pixels would be corrupted (and
     * could redirect the write out of bounds). */
    {
        int i;

        for (i = 0; i < plan->desc_count; ++i) {
            if (lt_overlap(&plan->written, &plan->descs[i])) {
                return 0;
            }
        }
    }

    plan->dw = bl_down(target->width, desc->downsample);
    plan->dh = bl_down(target->height, desc->downsample);
    /* dw, dh <= 4096, so 6 * dw * dh never overflows size_t. */
    plan->plane = 3u * plan->dw * plan->dh;
    plan->needed = 2u * plan->plane;
    return 1;
}

PIXELICA_Status pixelica_bloom_default(PIXELICA_BloomDesc *out)
{
    PIXELICA_BloomDesc d;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    d.mask = NULL;
    d.emissive = NULL;
    d.downsample = 4u;
    d.radius = 3u;
    d.passes = 2u;
    d.intensity = 150u;
    d.threshold = 0u;
    *out = d;
    return PIXELICA_OK;
}

size_t pixelica_bloom_scratch_pixels(const PIXELICA_LightSurface *target,
                                     const PIXELICA_BloomDesc *desc)
{
    BlPlan plan;

    return bl_plan(target, desc, &plan) ? plan.needed : 0u;
}

/* ---- passes -------------------------------------------------------------- */

/*
 * Selection + box downsample into `dst` (dw x dh, three 8-bit channels per
 * pixel). Each destination pixel averages the source pixels of its
 * factor x factor block; an edge block simply has fewer of them.
 */
static void bl_downsample(const PIXELICA_LightSurface *target,
                          const PIXELICA_BloomDesc *desc, uint16_t *dst,
                          size_t dw, size_t dh)
{
    const unsigned int factor = desc->downsample;
    const PIXELICA_LightMask *mask = desc->mask;
    const PIXELICA_FrameView *emissive = desc->emissive;
    const unsigned int threshold = desc->threshold;
    size_t by, bx;

    for (by = 0; by < dh; ++by) {
        size_t y0 = by * factor;
        size_t y1 = y0 + factor;

        if (y1 > (size_t)target->height) {
            y1 = (size_t)target->height;
        }
        for (bx = 0; bx < dw; ++bx) {
            size_t x0 = bx * factor;
            size_t x1 = x0 + factor;
            unsigned long sum_r = 0u, sum_g = 0u, sum_b = 0u, count = 0u;
            uint16_t *slot = &dst[(by * dw + bx) * 3u];
            size_t y, x;

            if (x1 > (size_t)target->width) {
                x1 = (size_t)target->width;
            }
            for (y = y0; y < y1; ++y) {
                for (x = x0; x < x1; ++x) {
                    BlRgb base = bl_unpack(emissive != NULL
                                               ? emissive->color[y * emissive->stride + x]
                                               : target->pixels[y * target->stride + x]);
                    unsigned int weight =
                        mask != NULL ? (unsigned int)mask->alpha[y * mask->stride + x] : 255u;

                    if (threshold != 0u && bl_luma(base) < threshold) {
                        weight = 0u;
                    }
                    if (weight != 0u) {
                        sum_r += bl_scale8(base.r, weight);
                        sum_g += bl_scale8(base.g, weight);
                        sum_b += bl_scale8(base.b, weight);
                    }
                    ++count;
                }
            }
            /* count >= 1: a block exists only where the target has pixels. */
            slot[0] = (uint16_t)((sum_r + count / 2u) / count);
            slot[1] = (uint16_t)((sum_g + count / 2u) / count);
            slot[2] = (uint16_t)((sum_b + count / 2u) / count);
        }
    }
}

/*
 * One separable box blur of 2 * radius + 1 taps, edge-clamped, over the
 * 8-bit-per-channel planes. `horizontal` selects the axis; src and dst are
 * distinct planes.
 */
static void bl_blur(const uint16_t *src, uint16_t *dst, size_t dw, size_t dh,
                    unsigned int radius, int horizontal)
{
    const unsigned long taps = 2ul * (unsigned long)radius + 1ul;
    size_t y, x;

    for (y = 0; y < dh; ++y) {
        for (x = 0; x < dw; ++x) {
            unsigned long sum[3];
            long k;
            unsigned int c;

            sum[0] = 0u;
            sum[1] = 0u;
            sum[2] = 0u;
            for (k = -(long)radius; k <= (long)radius; ++k) {
                size_t sx = x, sy = y;
                const uint16_t *tap;

                if (horizontal) {
                    long p = (long)x + k;

                    sx = p < 0 ? 0u : ((size_t)p >= dw ? dw - 1u : (size_t)p);
                } else {
                    long p = (long)y + k;

                    sy = p < 0 ? 0u : ((size_t)p >= dh ? dh - 1u : (size_t)p);
                }
                tap = &src[(sy * dw + sx) * 3u];
                sum[0] += tap[0];
                sum[1] += tap[1];
                sum[2] += tap[2];
            }
            for (c = 0; c < 3u; ++c) {
                dst[(y * dw + x) * 3u + c] = (uint16_t)((sum[c] + taps / 2u) / taps);
            }
        }
    }
}

/* Bilinear tap of the 8-bit-per-channel plane, edge-clamped. */
static void bl_sample(const uint16_t *plane, size_t dw, size_t dh, double sx, double sy,
                      double out[3])
{
    double fx0 = floor(sx), fy0 = floor(sy);
    double fx = sx - fx0, fy = sy - fy0;
    size_t x0, x1, y0, y1;
    const uint16_t *c00, *c10, *c01, *c11;
    unsigned int i;

    x0 = fx0 < 0.0 ? 0u : ((double)(dw - 1u) < fx0 ? dw - 1u : (size_t)fx0);
    y0 = fy0 < 0.0 ? 0u : ((double)(dh - 1u) < fy0 ? dh - 1u : (size_t)fy0);
    x1 = x0 + 1u >= dw ? dw - 1u : x0 + 1u;
    y1 = y0 + 1u >= dh ? dh - 1u : y0 + 1u;
    if (fx0 < 0.0) {
        fx = 0.0; /* clamped left of the first column: the tap is that column */
    }
    if (fy0 < 0.0) {
        fy = 0.0;
    }
    c00 = &plane[(y0 * dw + x0) * 3u];
    c10 = &plane[(y0 * dw + x1) * 3u];
    c01 = &plane[(y1 * dw + x0) * 3u];
    c11 = &plane[(y1 * dw + x1) * 3u];
    for (i = 0; i < 3u; ++i) {
        double top = (double)c00[i] * (1.0 - fx) + (double)c10[i] * fx;
        double bottom = (double)c01[i] * (1.0 - fx) + (double)c11[i] * fx;

        out[i] = top * (1.0 - fy) + bottom * fy;
    }
}

/*
 * Bilinear upsample and additive composite. The RGB8 glow value is converted
 * to native channel steps with ONE rounding
 *   add = floor(value * intensity / 255 * max / 255 + 0.5),  max = 31/63/31
 * and added saturating, which is the renderer's ADD rule with the rounding
 * done before the native quantization (so a dim glow is not truncated away).
 */
static void bl_composite(PIXELICA_LightSurface *target, const uint16_t *plane,
                         size_t dw, size_t dh, unsigned int factor, unsigned int intensity)
{
    static const double k_max[3] = {31.0, 63.0, 31.0};
    uint16_t *pixels = target->pixels;
    const size_t stride = target->stride;
    const size_t width = (size_t)target->width;
    const size_t height = (size_t)target->height;
    const double scale = (double)intensity / 255.0;
    size_t y, x;

    for (y = 0; y < height; ++y) {
        double sy = ((double)y + 0.5) / (double)factor - 0.5;

        for (x = 0; x < width; ++x) {
            double sx = ((double)x + 0.5) / (double)factor - 0.5;
            uint16_t *pixel = &pixels[y * stride + x];
            unsigned int channel[3];
            double glow[3];
            unsigned int i;

            channel[0] = ((unsigned int)*pixel >> 11) & 31u;
            channel[1] = ((unsigned int)*pixel >> 5) & 63u;
            channel[2] = (unsigned int)*pixel & 31u;
            bl_sample(plane, dw, dh, sx, sy, glow);
            for (i = 0; i < 3u; ++i) {
                double add = floor(glow[i] * scale * k_max[i] / 255.0 + 0.5);

                if (add > 0.0) {
                    channel[i] += (unsigned int)add;
                    if ((double)channel[i] > k_max[i]) {
                        channel[i] = (unsigned int)k_max[i];
                    }
                }
            }
            *pixel = (uint16_t)((channel[0] << 11) | (channel[1] << 5) | channel[2]);
        }
    }
}

PIXELICA_Status pixelica_bloom_apply(PIXELICA_LightSurface *target,
                                     const PIXELICA_BloomDesc *desc,
                                     uint16_t *scratch, size_t scratch_count)
{
    BlPlan plan;
    LtRange scratch_range;
    int i;
    unsigned int pass;

    if (scratch == NULL || !bl_plan(target, desc, &plan)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (scratch_count < plan.needed) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (!lt_range(scratch, plan.needed * sizeof(uint16_t), &scratch_range)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (lt_overlap(&scratch_range, &plan.written)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < plan.desc_count; ++i) {
        if (lt_overlap(&scratch_range, &plan.descs[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    for (i = 0; i < plan.source_count; ++i) {
        if (lt_overlap(&scratch_range, &plan.sources[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    if (desc->intensity == 0u) {
        return PIXELICA_OK; /* nothing to add: no work, no writes */
    }

    bl_downsample(target, desc, scratch, plan.dw, plan.dh);
    for (pass = 0; pass < desc->passes; ++pass) {
        bl_blur(scratch, scratch + plan.plane, plan.dw, plan.dh, desc->radius, 1);
        bl_blur(scratch + plan.plane, scratch, plan.dw, plan.dh, desc->radius, 0);
    }
    bl_composite(target, scratch, plan.dw, plan.dh, desc->downsample, desc->intensity);
    return PIXELICA_OK;
}

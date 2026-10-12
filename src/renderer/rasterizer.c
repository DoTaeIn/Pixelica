/*
 * Pixelica - integer edge-function triangle rasterizer.
 *
 * Coordinates are fixed point (PIXELICA_SUBPIXEL_BITS fractional bits), y down.
 * After orientation normalisation the signed area is positive, and a point
 * p is inside when every edge function
 *     E_ab(p) = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)
 * is >= 0. With this orientation (clockwise on a y-down screen):
 *     top edge  : dy == 0 && dx > 0   (interior below)
 *     left edge : dy < 0
 * A center lying exactly on an edge (E == 0) is covered only for top/left
 * edges, implemented as a -1 bias on the other edges.
 *
 * Range: |coords| <= 4096 << 4 = 2^16, so each product is < 2^34 and every
 * running edge value fits comfortably in int64_t.
 *
 * Texturing: the unbiased edge values E0, E1, E2 at a pixel center are the
 * exact (integer) barycentric weights of v0, v1, v2 times twice the area.
 * The perspective-correct coordinate is
 *     u = (E0*uq0 + E1*uq1 + E2*uq2) / (E0*q0 + E1*q1 + E2*q2)
 * where q = 1/w and uq = u/w; the area factor cancels, so no per-triangle
 * gradients (which could overflow for slivers) are needed. CLAMP axes keep
 * that float evaluation unchanged; REPEAT axes evaluate it in double.
 * Additional layers sample their own uq/vq the same way; vertex colors use
 * the same ratio in double. Shades without layers or vertex colors take the
 * original path unchanged. Bilinear shades evaluate the ratio in double and
 * mix four taps; the pixel hook runs only for depth-passing fragments.
 */
#include <math.h>
#include <stddef.h>

#include "raster.h"

#include "pixelica/renderer.h"

#define PIXELICA_SUB_ONE ((int32_t)1 << PIXELICA_SUBPIXEL_BITS)
#define PIXELICA_SUB_HALF ((int32_t)1 << (PIXELICA_SUBPIXEL_BITS - 1))

static int32_t pixelica_min3_i32(int32_t a, int32_t b, int32_t c)
{
    int32_t m = a < b ? a : b;
    return m < c ? m : c;
}

static int32_t pixelica_max3_i32(int32_t a, int32_t b, int32_t c)
{
    int32_t m = a > b ? a : b;
    return m > c ? m : c;
}

static float pixelica_min3_f(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

static float pixelica_max3_f(float a, float b, float c)
{
    float m = a > b ? a : b;
    return m > c ? m : c;
}

static int64_t pixelica_edge_bias(const PIXELICA_ScreenVertex *a, const PIXELICA_ScreenVertex *b)
{
    int32_t dx = b->x - a->x;
    int32_t dy = b->y - a->y;
    int top_left = (dy < 0) || (dy == 0 && dx > 0);
    return top_left ? 0 : -1;
}

/* First pixel index whose center (i * ONE + HALF) is >= v. */
static int32_t pixelica_first_center_at_or_after(int32_t v)
{
    if (v <= PIXELICA_SUB_HALF) {
        return 0;
    }
    return (v - PIXELICA_SUB_HALF + PIXELICA_SUB_ONE - 1) / PIXELICA_SUB_ONE;
}

/* Nearest texel index for a coordinate clamped to [0, 1]; NaN -> 0. */
static int32_t pixelica_texel_index(float t, int32_t size)
{
    int32_t i;

    if (!(t > 0.0f)) {
        return 0;
    }
    if (!(t < 1.0f)) {
        return size - 1;
    }
    i = (int32_t)(t * (float)size); /* in [0, size]: conversion is safe */
    return i < size ? i : size - 1;
}

/* Wrapped texel index for t: t - floor(t) in [0, 1] (exact integers -> 0).
 * The ratio is evaluated in double: for finite float vertex data numerator
 * and denominator stay finite, and NaN or a non-positive denominator maps
 * to texel 0, so no float->int conversion can be out of range. */
static int32_t pixelica_repeat_index(double num, double den, int32_t size)
{
    double t;

    if (!(den > 0.0)) {
        return 0;
    }
    t = num / den;
    if (!(t - t == 0.0)) { /* inf or NaN */
        return 0;
    }
    t = (t - floor(t)) * (double)size; /* tiny negative t may round to size */
    if (!(t > 0.0)) {
        return 0;
    }
    return t < (double)size ? (int32_t)t : size - 1;
}

/* Per-triangle attributes after orientation normalization, indexed by
 * vertex: q, premultiplied base UV and the extra attributes. */
typedef struct PIXELICA_TriAttrs {
    float q[3], uq[3], vq[3];
    float xq[PIXELICA_RASTER_EXTRA_ATTRS][3];
} PIXELICA_TriAttrs;

/* Texel offset for barycentric weights e0..e2 (non-negative inside). The
 * clamp path is the original float evaluation, bit for bit; color and alpha
 * are both fetched at this one offset. */
static size_t pixelica_texel_offset(const float q[3], const float uq[3],
                                    const float vq[3], int64_t e0, int64_t e1,
                                    int64_t e2, int32_t tex_width,
                                    int32_t tex_height, int repeat_u,
                                    int repeat_v)
{
    const float f0 = (float)e0;
    const float f1 = (float)e1;
    const float f2 = (float)e2;
    const float den = f0 * q[0] + f1 * q[1] + f2 * q[2];
    int32_t tx = 0;
    int32_t ty = 0;

    if (den > 0.0f) {
        if (!repeat_u) {
            tx = pixelica_texel_index((f0 * uq[0] + f1 * uq[1] + f2 * uq[2]) / den,
                                 tex_width);
        }
        if (!repeat_v) {
            ty = pixelica_texel_index((f0 * vq[0] + f1 * vq[1] + f2 * vq[2]) / den,
                                 tex_height);
        }
    }
    if (repeat_u || repeat_v) {
        const double d0 = (double)e0;
        const double d1 = (double)e1;
        const double d2 = (double)e2;
        const double dden = d0 * q[0] + d1 * q[1] + d2 * q[2];
        if (repeat_u) {
            tx = pixelica_repeat_index(d0 * uq[0] + d1 * uq[1] + d2 * uq[2],
                                  dden, tex_width);
        }
        if (repeat_v) {
            ty = pixelica_repeat_index(d0 * vq[0] + d1 * vq[1] + d2 * vq[2],
                                  dden, tex_height);
        }
    }
    return (size_t)ty * (size_t)tex_width + (size_t)tx;
}

/* Bilinear taps: texel offsets of (i0,j0), (i1,j0), (i0,j1), (i1,j1) and
 * the fractional weights (renderer.h, PIXELICA_MeshEffects). */
typedef struct PIXELICA_BilinearTaps {
    size_t off[4];
    double fx, fy;
} PIXELICA_BilinearTaps;

/* One axis: any double t (non-finite -> 0) gives two in-range taps. */
static void pixelica_bilinear_axis(double t, int32_t size, int repeat,
                                   int32_t *i0, int32_t *i1, double *f)
{
    double s;
    double fl;
    int32_t a, b;

    if (!(t - t == 0.0)) { /* inf or NaN */
        t = 0.0;
    }
    if (repeat) {
        t -= floor(t); /* [0, 1]; 1 (rounding) samples like 0 */
    } else if (t < -1.0) {
        t = -1.0; /* both taps already clamp to 0 */
    } else if (t > 2.0) {
        t = 2.0; /* both taps already clamp to size - 1 */
    }
    s = t * (double)size - 0.5;
    fl = floor(s);
    *f = s - fl;
    a = (int32_t)fl; /* |fl| <= 2 * size: in range */
    b = a + 1;
    if (repeat) {
        a = a < 0 ? a + size : (a >= size ? a - size : a);
        b = b < 0 ? b + size : (b >= size ? b - size : b);
    } else {
        a = a < 0 ? 0 : (a > size - 1 ? size - 1 : a);
        b = b < 0 ? 0 : (b > size - 1 ? size - 1 : b);
    }
    *i0 = a;
    *i1 = b;
}

static PIXELICA_BilinearTaps pixelica_bilinear_taps(
    const float q[3], const float uq[3], const float vq[3], int64_t e0,
    int64_t e1, int64_t e2, int32_t tex_width, int32_t tex_height,
    int repeat_u, int repeat_v)
{
    const double d0 = (double)e0;
    const double d1 = (double)e1;
    const double d2 = (double)e2;
    const double den = d0 * q[0] + d1 * q[1] + d2 * q[2];
    PIXELICA_BilinearTaps tp;
    double u = 0.0;
    double v = 0.0;
    int32_t x0, x1, y0, y1;

    if (den > 0.0) {
        u = (d0 * uq[0] + d1 * uq[1] + d2 * uq[2]) / den;
        v = (d0 * vq[0] + d1 * vq[1] + d2 * vq[2]) / den;
    }
    pixelica_bilinear_axis(u, tex_width, repeat_u, &x0, &x1, &tp.fx);
    pixelica_bilinear_axis(v, tex_height, repeat_v, &y0, &y1, &tp.fy);
    tp.off[0] = (size_t)y0 * (size_t)tex_width + (size_t)x0;
    tp.off[1] = (size_t)y0 * (size_t)tex_width + (size_t)x1;
    tp.off[2] = (size_t)y1 * (size_t)tex_width + (size_t)x0;
    tp.off[3] = (size_t)y1 * (size_t)tex_width + (size_t)x1;
    return tp;
}

/* Bilinear mix of four channel values, rounded to nearest (never above the
 * largest tap: a convex combination). */
static uint32_t pixelica_bilerp(uint32_t c00, uint32_t c10, uint32_t c01,
                                uint32_t c11, double fx, double fy)
{
    const double top = (double)c00 * (1.0 - fx) + (double)c10 * fx;
    const double bot = (double)c01 * (1.0 - fx) + (double)c11 * fx;
    return (uint32_t)floor(top * (1.0 - fy) + bot * fy + 0.5);
}

static uint16_t pixelica_bilinear565(const uint16_t *texels,
                                     const PIXELICA_BilinearTaps *tp)
{
    const uint16_t a = texels[tp->off[0]], b = texels[tp->off[1]];
    const uint16_t c = texels[tp->off[2]], d = texels[tp->off[3]];
    const uint32_t r = pixelica_bilerp((a >> 11) & 31u, (b >> 11) & 31u,
                                       (c >> 11) & 31u, (d >> 11) & 31u,
                                       tp->fx, tp->fy);
    const uint32_t g = pixelica_bilerp((a >> 5) & 63u, (b >> 5) & 63u,
                                       (c >> 5) & 63u, (d >> 5) & 63u,
                                       tp->fx, tp->fy);
    const uint32_t bl = pixelica_bilerp(a & 31u, b & 31u, c & 31u, d & 31u,
                                        tp->fx, tp->fy);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static uint32_t pixelica_bilinear_alpha(const uint8_t *alpha,
                                        const PIXELICA_BilinearTaps *tp)
{
    return pixelica_bilerp(alpha[tp->off[0]], alpha[tp->off[1]],
                           alpha[tp->off[2]], alpha[tp->off[3]], tp->fx,
                           tp->fy);
}

/* Per native 5/6/5 channel: (s*a + d*(255-a) + 127) / 255. */
static uint16_t pixelica_blend565(uint16_t s, uint16_t d, uint32_t a)
{
    const uint32_t ia = 255u - a;
    const uint32_t r = (((s >> 11) & 31u) * a + ((d >> 11) & 31u) * ia + 127u) / 255u;
    const uint32_t g = (((s >> 5) & 63u) * a + ((d >> 5) & 63u) * ia + 127u) / 255u;
    const uint32_t b = ((s & 31u) * a + (d & 31u) * ia + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Layer combine of texel t onto previous color p, then mixed by opacity. */
static uint16_t pixelica_combine565(uint16_t p, uint16_t t,
                                    PIXELICA_TextureCombine combine,
                                    uint32_t opacity)
{
    const uint32_t pr = (p >> 11) & 31u, pg = (p >> 5) & 63u, pb = p & 31u;
    const uint32_t tr = (t >> 11) & 31u, tg = (t >> 5) & 63u, tb = t & 31u;
    uint32_t r, g, b;

    if (combine == PIXELICA_TEXTURE_MULTIPLY) {
        r = (pr * tr + 15u) / 31u;
        g = (pg * tg + 31u) / 63u;
        b = (pb * tb + 15u) / 31u;
    } else if (combine == PIXELICA_TEXTURE_ADD) {
        r = pr + tr < 31u ? pr + tr : 31u;
        g = pg + tg < 63u ? pg + tg : 63u;
        b = pb + tb < 31u ? pb + tb : 31u;
    } else { /* PIXELICA_TEXTURE_LERP */
        r = tr;
        g = tg;
        b = tb;
    }
    return pixelica_blend565((uint16_t)((r << 11) | (g << 5) | b), p, opacity);
}

/* floor(x * c / 255 + 0.5) for x <= 255 and c in [0, 255]: never exceeds x,
 * and c == 255 returns x exactly. */
static uint32_t pixelica_modulate(uint32_t x, double c)
{
    return (uint32_t)((double)x * c / 255.0 + 0.5);
}

/* Layers then vertex color (raster.h). `alpha` is NULL for OPAQUE, else the
 * base alpha, multiplied in place by the vertex alpha. */
static uint16_t pixelica_shade_extras(const PIXELICA_RasterShade *shade,
                                      const PIXELICA_TriAttrs *t, int64_t e0,
                                      int64_t e1, int64_t e2, uint16_t src,
                                      uint32_t *alpha)
{
    uint32_t k;

    for (k = 0; k < shade->layer_count; ++k) {
        const PIXELICA_RasterLayer *l = &shade->layers[k];
        uint16_t texel;

        if (shade->bilinear) {
            const PIXELICA_BilinearTaps tp = pixelica_bilinear_taps(
                t->q, t->xq[2u * k], t->xq[2u * k + 1u], e0, e1, e2,
                l->tex_width, l->tex_height, l->repeat_u, l->repeat_v);
            texel = pixelica_bilinear565(l->texels, &tp);
        } else {
            const size_t off = pixelica_texel_offset(
                t->q, t->xq[2u * k], t->xq[2u * k + 1u], e0, e1, e2,
                l->tex_width, l->tex_height, l->repeat_u, l->repeat_v);
            texel = l->texels[off];
        }
        src = pixelica_combine565(src, texel, l->combine, l->opacity);
    }
    if (shade->vertex_colors) {
        const double d0 = (double)e0;
        const double d1 = (double)e1;
        const double d2 = (double)e2;
        const double den = d0 * t->q[0] + d1 * t->q[1] + d2 * t->q[2];
        double c[4];
        uint32_t i;

        for (i = 0; i < 4u; ++i) {
            const float *x = t->xq[PIXELICA_RASTER_COLOR_ATTR + i];
            double v = den > 0.0 ? (d0 * x[0] + d1 * x[1] + d2 * x[2]) / den : 0.0;
            if (!(v > 0.0)) { /* also NaN */
                v = 0.0;
            }
            c[i] = v < 255.0 ? v : 255.0;
        }
        src = (uint16_t)((pixelica_modulate((src >> 11) & 31u, c[0]) << 11) |
                         (pixelica_modulate((src >> 5) & 63u, c[1]) << 5) |
                         pixelica_modulate(src & 31u, c[2]));
        if (alpha != NULL) {
            *alpha = pixelica_modulate(*alpha, c[3]);
        }
    }
    return src;
}

void pixelica_raster_triangle(const PIXELICA_RasterTarget *target,
                         const PIXELICA_ScreenVertex *a,
                         const PIXELICA_ScreenVertex *b,
                         const PIXELICA_ScreenVertex *c,
                         int expected_sign, const PIXELICA_RasterShade *shade,
                         PIXELICA_RasterCounts *counts)
{
    PIXELICA_ScreenVertex v0, v1, v2;
    int64_t area;
    int32_t minx, maxx, miny, maxy;
    int32_t px0, px1, py0, py1;
    int32_t px, py;
    int64_t bias0, bias1, bias2;
    int64_t e0_dx, e1_dx, e2_dx; /* change per +1 pixel in x */
    int64_t e0_dy, e1_dy, e2_dy; /* change per +1 pixel in y */
    int64_t e0_row, e1_row, e2_row;
    int32_t sx0, sy0;
    float dz1, dz2, fdx1, fdy1, fdx2, fdy2, farea;
    float dzdx, dzdy, zmin, zmax;
    uint32_t written = 0;
    uint32_t covered = 0;
    uint32_t newly = 0;
    uint16_t flat_color;
    PIXELICA_AlphaMode mode;
    int textured;
    int extras;
    PIXELICA_TriAttrs tri;
    uint32_t k;

    if (counts == NULL) {
        return;
    }
    counts->written = 0;
    counts->covered = 0;
    counts->newly_covered = 0;
    if (target == NULL || target->coverage == NULL || a == NULL || b == NULL ||
        c == NULL || shade == NULL) {
        return;
    }
    flat_color = shade->color;
    mode = shade->alpha_mode;
    textured = shade->texels != NULL && shade->tex_width > 0 &&
               shade->tex_height > 0;

    v0 = *a;
    v1 = *b;
    v2 = *c;
    area = (int64_t)(v1.x - v0.x) * (int64_t)(v2.y - v0.y) -
           (int64_t)(v1.y - v0.y) * (int64_t)(v2.x - v0.x);
    if (area == 0) {
        return;
    }
    if ((area > 0 ? 1 : -1) != expected_sign) {
        return; /* sliver flipped by snapping: treat as degenerate */
    }
    if (area < 0) {
        PIXELICA_ScreenVertex t = v1;
        v1 = v2;
        v2 = t;
        area = -area;
    }
    extras = shade->layer_count != 0u || shade->vertex_colors;
    tri.q[0] = v0.q;
    tri.q[1] = v1.q;
    tri.q[2] = v2.q;
    tri.uq[0] = v0.uq;
    tri.uq[1] = v1.uq;
    tri.uq[2] = v2.uq;
    tri.vq[0] = v0.vq;
    tri.vq[1] = v1.vq;
    tri.vq[2] = v2.vq;
    for (k = 0; k < PIXELICA_RASTER_EXTRA_ATTRS; ++k) {
        tri.xq[k][0] = v0.xq[k];
        tri.xq[k][1] = v1.xq[k];
        tri.xq[k][2] = v2.xq[k];
    }

    minx = pixelica_min3_i32(v0.x, v1.x, v2.x);
    maxx = pixelica_max3_i32(v0.x, v1.x, v2.x);
    miny = pixelica_min3_i32(v0.y, v1.y, v2.y);
    maxy = pixelica_max3_i32(v0.y, v1.y, v2.y);
    if (maxx < PIXELICA_SUB_HALF || maxy < PIXELICA_SUB_HALF) {
        return;
    }
    px0 = pixelica_first_center_at_or_after(minx);
    py0 = pixelica_first_center_at_or_after(miny);
    px1 = (maxx - PIXELICA_SUB_HALF) / PIXELICA_SUB_ONE;
    py1 = (maxy - PIXELICA_SUB_HALF) / PIXELICA_SUB_ONE;
    /* Defensive clamp: never touch memory outside the target. */
    if (px1 > target->width - 1) {
        px1 = target->width - 1;
    }
    if (py1 > target->height - 1) {
        py1 = target->height - 1;
    }
    if (px0 > px1 || py0 > py1) {
        return;
    }

    /* Edge i is opposite vertex i: e0 = v1->v2, e1 = v2->v0, e2 = v0->v1. */
    bias0 = pixelica_edge_bias(&v1, &v2);
    bias1 = pixelica_edge_bias(&v2, &v0);
    bias2 = pixelica_edge_bias(&v0, &v1);

    e0_dx = -(int64_t)(v2.y - v1.y) * PIXELICA_SUB_ONE;
    e1_dx = -(int64_t)(v0.y - v2.y) * PIXELICA_SUB_ONE;
    e2_dx = -(int64_t)(v1.y - v0.y) * PIXELICA_SUB_ONE;
    e0_dy = (int64_t)(v2.x - v1.x) * PIXELICA_SUB_ONE;
    e1_dy = (int64_t)(v0.x - v2.x) * PIXELICA_SUB_ONE;
    e2_dy = (int64_t)(v1.x - v0.x) * PIXELICA_SUB_ONE;

    sx0 = px0 * PIXELICA_SUB_ONE + PIXELICA_SUB_HALF;
    sy0 = py0 * PIXELICA_SUB_ONE + PIXELICA_SUB_HALF;
    e0_row = (int64_t)(v2.x - v1.x) * (int64_t)(sy0 - v1.y) -
             (int64_t)(v2.y - v1.y) * (int64_t)(sx0 - v1.x) + bias0;
    e1_row = (int64_t)(v0.x - v2.x) * (int64_t)(sy0 - v2.y) -
             (int64_t)(v0.y - v2.y) * (int64_t)(sx0 - v2.x) + bias1;
    e2_row = (int64_t)(v1.x - v0.x) * (int64_t)(sy0 - v0.y) -
             (int64_t)(v1.y - v0.y) * (int64_t)(sx0 - v0.x) + bias2;

    /* Depth plane z = z0 + dzdx*(x - x0) + dzdy*(y - y0), in fixed units. */
    fdx1 = (float)(v1.x - v0.x);
    fdy1 = (float)(v1.y - v0.y);
    fdx2 = (float)(v2.x - v0.x);
    fdy2 = (float)(v2.y - v0.y);
    dz1 = v1.z - v0.z;
    dz2 = v2.z - v0.z;
    farea = (float)area;
    dzdx = (dz1 * fdy2 - dz2 * fdy1) / farea;
    dzdy = (fdx1 * dz2 - fdx2 * dz1) / farea;
    zmin = pixelica_min3_f(v0.z, v1.z, v2.z);
    zmax = pixelica_max3_f(v0.z, v1.z, v2.z);

    for (py = py0; py <= py1; ++py) {
        int64_t w0 = e0_row;
        int64_t w1 = e1_row;
        int64_t w2 = e2_row;
        int32_t cy = py * PIXELICA_SUB_ONE + PIXELICA_SUB_HALF;
        float zrow = v0.z + dzdy * (float)(cy - v0.y);
        uint16_t *crow = target->color + (size_t)py * (size_t)target->width;
        uint16_t *drow = target->depth + (size_t)py * (size_t)target->width;
        const size_t row_base = (size_t)py * (size_t)target->width;

        for (px = px0; px <= px1; ++px) {
            if ((w0 | w1 | w2) >= 0) {
                int32_t cx = px * PIXELICA_SUB_ONE + PIXELICA_SUB_HALF;
                float z = zrow + dzdx * (float)(cx - v0.x);
                uint16_t d;

                if (!(z >= zmin)) { /* also catches NaN */
                    z = zmin;
                }
                if (z > zmax) {
                    z = zmax;
                }
                d = (uint16_t)(z + 0.5f);
                if (d > PIXELICA_DEPTH_MAX_DRAWABLE) {
                    d = PIXELICA_DEPTH_MAX_DRAWABLE;
                }
                ++covered;
                {
                    const size_t bit = row_base + (size_t)px;
                    const unsigned char m = (unsigned char)(1u << (bit & 7u));
                    if ((target->coverage[bit >> 3] & m) == 0) {
                        target->coverage[bit >> 3] |= m;
                        ++newly; /* first fragment on this pixel this frame */
                    }
                }
                if (d < drow[px]) { /* early depth reject: nothing sampled */
                    uint16_t src = flat_color;
                    uint32_t alpha = 255u;
                    int keep = 1;

                    if (textured && shade->bilinear) {
                        const PIXELICA_BilinearTaps tp = pixelica_bilinear_taps(
                            tri.q, tri.uq, tri.vq, w0 - bias0, w1 - bias1,
                            w2 - bias2, shade->tex_width, shade->tex_height,
                            shade->repeat_u, shade->repeat_v);
                        src = pixelica_bilinear565(shade->texels, &tp);
                        if (shade->alpha != NULL) {
                            alpha = pixelica_bilinear_alpha(shade->alpha, &tp);
                        }
                    } else if (textured) {
                        const size_t off = pixelica_texel_offset(
                            tri.q, tri.uq, tri.vq, w0 - bias0, w1 - bias1,
                            w2 - bias2, shade->tex_width, shade->tex_height,
                            shade->repeat_u, shade->repeat_v);
                        src = shade->texels[off];
                        if (shade->alpha != NULL) {
                            alpha = shade->alpha[off];
                        }
                    }
                    if (extras) {
                        src = pixelica_shade_extras(
                            shade, &tri, w0 - bias0, w1 - bias1, w2 - bias2,
                            src, mode != PIXELICA_ALPHA_OPAQUE ? &alpha : NULL);
                    }
                    if (mode != PIXELICA_ALPHA_OPAQUE) {
                        alpha = (alpha * shade->opacity + 127u) / 255u;
                    }
                    if (shade->pixel != NULL) {
                        PIXELICA_Fragment frag;
                        uint16_t out_color = src;
                        uint8_t out_alpha = (uint8_t)alpha;

                        frag.x = (uint16_t)px;
                        frag.y = (uint16_t)py;
                        frag.depth = d;
                        frag.color = src;
                        frag.alpha = (uint8_t)alpha;
                        if (shade->pixel(shade->pixel_userdata, &frag,
                                         &out_color, &out_alpha)) {
                            src = out_color;
                            if (mode != PIXELICA_ALPHA_OPAQUE) {
                                alpha = out_alpha;
                            }
                        } else {
                            keep = 0;
                        }
                    }
                    if (keep && mode != PIXELICA_ALPHA_OPAQUE) {
                        keep = mode == PIXELICA_ALPHA_TEST
                                   ? alpha >= shade->alpha_cutoff
                                   : alpha != 0u;
                    }
                    if (keep) {
                        if (mode == PIXELICA_ALPHA_BLEND) {
                            if (alpha < 255u) {
                                src = pixelica_blend565(src, crow[px], alpha);
                            }
                        } else {
                            drow[px] = d;
                        }
                        crow[px] = src;
                        ++written;
                    }
                }
            }
            w0 += e0_dx;
            w1 += e1_dx;
            w2 += e2_dx;
        }
        e0_row += e0_dy;
        e1_row += e1_dy;
        e2_row += e2_dy;
    }
    counts->written = written;
    counts->covered = covered;
    counts->newly_covered = newly;
}

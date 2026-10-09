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
 * gradients (which could overflow for slivers) are needed.
 */
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

/* Texel for barycentric weights e0..e2 (non-negative inside). */
static uint16_t pixelica_sample(const PIXELICA_RasterShade *shade,
                           const PIXELICA_ScreenVertex *v0,
                           const PIXELICA_ScreenVertex *v1,
                           const PIXELICA_ScreenVertex *v2,
                           int64_t e0, int64_t e1, int64_t e2)
{
    const float f0 = (float)e0;
    const float f1 = (float)e1;
    const float f2 = (float)e2;
    const float den = f0 * v0->q + f1 * v1->q + f2 * v2->q;
    int32_t tx = 0;
    int32_t ty = 0;

    if (den > 0.0f) {
        const float u = (f0 * v0->uq + f1 * v1->uq + f2 * v2->uq) / den;
        const float v = (f0 * v0->vq + f1 * v1->vq + f2 * v2->vq) / den;
        tx = pixelica_texel_index(u, shade->tex_width);
        ty = pixelica_texel_index(v, shade->tex_height);
    }
    return shade->texels[(size_t)ty * (size_t)shade->tex_width + (size_t)tx];
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
    int textured;

    if (counts == NULL) {
        return;
    }
    counts->written = 0;
    counts->covered = 0;
    counts->newly_covered = 0;
    if (target == NULL || a == NULL || b == NULL || c == NULL ||
        shade == NULL) {
        return;
    }
    flat_color = shade->color;
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
                if (drow[px] == PIXELICA_DEPTH_CLEAR) {
                    ++newly; /* first fragment on this pixel this frame */
                }
                if (d < drow[px]) {
                    drow[px] = d;
                    if (textured) {
                        crow[px] = pixelica_sample(shade, &v0, &v1, &v2,
                                              w0 - bias0, w1 - bias1,
                                              w2 - bias2);
                    } else {
                        crow[px] = flat_color;
                    }
                    ++written;
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

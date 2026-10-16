/*
 * Pixelica - light/shadow shape masks, flat tint images and receiver-plane
 * quads (common library; see include/pixelica/light.h and docs/pixel_light.md).
 *
 * Pure CPU math over caller-owned buffers: no heap, no global state and no
 * dependency on either renderer. Shapes are evaluated in double and stored as
 * 8-bit coverage, so the result is identical on every platform with IEEE-754
 * doubles.
 */
#include "pixelica/light.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "light_internal.h"

/* ---- scalar helpers ------------------------------------------------------ */

/* Rounds `v` to float, rejecting NaN and anything outside the float range
 * (an out-of-range double-to-float conversion is undefined in C). */
static int lt_to_float(double v, float *out)
{
    if (!(v >= -(double)FLT_MAX && v <= (double)FLT_MAX)) {
        return 0;
    }
    *out = (float)v;
    return 1;
}

static double lt_clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/*
 * Soft edge: 1 well inside the shape, 0 outside it, smoothstep in between.
 * `d` is the distance to the border as a fraction of the half extent.
 */
static double lt_edge(double d, double softness)
{
    double t;

    if (softness <= 0.0) {
        return d >= 0.0 ? 1.0 : 0.0;
    }
    t = d / softness;
    if (t <= 0.0) {
        return 0.0;
    }
    if (t >= 1.0) {
        return 1.0;
    }
    return t * t * (3.0 - 2.0 * t);
}

/*
 * Window bar factor at pane coordinate `p` (0 .. panes). Internal pane
 * boundaries darken to 0 over a bar of `bar` pane pitches, with a one-texel
 * ramp (`ramp` is one texel expressed in pane units). The outer border is
 * left to the edge softness.
 */
static double lt_bar(double p, unsigned int panes, double bar, double ramp)
{
    double n, t;

    if (bar <= 0.0 || panes < 2u || !(ramp > 0.0)) {
        return 1.0;
    }
    n = floor(p + 0.5);
    if (n <= 0.0 || n >= (double)panes) {
        return 1.0;
    }
    t = (fabs(p - n) - bar * 0.5) / ramp;
    return t <= 0.0 ? 0.0 : (t >= 1.0 ? 1.0 : t);
}

/* ---- vectors (double, so winding decisions do not depend on float slop) - */

typedef struct LtVec {
    double x, y, z;
} LtVec;

static LtVec lt_vec(PIXELICA_Vec3 v)
{
    LtVec r;

    r.x = (double)v.x;
    r.y = (double)v.y;
    r.z = (double)v.z;
    return r;
}

static int lt_vec_finite(LtVec v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static LtVec lt_add(LtVec a, LtVec b)
{
    LtVec r;

    r.x = a.x + b.x;
    r.y = a.y + b.y;
    r.z = a.z + b.z;
    return r;
}

static LtVec lt_sub(LtVec a, LtVec b)
{
    LtVec r;

    r.x = a.x - b.x;
    r.y = a.y - b.y;
    r.z = a.z - b.z;
    return r;
}

static LtVec lt_scale(LtVec a, double s)
{
    LtVec r;

    r.x = a.x * s;
    r.y = a.y * s;
    r.z = a.z * s;
    return r;
}

static double lt_dot(LtVec a, LtVec b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static LtVec lt_cross(LtVec a, LtVec b)
{
    LtVec r;

    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}

/* *out = v / |v|; 0 for a zero-length or non-finite vector. */
static int lt_normalize(LtVec v, LtVec *out)
{
    double len = sqrt(lt_dot(v, v));

    if (!lt_vec_finite(v) || !(len > 0.0) || !isfinite(len)) {
        return 0;
    }
    *out = lt_scale(v, 1.0 / len);
    return 1;
}

static int lt_store_vec(LtVec v, PIXELICA_Vec3 *out)
{
    return lt_to_float(v.x, &out->x) && lt_to_float(v.y, &out->y) &&
           lt_to_float(v.z, &out->z);
}

/* ---- masks --------------------------------------------------------------- */

PIXELICA_Status pixelica_light_mask_default(PIXELICA_LightShape shape,
                                            PIXELICA_LightMaskDesc *out)
{
    PIXELICA_LightMaskDesc d;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(&d, 0, sizeof d);
    d.shape = shape;
    switch (shape) {
    case PIXELICA_LIGHT_SHAPE_RECT:
        d.width = 48u;
        d.height = 32u;
        d.softness = 0.35f;
        d.fade_v = 0.35f;
        d.peak = 200u;
        d.panes_x = 2u;
        d.panes_y = 3u;
        d.bar = 0.08f;
        break;
    case PIXELICA_LIGHT_SHAPE_ELLIPSE:
        d.width = 32u;
        d.height = 32u;
        d.softness = 0.75f;
        d.fade_v = 0.0f;
        d.peak = 160u;
        d.panes_x = 1u;
        d.panes_y = 1u;
        d.bar = 0.0f;
        break;
    default:
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = d;
    return PIXELICA_OK;
}

/* Every field is checked for every shape, including the ones the shape
 * ignores, so a stale or mistyped value is reported instead of silently
 * dropped. */
static int lt_desc_valid(const PIXELICA_LightMaskDesc *d)
{
    if (d->shape != PIXELICA_LIGHT_SHAPE_RECT &&
        d->shape != PIXELICA_LIGHT_SHAPE_ELLIPSE) {
        return 0;
    }
    if (d->width < 1u || d->height < 1u ||
        d->width > PIXELICA_MAX_TEXTURE_DIMENSION ||
        d->height > PIXELICA_MAX_TEXTURE_DIMENSION) {
        return 0;
    }
    if (!isfinite(d->softness) || d->softness < 0.0f || d->softness > 1.0f) {
        return 0;
    }
    if (!isfinite(d->fade_v) || d->fade_v < 0.0f || d->fade_v > 1.0f) {
        return 0;
    }
    if (!isfinite(d->bar) || d->bar < 0.0f || d->bar > 0.5f) {
        return 0;
    }
    if (d->panes_x < 1u || d->panes_x > PIXELICA_LIGHT_MAX_PANES ||
        d->panes_y < 1u || d->panes_y > PIXELICA_LIGHT_MAX_PANES) {
        return 0;
    }
    return 1;
}

PIXELICA_Status pixelica_light_mask_build(const PIXELICA_LightMaskDesc *desc,
                                          uint8_t *alpha, size_t alpha_count,
                                          PIXELICA_LightMask *out_mask)
{
    PIXELICA_LightMaskDesc d;
    PIXELICA_LightMask mask;
    LtRange written, other;
    size_t pixels;
    size_t x, y;
    double w, h, softness, fade, bar, peak, ramp_x, ramp_y;

    if (desc == NULL || alpha == NULL || out_mask == NULL || !lt_desc_valid(desc)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    d = *desc; /* a local copy cannot be disturbed by what we write */
    pixels = (size_t)d.width * (size_t)d.height;
    if (alpha_count < pixels) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (!lt_range(alpha, pixels, &written)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!lt_range(desc, sizeof *desc, &other) || lt_overlap(&written, &other) ||
        !lt_range(out_mask, sizeof *out_mask, &other) ||
        lt_overlap(&written, &other)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    w = (double)d.width;
    h = (double)d.height;
    softness = (double)d.softness;
    fade = (double)d.fade_v;
    bar = (double)d.bar;
    peak = (double)d.peak;
    ramp_x = (double)d.panes_x / w;
    ramp_y = (double)d.panes_y / h;

    for (y = 0; y < (size_t)d.height; ++y) {
        double v = ((double)y + 0.5) / h;

        for (x = 0; x < (size_t)d.width; ++x) {
            double u = ((double)x + 0.5) / w;
            double c;

            if (d.shape == PIXELICA_LIGHT_SHAPE_ELLIPSE) {
                double nx = 2.0 * u - 1.0;
                double ny = 2.0 * v - 1.0;

                c = lt_edge(1.0 - sqrt(nx * nx + ny * ny), softness);
            } else {
                double cx = lt_edge(1.0 - fabs(2.0 * u - 1.0), softness);
                double cy = lt_edge(1.0 - fabs(2.0 * v - 1.0), softness);

                c = cx < cy ? cx : cy;
                c *= lt_bar(u * (double)d.panes_x, d.panes_x, bar, ramp_x);
                c *= lt_bar(v * (double)d.panes_y, d.panes_y, bar, ramp_y);
            }
            c = lt_clamp01(c) * (1.0 - fade * v);
            alpha[y * (size_t)d.width + x] = (uint8_t)floor(lt_clamp01(c) * peak + 0.5);
        }
    }

    mask.alpha = alpha;
    mask.width = d.width;
    mask.height = d.height;
    mask.stride = (size_t)d.width;
    *out_mask = mask;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_light_tint_fill(uint16_t tint, uint16_t width, uint16_t height,
                                         uint16_t *pixels, size_t pixel_count,
                                         PIXELICA_Texture *out_texture)
{
    PIXELICA_Texture texture;
    LtRange written, other;
    size_t count, i;

    if (pixels == NULL || out_texture == NULL || width < 1u || height < 1u ||
        width > PIXELICA_MAX_TEXTURE_DIMENSION ||
        height > PIXELICA_MAX_TEXTURE_DIMENSION) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    count = (size_t)width * (size_t)height;
    if (pixel_count < count) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (count > SIZE_MAX / sizeof(uint16_t) ||
        !lt_range(pixels, count * sizeof(uint16_t), &written) ||
        !lt_range(out_texture, sizeof *out_texture, &other) ||
        lt_overlap(&written, &other)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    for (i = 0; i < count; ++i) {
        pixels[i] = tint;
    }
    texture.pixels = pixels;
    texture.width = width;
    texture.height = height;
    *out_texture = texture;
    return PIXELICA_OK;
}

/* ---- receiver quads ------------------------------------------------------ */

static const PIXELICA_Vec2 k_quad_uvs[4] = {
    {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
};

/*
 * Writes the triangle pair. `forward` means the winding (0,1,2)/(0,2,3) is
 * counter-clockwise seen from the +normal side; otherwise the reversed order
 * is emitted so the patch faces the viewer either way.
 */
static void lt_emit_indices(PIXELICA_LightQuad *q, int forward)
{
    static const uint16_t fwd[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    static const uint16_t rev[6] = {0u, 2u, 1u, 0u, 3u, 2u};
    const uint16_t *src = forward ? fwd : rev;
    int i;

    for (i = 0; i < 6; ++i) {
        q->indices[i] = src[i];
    }
}

/* Shared placement checks: finite fields, a usable normal and a real
 * parallelogram. Returns the pre-biased corner in *out_base. */
static int lt_placement(const PIXELICA_LightPlacement *p, LtVec *out_u, LtVec *out_v,
                        LtVec *out_normal, LtVec *out_base)
{
    LtVec origin = lt_vec(p->origin);
    LtVec u = lt_vec(p->u_axis);
    LtVec v = lt_vec(p->v_axis);
    LtVec n = lt_vec(p->normal);
    LtVec unit, cross;

    if (!lt_vec_finite(origin) || !lt_vec_finite(u) || !lt_vec_finite(v) ||
        !lt_vec_finite(n) || !isfinite(p->bias) || !lt_normalize(n, &unit)) {
        return 0;
    }
    cross = lt_cross(u, v);
    if (!lt_vec_finite(cross) || lt_dot(cross, cross) == 0.0) {
        return 0; /* u_axis parallel to v_axis: no surface */
    }
    *out_u = u;
    *out_v = v;
    *out_normal = n;
    *out_base = lt_add(origin, lt_scale(unit, (double)p->bias));
    return lt_vec_finite(*out_base);
}

/* Fills `out` from the four corners, choosing the winding from `normal`. */
static PIXELICA_Status lt_finish_quad(LtVec c0, LtVec c1, LtVec c2, LtVec c3,
                                      LtVec normal, PIXELICA_LightQuad *out)
{
    PIXELICA_LightQuad quad;
    double facing = lt_dot(lt_cross(lt_sub(c1, c0), lt_sub(c3, c0)), normal);
    int i;

    if (!isfinite(facing) || facing == 0.0) {
        return PIXELICA_ERR_INVALID_ARG; /* degenerate or edge-on: no front side */
    }
    if (!lt_store_vec(c0, &quad.vertices[0]) || !lt_store_vec(c1, &quad.vertices[1]) ||
        !lt_store_vec(c2, &quad.vertices[2]) || !lt_store_vec(c3, &quad.vertices[3])) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < 4; ++i) {
        quad.uvs[i] = k_quad_uvs[i];
    }
    lt_emit_indices(&quad, facing > 0.0);
    *out = quad;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_light_quad(const PIXELICA_LightPlacement *placement,
                                    PIXELICA_LightQuad *out)
{
    LtVec u, v, n, base;

    if (placement == NULL || out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!lt_placement(placement, &u, &v, &n, &base)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return lt_finish_quad(base, lt_add(base, u), lt_add(lt_add(base, u), v),
                          lt_add(base, v), n, out);
}

PIXELICA_Status pixelica_light_quad_centered(PIXELICA_Vec3 center, PIXELICA_Vec3 normal,
                                             PIXELICA_Vec3 u_axis,
                                             float radius_u, float radius_v, float bias,
                                             PIXELICA_LightQuad *out)
{
    LtVec c = lt_vec(center);
    LtVec n = lt_vec(normal);
    LtVec hint = lt_vec(u_axis);
    LtVec unit_n, unit_u, in_plane, v_dir, base, u_edge, v_edge;

    if (out == NULL || !lt_vec_finite(c) || !isfinite(radius_u) || !isfinite(radius_v) ||
        !isfinite(bias) || !(radius_u > 0.0f) || !(radius_v > 0.0f) ||
        !lt_normalize(n, &unit_n) || !lt_normalize(hint, &unit_u)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* Keep only the part of the hint that lies in the receiver plane. */
    in_plane = lt_sub(unit_u, lt_scale(unit_n, lt_dot(unit_u, unit_n)));
    if (!lt_normalize(in_plane, &unit_u)) {
        return PIXELICA_ERR_INVALID_ARG; /* u_axis is parallel to the normal */
    }
    v_dir = lt_cross(unit_n, unit_u);
    if (!lt_normalize(v_dir, &v_dir)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    u_edge = lt_scale(unit_u, 2.0 * (double)radius_u);
    v_edge = lt_scale(v_dir, 2.0 * (double)radius_v);
    base = lt_add(lt_sub(lt_sub(c, lt_scale(unit_u, (double)radius_u)),
                         lt_scale(v_dir, (double)radius_v)),
                  lt_scale(unit_n, (double)bias));
    if (!lt_vec_finite(base) || !lt_vec_finite(u_edge) || !lt_vec_finite(v_edge)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return lt_finish_quad(base, lt_add(base, u_edge),
                          lt_add(lt_add(base, u_edge), v_edge), lt_add(base, v_edge),
                          unit_n, out);
}

PIXELICA_Status pixelica_light_quad_project(const PIXELICA_LightPlacement *source,
                                            PIXELICA_Vec3 direction,
                                            PIXELICA_Vec3 plane_point,
                                            PIXELICA_Vec3 plane_normal,
                                            float bias,
                                            PIXELICA_LightQuad *out)
{
    LtVec u, v, n, base, dir, point, pn, unit;
    LtVec corner[4], hit[4];
    double denom;
    int i;

    if (source == NULL || out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    dir = lt_vec(direction);
    point = lt_vec(plane_point);
    pn = lt_vec(plane_normal);
    if (!lt_placement(source, &u, &v, &n, &base) || !lt_vec_finite(dir) ||
        !lt_vec_finite(point) || !isfinite(bias) || lt_dot(dir, dir) == 0.0 ||
        !lt_normalize(pn, &unit)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    denom = lt_dot(pn, dir);
    if (!isfinite(denom) || denom == 0.0) {
        return PIXELICA_ERR_INVALID_ARG; /* light runs parallel to the receiver */
    }

    corner[0] = base;
    corner[1] = lt_add(base, u);
    corner[2] = lt_add(lt_add(base, u), v);
    corner[3] = lt_add(base, v);
    for (i = 0; i < 4; ++i) {
        double t = lt_dot(pn, lt_sub(point, corner[i])) / denom;

        if (!isfinite(t) || !(t > 0.0)) {
            return PIXELICA_ERR_INVALID_ARG; /* the plane is not in front of this corner */
        }
        hit[i] = lt_add(lt_add(corner[i], lt_scale(dir, t)),
                        lt_scale(unit, (double)bias));
        if (!lt_vec_finite(hit[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return lt_finish_quad(hit[0], hit[1], hit[2], hit[3], pn, out);
}

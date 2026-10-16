/*
 * Pixelica - draw submission: validation, transform, cull, clip, project.
 *
 * Scratch memory: transformed vertices live in the context frame arena
 * (vertex_count * sizeof(PIXELICA_ClipVertex) bytes, reused by every draw).
 * A clipped polygon lives in a fixed stack array: a triangle clipped by
 * the 7 planes (w-epsilon + 6 frustum planes) gains at most one vertex per
 * plane, so it never exceeds 3 + 7 = 10 vertices (< PIXELICA_CLIP_MAX_VERTS).
 * Plane distances and intersections are computed in double and stored back
 * as float, so finite inputs up to +-FLT_MAX clip without overflow.
 * Texture coordinates (and, for layered materials, layer UVs and RGBA
 * vertex colors) ride along in the stack polygon only (the arena stays
 * position-only) and are interpolated at intersections with the same
 * parameter as the position.
 * Draw options (wrap/alpha) are validated with the mesh, before the arena
 * or any pixel is touched, and travel to the rasterizer in PIXELICA_RasterShade.
 * World clip planes (mesh effects) add up to 4 more planes, clipped first:
 * their per-vertex distances are recomputed per triangle into the stack
 * polygon (the arena stays 20 bytes per vertex), so a polygon never exceeds
 * 3 + 4 + 7 = 14 vertices. With an affine model, a world distance is affine
 * in the object position and therefore interpolates with the same edge
 * parameter as the homogeneous clip position.
 */
#include <float.h>
#include <math.h>
#include <string.h>

#include "../core/context_internal.h"
#include "draw_internal.h"
#include "raster.h"

#define PIXELICA_CLIP_MAX_VERTS 16
#define PIXELICA_CLIP_PLANE_COUNT 7
#define PIXELICA_CLIP_USER_FIRST PIXELICA_CLIP_PLANE_COUNT /* plane ids 7..10 */

typedef char pixelica_clip_poly_size_check
    [(3 + PIXELICA_CLIP_PLANE_COUNT + (int)PIXELICA_MAX_WORLD_CLIP_PLANES <=
      PIXELICA_CLIP_MAX_VERTS) ? 1 : -1];

/* Outcode bits, one per clip plane, same order as pixelica_plane_distance(). */
#define PIXELICA_OC_W_NEAR (1u << 0) /* w < PIXELICA_CLIP_W_EPSILON */
#define PIXELICA_OC_LEFT   (1u << 1) /* x < -w */
#define PIXELICA_OC_RIGHT  (1u << 2) /* x >  w */
#define PIXELICA_OC_BOTTOM (1u << 3) /* y < -w */
#define PIXELICA_OC_TOP    (1u << 4) /* y >  w */
#define PIXELICA_OC_NEAR   (1u << 5) /* z < -w */
#define PIXELICA_OC_FAR    (1u << 6) /* z >  w */

/* Compile-time check that the public arena sizing macro is large enough. */
typedef char pixelica_clip_vertex_size_check
    [(sizeof(PIXELICA_ClipVertex) <= PIXELICA_ARENA_BYTES_PER_VERTEX) ? 1 : -1];

/* Clip-space polygon vertex (stack only): position plus texture coords and
 * the extra attributes (raster.h layout, not premultiplied). */
typedef struct PIXELICA_PolyVertex {
    float x, y, z, w;
    float u, v;
    float x_attr[PIXELICA_RASTER_EXTRA_ATTRS];
    float pd[PIXELICA_MAX_WORLD_CLIP_PLANES]; /* world clip plane distances */
} PIXELICA_PolyVertex;

/* World clip planes of a mesh-effects draw (count == 0: none). */
typedef struct PIXELICA_DrawClip {
    const float *model; /* affine, validated */
    const PIXELICA_WorldClipPlane *planes;
    uint32_t count;     /* 0..PIXELICA_MAX_WORLD_CLIP_PLANES */
} PIXELICA_DrawClip;

/*
 * Prepared lighting state of one draw (renderer.h, PIXELICA_Lighting). The
 * normal matrix and the normalized light directions are computed once per
 * draw; the per-vertex factor itself is recomputed per triangle, exactly
 * like the world clip distances, so the frame arena stays position-only.
 */
typedef struct PIXELICA_DrawLight {
    float normal_matrix[16];      /* inverse transpose of model's 3x3 */
    const PIXELICA_Vec3 *normals; /* vertex_count object-space normals */
    double dir[PIXELICA_MAX_DIRECTIONAL_LIGHTS][3];   /* unit, toward light */
    double color[PIXELICA_MAX_DIRECTIONAL_LIGHTS][3]; /* 0..255 per channel */
    double ambient[3];            /* 0..255 per channel */
    uint32_t count;               /* 0..PIXELICA_MAX_DIRECTIONAL_LIGHTS */
    int two_sided;
} PIXELICA_DrawLight;

/* Per-vertex sources of the extra attributes of a layered draw. */
typedef struct PIXELICA_DrawAttrs {
    const PIXELICA_Vec2 *layer_uvs[PIXELICA_MAX_TEXTURE_LAYERS];
    uint32_t layer_count;
    const PIXELICA_ColorRGBA8 *vertex_colors; /* NULL = none */
    uint32_t active; /* x_attr slots carried through clipping */
} PIXELICA_DrawAttrs;

static int pixelica_isfinite_f(float v)
{
    return isfinite(v) != 0;
}

/* Evaluated in double: for finite float inputs the sum or difference of two
 * coordinates (up to 2 * FLT_MAX) cannot overflow, and its sign is exact. */
static double pixelica_plane_distance(float fx, float fy, float fz, float fw,
                                 int plane)
{
    const double x = (double)fx;
    const double y = (double)fy;
    const double z = (double)fz;
    const double w = (double)fw;

    switch (plane) {
    case 0: return w - (double)PIXELICA_CLIP_W_EPSILON;
    case 1: return x + w;
    case 2: return w - x;
    case 3: return y + w;
    case 4: return w - y;
    case 5: return z + w;
    default: return w - z;
    }
}

static double pixelica_poly_distance(const PIXELICA_PolyVertex *v, int plane)
{
    if (plane >= PIXELICA_CLIP_USER_FIRST) {
        return (double)v->pd[plane - PIXELICA_CLIP_USER_FIRST];
    }
    return pixelica_plane_distance(v->x, v->y, v->z, v->w, plane);
}

/* World clip distances of object vertex v: dot(n, model * v) + distance in
 * double. 0 if the world position or a distance leaves the float range. */
static int pixelica_world_distances(const PIXELICA_DrawClip *dc, PIXELICA_Vec3 v,
                                    float out[PIXELICA_MAX_WORLD_CLIP_PLANES])
{
    float p[4];
    uint32_t k;

    pixelica_mat4_transform_point(p, dc->model, v);
    if (!pixelica_isfinite_f(p[0]) || !pixelica_isfinite_f(p[1]) ||
        !pixelica_isfinite_f(p[2])) {
        return 0;
    }
    for (k = 0; k < dc->count; ++k) {
        const PIXELICA_WorldClipPlane *pl = &dc->planes[k];
        const double d = (double)pl->normal.x * p[0] +
                         (double)pl->normal.y * p[1] +
                         (double)pl->normal.z * p[2] + (double)pl->distance;
        if (!(fabs(d) <= (double)FLT_MAX)) {
            return 0;
        }
        out[k] = (float)d;
    }
    return 1;
}

/*
 * Lambert factor of one vertex, 0..255 per channel (renderer.h). The object
 * normal is taken into world space with the inverse-transpose matrix and
 * normalized; a normal that the (finite, non-singular) matrix maps to zero
 * or out of range contributes no diffuse term, leaving ambient only.
 */
static void pixelica_light_vertex(const PIXELICA_DrawLight *dl, PIXELICA_Vec3 n,
                                  double out[3])
{
    const float *m = dl->normal_matrix;
    double v[3];
    double len2;
    int c;

    for (c = 0; c < 3; ++c) {
        out[c] = dl->ambient[c];
        v[c] = (double)m[0 + c] * (double)n.x + (double)m[4 + c] * (double)n.y +
               (double)m[8 + c] * (double)n.z;
    }
    len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (len2 > 0.0 && len2 <= DBL_MAX) {
        const double inv = 1.0 / sqrt(len2);
        uint32_t k;

        for (k = 0; k < dl->count; ++k) {
            double d = (v[0] * dl->dir[k][0] + v[1] * dl->dir[k][1] +
                        v[2] * dl->dir[k][2]) * inv;

            if (dl->two_sided) {
                d = fabs(d);
            }
            if (d > 1.0) { /* rounding of a unit dot product */
                d = 1.0;
            }
            if (d > 0.0) {
                for (c = 0; c < 3; ++c) {
                    out[c] += floor(dl->color[k][c] * d + 0.5);
                }
            }
        }
    }
    for (c = 0; c < 3; ++c) {
        if (out[c] > 255.0) {
            out[c] = 255.0;
        }
    }
}

static uint32_t pixelica_outcode(const PIXELICA_ClipVertex *v)
{
    uint32_t code = 0;
    int plane;

    for (plane = 0; plane < PIXELICA_CLIP_PLANE_COUNT; ++plane) {
        if (pixelica_plane_distance(v->x, v->y, v->z, v->w, plane) < 0.0) {
            code |= 1u << plane;
        }
    }
    return code;
}

/* a + t * (b - a) in double, clamped to [min(a,b), max(a,b)] so rounding can
 * never leave the segment (or the finite float range) before narrowing. */
static float pixelica_lerp_clip(float a, float b, double t)
{
    const double da = (double)a;
    const double db = (double)b;
    const double lo = da < db ? da : db;
    const double hi = da < db ? db : da;
    double r = da + t * (db - da);

    if (r < lo) {
        r = lo;
    }
    if (r > hi) {
        r = hi;
    }
    return (float)r;
}

/* Point on segment in->out where the plane distance is zero. Always
 * interpolates from the inside vertex so shared edges clip identically. */
static PIXELICA_PolyVertex pixelica_intersect(const PIXELICA_PolyVertex *in,
                                    const PIXELICA_PolyVertex *out,
                                    double d_in, double d_out, int plane,
                                    uint32_t active, uint32_t nuser)
{
    PIXELICA_PolyVertex r;
    double t = d_in / (d_in - d_out); /* d_in >= 0 > d_out: t in [0,1] */
    uint32_t k;

    r.x = pixelica_lerp_clip(in->x, out->x, t);
    r.y = pixelica_lerp_clip(in->y, out->y, t);
    r.z = pixelica_lerp_clip(in->z, out->z, t);
    r.w = pixelica_lerp_clip(in->w, out->w, t);
    r.u = pixelica_lerp_clip(in->u, out->u, t);
    r.v = pixelica_lerp_clip(in->v, out->v, t);
    for (k = 0; k < PIXELICA_RASTER_EXTRA_ATTRS; ++k) {
        r.x_attr[k] = k < active
            ? pixelica_lerp_clip(in->x_attr[k], out->x_attr[k], t)
            : 0.0f;
    }
    for (k = 0; k < PIXELICA_MAX_WORLD_CLIP_PLANES; ++k) {
        r.pd[k] = k < nuser ? pixelica_lerp_clip(in->pd[k], out->pd[k], t)
                            : 0.0f;
    }
    /* Snap exactly onto the plane to avoid tiny overshoot. */
    if (plane >= PIXELICA_CLIP_USER_FIRST) {
        r.pd[plane - PIXELICA_CLIP_USER_FIRST] = 0.0f;
        return r;
    }
    switch (plane) {
    case 0: r.w = PIXELICA_CLIP_W_EPSILON; break;
    case 1: r.x = -r.w; break;
    case 2: r.x = r.w; break;
    case 3: r.y = -r.w; break;
    case 4: r.y = r.w; break;
    case 5: r.z = -r.w; break;
    default: r.z = r.w; break;
    }
    return r;
}

/* Sutherland-Hodgman against the planes in `planes_mask` (bits 0..6 the
 * frustum, bits 7.. the first `nuser` world planes, which go first).
 * Returns the resulting vertex count (0 when fully clipped). `poly` and
 * `tmp` hold PIXELICA_CLIP_MAX_VERTS entries. */
static int pixelica_clip_polygon(PIXELICA_PolyVertex *poly, PIXELICA_PolyVertex *tmp,
                            int count, uint32_t planes_mask, uint32_t active,
                            uint32_t nuser)
{
    const int steps = PIXELICA_CLIP_PLANE_COUNT + (int)nuser;
    int step;

    for (step = 0; step < steps && count >= 3; ++step) {
        const int plane = step < (int)nuser ? PIXELICA_CLIP_USER_FIRST + step
                                            : step - (int)nuser;
        int out_count = 0;
        int i;

        if ((planes_mask & (1u << plane)) == 0) {
            continue;
        }
        for (i = 0; i < count; ++i) {
            const PIXELICA_PolyVertex *cur = &poly[i];
            const PIXELICA_PolyVertex *nxt = &poly[(i + 1) % count];
            double dc = pixelica_poly_distance(cur, plane);
            double dn = pixelica_poly_distance(nxt, plane);
            int cur_in = dc >= 0.0;
            int nxt_in = dn >= 0.0;

            if (cur_in) {
                if (out_count >= PIXELICA_CLIP_MAX_VERTS) {
                    return 0; /* unreachable by construction; stay bounded */
                }
                tmp[out_count++] = *cur;
            }
            if (cur_in != nxt_in) {
                if (out_count >= PIXELICA_CLIP_MAX_VERTS) {
                    return 0;
                }
                tmp[out_count++] = cur_in
                    ? pixelica_intersect(cur, nxt, dc, dn, plane, active, nuser)
                    : pixelica_intersect(nxt, cur, dn, dc, plane, active, nuser);
            }
        }
        memcpy(poly, tmp, (size_t)out_count * sizeof *poly);
        count = out_count;
    }
    return count >= 3 ? count : 0;
}

static float pixelica_clampf(float v, float lo, float hi)
{
    if (!(v >= lo)) { /* NaN goes to lo */
        return lo;
    }
    return v > hi ? hi : v;
}

static PIXELICA_ScreenVertex pixelica_project(const PIXELICA_PolyVertex *v,
                                    float width, float height)
{
    const float sub = (float)(1 << PIXELICA_SUBPIXEL_BITS);
    const float depth_scale = 0.5f * (float)PIXELICA_DEPTH_MAX_DRAWABLE;
    PIXELICA_ScreenVertex s;
    float inv_w = 1.0f / v->w; /* w >= PIXELICA_CLIP_W_EPSILON here */
    float nx = v->x * inv_w;
    float ny = v->y * inv_w;
    float nz = v->z * inv_w;
    float sx = (nx + 1.0f) * 0.5f * width * sub;
    float sy = (1.0f - ny) * 0.5f * height * sub; /* row 0 = top */

    sx = pixelica_clampf(sx, 0.0f, width * sub);
    sy = pixelica_clampf(sy, 0.0f, height * sub);
    s.x = (int32_t)floorf(sx + 0.5f);
    s.y = (int32_t)floorf(sy + 0.5f);
    s.z = pixelica_clampf((nz + 1.0f) * depth_scale, 0.0f,
                     (float)PIXELICA_DEPTH_MAX_DRAWABLE);
    s.q = 0.0f;
    s.uq = 0.0f;
    s.vq = 0.0f;
    memset(s.xq, 0, sizeof s.xq);
    return s;
}

static void pixelica_add_counts(PIXELICA_Context *ctx, const PIXELICA_RasterCounts *c)
{
    ctx->stats.triangles_rasterized =
        pixelica_sat_add_u32(ctx->stats.triangles_rasterized, 1);
    ctx->stats.pixels_written =
        pixelica_sat_add_u32(ctx->stats.pixels_written, c->written);
    ctx->stats.fragments_covered =
        pixelica_sat_add_u32(ctx->stats.fragments_covered, c->covered);
    ctx->stats.pixels_covered =
        pixelica_sat_add_u32(ctx->stats.pixels_covered, c->newly_covered);
}

/* Triangle (ia, ib, ic) of the transformed vertices `clip`. `uvs` is NULL
 * for untextured draws, else the mesh's per-vertex texture coordinates.
 * `attrs` is NULL (or has active == 0) for draws without extra attributes. */
static void pixelica_draw_triangle(PIXELICA_Context *ctx, const PIXELICA_RasterTarget *target,
                              const PIXELICA_ClipVertex *clip, uint16_t ia,
                              uint16_t ib, uint16_t ic, const PIXELICA_Vec2 *uvs,
                              const PIXELICA_RasterShade *shade,
                              const PIXELICA_DrawAttrs *attrs,
                              const PIXELICA_Vec3 *vertices,
                              const PIXELICA_DrawClip *wclip,
                              const PIXELICA_DrawLight *light)
{
    const uint32_t active = attrs != NULL ? attrs->active : 0u;
    const uint32_t nuser = wclip != NULL ? wclip->count : 0u;
    float pd[3][PIXELICA_MAX_WORLD_CLIP_PLANES];
    uint32_t user_out = 0;
    PIXELICA_PolyVertex poly[PIXELICA_CLIP_MAX_VERTS];
    PIXELICA_PolyVertex tmp[PIXELICA_CLIP_MAX_VERTS];
    PIXELICA_ScreenVertex screen[PIXELICA_CLIP_MAX_VERTS];
    const PIXELICA_ClipVertex *a = &clip[ia];
    const PIXELICA_ClipVertex *b = &clip[ib];
    const PIXELICA_ClipVertex *c = &clip[ic];
    uint16_t idx[3];
    uint32_t any_out = a->outcode | b->outcode | c->outcode;
    double det;
    int front;
    int expected_sign;
    int count = 3;
    int i;
    float fw = (float)target->width;
    float fh = (float)target->height;

    if ((a->outcode & b->outcode & c->outcode) != 0) {
        ctx->stats.triangles_rejected =
            pixelica_sat_add_u32(ctx->stats.triangles_rejected, 1);
        return;
    }
    memset(pd, 0, sizeof pd);
    if (nuser != 0u) {
        uint32_t all_out = (1u << nuser) - 1u;
        uint32_t k;

        idx[0] = ia;
        idx[1] = ib;
        idx[2] = ic;
        for (i = 0; i < 3; ++i) {
            uint32_t code = 0;
            /* Validated for every vertex before the draw was accepted. */
            (void)pixelica_world_distances(wclip, vertices[idx[i]], pd[i]);
            for (k = 0; k < nuser; ++k) {
                if (pd[i][k] < 0.0f) {
                    code |= 1u << k;
                }
            }
            user_out |= code;
            all_out &= code;
        }
        if (all_out != 0u) {
            ctx->stats.triangles_rejected =
                pixelica_sat_add_u32(ctx->stats.triangles_rejected, 1);
            return;
        }
    }

    /*
     * Facing from the homogeneous determinant |a.xyw b.xyw c.xyw|. Its sign
     * tells which side of the triangle's plane the eye is on, independent of
     * w signs, so it is valid before clipping. For w > 0 it equals the sign
     * of the NDC signed area (positive = counter-clockwise = front).
     * Evaluated in double so large finite inputs cannot overflow.
     */
    det = (double)a->x * ((double)b->y * c->w - (double)b->w * c->y) -
          (double)a->y * ((double)b->x * c->w - (double)b->w * c->x) +
          (double)a->w * ((double)b->x * c->y - (double)b->y * c->x);
    if (!(det > 0.0) && !(det < 0.0)) {
        ctx->stats.triangles_culled =
            pixelica_sat_add_u32(ctx->stats.triangles_culled, 1);
        return; /* edge-on (or NaN): no area */
    }
    front = det > 0.0;
    if (!front && ctx->cull_mode == PIXELICA_CULL_BACK) {
        ctx->stats.triangles_culled =
            pixelica_sat_add_u32(ctx->stats.triangles_culled, 1);
        return;
    }
    /* Screen y points down, so front faces have a negative screen area. */
    expected_sign = front ? -1 : 1;

    idx[0] = ia;
    idx[1] = ib;
    idx[2] = ic;
    for (i = 0; i < 3; ++i) {
        const PIXELICA_ClipVertex *s = &clip[idx[i]];
        poly[i].x = s->x;
        poly[i].y = s->y;
        poly[i].z = s->z;
        poly[i].w = s->w;
        poly[i].u = uvs != NULL ? uvs[idx[i]].x : 0.0f;
        poly[i].v = uvs != NULL ? uvs[idx[i]].y : 0.0f;
        memset(poly[i].x_attr, 0, sizeof poly[i].x_attr);
        memcpy(poly[i].pd, pd[i], sizeof poly[i].pd);
        if (active != 0u) {
            uint32_t k;
            for (k = 0; k < attrs->layer_count; ++k) {
                poly[i].x_attr[2u * k] = attrs->layer_uvs[k][idx[i]].x;
                poly[i].x_attr[2u * k + 1u] = attrs->layer_uvs[k][idx[i]].y;
            }
            if (attrs->vertex_colors != NULL) {
                const PIXELICA_ColorRGBA8 *vc = &attrs->vertex_colors[idx[i]];
                poly[i].x_attr[PIXELICA_RASTER_COLOR_ATTR + 0u] = (float)vc->r;
                poly[i].x_attr[PIXELICA_RASTER_COLOR_ATTR + 1u] = (float)vc->g;
                poly[i].x_attr[PIXELICA_RASTER_COLOR_ATTR + 2u] = (float)vc->b;
                poly[i].x_attr[PIXELICA_RASTER_COLOR_ATTR + 3u] = (float)vc->a;
            } else if (light != NULL) {
                poly[i].x_attr[PIXELICA_RASTER_COLOR_ATTR + 3u] = 255.0f;
            }
        }
        if (light != NULL) {
            /* One modulation factor per channel: vertex color (if any)
             * times the Lambert factor, both in 0..255. */
            const int modulated = attrs != NULL && attrs->vertex_colors != NULL;
            double f[3];
            uint32_t ch;

            pixelica_light_vertex(light, light->normals[idx[i]], f);
            for (ch = 0; ch < 3u; ++ch) {
                float *slot = &poly[i].x_attr[PIXELICA_RASTER_COLOR_ATTR + ch];
                *slot = modulated ? (float)((double)*slot * f[ch] / 255.0)
                                  : (float)f[ch];
            }
        }
    }
    if (any_out != 0 || user_out != 0) {
        ctx->stats.triangles_clipped =
            pixelica_sat_add_u32(ctx->stats.triangles_clipped, 1);
        count = pixelica_clip_polygon(
            poly, tmp, count, any_out | (user_out << PIXELICA_CLIP_USER_FIRST),
            active, nuser);
        if (count < 3) {
            return;
        }
    }

    for (i = 0; i < count; ++i) {
        screen[i] = pixelica_project(&poly[i], fw, fh);
    }
    if (uvs != NULL || active != 0u) {
        /* q = w_min / w lies in (0, 1]: the same ratio as 1/w (the common
         * factor cancels in the perspective divide) without overflowing for
         * tiny w. */
        float w_min = poly[0].w;
        for (i = 1; i < count; ++i) {
            if (poly[i].w < w_min) {
                w_min = poly[i].w;
            }
        }
        for (i = 0; i < count; ++i) {
            float q = w_min / poly[i].w;
            screen[i].q = q;
            screen[i].uq = poly[i].u * q;
            screen[i].vq = poly[i].v * q;
            if (active != 0u) {
                uint32_t k;
                for (k = 0; k < active; ++k) {
                    screen[i].xq[k] = poly[i].x_attr[k] * q;
                }
            }
        }
    }
    for (i = 1; i + 1 < count; ++i) {
        PIXELICA_RasterCounts rc;
        pixelica_raster_triangle(target, &screen[0], &screen[i], &screen[i + 1],
                            expected_sign, shade, &rc);
        pixelica_add_counts(ctx, &rc);
    }
}

static PIXELICA_Status pixelica_fail(PIXELICA_Context *ctx, PIXELICA_Status st)
{
    ctx->stats.last_error = st;
    return st;
}

static int pixelica_texture_valid(const PIXELICA_Texture *tex)
{
    return tex != NULL && tex->pixels != NULL && tex->width != 0 &&
           tex->height != 0 &&
           (uint32_t)tex->width <= PIXELICA_MAX_TEXTURE_DIMENSION &&
           (uint32_t)tex->height <= PIXELICA_MAX_TEXTURE_DIMENSION;
}

/* 1 if uvs is NULL or all n coordinates are finite. */
static int pixelica_uvs_finite(const PIXELICA_Vec2 *uvs, uint32_t n)
{
    uint32_t i;

    if (uvs == NULL) {
        return 1;
    }
    for (i = 0; i < n; ++i) {
        if (!pixelica_isfinite_f(uvs[i].x) || !pixelica_isfinite_f(uvs[i].y)) {
            return 0;
        }
    }
    return 1;
}

PIXELICA_Status pixelica_check_mesh_structure(const PIXELICA_Mesh *mesh,
                                    const PIXELICA_Material *material)
{
    if (mesh == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if ((mesh->index_count % 3u) != 0 ||
        (mesh->vertex_count > 0 && mesh->vertices == NULL) ||
        (mesh->index_count > 0 && mesh->indices == NULL)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (material != NULL && material->texture != NULL) {
        if (!pixelica_texture_valid(material->texture)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (mesh->vertex_count > 0 && material->uvs == NULL) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_check_mesh_data(const PIXELICA_Mesh *mesh,
                               const PIXELICA_Material *material)
{
    uint32_t i;

    for (i = 0; i < mesh->index_count; ++i) {
        if ((uint32_t)mesh->indices[i] >= mesh->vertex_count) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    if (material != NULL && !pixelica_uvs_finite(material->uvs, mesh->vertex_count)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

/* 1 if [p, p + n) intersects the context-owned block (context struct through
 * the last guard). Arithmetic is on uintptr_t only; like the hybrid module,
 * a range whose end is not representable counts as overlapping. */
static int pixelica_overlaps_context(const PIXELICA_Context *ctx, const void *p,
                                size_t n)
{
    const uintptr_t lo = (uintptr_t)(const void *)ctx;
    const uintptr_t hi =
        (uintptr_t)(const void *)ctx->guards[PIXELICA_GUARD_COUNT - 1u] +
        PIXELICA_GUARD_BYTES; /* end of a real object: cannot wrap */
    const uintptr_t a = (uintptr_t)p;

    if (n == 0) {
        return 0;
    }
    if (n > UINTPTR_MAX - a) {
        return 1;
    }
    return a < hi && lo < a + n;
}

/* O(1) option checks (after pixelica_check_mesh_structure). */
static PIXELICA_Status pixelica_check_options(const PIXELICA_Context *ctx,
                                         const PIXELICA_Material *material,
                                         const PIXELICA_DrawOptions *o)
{
    const PIXELICA_Texture *tex = material != NULL ? material->texture : NULL;

    if (o == NULL) {
        return PIXELICA_OK;
    }
    if (pixelica_overlaps_context(ctx, o, sizeof *o) ||
        (o->wrap_u != PIXELICA_WRAP_CLAMP && o->wrap_u != PIXELICA_WRAP_REPEAT) ||
        (o->wrap_v != PIXELICA_WRAP_CLAMP && o->wrap_v != PIXELICA_WRAP_REPEAT) ||
        (o->alpha_mode != PIXELICA_ALPHA_OPAQUE &&
         o->alpha_mode != PIXELICA_ALPHA_TEST &&
         o->alpha_mode != PIXELICA_ALPHA_BLEND &&
         o->alpha_mode != PIXELICA_ALPHA_ADD)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (o->texture_alpha == NULL) {
        return o->texture_alpha_bytes == 0 ? PIXELICA_OK : PIXELICA_ERR_INVALID_ARG;
    }
    /* Texture dimensions were validated, so the product cannot overflow. */
    if (tex == NULL ||
        o->texture_alpha_bytes < (size_t)tex->width * (size_t)tex->height ||
        pixelica_overlaps_context(ctx, o->texture_alpha, o->texture_alpha_bytes)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

/* 1 if the array [p, p + count * elem) overlaps context-owned memory (an
 * unrepresentable size counts as overlapping). NULL or empty: 0. */
static int pixelica_array_overlaps_context(const PIXELICA_Context *ctx,
                                           const void *p, size_t count,
                                           size_t elem)
{
    if (p == NULL || count == 0) {
        return 0;
    }
    if (count > SIZE_MAX / elem) {
        return 1;
    }
    return pixelica_overlaps_context(ctx, p, count * elem);
}

static int pixelica_texture_overlaps_context(const PIXELICA_Context *ctx,
                                             const PIXELICA_Texture *tex)
{
    /* Called only for validated textures: the product cannot overflow. */
    return pixelica_overlaps_context(ctx, tex, sizeof *tex) ||
           pixelica_array_overlaps_context(
               ctx, tex->pixels, (size_t)tex->width * (size_t)tex->height,
               sizeof *tex->pixels);
}

/* O(1) checks of a layered material (after the base and option checks; the
 * material struct itself was checked for aliasing before any read). */
static PIXELICA_Status pixelica_check_layers(const PIXELICA_Context *ctx,
                                        const PIXELICA_Mesh *mesh,
                                        const PIXELICA_MaterialLayers *ml)
{
    const PIXELICA_Material *base = &ml->base;
    const size_t nv = (size_t)mesh->vertex_count;
    size_t k;

    if (ml->layer_count > PIXELICA_MAX_TEXTURE_LAYERS ||
        (ml->layer_count > 0 && ml->layers == NULL) ||
        pixelica_array_overlaps_context(ctx, ml->layers, ml->layer_count,
                                        sizeof *ml->layers) ||
        pixelica_array_overlaps_context(ctx, ml->vertex_colors, nv,
                                        sizeof *ml->vertex_colors) ||
        pixelica_array_overlaps_context(ctx, base->uvs, nv, sizeof *base->uvs) ||
        pixelica_array_overlaps_context(ctx, base->triangle_colors,
                                        (size_t)mesh->index_count / 3u,
                                        sizeof *base->triangle_colors) ||
        (base->texture != NULL &&
         pixelica_texture_overlaps_context(ctx, base->texture))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (k = 0; k < ml->layer_count; ++k) {
        const PIXELICA_TextureLayer *l = &ml->layers[k];

        if (pixelica_overlaps_context(ctx, l->texture, sizeof *l->texture) ||
            !pixelica_texture_valid(l->texture) ||
            pixelica_texture_overlaps_context(ctx, l->texture) ||
            (nv > 0 && l->uvs == NULL) ||
            pixelica_array_overlaps_context(ctx, l->uvs, nv, sizeof *l->uvs) ||
            (l->wrap_u != PIXELICA_WRAP_CLAMP && l->wrap_u != PIXELICA_WRAP_REPEAT) ||
            (l->wrap_v != PIXELICA_WRAP_CLAMP && l->wrap_v != PIXELICA_WRAP_REPEAT) ||
            (l->combine != PIXELICA_TEXTURE_MULTIPLY &&
             l->combine != PIXELICA_TEXTURE_ADD &&
             l->combine != PIXELICA_TEXTURE_LERP)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

/* O(n) layer checks (after pixelica_check_layers): finite layer UVs. */
static PIXELICA_Status pixelica_check_layer_data(const PIXELICA_Mesh *mesh,
                                            const PIXELICA_MaterialLayers *ml)
{
    size_t k;

    for (k = 0; k < ml->layer_count; ++k) {
        if (!pixelica_uvs_finite(ml->layers[k].uvs, mesh->vertex_count)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

/* O(planes) mesh-effect checks (model already known finite). */
static PIXELICA_Status pixelica_check_effects(const PIXELICA_Context *ctx,
                                         const float model[16],
                                         const PIXELICA_MeshEffects *fx)
{
    size_t k;

    if (fx == NULL) {
        return PIXELICA_OK;
    }
    if (pixelica_overlaps_context(ctx, fx, sizeof *fx) ||
        (fx->filter != PIXELICA_FILTER_NEAREST &&
         fx->filter != PIXELICA_FILTER_BILINEAR) ||
        fx->clip_plane_count > PIXELICA_MAX_WORLD_CLIP_PLANES ||
        (fx->clip_plane_count > 0 && fx->clip_planes == NULL) ||
        pixelica_array_overlaps_context(ctx, fx->clip_planes,
                                        fx->clip_plane_count,
                                        sizeof *fx->clip_planes)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (k = 0; k < fx->clip_plane_count; ++k) {
        const PIXELICA_WorldClipPlane *p = &fx->clip_planes[k];

        if (!pixelica_isfinite_f(p->normal.x) ||
            !pixelica_isfinite_f(p->normal.y) ||
            !pixelica_isfinite_f(p->normal.z) ||
            !pixelica_isfinite_f(p->distance) ||
            (p->normal.x == 0.0f && p->normal.y == 0.0f &&
             p->normal.z == 0.0f)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    if (fx->clip_plane_count > 0 &&
        (model[3] != 0.0f || model[7] != 0.0f || model[11] != 0.0f ||
         model[15] != 1.0f)) {
        return PIXELICA_ERR_INVALID_ARG; /* world planes need an affine model */
    }
    return PIXELICA_OK;
}

/*
 * O(lights) lighting checks (model already known finite). On success `out`
 * holds the normal matrix, the normalized light directions and the colors;
 * `out->normals` is set by the caller after the O(n) normal check.
 */
static PIXELICA_Status pixelica_check_lighting(const PIXELICA_Context *ctx,
                                          const PIXELICA_Mesh *mesh,
                                          const float model[16],
                                          const PIXELICA_Lighting *lt,
                                          PIXELICA_DrawLight *out)
{
    size_t k;

    memset(out, 0, sizeof *out);
    if (lt == NULL) {
        return PIXELICA_OK;
    }
    if (pixelica_overlaps_context(ctx, lt, sizeof *lt) ||
        (mesh->vertex_count > 0 && lt->normals == NULL) ||
        pixelica_array_overlaps_context(ctx, lt->normals, mesh->vertex_count,
                                        sizeof *lt->normals) ||
        lt->light_count > PIXELICA_MAX_DIRECTIONAL_LIGHTS ||
        (lt->light_count > 0 && lt->lights == NULL) ||
        pixelica_array_overlaps_context(ctx, lt->lights, lt->light_count,
                                        sizeof *lt->lights)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* Normals are only meaningful with a non-singular 3x3 model block. */
    if (pixelica_mat4_normal_matrix(out->normal_matrix, model) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (k = 0; k < lt->light_count; ++k) {
        const PIXELICA_DirectionalLight *l = &lt->lights[k];
        double d[3];
        double len2;
        double inv;
        int c;

        if (!pixelica_isfinite_f(l->direction.x) ||
            !pixelica_isfinite_f(l->direction.y) ||
            !pixelica_isfinite_f(l->direction.z)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        d[0] = (double)l->direction.x;
        d[1] = (double)l->direction.y;
        d[2] = (double)l->direction.z;
        len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (!(len2 > 0.0) || !(len2 <= DBL_MAX)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        /* Stored pointing TOWARD the light, so dot(N, dir) is the Lambert
         * term directly. */
        inv = -1.0 / sqrt(len2);
        for (c = 0; c < 3; ++c) {
            out->dir[k][c] = d[c] * inv;
        }
        out->color[k][0] = (double)l->r;
        out->color[k][1] = (double)l->g;
        out->color[k][2] = (double)l->b;
    }
    out->count = (uint32_t)lt->light_count;
    out->ambient[0] = (double)lt->ambient_r;
    out->ambient[1] = (double)lt->ambient_g;
    out->ambient[2] = (double)lt->ambient_b;
    out->two_sided = lt->two_sided ? 1 : 0;
    return PIXELICA_OK;
}

/* O(n) lighting check: every normal finite and non-zero. */
static PIXELICA_Status pixelica_check_normals(const PIXELICA_Mesh *mesh,
                                              const PIXELICA_Lighting *lt)
{
    uint32_t i;

    for (i = 0; i < mesh->vertex_count; ++i) {
        const PIXELICA_Vec3 n = lt->normals[i];

        if (!pixelica_isfinite_f(n.x) || !pixelica_isfinite_f(n.y) ||
            !pixelica_isfinite_f(n.z) ||
            (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

/* Shared implementation of every pixelica_draw_mesh* entry point. `ml` is
 * NULL for the legacy APIs; otherwise `material` is &ml->base. `fx` is
 * NULL except for pixelica_draw_mesh_effects and pixelica_draw_mesh_lit,
 * and `lt` is NULL except for pixelica_draw_mesh_lit with lighting. */
static PIXELICA_Status pixelica_draw_impl(PIXELICA_Context *ctx,
                                     const PIXELICA_Mesh *mesh,
                                     const float model[16],
                                     const PIXELICA_Material *material,
                                     const PIXELICA_DrawOptions *options,
                                     const PIXELICA_MaterialLayers *ml,
                                     const PIXELICA_MeshEffects *fx,
                                     const PIXELICA_Lighting *lt)
{
    /* context-alias checks */
    const int strict = ml != NULL || fx != NULL || lt != NULL;
    PIXELICA_DrawLight light;
    PIXELICA_DrawClip wclip;
    PIXELICA_WorldClipPlane planes_copy[PIXELICA_MAX_WORLD_CLIP_PLANES];
    float model_copy[16];
    float mvp[16];
    PIXELICA_ClipVertex *clip;
    PIXELICA_RasterTarget target;
    PIXELICA_RasterShade shade;
    PIXELICA_DrawAttrs attrs;
    PIXELICA_DrawOptions opt;
    const PIXELICA_Vec2 *uvs = NULL;
    const uint16_t *tri_colors = NULL;
    size_t arena_need;
    PIXELICA_Status st;
    uint32_t i;

    if (!pixelica_context_valid(ctx)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (ctx->pixel_callback_active) {
        return PIXELICA_ERR_INVALID_ARG; /* reentry from a pixel hook: no writes */
    }
    if (mesh == NULL || model == NULL || !ctx->in_frame ||
        !ctx->view_proj_valid ||
        (strict && (pixelica_overlaps_context(ctx, mesh, sizeof *mesh) ||
            pixelica_overlaps_context(ctx, model, 16u * sizeof *model))) ||
        (ml != NULL && pixelica_overlaps_context(ctx, ml, sizeof *ml)) ||
        !pixelica_mat4_is_finite(model)) {
        return pixelica_fail(ctx, PIXELICA_ERR_INVALID_ARG);
    }
    if (strict && (
        pixelica_array_overlaps_context(ctx, mesh->vertices, mesh->vertex_count,
            sizeof *mesh->vertices) ||
        pixelica_array_overlaps_context(ctx, mesh->indices, mesh->index_count,
            sizeof *mesh->indices) ||
        (material != NULL && material->texture != NULL &&
         pixelica_overlaps_context(ctx, material->texture,
                                   sizeof *material->texture)))) {
        return pixelica_fail(ctx, PIXELICA_ERR_INVALID_ARG);
    }
    st = pixelica_check_mesh_structure(mesh, material);
    if (st == PIXELICA_OK) {
        st = pixelica_check_options(ctx, material, options);
    }
    if (st == PIXELICA_OK && ml != NULL) {
        st = pixelica_check_layers(ctx, mesh, ml);
    }
    if (st == PIXELICA_OK) {
        st = pixelica_check_effects(ctx, model, fx);
    }
    if (st == PIXELICA_OK) {
        st = pixelica_check_lighting(ctx, mesh, model, lt, &light);
    }
    if (st != PIXELICA_OK) {
        return pixelica_fail(ctx, st);
    }
    opt = options != NULL ? *options : pixelica_draw_options_default();
    if (mesh->vertex_count > (uint32_t)ctx->cfg.max_vertices_per_mesh ||
        ctx->draw_count >= (uint32_t)ctx->cfg.max_draw_commands) {
        return pixelica_fail(ctx, PIXELICA_ERR_CAPACITY);
    }
    st = pixelica_check_mesh_data(mesh, material);
    if (st == PIXELICA_OK && ml != NULL) {
        st = pixelica_check_layer_data(mesh, ml);
    }
    if (st == PIXELICA_OK && lt != NULL) {
        st = pixelica_check_normals(mesh, lt);
    }
    if (st != PIXELICA_OK) {
        return pixelica_fail(ctx, st);
    }
    light.normals = lt != NULL ? lt->normals : NULL;

    /* Transform every vertex into the arena before touching any pixel so a
     * rejected draw leaves the frame unchanged. */
    arena_need = (size_t)mesh->vertex_count * sizeof(PIXELICA_ClipVertex);
    if (arena_need > ctx->cfg.frame_arena_bytes) {
        return pixelica_fail(ctx, PIXELICA_ERR_CAPACITY); /* guarded by config check */
    }
    /* Private copies: the hook cannot change validated plane data or the
     * model between triangles. */
    memcpy(model_copy, model, sizeof model_copy);
    wclip.model = model_copy;
    wclip.planes = planes_copy;
    wclip.count = fx != NULL ? (uint32_t)fx->clip_plane_count : 0u; /* <= 4 */
    if (wclip.count != 0u) {
        memcpy(planes_copy, fx->clip_planes,
               (size_t)wclip.count * sizeof *planes_copy);
    }
    clip = (PIXELICA_ClipVertex *)(void *)ctx->arena;
    pixelica_mat4_multiply(mvp, ctx->view_proj, model);
    for (i = 0; i < mesh->vertex_count; ++i) {
        PIXELICA_Vec3 v = mesh->vertices[i];
        float out[4];
        float pd[PIXELICA_MAX_WORLD_CLIP_PLANES];

        if (!pixelica_isfinite_f(v.x) || !pixelica_isfinite_f(v.y) ||
            !pixelica_isfinite_f(v.z) ||
            (wclip.count != 0u && !pixelica_world_distances(&wclip, v, pd))) {
            return pixelica_fail(ctx, PIXELICA_ERR_INVALID_ARG);
        }
        pixelica_mat4_transform_point(out, mvp, v);
        if (!pixelica_isfinite_f(out[0]) || !pixelica_isfinite_f(out[1]) ||
            !pixelica_isfinite_f(out[2]) || !pixelica_isfinite_f(out[3])) {
            return pixelica_fail(ctx, PIXELICA_ERR_INVALID_ARG);
        }
        clip[i].x = out[0];
        clip[i].y = out[1];
        clip[i].z = out[2];
        clip[i].w = out[3];
        clip[i].outcode = pixelica_outcode(&clip[i]);
    }
    if ((uint32_t)arena_need > ctx->stats.arena_high_water) {
        ctx->stats.arena_high_water = (uint32_t)arena_need;
    }

    ctx->draw_count += 1;
    ctx->stats.draw_calls = pixelica_sat_add_u32(ctx->stats.draw_calls, 1);
    ctx->stats.triangles_submitted = pixelica_sat_add_u32(
        ctx->stats.triangles_submitted, mesh->index_count / 3u);

    target.color = ctx->color;
    target.depth = ctx->depth;
    target.coverage = ctx->coverage;
    target.width = (int32_t)ctx->cfg.width;
    target.height = (int32_t)ctx->cfg.height;

    memset(&shade, 0, sizeof shade); /* layered fields stay zero for legacy */
    memset(&attrs, 0, sizeof attrs);
    shade.color = mesh->color;
    shade.texels = NULL;
    shade.tex_width = 0;
    shade.tex_height = 0;
    shade.alpha = NULL;
    shade.repeat_u = opt.wrap_u == PIXELICA_WRAP_REPEAT;
    shade.repeat_v = opt.wrap_v == PIXELICA_WRAP_REPEAT;
    shade.alpha_mode = opt.alpha_mode;
    shade.opacity = opt.opacity;
    shade.alpha_cutoff = opt.alpha_cutoff;
    if (material != NULL) {
        tri_colors = material->triangle_colors;
        if (material->texture != NULL) {
            shade.texels = material->texture->pixels;
            shade.tex_width = (int32_t)material->texture->width;
            shade.tex_height = (int32_t)material->texture->height;
            uvs = material->uvs;
            if (opt.alpha_mode != PIXELICA_ALPHA_OPAQUE) {
                shade.alpha = opt.texture_alpha; /* validated above */
            }
        }
    }
    if (ml != NULL) {
        uint32_t k;

        attrs.layer_count = (uint32_t)ml->layer_count; /* <= 3, validated */
        for (k = 0; k < attrs.layer_count; ++k) {
            const PIXELICA_TextureLayer *l = &ml->layers[k];
            PIXELICA_RasterLayer *rl = &shade.layers[k];

            attrs.layer_uvs[k] = l->uvs;
            rl->texels = l->texture->pixels;
            rl->tex_width = (int32_t)l->texture->width;
            rl->tex_height = (int32_t)l->texture->height;
            rl->repeat_u = l->wrap_u == PIXELICA_WRAP_REPEAT;
            rl->repeat_v = l->wrap_v == PIXELICA_WRAP_REPEAT;
            rl->combine = l->combine;
            rl->opacity = l->opacity;
        }
        attrs.vertex_colors = ml->vertex_colors;
        attrs.active = attrs.vertex_colors != NULL
                           ? PIXELICA_RASTER_EXTRA_ATTRS
                           : 2u * attrs.layer_count;
        shade.layer_count = attrs.layer_count;
        shade.vertex_colors = attrs.vertex_colors != NULL;
    }
    if (lt != NULL) {
        /* Lighting rides the vertex-color modulation slots. */
        attrs.active = PIXELICA_RASTER_EXTRA_ATTRS;
        shade.vertex_colors = 1;
    }
    if (fx != NULL) {
        shade.bilinear = fx->filter == PIXELICA_FILTER_BILINEAR;
        shade.pixel = fx->pixel;
        shade.pixel_userdata = fx->userdata;
    }

    /* The hook may not mutate this context; the guard makes every
     * same-context mutator refuse until the draw returns. */
    ctx->pixel_callback_active = shade.pixel != NULL;
    /* index_count % 3 == 0 was validated, so i never passes it and the
     * i + 1 / i + 2 reads stay in range without any wrapping arithmetic. */
    for (i = 0; i < mesh->index_count; i += 3) {
        if (uvs == NULL && tri_colors != NULL) {
            shade.color = tri_colors[i / 3u];
        }
        pixelica_draw_triangle(ctx, &target, clip, mesh->indices[i],
                          mesh->indices[i + 1], mesh->indices[i + 2], uvs,
                          &shade, attrs.active != 0u ? &attrs : NULL,
                          mesh->vertices, wclip.count != 0u ? &wclip : NULL,
                          lt != NULL ? &light : NULL);
    }
    ctx->pixel_callback_active = 0;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_draw_mesh(PIXELICA_Context *ctx, const PIXELICA_Mesh *mesh,
                         const float model[16])
{
    return pixelica_draw_impl(ctx, mesh, model, NULL, NULL, NULL, NULL, NULL);
}

PIXELICA_Status pixelica_draw_mesh_ex(PIXELICA_Context *ctx, const PIXELICA_Mesh *mesh,
                            const float model[16],
                            const PIXELICA_Material *material)
{
    return pixelica_draw_impl(ctx, mesh, model, material, NULL, NULL, NULL, NULL);
}

PIXELICA_Status pixelica_draw_mesh_options(PIXELICA_Context *ctx,
                                 const PIXELICA_Mesh *mesh,
                                 const float model[16],
                                 const PIXELICA_Material *material,
                                 const PIXELICA_DrawOptions *options)
{
    return pixelica_draw_impl(ctx, mesh, model, material, options, NULL, NULL,
                              NULL);
}

PIXELICA_Status pixelica_draw_mesh_layers(PIXELICA_Context *ctx,
                                const PIXELICA_Mesh *mesh,
                                const float model[16],
                                const PIXELICA_MaterialLayers *material,
                                const PIXELICA_DrawOptions *options)
{
    return pixelica_draw_impl(ctx, mesh, model,
                              material != NULL ? &material->base : NULL,
                              options, material, NULL, NULL);
}

PIXELICA_Status pixelica_draw_mesh_effects(PIXELICA_Context *ctx,
                                 const PIXELICA_Mesh *mesh,
                                 const float model[16],
                                 const PIXELICA_MaterialLayers *material,
                                 const PIXELICA_DrawOptions *options,
                                 const PIXELICA_MeshEffects *effects)
{
    return pixelica_draw_impl(ctx, mesh, model,
                              material != NULL ? &material->base : NULL,
                              options, material, effects, NULL);
}

PIXELICA_Status pixelica_draw_mesh_lit(PIXELICA_Context *ctx,
                             const PIXELICA_Mesh *mesh,
                             const float model[16],
                             const PIXELICA_MaterialLayers *material,
                             const PIXELICA_DrawOptions *options,
                             const PIXELICA_MeshEffects *effects,
                             const PIXELICA_Lighting *lighting)
{
    return pixelica_draw_impl(ctx, mesh, model,
                              material != NULL ? &material->base : NULL,
                              options, material, effects, lighting);
}

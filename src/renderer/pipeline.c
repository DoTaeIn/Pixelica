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
 * Texture coordinates ride along in the stack polygon only (the arena
 * stays position-only) and are interpolated at intersections with the
 * same parameter as the position.
 */
#include <math.h>
#include <string.h>

#include "../core/context_internal.h"
#include "draw_internal.h"
#include "raster.h"

#define PIXELICA_CLIP_MAX_VERTS 16
#define PIXELICA_CLIP_PLANE_COUNT 7

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

/* Clip-space polygon vertex (stack only): position plus texture coords. */
typedef struct PIXELICA_PolyVertex {
    float x, y, z, w;
    float u, v;
} PIXELICA_PolyVertex;

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
    return pixelica_plane_distance(v->x, v->y, v->z, v->w, plane);
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
                                    double d_in, double d_out, int plane)
{
    PIXELICA_PolyVertex r;
    double t = d_in / (d_in - d_out); /* d_in >= 0 > d_out: t in [0,1] */

    r.x = pixelica_lerp_clip(in->x, out->x, t);
    r.y = pixelica_lerp_clip(in->y, out->y, t);
    r.z = pixelica_lerp_clip(in->z, out->z, t);
    r.w = pixelica_lerp_clip(in->w, out->w, t);
    r.u = pixelica_lerp_clip(in->u, out->u, t);
    r.v = pixelica_lerp_clip(in->v, out->v, t);
    /* Snap exactly onto the plane to avoid tiny overshoot. */
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

/* Sutherland-Hodgman against the planes in `planes_mask`. Returns the
 * resulting vertex count (0 when fully clipped). `poly` and `tmp` hold
 * PIXELICA_CLIP_MAX_VERTS entries. */
static int pixelica_clip_polygon(PIXELICA_PolyVertex *poly, PIXELICA_PolyVertex *tmp,
                            int count, uint32_t planes_mask)
{
    int plane;

    for (plane = 0; plane < PIXELICA_CLIP_PLANE_COUNT && count >= 3; ++plane) {
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
                    ? pixelica_intersect(cur, nxt, dc, dn, plane)
                    : pixelica_intersect(nxt, cur, dn, dc, plane);
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
 * for untextured draws, else the mesh's per-vertex texture coordinates. */
static void pixelica_draw_triangle(PIXELICA_Context *ctx, const PIXELICA_RasterTarget *target,
                              const PIXELICA_ClipVertex *clip, uint16_t ia,
                              uint16_t ib, uint16_t ic, const PIXELICA_Vec2 *uvs,
                              const PIXELICA_RasterShade *shade)
{
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
    }
    if (any_out != 0) {
        ctx->stats.triangles_clipped =
            pixelica_sat_add_u32(ctx->stats.triangles_clipped, 1);
        count = pixelica_clip_polygon(poly, tmp, count, any_out);
        if (count < 3) {
            return;
        }
    }

    for (i = 0; i < count; ++i) {
        screen[i] = pixelica_project(&poly[i], fw, fh);
    }
    if (uvs != NULL) {
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
        const PIXELICA_Texture *tex = material->texture;
        if (tex->pixels == NULL || tex->width == 0 || tex->height == 0 ||
            (uint32_t)tex->width > PIXELICA_MAX_TEXTURE_DIMENSION ||
            (uint32_t)tex->height > PIXELICA_MAX_TEXTURE_DIMENSION) {
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
    if (material != NULL && material->uvs != NULL) {
        for (i = 0; i < mesh->vertex_count; ++i) {
            if (!pixelica_isfinite_f(material->uvs[i].x) ||
                !pixelica_isfinite_f(material->uvs[i].y)) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_draw_mesh(PIXELICA_Context *ctx, const PIXELICA_Mesh *mesh,
                         const float model[16])
{
    return pixelica_draw_mesh_ex(ctx, mesh, model, NULL);
}

PIXELICA_Status pixelica_draw_mesh_ex(PIXELICA_Context *ctx, const PIXELICA_Mesh *mesh,
                            const float model[16],
                            const PIXELICA_Material *material)
{
    float mvp[16];
    PIXELICA_ClipVertex *clip;
    PIXELICA_RasterTarget target;
    PIXELICA_RasterShade shade;
    const PIXELICA_Vec2 *uvs = NULL;
    const uint16_t *tri_colors = NULL;
    size_t arena_need;
    PIXELICA_Status st;
    uint32_t i;

    if (!pixelica_context_valid(ctx)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (mesh == NULL || model == NULL || !ctx->in_frame ||
        !ctx->view_proj_valid || !pixelica_mat4_is_finite(model)) {
        return pixelica_fail(ctx, PIXELICA_ERR_INVALID_ARG);
    }
    st = pixelica_check_mesh_structure(mesh, material);
    if (st != PIXELICA_OK) {
        return pixelica_fail(ctx, st);
    }
    if (mesh->vertex_count > (uint32_t)ctx->cfg.max_vertices_per_mesh ||
        ctx->draw_count >= (uint32_t)ctx->cfg.max_draw_commands) {
        return pixelica_fail(ctx, PIXELICA_ERR_CAPACITY);
    }
    st = pixelica_check_mesh_data(mesh, material);
    if (st != PIXELICA_OK) {
        return pixelica_fail(ctx, st);
    }

    /* Transform every vertex into the arena before touching any pixel so a
     * rejected draw leaves the frame unchanged. */
    arena_need = (size_t)mesh->vertex_count * sizeof(PIXELICA_ClipVertex);
    if (arena_need > ctx->cfg.frame_arena_bytes) {
        return pixelica_fail(ctx, PIXELICA_ERR_CAPACITY); /* guarded by config check */
    }
    clip = (PIXELICA_ClipVertex *)(void *)ctx->arena;
    pixelica_mat4_multiply(mvp, ctx->view_proj, model);
    for (i = 0; i < mesh->vertex_count; ++i) {
        PIXELICA_Vec3 v = mesh->vertices[i];
        float out[4];

        if (!pixelica_isfinite_f(v.x) || !pixelica_isfinite_f(v.y) ||
            !pixelica_isfinite_f(v.z)) {
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
    target.width = (int32_t)ctx->cfg.width;
    target.height = (int32_t)ctx->cfg.height;

    shade.color = mesh->color;
    shade.texels = NULL;
    shade.tex_width = 0;
    shade.tex_height = 0;
    if (material != NULL) {
        tri_colors = material->triangle_colors;
        if (material->texture != NULL) {
            shade.texels = material->texture->pixels;
            shade.tex_width = (int32_t)material->texture->width;
            shade.tex_height = (int32_t)material->texture->height;
            uvs = material->uvs;
        }
    }

    /* index_count % 3 == 0 was validated, so i never passes it and the
     * i + 1 / i + 2 reads stay in range without any wrapping arithmetic. */
    for (i = 0; i < mesh->index_count; i += 3) {
        if (uvs == NULL && tri_colors != NULL) {
            shade.color = tri_colors[i / 3u];
        }
        pixelica_draw_triangle(ctx, &target, clip, mesh->indices[i],
                          mesh->indices[i + 1], mesh->indices[i + 2], uvs,
                          &shade);
    }
    return PIXELICA_OK;
}

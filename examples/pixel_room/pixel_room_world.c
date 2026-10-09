/*
 * Pixel Room world construction: generated geometry (floor with real UVs,
 * walls with a window and a door, rug, player cube, markers, HUD), prop
 * placement from measured asset bounds, baked directional lighting, the scene
 * and the collision world. All storage is inside the PixelRoom allocation.
 *
 * Winding: every face is emitted counter-clockwise when seen from outside (or
 * from inside the room for the single-sided walls), matching the renderer's
 * default back-face culling. Walls have no back side on purpose, so rotating
 * the camera behind them looks into the room instead of at a blank wall.
 */
#include "pixel_room_internal.h"

#include <math.h>
#include <string.h>

typedef struct PRRgb {
    uint8_t r, g, b;
} PRRgb;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

#define PR_LIGHT_X 0.4206f
#define PR_LIGHT_Y 0.7477f
#define PR_LIGHT_Z 0.5140f

static PIXELICA_Vec3 pr_v3(float x, float y, float z)
{
    PIXELICA_Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static PIXELICA_Vec2 pr_v2(float x, float y)
{
    PIXELICA_Vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

static PIXELICA_AABB pr_aabb(float x0, float y0, float z0, float x1, float y1, float z1)
{
    PIXELICA_AABB box;
    box.min = pr_v3(x0, y0, z0);
    box.max = pr_v3(x1, y1, z1);
    return box;
}

/* Fixed ambient + one directional light, evaluated once at build time. */
static float pr_light(float nx, float ny, float nz)
{
    float d = nx * PR_LIGHT_X + ny * PR_LIGHT_Y + nz * PR_LIGHT_Z;
    if (d < 0.0f) {
        d = 0.0f;
    }
    return 0.55f + 0.45f * d;
}

static uint16_t pr_shade_rgb(uint32_t r, uint32_t g, uint32_t b, float factor)
{
    uint32_t s = (uint32_t)(factor * 256.0f + 0.5f);
    if (s > 256u) {
        s = 256u;
    }
    return PIXELICA_RGB565((r * s) / 256u, (g * s) / 256u, (b * s) / 256u);
}

static void pr_triangle_normal(PIXELICA_Vec3 a, PIXELICA_Vec3 b, PIXELICA_Vec3 c, float n[3])
{
    float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    float cx = uy * vz - uz * vy;
    float cy = uz * vx - ux * vz;
    float cz = ux * vy - uy * vx;
    float len = sqrtf(cx * cx + cy * cy + cz * cz);
    if (len > 1e-12f) {
        n[0] = cx / len;
        n[1] = cy / len;
        n[2] = cz / len;
    } else {
        n[0] = 0.0f;
        n[1] = 1.0f;
        n[2] = 0.0f;
    }
}

static PRRgb pr_rgb(uint32_t r, uint32_t g, uint32_t b)
{
    PRRgb c;
    c.r = (uint8_t)r;
    c.g = (uint8_t)g;
    c.b = (uint8_t)b;
    return c;
}

/* ------------------------------------------------------------------------ */
/* Mesh builder over the room's fixed pools                                  */
/* ------------------------------------------------------------------------ */

typedef struct PRBuilder {
    PixelRoom *room;
    PIXELICA_Vec3 *v;
    uint16_t *idx;
    uint16_t *tri_col;
    PIXELICA_Vec2 *uv; /* NULL for flat meshes */
    uint32_t v_count, i_count;
    uint32_t v_cap, i_cap, uv_cap;
    bool failed;
} PRBuilder;

static void pr_begin(PRBuilder *b, PixelRoom *room, bool with_uv)
{
    memset(b, 0, sizeof(*b));
    b->room = room;
    b->v = room->vpool + room->v_used;
    b->v_cap = PR_VERT_POOL - room->v_used;
    b->idx = room->ipool + room->i_used;
    b->i_cap = PR_IDX_POOL - room->i_used;
    b->tri_col = room->tpool + room->i_used / 3u;
    if (with_uv) {
        b->uv = room->uvpool + room->uv_used;
        b->uv_cap = PR_UV_POOL - room->uv_used;
    }
}

static uint16_t pr_vert(PRBuilder *b, PIXELICA_Vec3 p)
{
    if (b->failed || b->v_count >= b->v_cap || b->v_count >= 65535u ||
        (b->uv != NULL && b->v_count >= b->uv_cap)) {
        b->failed = true;
        return 0u;
    }
    b->v[b->v_count] = p;
    if (b->uv != NULL) {
        b->uv[b->v_count] = pr_v2(0.0f, 0.0f);
    }
    return (uint16_t)b->v_count++;
}

static void pr_tri_indices(PRBuilder *b, uint16_t a, uint16_t c, uint16_t d, uint16_t color)
{
    if (b->failed || b->i_count + 3u > b->i_cap) {
        b->failed = true;
        return;
    }
    b->tri_col[b->i_count / 3u] = color;
    b->idx[b->i_count++] = a;
    b->idx[b->i_count++] = c;
    b->idx[b->i_count++] = d;
}

/* Quad p0..p3 counter-clockwise seen from the front; optional per-corner UVs. */
static void pr_quad_color(PRBuilder *b, PIXELICA_Vec3 p0, PIXELICA_Vec3 p1, PIXELICA_Vec3 p2,
                          PIXELICA_Vec3 p3, uint16_t color, const PIXELICA_Vec2 *uv)
{
    uint16_t i0 = pr_vert(b, p0);
    uint16_t i1 = pr_vert(b, p1);
    uint16_t i2 = pr_vert(b, p2);
    uint16_t i3 = pr_vert(b, p3);
    if (b->failed) {
        return;
    }
    if (uv != NULL && b->uv != NULL) {
        b->uv[i0] = uv[0];
        b->uv[i1] = uv[1];
        b->uv[i2] = uv[2];
        b->uv[i3] = uv[3];
    }
    pr_tri_indices(b, i0, i1, i2, color);
    pr_tri_indices(b, i0, i2, i3, color);
}

static void pr_quad_lit(PRBuilder *b, PIXELICA_Vec3 p0, PIXELICA_Vec3 p1, PIXELICA_Vec3 p2,
                        PIXELICA_Vec3 p3, PRRgb rgb)
{
    float n[3];
    pr_triangle_normal(p0, p1, p2, n);
    pr_quad_color(b, p0, p1, p2, p3, pr_shade_rgb(rgb.r, rgb.g, rgb.b, pr_light(n[0], n[1], n[2])),
                  NULL);
}

static void pr_tri_lit(PRBuilder *b, PIXELICA_Vec3 p0, PIXELICA_Vec3 p1, PIXELICA_Vec3 p2, PRRgb rgb)
{
    float n[3];
    uint16_t i0, i1, i2;
    pr_triangle_normal(p0, p1, p2, n);
    i0 = pr_vert(b, p0);
    i1 = pr_vert(b, p1);
    i2 = pr_vert(b, p2);
    pr_tri_indices(b, i0, i1, i2, pr_shade_rgb(rgb.r, rgb.g, rgb.b, pr_light(n[0], n[1], n[2])));
}

enum { PR_PX, PR_NX, PR_PY, PR_NY, PR_PZ, PR_NZ, PR_FACE_COUNT };
#define PR_FACE(f) (1u << (f))
#define PR_ALL_FACES 0x3Fu

/* Axis-aligned box with outward counter-clockwise faces; `mask` selects faces. */
static void pr_box(PRBuilder *b, PIXELICA_Vec3 lo, PIXELICA_Vec3 hi, const PRRgb face[PR_FACE_COUNT],
                   uint32_t mask)
{
    if ((mask & PR_FACE(PR_PX)) != 0u) {
        pr_quad_lit(b, pr_v3(hi.x, lo.y, hi.z), pr_v3(hi.x, lo.y, lo.z),
                    pr_v3(hi.x, hi.y, lo.z), pr_v3(hi.x, hi.y, hi.z), face[PR_PX]);
    }
    if ((mask & PR_FACE(PR_NX)) != 0u) {
        pr_quad_lit(b, pr_v3(lo.x, lo.y, lo.z), pr_v3(lo.x, lo.y, hi.z),
                    pr_v3(lo.x, hi.y, hi.z), pr_v3(lo.x, hi.y, lo.z), face[PR_NX]);
    }
    if ((mask & PR_FACE(PR_PY)) != 0u) {
        pr_quad_lit(b, pr_v3(lo.x, hi.y, hi.z), pr_v3(hi.x, hi.y, hi.z),
                    pr_v3(hi.x, hi.y, lo.z), pr_v3(lo.x, hi.y, lo.z), face[PR_PY]);
    }
    if ((mask & PR_FACE(PR_NY)) != 0u) {
        pr_quad_lit(b, pr_v3(lo.x, lo.y, lo.z), pr_v3(hi.x, lo.y, lo.z),
                    pr_v3(hi.x, lo.y, hi.z), pr_v3(lo.x, lo.y, hi.z), face[PR_NY]);
    }
    if ((mask & PR_FACE(PR_PZ)) != 0u) {
        pr_quad_lit(b, pr_v3(lo.x, lo.y, hi.z), pr_v3(hi.x, lo.y, hi.z),
                    pr_v3(hi.x, hi.y, hi.z), pr_v3(lo.x, hi.y, hi.z), face[PR_PZ]);
    }
    if ((mask & PR_FACE(PR_NZ)) != 0u) {
        pr_quad_lit(b, pr_v3(hi.x, lo.y, lo.z), pr_v3(lo.x, lo.y, lo.z),
                    pr_v3(lo.x, hi.y, lo.z), pr_v3(hi.x, hi.y, lo.z), face[PR_NZ]);
    }
}

static void pr_box1(PRBuilder *b, float x0, float y0, float z0, float x1, float y1, float z1,
                    PRRgb color, uint32_t mask)
{
    PRRgb faces[PR_FACE_COUNT];
    int i;
    for (i = 0; i < PR_FACE_COUNT; ++i) {
        faces[i] = color;
    }
    pr_box(b, pr_v3(x0, y0, z0), pr_v3(x1, y1, z1), faces, mask);
}

/* Closes a builder into a PRMesh and advances the pools. */
static PIXELICA_Status pr_finish(PixelRoom *room, PRBuilder *b, PRMesh *out, uint16_t fallback_color)
{
    if (b->failed || b->v_count == 0u || b->i_count == 0u) {
        return PIXELICA_ERR_CAPACITY;
    }
    out->mesh.vertices = b->v;
    out->mesh.vertex_count = b->v_count;
    out->mesh.indices = b->idx;
    out->mesh.index_count = b->i_count;
    out->mesh.color = fallback_color;
    out->material.uvs = b->uv;
    out->material.triangle_colors = b->tri_col;
    out->material.texture = NULL;
    room->v_used += b->v_count;
    room->i_used += b->i_count;
    if (b->uv != NULL) {
        room->uv_used += b->v_count;
    }
    return PIXELICA_OK;
}

#define PR_DEBUG_MAGENTA 0xF81Fu

/* ------------------------------------------------------------------------ */
/* Static room geometry                                                      */
/* ------------------------------------------------------------------------ */

#define PR_WALL_TOP 2.85f
#define PR_CAP_TOP 3.0f
#define PR_WALL_LENGTH 8.0f

#define PR_FLOOR_TILES 4u

static PIXELICA_Status pr_build_floor(PixelRoom *room)
{
    PRBuilder b;
    uint32_t i, j;
    const float tile = (2.0f * PIXEL_ROOM_HALF_EXTENT) / (float)PR_FLOOR_TILES;

    pr_begin(&b, room, true);
    for (j = 0; j < PR_FLOOR_TILES; ++j) {
        for (i = 0; i < PR_FLOOR_TILES; ++i) {
            float x0 = -PIXEL_ROOM_HALF_EXTENT + tile * (float)i;
            float z0 = -PIXEL_ROOM_HALF_EXTENT + tile * (float)j;
            float x1 = x0 + tile;
            float z1 = z0 + tile;
            /* Seen from above (+Y); texture top row toward -Z. Each tile maps
             * the whole texture because UVs are clamped, not wrapped. */
            PIXELICA_Vec2 uv[4];
            uv[0] = pr_v2(0.0f, 1.0f);
            uv[1] = pr_v2(1.0f, 1.0f);
            uv[2] = pr_v2(1.0f, 0.0f);
            uv[3] = pr_v2(0.0f, 0.0f);
            pr_quad_color(&b, pr_v3(x0, 0.0f, z1), pr_v3(x1, 0.0f, z1), pr_v3(x1, 0.0f, z0),
                          pr_v3(x0, 0.0f, z0), 0xFFFFu, uv);
        }
    }
    return pr_finish(room, &b, &room->floor, PR_DEBUG_MAGENTA);
}

typedef struct PRBand {
    float y0, y1;
    PRRgb color;
} PRBand;

typedef struct PROpening {
    float s0, s1, y0, y1;
} PROpening;

static void pr_wall_rect(PRBuilder *b, PIXELICA_Vec3 origin, PIXELICA_Vec3 u, float s0, float s1,
                         float ya, float yb, PRRgb color)
{
    PIXELICA_Vec3 p00 = pr_v3(origin.x + u.x * s0, ya, origin.z + u.z * s0);
    PIXELICA_Vec3 p10 = pr_v3(origin.x + u.x * s1, ya, origin.z + u.z * s1);
    PIXELICA_Vec3 p11 = pr_v3(p10.x, yb, p10.z);
    PIXELICA_Vec3 p01 = pr_v3(p00.x, yb, p00.z);
    pr_quad_lit(b, p00, p10, p11, p01, color);
}

/*
 * One wall facing the room: the front normal is u x up. Horizontal bands give
 * the colours; an optional rectangular opening is left out of the geometry.
 */
static void pr_wall(PRBuilder *b, PIXELICA_Vec3 origin, PIXELICA_Vec3 u, float length, const PRBand *bands,
                    size_t band_count, const PROpening *opening)
{
    float brk[16];
    float cuts[4];
    size_t n = 0, i, k, cut_count = 0;

    cuts[cut_count++] = 0.0f;
    if (opening != NULL) {
        if (opening->s0 > 0.0f && opening->s0 < length) {
            cuts[cut_count++] = opening->s0;
        }
        if (opening->s1 > 0.0f && opening->s1 < length) {
            cuts[cut_count++] = opening->s1;
        }
    }
    cuts[cut_count++] = length;

    for (i = 0; i < band_count && n + 2u <= 14u; ++i) {
        brk[n++] = bands[i].y0;
        brk[n++] = bands[i].y1;
    }
    if (opening != NULL) {
        brk[n++] = opening->y0;
        brk[n++] = opening->y1;
    }
    for (i = 1; i < n; ++i) {
        float v = brk[i];
        size_t j = i;
        while (j > 0u && brk[j - 1u] > v) {
            brk[j] = brk[j - 1u];
            --j;
        }
        brk[j] = v;
    }
    for (i = 0; i + 1u < n; ++i) {
        float ya = brk[i], yb = brk[i + 1u];
        float mid = 0.5f * (ya + yb);
        const PRBand *band = NULL;
        bool in_opening;
        if (yb - ya < 1e-4f) {
            continue;
        }
        for (k = 0; k < band_count; ++k) {
            if (mid > bands[k].y0 && mid < bands[k].y1) {
                band = &bands[k];
            }
        }
        if (band == NULL) {
            continue;
        }
        in_opening = opening != NULL && mid > opening->y0 && mid < opening->y1;
        /*
         * Every row is cut at the opening's sides too, so all rectangles form one
         * grid with shared corners. A full-width row above or below the opening
         * would otherwise leave T-junctions (the neighbours have extra vertices on
         * its edge) and rasterisation cracks show the background as black dots.
         */
        for (k = 0; k + 1u < cut_count; ++k) {
            float sa = cuts[k], sb = cuts[k + 1u];
            float smid = 0.5f * (sa + sb);
            if (in_opening && smid > opening->s0 && smid < opening->s1) {
                continue;
            }
            pr_wall_rect(b, origin, u, sa, sb, ya, yb, band->color);
        }
    }
}

/* Opening placement shared by the shell (hole) and the window/door meshes. */
static const PROpening k_window_opening = {4.5f, 6.5f, 1.0f, 2.3f};  /* north wall, s = x + 4 */
static const PROpening k_door_opening = {2.3f, 3.7f, 0.0f, 2.3f};    /* west wall, s = 4 - z */

static PIXELICA_Status pr_build_shell(PixelRoom *room)
{
    PRBuilder b;
    PRBand bands[3];
    const float h = PIXEL_ROOM_HALF_EXTENT;

    bands[0].y0 = 0.0f;
    bands[0].y1 = 0.9f;
    bands[0].color = pr_rgb(92, 132, 150); /* wainscot */
    bands[1].y0 = 0.9f;
    bands[1].y1 = 1.0f;
    bands[1].color = pr_rgb(246, 238, 218); /* trim */
    bands[2].y0 = 1.0f;
    bands[2].y1 = PR_WALL_TOP;
    bands[2].color = pr_rgb(236, 204, 156); /* plaster */

    pr_begin(&b, room, false);

    /* North wall (z = -4), faces +Z. West wall (x = -4), faces +X. */
    pr_wall(&b, pr_v3(-h, 0.0f, -h), pr_v3(1.0f, 0.0f, 0.0f), PR_WALL_LENGTH, bands, 3u,
            &k_window_opening);
    pr_wall(&b, pr_v3(-h, 0.0f, h), pr_v3(0.0f, 0.0f, -1.0f), PR_WALL_LENGTH, bands, 3u,
            &k_door_opening);

    /* Wall caps (dark fascia + top), leaving no coplanar faces with the walls. */
    pr_box1(&b, -4.5f, PR_WALL_TOP, -4.5f, -h, PR_CAP_TOP, h, pr_rgb(122, 86, 62),
            PR_ALL_FACES);
    pr_box1(&b, -h, PR_WALL_TOP, -4.5f, h, PR_CAP_TOP, -h, pr_rgb(122, 86, 62),
            PR_ALL_FACES & ~PR_FACE(PR_NX));

    /* Floor slab (visible as a ring and as thickness) and low curbs on the open sides. */
    pr_box1(&b, -4.5f, -0.4f, -4.5f, 4.5f, -0.03f, 4.5f, pr_rgb(96, 68, 52), PR_ALL_FACES);
    pr_box1(&b, h, -0.03f, -4.5f, 4.5f, 0.18f, 4.5f, pr_rgb(158, 114, 80),
            PR_ALL_FACES & ~PR_FACE(PR_NY));
    pr_box1(&b, -4.5f, -0.03f, h, h, 0.18f, 4.5f, pr_rgb(158, 114, 80),
            PR_ALL_FACES & ~(PR_FACE(PR_NY) | PR_FACE(PR_PX)));

    return pr_finish(room, &b, &room->shell, PR_DEBUG_MAGENTA);
}

static PIXELICA_Status pr_build_decor(PixelRoom *room)
{
    PRBuilder b;
    const PRRgb border = pr_rgb(176, 72, 72);
    const PRRgb inner = pr_rgb(222, 128, 98);
    const PRRgb stripe = pr_rgb(246, 214, 150);

    pr_begin(&b, room, false);
    /* Rug under the desk and chair; heights differ by well over a depth step. */
    pr_quad_lit(&b, pr_v3(-3.4f, 0.03f, 0.3f), pr_v3(0.6f, 0.03f, 0.3f),
                pr_v3(0.6f, 0.03f, -3.5f), pr_v3(-3.4f, 0.03f, -3.5f), border);
    pr_quad_lit(&b, pr_v3(-3.15f, 0.06f, 0.05f), pr_v3(0.35f, 0.06f, 0.05f),
                pr_v3(0.35f, 0.06f, -3.25f), pr_v3(-3.15f, 0.06f, -3.25f), inner);
    pr_quad_lit(&b, pr_v3(-2.9f, 0.09f, -0.2f), pr_v3(0.1f, 0.09f, -0.2f),
                pr_v3(0.1f, 0.09f, -0.45f), pr_v3(-2.9f, 0.09f, -0.45f), stripe);
    return pr_finish(room, &b, &room->decor, PR_DEBUG_MAGENTA);
}

static PIXELICA_Status pr_build_door(PixelRoom *room)
{
    PRBuilder b;
    const PRRgb wood = pr_rgb(158, 104, 64);
    const PRRgb panel = pr_rgb(124, 78, 46);
    const PRRgb frame = pr_rgb(244, 236, 216);
    const PRRgb gold = pr_rgb(236, 196, 74);

    /* Door in the west wall (x = -4), opening z in [0.3, 1.7], y in [0, 2.3]. */
    pr_begin(&b, room, false);
    pr_box1(&b, -4.14f, -0.03f, 0.2f, -4.06f, 2.4f, 1.8f, wood,
            PR_ALL_FACES & ~PR_FACE(PR_NY));
    pr_box1(&b, -4.06f, 1.35f, 0.4f, -4.03f, 2.15f, 1.6f, panel,
            PR_ALL_FACES & ~PR_FACE(PR_NX));
    pr_box1(&b, -4.06f, 0.2f, 0.4f, -4.03f, 1.15f, 1.6f, panel,
            PR_ALL_FACES & ~PR_FACE(PR_NX));
    pr_box1(&b, -4.06f, 1.17f, 0.45f, -3.98f, 1.31f, 0.58f, gold,
            PR_ALL_FACES & ~PR_FACE(PR_NX));
    /* Frame protruding 0.1 into the room. */
    pr_box1(&b, -4.0f, 0.0f, 1.7f, -3.9f, 2.3f, 1.82f, frame,
            PR_ALL_FACES & ~(PR_FACE(PR_NX) | PR_FACE(PR_NY) | PR_FACE(PR_PY)));
    pr_box1(&b, -4.0f, 0.0f, 0.18f, -3.9f, 2.3f, 0.3f, frame,
            PR_ALL_FACES & ~(PR_FACE(PR_NX) | PR_FACE(PR_NY) | PR_FACE(PR_PY)));
    pr_box1(&b, -4.0f, 2.3f, 0.18f, -3.9f, 2.42f, 1.82f, frame,
            PR_ALL_FACES & ~PR_FACE(PR_NX));
    return pr_finish(room, &b, &room->door, PR_DEBUG_MAGENTA);
}

static PIXELICA_Status pr_build_window(PixelRoom *room)
{
    PRBuilder b;
    const PRRgb frame = pr_rgb(244, 236, 216);
    const PRRgb sill = pr_rgb(164, 112, 72);
    const uint16_t sky_low = PIXELICA_RGB565(158, 212, 238);
    const uint16_t sky_high = PIXELICA_RGB565(104, 164, 226);
    const uint16_t sun = PIXELICA_RGB565(255, 240, 168);
    const uint16_t cloud = PIXELICA_RGB565(252, 252, 252);
    const uint32_t no_y = ~(PR_FACE(PR_PY) | PR_FACE(PR_NY));
    const uint32_t no_x = ~(PR_FACE(PR_PX) | PR_FACE(PR_NX));

    /* Window in the north wall (z = -4), opening x in [0.5, 2.5], y in [1, 2.3]. */
    pr_begin(&b, room, false);
    /* Unlit sky behind the wall plane (farther from the camera than the frame). */
    pr_quad_color(&b, pr_v3(0.4f, 0.9f, -4.1f), pr_v3(2.6f, 0.9f, -4.1f),
                  pr_v3(2.6f, 1.65f, -4.1f), pr_v3(0.4f, 1.65f, -4.1f), sky_low, NULL);
    pr_quad_color(&b, pr_v3(0.4f, 1.65f, -4.1f), pr_v3(2.6f, 1.65f, -4.1f),
                  pr_v3(2.6f, 2.4f, -4.1f), pr_v3(0.4f, 2.4f, -4.1f), sky_high, NULL);
    pr_quad_color(&b, pr_v3(1.7f, 1.9f, -4.07f), pr_v3(2.1f, 1.9f, -4.07f),
                  pr_v3(2.1f, 2.2f, -4.07f), pr_v3(1.7f, 2.2f, -4.07f), sun, NULL);
    pr_quad_color(&b, pr_v3(0.62f, 1.28f, -4.07f), pr_v3(1.2f, 1.28f, -4.07f),
                  pr_v3(1.2f, 1.44f, -4.07f), pr_v3(0.62f, 1.44f, -4.07f), cloud, NULL);
    /* Frame (-Z faces would be coplanar with the wall, so they are skipped). */
    pr_box1(&b, 0.38f, 1.0f, -4.0f, 0.5f, 2.3f, -3.92f, frame,
            PR_ALL_FACES & ~(PR_FACE(PR_NZ) | PR_FACE(PR_PY) | PR_FACE(PR_NY)));
    pr_box1(&b, 2.5f, 1.0f, -4.0f, 2.62f, 2.3f, -3.92f, frame,
            PR_ALL_FACES & ~(PR_FACE(PR_NZ) | PR_FACE(PR_PY) | PR_FACE(PR_NY)));
    pr_box1(&b, 0.38f, 2.3f, -4.0f, 2.62f, 2.42f, -3.92f, frame, PR_ALL_FACES & ~PR_FACE(PR_NZ));
    pr_box1(&b, 0.3f, 0.92f, -4.0f, 2.7f, 1.0f, -3.8f, sill, PR_ALL_FACES & ~PR_FACE(PR_NZ));
    /* Mullions. */
    pr_box1(&b, 1.46f, 1.0f, -4.06f, 1.54f, 2.3f, -3.97f, frame, PR_ALL_FACES & no_y);
    pr_box1(&b, 0.5f, 1.61f, -4.06f, 2.5f, 1.69f, -3.97f, frame, PR_ALL_FACES & no_x);
    return pr_finish(room, &b, &room->window, PR_DEBUG_MAGENTA);
}

static PIXELICA_Status pr_build_cube(PixelRoom *room)
{
    PRBuilder b;
    PRRgb faces[PR_FACE_COUNT];
    int i;
    const float hw = PIXEL_ROOM_PLAYER_HALF;

    for (i = 0; i < PR_FACE_COUNT; ++i) {
        faces[i] = pr_rgb(64, 132, 222);
    }
    faces[PR_PZ] = pr_rgb(255, 172, 56); /* the face the cube looks along */
    faces[PR_PY] = pr_rgb(120, 186, 255);
    pr_begin(&b, room, false);
    pr_box(&b, pr_v3(-hw, 0.0f, -hw), pr_v3(hw, PIXEL_ROOM_PLAYER_HEIGHT, hw), faces,
           PR_ALL_FACES & ~PR_FACE(PR_NY));
    return pr_finish(room, &b, &room->cube, PR_DEBUG_MAGENTA);
}

static PIXELICA_Status pr_build_markers(PixelRoom *room)
{
    static const PRRgb colors[PR_MARKER_VARIANTS] = {
        {255, 214, 58},  /* 0: in reach */
        {80, 210, 112},  /* 1: investigated */
        {88, 232, 232},  /* 2: in reach and investigated */
        {255, 255, 255}, /* 3: flash after investigating */
    };
    const float h = 0.28f, r = 0.2f;
    uint32_t variant;

    for (variant = 0; variant < PR_MARKER_VARIANTS; ++variant) {
        PRBuilder b;
        PIXELICA_Vec3 top = pr_v3(0.0f, h, 0.0f);
        PIXELICA_Vec3 bottom = pr_v3(0.0f, -h, 0.0f);
        PIXELICA_Vec3 ring[4];
        uint32_t k;
        PIXELICA_Status status;

        ring[0] = pr_v3(r, 0.0f, 0.0f);
        ring[1] = pr_v3(0.0f, 0.0f, r);
        ring[2] = pr_v3(-r, 0.0f, 0.0f);
        ring[3] = pr_v3(0.0f, 0.0f, -r);
        pr_begin(&b, room, false);
        for (k = 0; k < 4u; ++k) {
            uint32_t next = (k + 1u) % 4u;
            pr_tri_lit(&b, top, ring[next], ring[k], colors[variant]);
            pr_tri_lit(&b, bottom, ring[k], ring[next], colors[variant]);
        }
        status = pr_finish(room, &b, &room->marker[variant], PR_DEBUG_MAGENTA);
        if (status != PIXELICA_OK) {
            return status;
        }
    }
    return PIXELICA_OK;
}

static void pr_hud_quad(PRBuilder *b, float w, float h, float px0, float py0, float px1,
                        float py1, float z, uint16_t color)
{
    float x0 = -1.0f + 2.0f * px0 / w;
    float x1 = -1.0f + 2.0f * px1 / w;
    float ytop = 1.0f - 2.0f * py0 / h;
    float ybot = 1.0f - 2.0f * py1 / h;
    pr_quad_color(b, pr_v3(x0, ybot, z), pr_v3(x1, ybot, z), pr_v3(x1, ytop, z),
                  pr_v3(x0, ytop, z), color, NULL);
}

/* Quads, in order: strip, one pip per prop, result flash. Colours come from pr_sync_scene. */
static PIXELICA_Status pr_build_hud(PixelRoom *room)
{
    PRBuilder b;
    const float w = (float)room->width;
    const float h = (float)room->height;
    float pip = floorf(h / 24.0f);
    float gap, margin, y0, y1, flash_x0, flash_x1;
    uint32_t i;
    PIXELICA_Status status;

    if (pip < 3.0f) {
        pip = 3.0f;
    }
    gap = floorf(pip * 0.5f);
    if (gap < 1.0f) {
        gap = 1.0f;
    }
    margin = pip;
    y0 = margin;
    y1 = margin + pip;
    flash_x0 = margin + (float)PIXEL_ROOM_PROP_COUNT * (pip + gap) + gap;
    flash_x1 = flash_x0 + 2.0f * pip;

    pr_begin(&b, room, false);
    pr_hud_quad(&b, w, h, margin - gap, y0 - gap, flash_x1 + gap, y1 + gap, -0.99f, 0u);
    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        float x0 = margin + (float)i * (pip + gap);
        pr_hud_quad(&b, w, h, x0, y0, x0 + pip, y1, -0.995f, 0u);
    }
    pr_hud_quad(&b, w, h, flash_x0, y0, flash_x1, y1, -0.995f, 0u);

    room->hud_colors = b.tri_col;
    status = pr_finish(room, &b, &room->hud, PR_DEBUG_MAGENTA);
    if (status == PIXELICA_OK && room->hud.mesh.index_count != PR_HUD_QUADS * 6u) {
        return PIXELICA_ERR_CAPACITY;
    }
    return status;
}

static PIXELICA_Status pr_build_geometry(PixelRoom *room)
{
    PIXELICA_Status status = pr_build_floor(room);
    if (status == PIXELICA_OK) {
        room->floor.material.texture = &room->checker;
        status = pr_build_shell(room);
    }
    if (status == PIXELICA_OK) {
        status = pr_build_decor(room);
    }
    if (status == PIXELICA_OK) {
        status = pr_build_door(room);
    }
    if (status == PIXELICA_OK) {
        status = pr_build_window(room);
    }
    if (status == PIXELICA_OK) {
        status = pr_build_cube(room);
    }
    if (status == PIXELICA_OK) {
        status = pr_build_markers(room);
    }
    if (status == PIXELICA_OK) {
        status = pr_build_hud(room);
    }
    return status;
}

/* ------------------------------------------------------------------------ */
/* Props                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct PRPropSpec {
    const char *name;
    float extent;   /* largest dimension after scaling */
    float x, z;     /* floor position (ignored when on_top >= 0) */
    float yaw;
    int on_top;     /* prop index this one sits on, or -1 */
    float fx, fz;   /* fraction of the base footprint when on_top >= 0 */
    float reach;
    uint8_t priority;
    bool solid;
} PRPropSpec;

static const PRPropSpec k_props[PIXEL_ROOM_PROP_COUNT] = {
    {"desk", 3.0f, -1.6f, -3.0f, 0.0f, -1, 0.0f, 0.0f, PIXEL_ROOM_REACH, 0u, true},
    {"chair", 1.35f, -1.5f, -1.45f, PR_PI + 0.2f, -1, 0.0f, 0.0f, PIXEL_ROOM_REACH, 0u, true},
    /* The small items sit near the desk's front (+Z) edge so the cube can get within reach. */
    {"book", 0.6f, 0.0f, 0.0f, 0.35f, (int)PIXEL_ROOM_PROP_DESK, 0.30f, 0.76f,
     PIXEL_ROOM_SMALL_REACH, 1u, false},
    {"cup", 0.34f, 0.0f, 0.0f, 0.0f, (int)PIXEL_ROOM_PROP_DESK, 0.80f, 0.80f,
     PIXEL_ROOM_SMALL_REACH, 1u, false},
    {"plant", 1.8f, 3.0f, -3.0f, 0.0f, -1, 0.0f, 0.0f, PIXEL_ROOM_REACH, 0u, true},
};

const char *pixel_room_prop_name(uint32_t prop)
{
    return prop < PIXEL_ROOM_PROP_COUNT ? k_props[prop].name : "";
}

static void pr_prop_model(float out[16], float wx, float wy, float wz, float yaw, float scale,
                          float cx, float miny, float cz)
{
    float t[16], r[16], s[16], c[16], a[16], b[16];
    pixelica_mat4_translation(c, -cx, -miny, -cz);
    pixelica_mat4_scale(s, scale, scale, scale);
    pixelica_mat4_rotation_y(r, yaw);
    pixelica_mat4_translation(t, wx, wy, wz);
    pixelica_mat4_multiply(a, s, c);
    pixelica_mat4_multiply(b, r, a);
    pixelica_mat4_multiply(out, t, b);
}

static PIXELICA_AABB pr_transform_box(const float m[16], PIXELICA_Vec3 lo, PIXELICA_Vec3 hi)
{
    PIXELICA_AABB out;
    int i;
    for (i = 0; i < 8; ++i) {
        float q[4];
        PIXELICA_Vec3 p = pr_v3((i & 1) != 0 ? hi.x : lo.x, (i & 2) != 0 ? hi.y : lo.y,
                           (i & 4) != 0 ? hi.z : lo.z);
        pixelica_mat4_transform_point(q, m, p);
        if (i == 0) {
            out.min = pr_v3(q[0], q[1], q[2]);
            out.max = out.min;
        } else {
            out.min.x = q[0] < out.min.x ? q[0] : out.min.x;
            out.min.y = q[1] < out.min.y ? q[1] : out.min.y;
            out.min.z = q[2] < out.min.z ? q[2] : out.min.z;
            out.max.x = q[0] > out.max.x ? q[0] : out.max.x;
            out.max.y = q[1] > out.max.y ? q[1] : out.max.y;
            out.max.z = q[2] > out.max.z ? q[2] : out.max.z;
        }
    }
    return out;
}

/*
 * Bakes the room's light into the asset's per-triangle colours. The colours
 * live in the room's own storage (the loader copied them there), so writing
 * them is legitimate; each prop is used exactly once so baking in place is safe.
 */
static void pr_bake_prop_lighting(PIXELICA_MeshAsset *asset, float yaw)
{
    uint16_t *colors = (uint16_t *)(uintptr_t)asset->material.triangle_colors;
    const PIXELICA_Mesh *mesh = &asset->mesh;
    const float cs = cosf(yaw), sn = sinf(yaw);
    uint32_t t, tri_count = mesh->index_count / 3u;

    if (colors == NULL) {
        return;
    }
    for (t = 0; t < tri_count; ++t) {
        const PIXELICA_Vec3 *a = &mesh->vertices[mesh->indices[t * 3u]];
        const PIXELICA_Vec3 *b = &mesh->vertices[mesh->indices[t * 3u + 1u]];
        const PIXELICA_Vec3 *c = &mesh->vertices[mesh->indices[t * 3u + 2u]];
        float n[3], wx, wz;
        uint32_t c565 = colors[t];
        uint32_t r5 = (c565 >> 11) & 0x1Fu, g6 = (c565 >> 5) & 0x3Fu, b5 = c565 & 0x1Fu;
        uint32_t r8 = (r5 << 3) | (r5 >> 2), g8 = (g6 << 2) | (g6 >> 4), b8 = (b5 << 3) | (b5 >> 2);

        pr_triangle_normal(*a, *b, *c, n);
        wx = n[0] * cs + n[2] * sn; /* rotate the normal by the prop's yaw */
        wz = -n[0] * sn + n[2] * cs;
        colors[t] = pr_shade_rgb(r8, g8, b8, pr_light(wx, n[1], wz));
    }
}

static PIXELICA_Status pr_place_props(PixelRoom *room)
{
    uint32_t i;
    const float limit = PIXEL_ROOM_HALF_EXTENT - 0.02f;

    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        const PRPropSpec *spec = &k_props[i];
        PRProp *prop = &room->props[i];
        const PIXELICA_Mesh *mesh = &prop->asset.mesh;
        PIXELICA_Vec3 lo, hi;
        float m[16], scale, cx, cz, wx, wy = 0.0f, wz, biggest;
        uint32_t v;
        PIXELICA_AABB bounds;

        if (mesh->vertex_count == 0u || mesh->vertices == NULL) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        lo = hi = mesh->vertices[0];
        for (v = 1; v < mesh->vertex_count; ++v) {
            const PIXELICA_Vec3 *p = &mesh->vertices[v];
            lo.x = p->x < lo.x ? p->x : lo.x;
            lo.y = p->y < lo.y ? p->y : lo.y;
            lo.z = p->z < lo.z ? p->z : lo.z;
            hi.x = p->x > hi.x ? p->x : hi.x;
            hi.y = p->y > hi.y ? p->y : hi.y;
            hi.z = p->z > hi.z ? p->z : hi.z;
        }
        biggest = hi.x - lo.x;
        biggest = (hi.y - lo.y) > biggest ? (hi.y - lo.y) : biggest;
        biggest = (hi.z - lo.z) > biggest ? (hi.z - lo.z) : biggest;
        if (!(biggest > 1e-6f)) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        scale = spec->extent / biggest;
        cx = 0.5f * (lo.x + hi.x);
        cz = 0.5f * (lo.z + hi.z);

        if (spec->on_top >= 0) {
            const PIXELICA_AABB *base = &room->props[spec->on_top].bounds;
            wx = base->min.x + (base->max.x - base->min.x) * spec->fx;
            wz = base->min.z + (base->max.z - base->min.z) * spec->fz;
            wy = base->max.y;
        } else {
            wx = spec->x;
            wz = spec->z;
        }
        pr_prop_model(m, wx, wy, wz, spec->yaw, scale, cx, lo.y, cz);
        bounds = pr_transform_box(m, lo, hi);
        if (spec->on_top < 0) {
            /* Keep floor props inside the walls whatever the asset's proportions. */
            if (bounds.min.x < -limit) {
                wx += -limit - bounds.min.x;
            } else if (bounds.max.x > limit) {
                wx += limit - bounds.max.x;
            }
            if (bounds.min.z < -limit) {
                wz += -limit - bounds.min.z;
            } else if (bounds.max.z > limit) {
                wz += limit - bounds.max.z;
            }
            pr_prop_model(m, wx, wy, wz, spec->yaw, scale, cx, lo.y, cz);
            bounds = pr_transform_box(m, lo, hi);
        }
        prop->bounds = bounds;
        prop->priority = spec->priority;
        prop->reach = spec->reach;
        prop->solid = spec->solid;
        pr_bake_prop_lighting(&prop->asset, spec->yaw);

        /* The node is added right away: it needs this prop's final model matrix. */
        {
            PIXELICA_SceneNode node;
            PIXELICA_Status status;
            memset(&node, 0, sizeof(node));
            node.mesh = &prop->asset.mesh;
            node.material = &prop->asset.material;
            memcpy(node.model, m, sizeof(node.model));
            node.bounds = bounds;
            node.id = PR_ID_PROP_BASE + i;
            node.solid = spec->solid;
            prop->node = (uint32_t)room->scene.count;
            status = pixelica_scene_add(&room->scene, &node);
            if (status != PIXELICA_OK) {
                return status;
            }
        }
    }
    return PIXELICA_OK;
}

/* ------------------------------------------------------------------------ */
/* Scene, collision world, camera                                            */
/* ------------------------------------------------------------------------ */

static PIXELICA_Status pr_add_node(PixelRoom *room, const PRMesh *mesh, PIXELICA_AABB bounds, uint32_t id,
                              uint32_t *out_index)
{
    PIXELICA_SceneNode node;
    uint32_t index = (uint32_t)room->scene.count;
    PIXELICA_Status status;

    memset(&node, 0, sizeof(node));
    node.mesh = &mesh->mesh;
    node.material = &mesh->material;
    pixelica_mat4_identity(node.model);
    node.bounds = bounds;
    node.id = id;
    node.solid = false;
    status = pixelica_scene_add(&room->scene, &node);
    if (status == PIXELICA_OK && out_index != NULL) {
        *out_index = index;
    }
    return status;
}

PIXELICA_AABB pr_player_box(float x, float z)
{
    return pr_aabb(x - PIXEL_ROOM_PLAYER_HALF, 0.0f, z - PIXEL_ROOM_PLAYER_HALF,
                   x + PIXEL_ROOM_PLAYER_HALF, PIXEL_ROOM_PLAYER_HEIGHT,
                   z + PIXEL_ROOM_PLAYER_HALF);
}

bool pr_box_is_free(const PixelRoom *room, const PIXELICA_AABB *box)
{
    size_t i;
    for (i = 0; i < room->obstacle_count; ++i) {
        if (pixelica_aabb_overlaps(box, &room->obstacles[i])) {
            return false;
        }
    }
    return true;
}

static bool pr_box_inside_room(const PIXELICA_AABB *box)
{
    const float h = PIXEL_ROOM_HALF_EXTENT;
    return box->min.x >= -h && box->max.x <= h && box->min.z >= -h && box->max.z <= h;
}

void pr_camera_view(float out[16], float yaw)
{
    /* view = T(0,0,-dist) * Rx(pitch) * Ry(-yaw) * T(-target): an orbit around the target. */
    float t_target[16], ry[16], rx[16], t_dist[16], a[16], b[16];
    pixelica_mat4_translation(t_target, 0.0f, -1.0f, 0.0f);
    pixelica_mat4_rotation_y(ry, -yaw);
    pixelica_mat4_rotation_x(rx, 0.62f);
    pixelica_mat4_translation(t_dist, 0.0f, 0.0f, -13.5f);
    pixelica_mat4_multiply(a, ry, t_target);
    pixelica_mat4_multiply(b, rx, a);
    pixelica_mat4_multiply(out, t_dist, b);
}

PIXELICA_Status pr_build_world(PixelRoom *room)
{
    static const float k_spawn[][2] = {{1.0f, 1.5f}, {0.0f, 0.5f}, {2.0f, 2.0f}, {-1.0f, 2.0f},
                                       {2.5f, 0.0f}};
    PIXELICA_Status status;
    uint32_t i;
    size_t n;
    PIXELICA_AABB zero_box = pr_aabb(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    bool spawned = false;

    pixelica_scene_init(&room->scene, room->nodes, PR_NODE_CAP);

    status = pr_build_geometry(room);
    if (status != PIXELICA_OK) {
        return status;
    }

    /* Static scene nodes first, then props, the cube and the markers. */
    status = pr_add_node(room, &room->floor, pr_aabb(-4.0f, -0.4f, -4.0f, 4.0f, 0.0f, 4.0f),
                         PR_ID_FLOOR, NULL);
    if (status == PIXELICA_OK) {
        status = pr_add_node(room, &room->shell, pr_aabb(-4.5f, -0.4f, -4.5f, 4.5f, 3.0f, 4.5f),
                             PR_ID_SHELL, NULL);
    }
    if (status == PIXELICA_OK) {
        status = pr_add_node(room, &room->decor, pr_aabb(-3.4f, 0.0f, -3.5f, 0.6f, 0.1f, 0.3f),
                             PR_ID_DECOR, NULL);
    }
    if (status == PIXELICA_OK) {
        status = pr_add_node(room, &room->door, pr_aabb(-4.14f, 0.0f, 0.18f, -3.9f, 2.42f, 1.82f),
                             PR_ID_DOOR, NULL);
    }
    if (status == PIXELICA_OK) {
        status = pr_add_node(room, &room->window, pr_aabb(0.3f, 0.9f, -4.1f, 2.7f, 2.42f, -3.8f),
                             PR_ID_WINDOW, NULL);
    }
    if (status == PIXELICA_OK) {
        status = pr_place_props(room);
    }
    if (status == PIXELICA_OK) {
        status = pr_add_node(room, &room->cube, pr_player_box(0.0f, 0.0f), PR_ID_PLAYER,
                             &room->player_node);
    }
    for (i = 0; status == PIXELICA_OK && i < PIXEL_ROOM_PROP_COUNT; ++i) {
        status = pr_add_node(room, &room->marker[0], zero_box, PR_ID_MARKER_BASE + i,
                             &room->props[i].marker_node);
    }
    if (status != PIXELICA_OK) {
        return status;
    }

    status = pixelica_mat4_perspective(room->projection, 0.8f,
                                  (float)room->width / (float)room->height, 1.0f, 40.0f);
    if (status != PIXELICA_OK) {
        return status;
    }

    /* Collision world: four boundary walls plus every solid scene node. */
    room->walls[0] = pr_aabb(-4.5f, 0.0f, -4.5f, 4.5f, 3.0f, -PIXEL_ROOM_HALF_EXTENT);
    room->walls[1] = pr_aabb(-4.5f, 0.0f, -4.5f, -PIXEL_ROOM_HALF_EXTENT, 3.0f, 4.5f);
    room->walls[2] = pr_aabb(PIXEL_ROOM_HALF_EXTENT, 0.0f, -4.5f, 4.5f, 3.0f, 4.5f);
    room->walls[3] = pr_aabb(-4.5f, 0.0f, PIXEL_ROOM_HALF_EXTENT, 4.5f, 3.0f, 4.5f);
    for (n = 0; n < PR_WALL_COUNT; ++n) {
        room->obstacles[room->obstacle_count++] = room->walls[n];
    }
    /* The scene owns "solid"; its helper gathers those bounds (the cube node is not solid). */
    n = 0;
    for (i = 0; i < room->scene.count; ++i) {
        n += room->scene.nodes[i].solid ? 1u : 0u;
    }
    if (n > PR_OBSTACLE_CAP - room->obstacle_count) {
        return PIXELICA_ERR_CAPACITY;
    }
    room->obstacle_count += pixelica_scene_solid_bounds(&room->scene, NULL,
                                                   room->obstacles + room->obstacle_count,
                                                   PR_OBSTACLE_CAP - room->obstacle_count);

    for (n = 0; n < sizeof(k_spawn) / sizeof(k_spawn[0]) && !spawned; ++n) {
        PIXELICA_AABB box = pr_player_box(k_spawn[n][0], k_spawn[n][1]);
        if (pr_box_inside_room(&box) && pr_box_is_free(room, &box)) {
            room->player = box;
            spawned = true;
        }
    }
    if (!spawned) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    room->face_x = 0.0f;
    room->face_z = -1.0f;
    room->camera_yaw = PIXEL_ROOM_START_CAMERA_YAW;
    room->focus = -1;
    room->event = PIXEL_ROOM_EVENT_NONE;
    room->event_prop = -1;
    return PIXELICA_OK;
}

/* Interior test shared with pixel_room.c (teleport). */
bool pr_box_in_room(const PIXELICA_AABB *box)
{
    return pr_box_inside_room(box);
}

/* ------------------------------------------------------------------------ */
/* Dynamic scene state                                                       */
/* ------------------------------------------------------------------------ */

static void pr_hud_set(PixelRoom *room, uint32_t quad, uint16_t color)
{
    room->hud_colors[quad * 2u] = color;
    room->hud_colors[quad * 2u + 1u] = color;
}

void pr_sync_scene(PixelRoom *room)
{
    PIXELICA_SceneNode *pn = &room->scene.nodes[room->player_node];
    float t[16], r[16], tr[16];
    const uint16_t strip = PIXELICA_RGB565(20, 22, 34);
    const uint16_t gray = PIXELICA_RGB565(70, 74, 92);
    const uint16_t done = PIXELICA_RGB565(80, 205, 115);
    const uint16_t focus = PIXELICA_RGB565(250, 220, 80);
    const uint16_t both = PIXELICA_RGB565(110, 235, 235);
    uint32_t i;
    uint32_t spin_tick = room->tick % 120u;

    /* Cube: translate to the box centre, turn to face the last move direction. */
    pixelica_mat4_translation(t, 0.5f * (room->player.min.x + room->player.max.x), 0.0f,
                         0.5f * (room->player.min.z + room->player.max.z));
    pixelica_mat4_rotation_y(r, atan2f(room->face_x, room->face_z));
    pixelica_mat4_multiply(tr, t, r);
    memcpy(pn->model, tr, sizeof(pn->model));
    pn->bounds = room->player;

    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        PRProp *prop = &room->props[i];
        PIXELICA_SceneNode *mn = &room->scene.nodes[prop->marker_node];
        bool is_done = ((room->investigated_mask >> i) & 1u) != 0u;
        bool is_focus = room->focus == (int32_t)i;
        bool flash = room->event == PIXEL_ROOM_EVENT_INVESTIGATED && room->event_prop == (int32_t)i &&
                     room->event_ticks > 0u && ((room->event_ticks / 4u) % 2u) == 0u;
        int32_t variant = -1;
        uint16_t hud_color = gray;

        if (is_focus && is_done) {
            variant = 2;
            hud_color = both;
        } else if (is_focus) {
            variant = 0;
            hud_color = focus;
        } else if (is_done) {
            variant = 1;
            hud_color = done;
        }
        if (flash) {
            variant = 3;
        }
        pr_hud_set(room, 1u + i, hud_color);

        if (variant < 0) {
            /* A node without a mesh is skipped by pixelica_scene_render. */
            mn->mesh = NULL;
            mn->material = NULL;
        } else {
            mn->mesh = &room->marker[variant].mesh;
            mn->material = &room->marker[variant].material;
        }
        {
            /* Integer triangle wave: deterministic bobbing without libm. */
            uint32_t phase = (room->tick + i * 7u) % 48u;
            uint32_t wave = phase < 24u ? phase : 48u - phase;
            float cxm = 0.5f * (prop->bounds.min.x + prop->bounds.max.x);
            float czm = 0.5f * (prop->bounds.min.z + prop->bounds.max.z);
            float cym = prop->bounds.max.y + 0.4f + (float)wave * (0.12f / 24.0f);
            float scale = is_focus ? 1.0f : 0.65f;
            float s[16], tm[16], rm[16], a[16];
            pixelica_mat4_translation(tm, cxm, cym, czm);
            pixelica_mat4_rotation_y(rm, (float)spin_tick * (PR_TWO_PI / 120.0f));
            pixelica_mat4_scale(s, scale, scale, scale);
            pixelica_mat4_multiply(a, rm, s);
            pixelica_mat4_multiply(mn->model, tm, a);
            mn->bounds = pr_aabb(cxm - 0.2f, cym - 0.28f, czm - 0.2f, cxm + 0.2f, cym + 0.28f,
                                 czm + 0.2f);
        }
    }

    pr_hud_set(room, 0u, strip);
    if (room->event == PIXEL_ROOM_EVENT_INVESTIGATED) {
        pr_hud_set(room, 1u + PIXEL_ROOM_PROP_COUNT, PIXELICA_RGB565(90, 232, 120));
    } else if (room->event == PIXEL_ROOM_EVENT_NOTHING) {
        pr_hud_set(room, 1u + PIXEL_ROOM_PROP_COUNT, PIXELICA_RGB565(232, 80, 80));
    } else {
        pr_hud_set(room, 1u + PIXEL_ROOM_PROP_COUNT, strip);
    }
}

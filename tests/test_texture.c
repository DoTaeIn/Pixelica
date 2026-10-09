/*
 * Pixelica - materials: nearest texture sampling (exact texel maps under
 * the identity camera), perspective-correct UVs against an analytic
 * reference, UV clipping robustness, per-triangle colors, validation before
 * writes, G0 flat-path compatibility, and coverage/overdraw counters.
 */
#include <float.h>

#include "test_util.h"

#define BG ((uint16_t)0x0000)
#define RED ((uint16_t)0xF800)
#define GREEN ((uint16_t)0x07E0)
#define BLUE ((uint16_t)0x001F)
#define PI_F 3.14159265358979323846f

typedef struct Target {
    PIXELICA_Context *ctx;
    uint16_t w, h;
} Target;

static const float k_identity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};

static const uint16_t k_quad_idx[6] = {0, 1, 2, 0, 2, 3};

static int open_target(Target *t, uint16_t w, uint16_t h)
{
    PIXELICA_Config cfg = pixelica_t_config(w, h);
    PIXELICA_Status st;

    t->ctx = NULL;
    t->w = w;
    t->h = h;
    st = pixelica_create(&cfg, NULL, &t->ctx);
    CHECK(st == PIXELICA_OK);
    return st == PIXELICA_OK;
}

static void close_target(Target *t)
{
    CHECK(pixelica_memory_guards_intact(t->ctx) == 1);
    pixelica_destroy(t->ctx);
    t->ctx = NULL;
}

static size_t npix(const Target *t)
{
    return (size_t)t->w * (size_t)t->h;
}

static uint16_t color_at(const Target *t, int x, int y)
{
    return pixelica_t_color_at(t->ctx, t->w, x, y);
}

static uint32_t count(const Target *t, uint16_t color)
{
    return pixelica_t_count_color(t->ctx, t->w, t->h, color);
}

static uint64_t frame_hash(const Target *t)
{
    return pixelica_t_hash16(pixelica_color_buffer(t->ctx), npix(t)) ^
           (pixelica_t_hash16(pixelica_depth_buffer(t->ctx), npix(t)) * 31u);
}

static PIXELICA_Vec3 vec3(float x, float y, float z)
{
    PIXELICA_Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static PIXELICA_Vec2 vec2(float x, float y)
{
    PIXELICA_Vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

static PIXELICA_Mesh make_mesh(const PIXELICA_Vec3 *v, uint32_t vc, const uint16_t *idx,
                          uint32_t ic, uint16_t color)
{
    PIXELICA_Mesh m;
    m.vertices = v;
    m.vertex_count = vc;
    m.indices = idx;
    m.index_count = ic;
    m.color = color;
    return m;
}

static PIXELICA_Material make_material(const PIXELICA_Vec2 *uvs, const uint16_t *tri,
                                  const PIXELICA_Texture *tex)
{
    PIXELICA_Material m;
    m.uvs = uvs;
    m.triangle_colors = tri;
    m.texture = tex;
    return m;
}

static PIXELICA_Texture make_texture(const uint16_t *px, uint16_t w, uint16_t h)
{
    PIXELICA_Texture t;
    t.pixels = px;
    t.width = w;
    t.height = h;
    return t;
}

/* Full-screen NDC quad (identity camera), counter-clockwise from +Z, with
 * UV corners: top-left (u0,v0), bottom-right (u1,v1). */
static void fullscreen_quad(PIXELICA_Vec3 v[4], PIXELICA_Vec2 uv[4], float u0, float v0,
                            float u1, float v1)
{
    v[0] = vec3(-1.0f, -1.0f, 0.0f); /* bottom-left */
    v[1] = vec3(1.0f, -1.0f, 0.0f);  /* bottom-right */
    v[2] = vec3(1.0f, 1.0f, 0.0f);   /* top-right */
    v[3] = vec3(-1.0f, 1.0f, 0.0f);  /* top-left */
    uv[0] = vec2(u0, v1);
    uv[1] = vec2(u1, v1);
    uv[2] = vec2(u1, v0);
    uv[3] = vec2(u0, v0);
}

/* Expected nearest texel index for coordinate t (clamped), or -1 when t is
 * within `margin` texels of a texel boundary (ambiguous under rounding). */
static int expected_texel(double t, int size, double margin)
{
    double s;
    double frac;
    int i;

    if (t <= 0.0) {
        return t > -margin / (double)size ? -1 : 0;
    }
    if (t >= 1.0) {
        return t < 1.0 + margin / (double)size ? -1 : size - 1;
    }
    s = t * (double)size;
    frac = s - floor(s);
    if (frac < margin || frac > 1.0 - margin) {
        return -1;
    }
    i = (int)s;
    return i < size ? i : size - 1;
}

/* ------------------------------------------------------------------------ */

static void test_checker_quadrants(void)
{
    static const uint16_t tex_px[4] = {RED, GREEN, BLUE, 0xFFE0};
    PIXELICA_Texture tex = make_texture(tex_px, 2, 2);
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec2 uv[4];
    PIXELICA_Mesh mesh;
    PIXELICA_Material mat;
    Target t;
    uint32_t bad = 0;
    int x, y;

    if (!open_target(&t, 32, 32)) {
        return;
    }
    fullscreen_quad(v, uv, 0.0f, 0.0f, 1.0f, 1.0f);
    mesh = make_mesh(v, 4, k_quad_idx, 6, BG);
    mat = make_material(uv, NULL, &tex);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    /* Texture row 0 is the top of the screen. */
    for (y = 0; y < 32; ++y) {
        for (x = 0; x < 32; ++x) {
            uint16_t want = tex_px[(y < 16 ? 0 : 2) + (x < 16 ? 0 : 1)];
            if (color_at(&t, x, y) != want) {
                ++bad;
            }
        }
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK_EQ_LONG(count(&t, RED), 256);
    CHECK_EQ_LONG(count(&t, 0xFFE0), 256);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 32 * 32);
    close_target(&t);
}

static uint16_t grid_texel(int tx, int ty)
{
    return (uint16_t)(0x1000 + ty * 16 + tx);
}

/* Every pixel of an exactly mapped and of a clamped (UV -1..2) quad matches
 * floor(u * size) computed in double, except within 1% of a texel edge. */
static void test_nearest_mapping_and_clamp(void)
{
    static uint16_t tex_px[4 * 4];
    PIXELICA_Texture tex = make_texture(tex_px, 4, 4);
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec2 uv[4];
    PIXELICA_Mesh mesh;
    PIXELICA_Material mat;
    Target t;
    int pass;

    for (pass = 0; pass < 16; ++pass) {
        tex_px[pass] = grid_texel(pass % 4, pass / 4);
    }
    if (!open_target(&t, 32, 32)) {
        return;
    }
    for (pass = 0; pass < 2; ++pass) {
        const float lo = pass == 0 ? 0.0f : -1.0f;
        const float hi = pass == 0 ? 1.0f : 2.0f;
        uint32_t bad = 0, checked = 0;
        int x, y;

        fullscreen_quad(v, uv, lo, lo, hi, hi);
        mesh = make_mesh(v, 4, k_quad_idx, 6, BG);
        mat = make_material(uv, NULL, &tex);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
        for (y = 0; y < 32; ++y) {
            for (x = 0; x < 32; ++x) {
                double u = (double)lo + (double)(hi - lo) * ((double)x + 0.5) / 32.0;
                double vv = (double)lo + (double)(hi - lo) * ((double)y + 0.5) / 32.0;
                int tx = expected_texel(u, 4, 0.01);
                int ty = expected_texel(vv, 4, 0.01);
                if (tx < 0 || ty < 0) {
                    continue;
                }
                ++checked;
                if (color_at(&t, x, y) != grid_texel(tx, ty)) {
                    ++bad;
                }
            }
        }
        CHECK_EQ_LONG(bad, 0);
        CHECK(checked > 900u);
    }
    /* Clamped pass: corners show the corner texels. */
    CHECK(color_at(&t, 0, 0) == grid_texel(0, 0));
    CHECK(color_at(&t, 31, 0) == grid_texel(3, 0));
    CHECK(color_at(&t, 0, 31) == grid_texel(0, 3));
    CHECK(color_at(&t, 31, 31) == grid_texel(3, 3));
    close_target(&t);
}

/*
 * Floor y = -1 from z = -1 (v = 1) to z = -9 (v = 0) under a 90 degree
 * perspective camera at the origin. Pixel row y sees the floor at
 * z = 1 / ndc_y, so the reference coordinate is v = (z + 9) / 8. Affine
 * screen-space interpolation would differ by up to ~0.45 in v.
 */
static void test_perspective_correct_floor(void)
{
    static uint16_t tex_px[8];
    PIXELICA_Texture tex = make_texture(tex_px, 1, 8);
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec2 uv[4];
    PIXELICA_Mesh mesh;
    PIXELICA_Material mat;
    float persp[16];
    Target t;
    uint32_t bad = 0, checked = 0, covered_rows = 0;
    int x, y, i;

    for (i = 0; i < 8; ++i) {
        tex_px[i] = (uint16_t)(0x2000 + i);
    }
    if (!open_target(&t, 64, 64)) {
        return;
    }
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 0.1f, 100.0f) ==
          PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    /* Counter-clockwise seen from above (+Y), i.e. facing the camera. */
    v[0] = vec3(-4.0f, -1.0f, -1.0f);
    v[1] = vec3(4.0f, -1.0f, -1.0f);
    v[2] = vec3(4.0f, -1.0f, -9.0f);
    v[3] = vec3(-4.0f, -1.0f, -9.0f);
    uv[0] = vec2(0.0f, 1.0f);
    uv[1] = vec2(1.0f, 1.0f);
    uv[2] = vec2(1.0f, 0.0f);
    uv[3] = vec2(0.0f, 0.0f);
    mesh = make_mesh(v, 4, k_quad_idx, 6, BG);
    mat = make_material(uv, NULL, &tex);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    CHECK(pixelica_frame_stats(t.ctx).triangles_culled == 0);

    for (y = 0; y < 64; ++y) {
        double ndc_y = 1.0 - 2.0 * ((double)y + 0.5) / 64.0;
        int row_covered = color_at(&t, 32, y) != BG;

        if (row_covered) {
            ++covered_rows;
        }
        if (ndc_y >= -1.0 / 9.0 - 1e-3) {
            continue; /* above the far edge (or too close to it) */
        }
        /* |ndc x| < 0.25 is inside the floor (half width 4/|z| >= 4/9). */
        for (x = 24; x < 40; ++x) {
            double z = 1.0 / ndc_y;
            int ty = expected_texel((z + 9.0) / 8.0, 8, 0.02);
            if (ty < 0) {
                continue;
            }
            ++checked;
            if (color_at(&t, x, y) != tex_px[ty]) {
                ++bad;
            }
        }
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK(checked > 300u);
    /* Far edge projects to row 35.56: rows 36..63 are floor. */
    CHECK_EQ_LONG(covered_rows, 28);
    CHECK(color_at(&t, 32, 35) == BG);
    /* Row 45: perspective v = 0.83 (texel 6); affine would give ~0.35. */
    CHECK(color_at(&t, 32, 45) == tex_px[6]);
    close_target(&t);
}

/* Textured geometry needing every clip plane, the near plane with a vertex
 * behind the camera and +-FLT_MAX coordinates: only texture colors appear,
 * coverage is exact where known, and nothing outside the buffers is hit. */
static void test_textured_clipping_robustness(void)
{
    static const uint16_t tex_px[4] = {0x1111, 0x2222, 0x3333, 0x4444};
    static const float sizes[3] = {100.0f, 1.0e20f, FLT_MAX};
    static const uint16_t tri_idx[3] = {0, 1, 2};
    static const uint16_t tri_idx_front[3] = {0, 2, 1};
    PIXELICA_Texture tex = make_texture(tex_px, 2, 2);
    PIXELICA_Vec3 v[3];
    PIXELICA_Vec2 uv[3];
    PIXELICA_Mesh mesh;
    PIXELICA_Material mat;
    float persp[16];
    Target t;
    int s;

    if (!open_target(&t, 64, 48)) {
        return;
    }
    uv[0] = vec2(0.0f, 1.0f);
    uv[1] = vec2(1.0f, 1.0f);
    uv[2] = vec2(0.5f, 0.0f);
    for (s = 0; s < 3; ++s) {
        const float h = sizes[s];
        uint32_t texture_px;

        v[0] = vec3(-h, -h, 0.0f);
        v[1] = vec3(h, -h, 0.0f);
        v[2] = vec3(0.0f, h, 0.0f);
        mesh = make_mesh(v, 3, tri_idx, 3, RED);
        mat = make_material(uv, NULL, &tex);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
        texture_px = count(&t, 0x1111) + count(&t, 0x2222) +
                     count(&t, 0x3333) + count(&t, 0x4444);
        CHECK_EQ_LONG(texture_px, 64 * 48);
        CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 64 * 48);
        CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).triangles_clipped, 1);
    }

    /* Near-plane crossing (one vertex behind the camera). */
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 1.5f, 100.0f) ==
          PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    v[0] = vec3(-1.0f, -1.0f, -2.0f);
    v[1] = vec3(1.0f, -1.0f, -2.0f);
    v[2] = vec3(0.0f, -1.0f, 2.0f);
    mesh = make_mesh(v, 3, tri_idx_front, 3, RED);
    mat = make_material(uv, NULL, &tex);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    {
        uint32_t texture_px = count(&t, 0x1111) + count(&t, 0x2222) +
                              count(&t, 0x3333) + count(&t, 0x4444);
        CHECK(texture_px > 0u);
        CHECK_EQ_LONG(texture_px + count(&t, BG), 64 * 48);
        CHECK_EQ_LONG(texture_px, pixelica_frame_stats(t.ctx).pixels_written);
    }

    /* Extreme depth ratio: one vertex just past the near plane (w = 0.2),
     * two at w = 1e36, so per-vertex 1/w spans ~36 orders of magnitude.
     * Must stay bounded and produce only texture colors. */
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 0.1f, 1.0e37f) ==
          PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    pixelica_set_cull_mode(t.ctx, PIXELICA_CULL_NONE);
    v[0] = vec3(0.0f, -1.0f, -0.2f);
    v[1] = vec3(-1.0e35f, -1.0f, -1.0e36f);
    v[2] = vec3(1.0e35f, -1.0f, -1.0e36f);
    mesh = make_mesh(v, 3, tri_idx, 3, RED);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    {
        uint32_t texture_px = count(&t, 0x1111) + count(&t, 0x2222) +
                              count(&t, 0x3333) + count(&t, 0x4444);
        CHECK(texture_px > 0u);
        CHECK_EQ_LONG(count(&t, RED), 0);
        CHECK_EQ_LONG(texture_px + count(&t, BG), 64 * 48);
    }
    close_target(&t);
}

static void test_triangle_colors_and_flat_compat(void)
{
    static const uint16_t two_colors[2] = {GREEN, BLUE};
    static const uint16_t same_colors[2] = {RED, RED};
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec2 uv[4];
    PIXELICA_Mesh mesh;
    PIXELICA_Material mat;
    Target t;
    uint64_t flat_hash;
    uint32_t g, b;

    if (!open_target(&t, 32, 32)) {
        return;
    }
    fullscreen_quad(v, uv, 0.0f, 0.0f, 1.0f, 1.0f);
    v[0].x = -0.5f; /* not full screen, so the background stays visible */
    v[3].x = -0.5f;
    mesh = make_mesh(v, 4, k_quad_idx, 6, RED);

    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &mesh, k_identity) == PIXELICA_OK);
    flat_hash = frame_hash(&t);

    /* NULL material, an all-NULL material, and per-triangle colors equal to
     * mesh.color all reproduce the plain flat draw bit for bit. UVs alone
     * (no texture) do not change flat shading either. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, NULL) == PIXELICA_OK);
    CHECK(frame_hash(&t) == flat_hash);
    mat = make_material(NULL, NULL, NULL);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    CHECK(frame_hash(&t) == flat_hash);
    mat = make_material(uv, same_colors, NULL);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    CHECK(frame_hash(&t) == flat_hash);

    /* Distinct triangle colors split the quad; mesh.color is unused. */
    mat = make_material(NULL, two_colors, NULL);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    g = count(&t, GREEN);
    b = count(&t, BLUE);
    CHECK(g > 100u && b > 100u);
    CHECK_EQ_LONG(g + b, 24 * 32);
    CHECK_EQ_LONG(count(&t, RED), 0);
    /* Triangle 0 = (bl, br, tr) holds the bottom-right corner. */
    CHECK(color_at(&t, 30, 30) == GREEN);
    CHECK(color_at(&t, 10, 1) == BLUE);
    close_target(&t);
}

/* Cube rendered through every entry point: identical color+depth hashes. */
static void test_flat_cube_hash_unchanged(void)
{
    static const PIXELICA_Vec3 verts[8] = {
        {-0.8f, -0.8f, -0.8f}, {0.8f, -0.8f, -0.8f},
        {-0.8f, 0.8f, -0.8f},  {0.8f, 0.8f, -0.8f},
        {-0.8f, -0.8f, 0.8f},  {0.8f, -0.8f, 0.8f},
        {-0.8f, 0.8f, 0.8f},   {0.8f, 0.8f, 0.8f},
    };
    static const uint16_t faces[6][6] = {
        {4, 5, 7, 4, 7, 6}, {1, 0, 2, 1, 2, 3}, {5, 1, 3, 5, 3, 7},
        {0, 4, 6, 0, 6, 2}, {6, 7, 3, 6, 3, 2}, {0, 1, 5, 0, 5, 4},
    };
    static const uint16_t colors[6] = {0xF800, 0x07E0, 0x001F,
                                       0xFFE0, 0xF81F, 0x07FF};
    float proj[16], view[16], vp[16], rx[16], ry[16], model[16];
    uint64_t hashes[3];
    Target t;
    int mode, f;

    if (!open_target(&t, 256, 192)) {
        return;
    }
    CHECK(pixelica_mat4_perspective(proj, 0.8726646f, 256.0f / 192.0f, 0.1f,
                               50.0f) == PIXELICA_OK);
    pixelica_mat4_translation(view, 0.0f, 0.0f, -3.0f);
    pixelica_mat4_multiply(vp, proj, view);
    pixelica_mat4_rotation_x(rx, 0.66f);
    pixelica_mat4_rotation_y(ry, 1.1f);
    pixelica_mat4_multiply(model, ry, rx);
    pixelica_set_view_projection(t.ctx, vp);
    for (mode = 0; mode < 3; ++mode) {
        pixelica_begin_frame(t.ctx, 0x1084);
        for (f = 0; f < 6; ++f) {
            PIXELICA_Mesh m = make_mesh(verts, 8, faces[f], 6, colors[f]);
            uint16_t tri[2];
            PIXELICA_Material mat;
            tri[0] = colors[f];
            tri[1] = colors[f];
            mat = make_material(NULL, tri, NULL);
            if (mode == 0) {
                CHECK(pixelica_draw_mesh(t.ctx, &m, model) == PIXELICA_OK);
            } else if (mode == 1) {
                CHECK(pixelica_draw_mesh_ex(t.ctx, &m, model, NULL) == PIXELICA_OK);
            } else {
                CHECK(pixelica_draw_mesh_ex(t.ctx, &m, model, &mat) == PIXELICA_OK);
            }
        }
        hashes[mode] = frame_hash(&t);
    }
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] == hashes[2]);
    CHECK(count(&t, 0x1084) < 256u * 192u);
    close_target(&t);
}

static void test_material_validation(void)
{
    static const uint16_t tex_px[4] = {RED, GREEN, BLUE, RED};
    PIXELICA_Texture tex = make_texture(tex_px, 2, 2);
    PIXELICA_Texture bad_tex;
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec2 uv[4];
    PIXELICA_Mesh mesh;
    PIXELICA_Material mat;
    PIXELICA_FrameStats s;
    Target t;
    uint64_t before;

    if (!open_target(&t, 32, 32)) {
        return;
    }
    fullscreen_quad(v, uv, 0.0f, 0.0f, 1.0f, 1.0f);
    v[0].x = 0.0f;
    v[3].x = 0.0f;
    mesh = make_mesh(v, 4, k_quad_idx, 6, RED);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &mesh, k_identity) == PIXELICA_OK);
    v[0].x = -1.0f;
    v[3].x = -1.0f;
    before = frame_hash(&t);

    /* Texture without UVs. */
    mat = make_material(NULL, NULL, &tex);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);
    /* Non-finite UV in the LAST vertex: rejected before any triangle. */
    uv[3].x = pixelica_t_nan();
    mat = make_material(uv, NULL, &tex);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);
    uv[3].x = 0.0f;
    uv[3].y = pixelica_t_inf();
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);
    /* Supplied UVs are validated even without a texture. */
    mat = make_material(uv, NULL, NULL);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);
    uv[3].y = 0.0f;

    /* Invalid textures. */
    bad_tex = make_texture(NULL, 2, 2);
    mat = make_material(uv, NULL, &bad_tex);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);
    bad_tex = make_texture(tex_px, 0, 2);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);
    bad_tex = make_texture(tex_px, 2, 0);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);
    bad_tex = make_texture(tex_px, (uint16_t)(PIXELICA_MAX_TEXTURE_DIMENSION + 1u), 1);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);
    bad_tex = make_texture(tex_px, 1, (uint16_t)(PIXELICA_MAX_TEXTURE_DIMENSION + 1u));
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) ==
          PIXELICA_ERR_INVALID_ARG);

    s = pixelica_frame_stats(t.ctx);
    CHECK(frame_hash(&t) == before);
    CHECK_EQ_LONG(s.draw_calls, 1);
    CHECK_EQ_LONG(s.last_error, PIXELICA_ERR_INVALID_ARG);

    /* Boundary sizes and an empty textured mesh are accepted. */
    bad_tex = make_texture(tex_px, 1, 1);
    mat = make_material(uv, NULL, &bad_tex);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 32 * 32);
    mesh = make_mesh(NULL, 0, NULL, 0, RED);
    mat = make_material(NULL, NULL, &tex);
    CHECK(pixelica_draw_mesh_ex(t.ctx, &mesh, k_identity, &mat) == PIXELICA_OK);
    close_target(&t);
}

static void draw_rect(const Target *t, float x0, float y0, float x1, float y1,
                      float z, uint16_t color, const PIXELICA_Material *mat)
{
    PIXELICA_Vec3 v[4];
    PIXELICA_Mesh mesh;
    /* Pixel rectangle -> NDC (y down), counter-clockwise on screen. */
    float nx0 = x0 / (float)t->w * 2.0f - 1.0f;
    float nx1 = x1 / (float)t->w * 2.0f - 1.0f;
    float ny0 = 1.0f - y0 / (float)t->h * 2.0f;
    float ny1 = 1.0f - y1 / (float)t->h * 2.0f;

    v[0] = vec3(nx0, ny1, z);
    v[1] = vec3(nx1, ny1, z);
    v[2] = vec3(nx1, ny0, z);
    v[3] = vec3(nx0, ny0, z);
    mesh = make_mesh(v, 4, k_quad_idx, 6, color);
    CHECK(pixelica_draw_mesh_ex(t->ctx, &mesh, k_identity, mat) == PIXELICA_OK);
}

static void test_coverage_counters(void)
{
    static const uint16_t tex_px[1] = {GREEN};
    static const PIXELICA_Vec2 uv[4] = {{0.0f, 1.0f}, {1.0f, 1.0f},
                                   {1.0f, 0.0f}, {0.0f, 0.0f}};
    PIXELICA_Texture tex = make_texture(tex_px, 1, 1);
    PIXELICA_Material mat = make_material(uv, NULL, &tex);
    PIXELICA_FrameStats s;
    Target t;
    int textured;

    if (!open_target(&t, 32, 32)) {
        return;
    }
    for (textured = 0; textured < 2; ++textured) {
        const PIXELICA_Material *m = textured ? &mat : NULL;

        /* Far 16x16 then overlapping near 16x16 (64 px shared). */
        pixelica_begin_frame(t.ctx, BG);
        draw_rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, 0.5f, RED, m);
        draw_rect(&t, 8.0f, 8.0f, 24.0f, 24.0f, -0.5f, BLUE, m);
        s = pixelica_frame_stats(t.ctx);
        CHECK_EQ_LONG(s.fragments_covered, 512);
        CHECK_EQ_LONG(s.pixels_covered, 448);
        CHECK_EQ_LONG(s.pixels_written, 512);

        /* Front to back: the hidden part of the far rect is not written,
         * but still covered. */
        pixelica_begin_frame(t.ctx, BG);
        draw_rect(&t, 8.0f, 8.0f, 24.0f, 24.0f, -0.5f, BLUE, m);
        draw_rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, 0.5f, RED, m);
        s = pixelica_frame_stats(t.ctx);
        CHECK_EQ_LONG(s.fragments_covered, 512);
        CHECK_EQ_LONG(s.pixels_covered, 448);
        CHECK_EQ_LONG(s.pixels_written, 448);

        /* The same rect three times: overdraw 3, written once (equal depth
         * fails the strict test). */
        pixelica_begin_frame(t.ctx, BG);
        draw_rect(&t, 0.0f, 0.0f, 32.0f, 32.0f, 0.0f, RED, m);
        draw_rect(&t, 0.0f, 0.0f, 32.0f, 32.0f, 0.0f, RED, m);
        draw_rect(&t, 0.0f, 0.0f, 32.0f, 32.0f, 0.0f, RED, m);
        s = pixelica_frame_stats(t.ctx);
        CHECK_EQ_LONG(s.fragments_covered, 3 * 1024);
        CHECK_EQ_LONG(s.pixels_covered, 1024);
        CHECK_EQ_LONG(s.pixels_written, 1024);
        CHECK(s.pixels_covered == (uint32_t)npix(&t)); /* coverage 100% */

        /* Fully hidden geometry still counts as covered fragments. */
        draw_rect(&t, 4.0f, 4.0f, 8.0f, 8.0f, 0.9f, BLUE, m);
        s = pixelica_frame_stats(t.ctx);
        CHECK_EQ_LONG(s.fragments_covered, 3 * 1024 + 16);
        CHECK_EQ_LONG(s.pixels_written, 1024);
    }
    pixelica_begin_frame(t.ctx, BG);
    s = pixelica_frame_stats(t.ctx);
    CHECK(s.fragments_covered == 0 && s.pixels_covered == 0);
    close_target(&t);
}

int main(void)
{
    test_checker_quadrants();
    test_nearest_mapping_and_clamp();
    test_perspective_correct_floor();
    test_textured_clipping_robustness();
    test_triangle_colors_and_flat_compat();
    test_flat_cube_hash_unchanged();
    test_material_validation();
    test_coverage_counters();
    return pixelica_t_finish("test_texture");
}

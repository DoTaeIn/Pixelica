/*
 * Pixelica - draw options: legacy bit-identity, REPEAT wrapping (independent
 * axes, negative/integer/huge UVs, non-power-of-two, clipped perspective),
 * alpha TEST/BLEND arithmetic and depth rules, unique coverage stats, and
 * validation/aliasing rejects that leave the frame untouched.
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

static const float k_id[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};
static const uint16_t k_quad[6] = {0, 1, 2, 0, 2, 3};
static const float k_uv01[4] = {0.0f, 0.0f, 1.0f, 1.0f}; /* u0 v0 u1 v1 */
static const uint16_t k_tex32[6] = {0x1000, 0x1001, 0x1002,
                                    0x1003, 0x1004, 0x1005}; /* 3x2 */
static int g_use_ex; /* rect(): call pixelica_draw_mesh_ex (o ignored) */

static int open_t(Target *t, uint16_t w, uint16_t h)
{
    PIXELICA_Config cfg = pixelica_t_config(w, h);

    t->w = w;
    t->h = h;
    t->ctx = NULL;
    CHECK(pixelica_create(&cfg, NULL, &t->ctx) == PIXELICA_OK);
    return t->ctx != NULL;
}

static void close_t(Target *t)
{
    CHECK(pixelica_memory_guards_intact(t->ctx) == 1);
    pixelica_destroy(t->ctx);
}

static size_t npix(const Target *t) { return (size_t)t->w * t->h; }
static uint16_t px(const Target *t, int x, int y) { return pixelica_t_color_at(t->ctx, t->w, x, y); }
static uint16_t dp(const Target *t, int x, int y) { return pixelica_t_depth_at(t->ctx, t->w, x, y); }
static uint64_t depth_hash(const Target *t) { return pixelica_t_hash16(pixelica_depth_buffer(t->ctx), npix(t)); }
static uint64_t frame_hash(const Target *t)
{
    return pixelica_t_hash16(pixelica_color_buffer(t->ctx), npix(t)) ^ (depth_hash(t) * 31u);
}
static uint32_t count(const Target *t, uint16_t c) { return pixelica_t_count_color(t->ctx, t->w, t->h, c); }

/* Pixel rect [x0,x1) x [y0,y1) at NDC z, CCW; UV u0 left, v0 top. A NULL
 * texture draws untextured (flat `color`). */
static PIXELICA_Status rect(const Target *t, float x0, float y0, float x1, float y1,
                            float z, uint16_t color, const PIXELICA_Texture *tex,
                            const float uvr[4], const PIXELICA_DrawOptions *o)
{
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec2 uv[4];
    PIXELICA_Mesh m;
    PIXELICA_Material mat;
    const float nx0 = x0 / (float)t->w * 2.0f - 1.0f, nx1 = x1 / (float)t->w * 2.0f - 1.0f;
    const float ny0 = 1.0f - y0 / (float)t->h * 2.0f, ny1 = 1.0f - y1 / (float)t->h * 2.0f;

    v[0].x = nx0; v[0].y = ny1; v[1].x = nx1; v[1].y = ny1;
    v[2].x = nx1; v[2].y = ny0; v[3].x = nx0; v[3].y = ny0;
    v[0].z = v[1].z = v[2].z = v[3].z = z;
    uv[0].x = uvr[0]; uv[0].y = uvr[3]; uv[1].x = uvr[2]; uv[1].y = uvr[3];
    uv[2].x = uvr[2]; uv[2].y = uvr[1]; uv[3].x = uvr[0]; uv[3].y = uvr[1];
    m.vertices = v; m.vertex_count = 4; m.indices = k_quad; m.index_count = 6; m.color = color;
    mat.uvs = uv; mat.triangle_colors = NULL; mat.texture = tex;
    if (g_use_ex) {
        return pixelica_draw_mesh_ex(t->ctx, &m, k_id, tex != NULL ? &mat : NULL);
    }
    return pixelica_draw_mesh_options(t->ctx, &m, k_id, tex != NULL ? &mat : NULL, o);
}

static PIXELICA_Status full(const Target *t, float z, uint16_t color,
                            const PIXELICA_Texture *tex, const PIXELICA_DrawOptions *o)
{
    return rect(t, 0.0f, 0.0f, (float)t->w, (float)t->h, z, color, tex, k_uv01, o);
}

static PIXELICA_DrawOptions opts(PIXELICA_AlphaMode mode, uint8_t opacity, uint8_t cutoff,
                                 const uint8_t *alpha, size_t bytes)
{
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    o.alpha_mode = mode;
    o.opacity = opacity;
    o.alpha_cutoff = cutoff;
    o.texture_alpha = alpha;
    o.texture_alpha_bytes = bytes;
    return o;
}

static PIXELICA_Texture tex_of(const uint16_t *p, uint16_t w, uint16_t h)
{
    PIXELICA_Texture t;
    t.pixels = p;
    t.width = w;
    t.height = h;
    return t;
}

/* Old entry points, NULL options, explicit defaults and OPAQUE with a
 * supplied (ignored) alpha plane/opacity give identical color, depth, stats. */
static void test_legacy_identical(void)
{
    static const uint8_t zero_alpha[6] = {0};
    static const float uv_clamp[4] = {-1.0f, -0.5f, 2.0f, 1.7f};
    PIXELICA_Texture tex = tex_of(k_tex32, 3, 2);
    PIXELICA_DrawOptions o[2];
    PIXELICA_FrameStats s[4];
    uint64_t h[4];
    Target t;
    int mode;

    if (!open_t(&t, 32, 24)) {
        return;
    }
    o[0] = pixelica_draw_options_default();
    o[1] = opts(PIXELICA_ALPHA_OPAQUE, 0, 255, zero_alpha, 6);
    for (mode = 0; mode < 4; ++mode) {
        const PIXELICA_DrawOptions *op = mode < 2 ? NULL : &o[mode - 2];
        PIXELICA_DrawOptions flat = op != NULL ? *op : pixelica_draw_options_default();
        flat.texture_alpha = NULL; /* a flat draw has no texture for an alpha plane */
        flat.texture_alpha_bytes = 0u;
        g_use_ex = mode == 0;
        pixelica_begin_frame(t.ctx, 0x1084);
        CHECK(rect(&t, 1.3f, 2.1f, 30.2f, 22.7f, 0.3f, RED, &tex, uv_clamp, op) == PIXELICA_OK);
        CHECK(rect(&t, 5.0f, 0.0f, 17.0f, 24.0f, -0.2f, GREEN, NULL, k_uv01,
                   op != NULL ? &flat : NULL) == PIXELICA_OK);
        CHECK(rect(&t, 0.0f, 9.0f, 32.0f, 13.0f, 0.6f, BLUE, &tex, k_uv01, op) == PIXELICA_OK);
        h[mode] = frame_hash(&t);
        s[mode] = pixelica_frame_stats(t.ctx);
    }
    g_use_ex = 0;
    for (mode = 1; mode < 4; ++mode) {
        CHECK(h[mode] == h[0]);
        CHECK(s[mode].pixels_written == s[0].pixels_written);
        CHECK(s[mode].fragments_covered == s[0].fragments_covered);
        CHECK(s[mode].pixels_covered == s[0].pixels_covered);
    }
    close_t(&t);
}

static int clamp_texel(double t, int size)
{
    int i = t <= 0.0 ? 0 : (int)floor(t * size);
    return i < size ? i : size - 1;
}

static int repeat_texel(double t, int size)
{
    return (int)floor((t - floor(t)) * size);
}

/* 3x2 texture, u -1..1 and v 0..2 across a 24x16 target; each axis wraps or
 * clamps independently. Texel boundaries stay >= 1/8 texel from centers. */
static void test_repeat_axes(void)
{
    static const float uvr[4] = {-1.0f, 0.0f, 1.0f, 2.0f};
    PIXELICA_Texture tex = tex_of(k_tex32, 3, 2);
    Target t;
    int combo;

    if (!open_t(&t, 24, 16)) {
        return;
    }
    for (combo = 0; combo < 3; ++combo) {
        PIXELICA_DrawOptions o = pixelica_draw_options_default();
        uint32_t bad = 0;
        int x, y;

        o.wrap_u = combo != 1 ? PIXELICA_WRAP_REPEAT : PIXELICA_WRAP_CLAMP;
        o.wrap_v = combo != 0 ? PIXELICA_WRAP_REPEAT : PIXELICA_WRAP_CLAMP;
        pixelica_begin_frame(t.ctx, BG);
        CHECK(rect(&t, 0.0f, 0.0f, 24.0f, 16.0f, 0.0f, BG, &tex, uvr, &o) == PIXELICA_OK);
        for (y = 0; y < 16; ++y) {
            for (x = 0; x < 24; ++x) {
                double u = -1.0 + 2.0 * (x + 0.5) / 24.0, v = 2.0 * (y + 0.5) / 16.0;
                int tx = o.wrap_u ? repeat_texel(u, 3) : clamp_texel(u, 3);
                int ty = o.wrap_v ? repeat_texel(v, 2) : clamp_texel(v, 2);
                bad += px(&t, x, y) != k_tex32[ty * 3 + tx];
            }
        }
        CHECK_EQ_LONG(bad, 0);
    }
    close_t(&t);
}

/* Constant UVs: exact integers (incl. huge/FLT_MAX) map to texel 0, negative
 * fractions wrap. A -FLT_MAX..FLT_MAX span samples only texture colors. */
static void test_repeat_extremes(void)
{
    static const struct { float u, v; int tx, ty; } cases[] = {
        {1.0f, 1.0f, 0, 0}, {-3.0f, 2.0f, 0, 0}, {0.5f, -0.25f, 1, 1},
        {-0.25f, 0.5f, 2, 1}, {1e30f, -1e30f, 0, 0}, {FLT_MAX, -FLT_MAX, 0, 0},
    };
    static const float span[4] = {-FLT_MAX, -1e30f, FLT_MAX, 3.5f};
    PIXELICA_Texture tex = tex_of(k_tex32, 3, 2);
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    Target t;
    size_t i;
    uint32_t texels = 0;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    o.wrap_u = o.wrap_v = PIXELICA_WRAP_REPEAT;
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const float uvr[4] = {cases[i].u, cases[i].v, cases[i].u, cases[i].v};
        pixelica_begin_frame(t.ctx, BG);
        CHECK(rect(&t, 0.0f, 0.0f, 8.0f, 8.0f, 0.0f, BG, &tex, uvr, &o) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, k_tex32[cases[i].ty * 3 + cases[i].tx]), 64);
    }
    pixelica_begin_frame(t.ctx, BG);
    CHECK(rect(&t, 0.0f, 0.0f, 8.0f, 8.0f, 0.0f, BG, &tex, span, &o) == PIXELICA_OK);
    for (i = 0; i < 6; ++i) {
        texels += count(&t, k_tex32[i]);
    }
    CHECK_EQ_LONG(texels, 64);
    close_t(&t);
}

/*
 * Floor y=-1 from z=-1 (v=4) to z=-9 (v=0), near plane 1.5, so the near end
 * is clipped. A 1x4 texture repeats: reference v = (z+9)/2 with z = 1/ndc_y.
 * Then TEST with alpha {255,0,255,0} must keep exactly the even texels of
 * the OPAQUE render (alpha sampled at the color's texel) with their depth.
 */
static void test_perspective_repeat_alpha(void)
{
    static const uint16_t tex_px[4] = {0x2000, 0x2001, 0x2002, 0x2003};
    static const uint8_t alpha[4] = {255, 0, 255, 0};
    static uint16_t ref_c[64 * 64], ref_d[64 * 64];
    PIXELICA_Texture tex = tex_of(tex_px, 1, 4);
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    PIXELICA_Vec3 v[4] = {{-4, -1, -1}, {4, -1, -1}, {4, -1, -9}, {-4, -1, -9}};
    PIXELICA_Vec2 uv[4] = {{0, 4}, {1, 4}, {1, 0}, {0, 0}};
    PIXELICA_Mesh m;
    PIXELICA_Material mat;
    float persp[16];
    uint32_t bad = 0, checked = 0, kept = 0;
    Target t;
    int x, y, i;

    if (!open_t(&t, 64, 64)) {
        return;
    }
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 1.5f, 100.0f) == PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    m.vertices = v; m.vertex_count = 4; m.indices = k_quad; m.index_count = 6; m.color = BG;
    mat.uvs = uv; mat.triangle_colors = NULL; mat.texture = &tex;
    o.wrap_v = PIXELICA_WRAP_REPEAT;
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_options(t.ctx, &m, k_id, &mat, &o) == PIXELICA_OK);
    CHECK(pixelica_frame_stats(t.ctx).triangles_clipped > 0u);
    for (y = 0; y < 64; ++y) {
        double ndc_y = 1.0 - 2.0 * (y + 0.5) / 64.0, s;
        if (ndc_y > -1.0 / 9.0 - 0.01 || ndc_y < -1.0 / 1.5 + 0.01) {
            continue;
        }
        s = ((1.0 / ndc_y + 9.0) / 2.0 - floor((1.0 / ndc_y + 9.0) / 2.0)) * 4.0;
        if (s - floor(s) < 0.03 || s - floor(s) > 0.97) {
            continue;
        }
        for (x = 24; x < 40; ++x, ++checked) {
            bad += px(&t, x, y) != tex_px[(int)s];
        }
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK(checked > 150u);
    CHECK(px(&t, 32, 63) == BG); /* below the near clip line */
    memcpy(ref_c, pixelica_color_buffer(t.ctx), sizeof ref_c);
    memcpy(ref_d, pixelica_depth_buffer(t.ctx), sizeof ref_d);

    o.alpha_mode = PIXELICA_ALPHA_TEST;
    o.texture_alpha = alpha;
    o.texture_alpha_bytes = 4;
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_options(t.ctx, &m, k_id, &mat, &o) == PIXELICA_OK);
    bad = 0;
    for (i = 0; i < 64 * 64; ++i) {
        uint16_t c = pixelica_color_buffer(t.ctx)[i], d = pixelica_depth_buffer(t.ctx)[i];
        int even = ref_c[i] == tex_px[0] || ref_c[i] == tex_px[2];
        kept += (uint32_t)even;
        bad += even ? (c != ref_c[i] || d != ref_d[i])
                    : (c != BG || d != PIXELICA_DEPTH_CLEAR);
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK(kept > 100u && kept < 64u * 64u);
    CHECK(count(&t, tex_px[1]) + count(&t, tex_px[3]) == 0u);
    close_t(&t);
}

/* Threshold edges (map*opacity), flat uniform opacity, cutoff 0; discards
 * write neither color nor depth, so later farther geometry shows through. */
static void test_alpha_test(void)
{
    static const struct { uint8_t map, opacity, cutoff, textured, keep; } cases[] = {
        {128, 255, 128, 1, 1}, {128, 255, 129, 1, 0}, {200, 128, 100, 1, 1},
        {200, 128, 101, 1, 0}, {0, 77, 77, 0, 1},     {0, 77, 78, 0, 0},
        {0, 255, 0, 1, 1},     {0, 255, 1, 1, 0},     {255, 255, 255, 1, 1},
    };
    static const uint16_t tex_px[1] = {RED};
    PIXELICA_Texture tex = tex_of(tex_px, 1, 1);
    Target t;
    size_t i;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t a = cases[i].map;
        PIXELICA_DrawOptions o = opts(PIXELICA_ALPHA_TEST, cases[i].opacity, cases[i].cutoff,
                                      cases[i].textured ? &a : NULL, cases[i].textured);
        uint64_t before;
        uint16_t far_d;

        pixelica_begin_frame(t.ctx, BG);
        CHECK(full(&t, 0.5f, BLUE, NULL, NULL) == PIXELICA_OK);
        far_d = dp(&t, 3, 3);
        before = frame_hash(&t);
        CHECK(full(&t, -0.5f, RED, cases[i].textured ? &tex : NULL, &o) == PIXELICA_OK);
        if (cases[i].keep) {
            CHECK_EQ_LONG(count(&t, RED), 64);
            CHECK(dp(&t, 3, 3) < far_d);
            CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 128);
        } else {
            CHECK(frame_hash(&t) == before);
            CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 64);
            CHECK(full(&t, 0.0f, GREEN, NULL, NULL) == PIXELICA_OK);
            CHECK_EQ_LONG(count(&t, GREEN), 64);
        }
    }
    close_t(&t);
}

/* Per-channel 5/6/5 arithmetic, alpha 0/255, map*opacity, flat opacity,
 * opaque occlusion, caller-ordered layers; BLEND never writes depth. */
static void test_alpha_blend(void)
{
    static const struct { uint16_t src, dst; uint8_t map, opacity, textured; uint16_t want; } cases[] = {
        {RED, BLUE, 128, 255, 1, 0x800F},     {GREEN, 0xFFFF, 64, 255, 1, 0xBFF7},
        {GREEN, 0xFFFF, 128, 128, 1, 0xBFF7}, {0x1234, 0xABCD, 100, 255, 1, 0x7330},
        {0x1234, 0xABCD, 255, 255, 1, 0x1234}, {0x1234, 0xABCD, 0, 255, 1, 0xABCD},
        {0x1234, 0xABCD, 255, 0, 1, 0xABCD},  {RED, BLUE, 0, 128, 0, 0x800F},
    };
    PIXELICA_DrawOptions o;
    Target t;
    size_t i;
    uint8_t a = 128;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint16_t src = cases[i].src;
        uint8_t map = cases[i].map;
        PIXELICA_Texture tex = tex_of(&src, 1, 1);
        uint64_t d_before;
        int skip = cases[i].want == cases[i].dst && cases[i].src != cases[i].dst;

        o = opts(PIXELICA_ALPHA_BLEND, cases[i].opacity, 128,
                 cases[i].textured ? &map : NULL, cases[i].textured);
        pixelica_begin_frame(t.ctx, cases[i].dst);
        d_before = depth_hash(&t);
        CHECK(full(&t, 0.0f, src, cases[i].textured ? &tex : NULL, &o) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, cases[i].want), 64);
        CHECK(depth_hash(&t) == d_before);
        CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, skip ? 0 : 64);
    }

    /* Opaque near geometry occludes a farther blend; depth stays opaque's. */
    o = opts(PIXELICA_ALPHA_BLEND, 128, 128, NULL, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(rect(&t, 0.0f, 0.0f, 4.0f, 8.0f, -0.5f, GREEN, NULL, k_uv01, NULL) == PIXELICA_OK);
    {
        uint64_t d_before = depth_hash(&t);
        CHECK(full(&t, 0.5f, RED, NULL, &o) == PIXELICA_OK);
        CHECK(depth_hash(&t) == d_before);
    }
    CHECK_EQ_LONG(count(&t, GREEN), 32);
    CHECK_EQ_LONG(count(&t, 0x8000), 32);

    /* Back-to-front vs front-to-back: no sorting, both layers blend. */
    {
        uint16_t g = GREEN, r = RED;
        PIXELICA_Texture tg = tex_of(&g, 1, 1), tr = tex_of(&r, 1, 1);
        o = opts(PIXELICA_ALPHA_BLEND, 255, 128, &a, 1);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(full(&t, 0.5f, BG, &tg, &o) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, 0x0400), 64);
        CHECK(full(&t, -0.5f, BG, &tr, &o) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, 0x8200), 64);
        CHECK(dp(&t, 0, 0) == PIXELICA_DEPTH_CLEAR && dp(&t, 7, 7) == PIXELICA_DEPTH_CLEAR);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(full(&t, -0.5f, BG, &tr, &o) == PIXELICA_OK);
        CHECK(full(&t, 0.5f, BG, &tg, &o) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, 0x4400), 64);
    }
    close_t(&t);
}

/* pixels_covered stays unique geometric coverage with no-depth fragments. */
static void test_unique_coverage(void)
{
    PIXELICA_DrawOptions blend = opts(PIXELICA_ALPHA_BLEND, 128, 0, NULL, 0);
    PIXELICA_DrawOptions discard = opts(PIXELICA_ALPHA_TEST, 0, 1, NULL, 0);
    PIXELICA_FrameStats s;
    Target t;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    pixelica_begin_frame(t.ctx, BG);
    CHECK(full(&t, 0.0f, RED, NULL, &blend) == PIXELICA_OK);
    CHECK(full(&t, 0.0f, RED, NULL, &blend) == PIXELICA_OK);
    CHECK(full(&t, 0.0f, RED, NULL, &blend) == PIXELICA_OK);
    s = pixelica_frame_stats(t.ctx);
    CHECK_EQ_LONG(s.pixels_covered, 64);
    CHECK_EQ_LONG(s.fragments_covered, 192);
    CHECK_EQ_LONG(s.pixels_written, 192);

    pixelica_begin_frame(t.ctx, BG);
    CHECK(rect(&t, 0.0f, 0.0f, 8.0f, 4.0f, 0.0f, RED, NULL, k_uv01, &discard) == PIXELICA_OK);
    CHECK(rect(&t, 0.0f, 0.0f, 8.0f, 4.0f, 0.0f, RED, NULL, k_uv01, &discard) == PIXELICA_OK);
    s = pixelica_frame_stats(t.ctx);
    CHECK_EQ_LONG(s.pixels_covered, 32);
    CHECK_EQ_LONG(s.pixels_written, 0);
    CHECK_EQ_LONG(count(&t, BG), 64);
    CHECK(rect(&t, 0.0f, 2.0f, 8.0f, 6.0f, 0.0f, RED, NULL, k_uv01, NULL) == PIXELICA_OK);
    s = pixelica_frame_stats(t.ctx);
    CHECK_EQ_LONG(s.pixels_covered, 48);
    CHECK_EQ_LONG(s.fragments_covered, 96);

    pixelica_begin_frame(t.ctx, BG);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_covered, 0);
    CHECK(rect(&t, 2.0f, 2.0f, 4.0f, 4.0f, 0.0f, RED, NULL, k_uv01, &blend) == PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_covered, 4);
    close_t(&t);
}

/* Invalid options and aliasing are rejected without touching the frame, on a
 * caller-owned buffer whose guards and trailing bytes stay intact. */
static void test_validation_and_memory(void)
{
    static unsigned char buf[32768];
    PIXELICA_Config cfg = pixelica_t_config(16, 16);
    PIXELICA_Texture tex = tex_of(k_tex32, 3, 2);
    PIXELICA_DrawOptions bad[10];
    PIXELICA_DrawOptions o;
    const uint8_t *wrap_ptr = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 3u);
    uint8_t ok_alpha[16];
    size_t req = 0, i, n = 0;
    uint64_t before;
    uint32_t calls;
    Target t;

    CHECK(pixelica_memory_requirement(&cfg, &req) == PIXELICA_OK);
    CHECK(req + 64u <= sizeof buf);
    memset(ok_alpha, 255, sizeof ok_alpha);
    memset(buf + req, 0x5A, sizeof buf - req);
    t.w = t.h = 16;
    t.ctx = NULL;
    CHECK(pixelica_create_in_buffer(&cfg, NULL, buf, req, &t.ctx) == PIXELICA_OK);
    if (t.ctx == NULL) {
        return;
    }
    pixelica_begin_frame(t.ctx, BG);
    CHECK(rect(&t, 0.0f, 0.0f, 8.0f, 16.0f, 0.0f, RED, NULL, k_uv01, NULL) == PIXELICA_OK);
    before = frame_hash(&t);
    calls = pixelica_frame_stats(t.ctx).draw_calls;

    for (i = 0; i < 10; ++i) {
        bad[i] = opts(PIXELICA_ALPHA_BLEND, 255, 128, ok_alpha, 6);
    }
    bad[n++].wrap_u = (PIXELICA_WrapMode)2;
    bad[n++].wrap_v = (PIXELICA_WrapMode)-1;
    bad[n++].alpha_mode = (PIXELICA_AlphaMode)4; /* one past PIXELICA_ALPHA_ADD */
    bad[n++].alpha_mode = (PIXELICA_AlphaMode)-1;
    bad[n].texture_alpha = NULL;                  /* bytes without a plane */
    bad[n++].texture_alpha_bytes = 4;
    bad[n++].texture_alpha_bytes = 5;             /* < 3*2 */
    bad[n].alpha_mode = PIXELICA_ALPHA_OPAQUE;    /* validated even if ignored */
    bad[n++].texture_alpha = (const uint8_t *)pixelica_color_buffer(t.ctx);
    bad[n++].texture_alpha = (const uint8_t *)(pixelica_depth_buffer(t.ctx) + 256);
    bad[n++].texture_alpha = wrap_ptr;            /* end not representable */
    for (i = 0; i < n; ++i) {
        CHECK(rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, -0.5f, GREEN, &tex, k_uv01, &bad[i]) ==
              PIXELICA_ERR_INVALID_ARG);
    }
    o = opts(PIXELICA_ALPHA_TEST, 255, 128, ok_alpha, 6);
    CHECK(rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, -0.5f, GREEN, NULL, k_uv01, &o) ==
          PIXELICA_ERR_INVALID_ARG); /* alpha plane without a texture */
    CHECK(rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, -0.5f, GREEN, &tex, k_uv01,
               (const PIXELICA_DrawOptions *)(const void *)pixelica_color_buffer(t.ctx)) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, -0.5f, GREEN, &tex, k_uv01,
               (const PIXELICA_DrawOptions *)(const void *)t.ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(frame_hash(&t) == before);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).draw_calls, calls);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).last_error, PIXELICA_ERR_INVALID_ARG);

    /* Accepted: plane just past the owned block, oversized byte count,
     * NULL plane with zero bytes. */
    memset(buf + req, 255, 6);
    o = opts(PIXELICA_ALPHA_TEST, 255, 128, buf + req, 6);
    CHECK(rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, -0.5f, GREEN, &tex, k_uv01, &o) == PIXELICA_OK);
    o = opts(PIXELICA_ALPHA_BLEND, 128, 128, ok_alpha, sizeof ok_alpha);
    CHECK(rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, -0.7f, GREEN, &tex, k_uv01, &o) == PIXELICA_OK);
    o = opts(PIXELICA_ALPHA_BLEND, 128, 128, NULL, 0);
    o.wrap_u = o.wrap_v = PIXELICA_WRAP_REPEAT;
    CHECK(rect(&t, 0.0f, 0.0f, 16.0f, 16.0f, -0.9f, GREEN, &tex, k_uv01, &o) == PIXELICA_OK);
    CHECK(pixelica_memory_guards_intact(t.ctx) == 1);
    for (i = req + 6u, n = 0; i < sizeof buf; ++i) {
        n += buf[i] != 0x5A;
    }
    CHECK_EQ_LONG(n, 0);
    pixelica_destroy(t.ctx);
}

int main(void)
{
    test_legacy_identical();
    test_repeat_axes();
    test_repeat_extremes();
    test_perspective_repeat_alpha();
    test_alpha_test();
    test_alpha_blend();
    test_unique_coverage();
    test_validation_and_memory();
    return pixelica_t_finish("test_texture_options");
}

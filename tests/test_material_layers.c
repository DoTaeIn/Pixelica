/*
 * Pixelica - layered materials (pixelica_draw_mesh_layers): legacy
 * bit-identity, constant/varying/perspective RGBA8 vertex colors, independent
 * layer UVs and wrapping, multiply/add/lerp arithmetic with opacity, base-only
 * alpha with vertex-alpha composition, and rejects that leave the frame alone.
 */
#include <stdlib.h>

#include "test_util.h"

#define BG ((uint16_t)0x0000)
#define RED ((uint16_t)0xF800)
#define GREEN ((uint16_t)0x07E0)
#define BLUE ((uint16_t)0x001F)
#define WHITE ((uint16_t)0xFFFF)
#define PI_F 3.14159265358979323846f

typedef struct Target {
    PIXELICA_Context *ctx;
    uint16_t w, h;
} Target;

typedef struct Quad {
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec2 uv[4];
    PIXELICA_Mesh m;
} Quad;

static const float k_id[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};
static const uint16_t k_quad[6] = {0, 1, 2, 0, 2, 3};
static const float k_uv01[4] = {0.0f, 0.0f, 1.0f, 1.0f}; /* u0 v0 u1 v1 */
static const uint16_t k_tex32[6] = {0x1000, 0x1001, 0x1002,
                                    0x1003, 0x1004, 0x1005}; /* 3x2 */

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
static uint64_t depth_hash(const Target *t) { return pixelica_t_hash16(pixelica_depth_buffer(t->ctx), npix(t)); }
static uint64_t frame_hash(const Target *t)
{
    return pixelica_t_hash16(pixelica_color_buffer(t->ctx), npix(t)) ^ (depth_hash(t) * 31u);
}
static uint32_t count(const Target *t, uint16_t c) { return pixelica_t_count_color(t->ctx, t->w, t->h, c); }

/* ---- reference arithmetic (native 5/6/5 channels) ---------------------- */

static int ch_r(uint16_t c) { return c >> 11; }
static int ch_g(uint16_t c) { return (c >> 5) & 63; }
static int ch_b(uint16_t c) { return c & 31; }
static uint16_t pack(int r, int g, int b) { return (uint16_t)((r << 11) | (g << 5) | b); }
static int near_div(int v) { return (v + 127) / 255; }
static int mixc(int r, int p, int a) { return near_div(r * a + p * (255 - a)); }

static int comb_ch(int p, int t, PIXELICA_TextureCombine c, int m)
{
    if (c == PIXELICA_TEXTURE_MULTIPLY) {
        return (p * t + m / 2) / m;
    }
    if (c == PIXELICA_TEXTURE_ADD) {
        return p + t < m ? p + t : m;
    }
    return t;
}

static uint16_t ref_layer(uint16_t p, uint16_t t, PIXELICA_TextureCombine c, int a)
{
    return pack(mixc(comb_ch(ch_r(p), ch_r(t), c, 31), ch_r(p), a),
                mixc(comb_ch(ch_g(p), ch_g(t), c, 63), ch_g(p), a),
                mixc(comb_ch(ch_b(p), ch_b(t), c, 31), ch_b(p), a));
}

static PIXELICA_ColorRGBA8 rgba(int r, int g, int b, int a)
{
    PIXELICA_ColorRGBA8 c;

    c.r = (uint8_t)r;
    c.g = (uint8_t)g;
    c.b = (uint8_t)b;
    c.a = (uint8_t)a;
    return c;
}

static uint16_t ref_modulate(uint16_t c, PIXELICA_ColorRGBA8 v)
{
    return pack(near_div(ch_r(c) * v.r), near_div(ch_g(c) * v.g), near_div(ch_b(c) * v.b));
}

static uint16_t ref_blend(uint16_t src, uint16_t dst, int a)
{
    return pack(mixc(ch_r(src), ch_r(dst), a), mixc(ch_g(src), ch_g(dst), a),
                mixc(ch_b(src), ch_b(dst), a));
}

/* ---- builders ----------------------------------------------------------- */

static void set_uv(PIXELICA_Vec2 uv[4], const float uvr[4])
{
    uv[0].x = uvr[0]; uv[0].y = uvr[3]; uv[1].x = uvr[2]; uv[1].y = uvr[3];
    uv[2].x = uvr[2]; uv[2].y = uvr[1]; uv[3].x = uvr[0]; uv[3].y = uvr[1];
}

/* Pixel rect [x0,x1) x [y0,y1) at NDC z, CCW; v0 BL, v1 BR, v2 TR, v3 TL. */
static void quad(Quad *q, const Target *t, float x0, float y0, float x1, float y1,
                 float z, uint16_t color, const float uvr[4])
{
    const float nx0 = x0 / (float)t->w * 2.0f - 1.0f, nx1 = x1 / (float)t->w * 2.0f - 1.0f;
    const float ny0 = 1.0f - y0 / (float)t->h * 2.0f, ny1 = 1.0f - y1 / (float)t->h * 2.0f;

    q->v[0].x = nx0; q->v[0].y = ny1; q->v[1].x = nx1; q->v[1].y = ny1;
    q->v[2].x = nx1; q->v[2].y = ny0; q->v[3].x = nx0; q->v[3].y = ny0;
    q->v[0].z = q->v[1].z = q->v[2].z = q->v[3].z = z;
    set_uv(q->uv, uvr);
    q->m.vertices = q->v; q->m.vertex_count = 4; q->m.indices = k_quad;
    q->m.index_count = 6; q->m.color = color;
}

static PIXELICA_Texture tex_of(const uint16_t *p, uint16_t w, uint16_t h)
{
    PIXELICA_Texture t;
    t.pixels = p;
    t.width = w;
    t.height = h;
    return t;
}

static PIXELICA_TextureLayer layer_of(const PIXELICA_Texture *tex, const PIXELICA_Vec2 *uvs,
                                      PIXELICA_TextureCombine c, uint8_t opacity)
{
    PIXELICA_TextureLayer l;
    l.texture = tex;
    l.uvs = uvs;
    l.wrap_u = l.wrap_v = PIXELICA_WRAP_CLAMP;
    l.combine = c;
    l.opacity = opacity;
    return l;
}

static PIXELICA_MaterialLayers ml_of(const PIXELICA_Vec2 *uvs, const PIXELICA_Texture *tex,
                                     const PIXELICA_ColorRGBA8 *vc,
                                     const PIXELICA_TextureLayer *layers, size_t n)
{
    PIXELICA_MaterialLayers ml;
    memset(&ml, 0, sizeof ml);
    ml.base.uvs = uvs;
    ml.base.texture = tex;
    ml.vertex_colors = vc;
    ml.layers = layers;
    ml.layer_count = n;
    return ml;
}

static void fill_vc(PIXELICA_ColorRGBA8 vc[4], PIXELICA_ColorRGBA8 c)
{
    vc[0] = vc[1] = vc[2] = vc[3] = c;
}

static PIXELICA_Status draw(const Target *t, const Quad *q, const PIXELICA_MaterialLayers *ml,
                            const PIXELICA_DrawOptions *o)
{
    return pixelica_draw_mesh_layers(t->ctx, &q->m, k_id, ml, o);
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

/* ---- tests -------------------------------------------------------------- */

/* A layer-less, vertex-color-less material equals the old API exactly: color,
 * depth and stats, with and without white vertex colors / a zero-count array. */
static void test_legacy_identical(void)
{
    static const float uv_clamp[4] = {-1.0f, -0.5f, 2.0f, 1.7f};
    static const uint8_t alpha[6] = {255, 200, 128, 90, 0, 255};
    static const uint16_t tri_colors[2] = {RED, GREEN};
    PIXELICA_Texture tex = tex_of(k_tex32, 3, 2);
    PIXELICA_ColorRGBA8 white[4];
    PIXELICA_TextureLayer unused[1];
    PIXELICA_DrawOptions o_tex[3], o_flat[3];
    PIXELICA_FrameStats s_ref = {0};
    Target t;
    int set, mode;

    if (!open_t(&t, 32, 24)) {
        return;
    }
    fill_vc(white, rgba(255, 255, 255, 255));
    unused[0] = layer_of(&tex, NULL, PIXELICA_TEXTURE_ADD, 255);
    o_tex[0] = pixelica_draw_options_default();
    o_tex[1] = opts(PIXELICA_ALPHA_TEST, 200, 100, alpha, 6);
    o_tex[1].wrap_u = o_tex[1].wrap_v = PIXELICA_WRAP_REPEAT;
    o_tex[2] = opts(PIXELICA_ALPHA_BLEND, 128, 0, alpha, 6);
    for (set = 0; set < 3; ++set) {
        o_flat[set] = o_tex[set];
        o_flat[set].texture_alpha = NULL;
        o_flat[set].texture_alpha_bytes = 0u;
    }
    for (set = 0; set < 3; ++set) {
        uint64_t ref = 0;

        for (mode = 0; mode < 3; ++mode) {
            Quad a, b;
            PIXELICA_Material mat;
            PIXELICA_MaterialLayers ml;

            pixelica_begin_frame(t.ctx, 0x1084);
            quad(&a, &t, 1.3f, 2.1f, 30.2f, 22.7f, 0.3f, BLUE, uv_clamp);
            quad(&b, &t, 5.0f, 0.0f, 17.0f, 24.0f, -0.2f, BLUE, k_uv01);
            if (mode == 0) {
                memset(&mat, 0, sizeof mat);
                mat.uvs = a.uv;
                mat.texture = &tex;
                CHECK(pixelica_draw_mesh_options(t.ctx, &a.m, k_id, &mat, &o_tex[set]) == PIXELICA_OK);
                memset(&mat, 0, sizeof mat);
                mat.triangle_colors = tri_colors;
                CHECK(pixelica_draw_mesh_options(t.ctx, &b.m, k_id, &mat, &o_flat[set]) == PIXELICA_OK);
            } else {
                const PIXELICA_ColorRGBA8 *vc = mode == 2 ? white : NULL;
                const PIXELICA_TextureLayer *ly = mode == 2 ? unused : NULL;

                ml = ml_of(a.uv, &tex, vc, ly, 0);
                CHECK(draw(&t, &a, &ml, &o_tex[set]) == PIXELICA_OK);
                ml = ml_of(NULL, NULL, vc, ly, 0);
                ml.base.triangle_colors = tri_colors;
                CHECK(draw(&t, &b, &ml, &o_flat[set]) == PIXELICA_OK);
            }
            if (mode == 0) {
                ref = frame_hash(&t);
                s_ref = pixelica_frame_stats(t.ctx);
            } else {
                PIXELICA_FrameStats s = pixelica_frame_stats(t.ctx);
                CHECK(frame_hash(&t) == ref);
                CHECK_EQ_LONG(s.pixels_written, s_ref.pixels_written);
                CHECK_EQ_LONG(s.fragments_covered, s_ref.fragments_covered);
                CHECK_EQ_LONG(s.pixels_covered, s_ref.pixels_covered);
            }
        }
    }
    close_t(&t);
}

/* Constant vertex colors, textured and untextured; OPAQUE ignores A; 255 is an
 * exact identity and 0 is black. Modulation uses the 5/6/5 channel value. */
static void test_constant_vertex_color(void)
{
    static const uint16_t texel[1] = {0xA5D3};
    static const PIXELICA_ColorRGBA8 cases[] = {
        {255, 255, 255, 255}, {0, 0, 0, 255}, {128, 64, 255, 255},
        {255, 255, 255, 0},   {37, 200, 255, 77},
    };
    PIXELICA_Texture tex = tex_of(texel, 1, 1);
    PIXELICA_ColorRGBA8 vc[4];
    Target t;
    size_t i;
    int textured;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        for (textured = 0; textured < 2; ++textured) {
            const uint16_t flat = 0x5ACB;
            const uint16_t src = textured ? texel[0] : flat;
            const uint16_t want = ref_modulate(src, cases[i]);
            Quad q;
            PIXELICA_MaterialLayers ml;

            fill_vc(vc, cases[i]);
            quad(&q, &t, 0, 0, 8, 8, 0.0f, flat, k_uv01);
            ml = ml_of(textured ? q.uv : NULL, textured ? &tex : NULL, vc, NULL, 0);
            pixelica_begin_frame(t.ctx, BG);
            CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_OK);
            CHECK_EQ_LONG(count(&t, want), 64);
            if (i == 0) {
                CHECK_EQ_LONG(count(&t, src), 64); /* white 255 is exact identity */
            }
            if (i == 1) {
                CHECK_EQ_LONG(count(&t, BG), 64);
            }
            if (i == 3) {
                CHECK_EQ_LONG(count(&t, src), 64); /* OPAQUE ignores A */
            }
        }
    }
    close_t(&t);
}

/* Per-vertex RGB and alpha vary across the quad, interpolated at pixel
 * centers in full precision and modulated once at the end. */
static void test_varying_vertex_color(void)
{
    PIXELICA_ColorRGBA8 vc[4];
    PIXELICA_DrawOptions o = opts(PIXELICA_ALPHA_BLEND, 255, 0, NULL, 0);
    PIXELICA_MaterialLayers ml;
    Quad q;
    Target t;
    int x, prev_r = -1, bad = 0;

    if (!open_t(&t, 16, 8)) {
        return;
    }
    /* RGB: red channel 0 (left) -> 255 (right); green 255 -> 0 */
    vc[0] = vc[3] = rgba(0, 255, 255, 255);
    vc[1] = vc[2] = rgba(255, 0, 255, 255);
    quad(&q, &t, 0, 0, 16, 8, 0.0f, WHITE, k_uv01);
    ml = ml_of(NULL, NULL, vc, NULL, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_OK);
    for (x = 0; x < 16; ++x) {
        double u = (x + 0.5) / 16.0;
        int r = (int)floor(31.0 * u + 0.5), g = (int)floor(63.0 * (1.0 - u) + 0.5);
        uint16_t c = px(&t, x, 3);

        bad += abs(ch_r(c) - r) > 1 || abs(ch_g(c) - g) > 1 || ch_b(c) != 31;
        bad += ch_r(c) < prev_r;
        prev_r = ch_r(c);
        CHECK(px(&t, x, 0) == c && px(&t, x, 7) == c); /* constant down a column */
    }
    CHECK_EQ_LONG(bad, 0);

    /* Alpha 0 (left) -> 255 (right), flat RED blended over black. */
    vc[0] = vc[3] = rgba(255, 255, 255, 0);
    vc[1] = vc[2] = rgba(255, 255, 255, 255);
    quad(&q, &t, 0, 0, 16, 8, 0.0f, RED, k_uv01);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
    bad = 0;
    for (x = 0; x < 16; ++x) {
        double a = 255.0 * (x + 0.5) / 16.0;
        int want = (int)floor(31.0 * a / 255.0 + 0.5);
        bad += abs(ch_r(px(&t, x, 4)) - want) > 1;
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK_EQ_LONG(count(&t, RED), 0);
    close_t(&t);
}

/* Floor receding from z=-1 to z=-9, near plane 1.5 (clipped). Vertex R (and in
 * a second pass A) goes 255 -> 0 linearly in world z, so perspective-correct
 * interpolation gives t = (z + 9) / 8 with z = 1 / ndc_y. */
static void test_clipped_perspective_vertex_color(void)
{
    PIXELICA_Vec3 v[4] = {{-4, -1, -1}, {4, -1, -1}, {4, -1, -9}, {-4, -1, -9}};
    PIXELICA_ColorRGBA8 vc[4];
    PIXELICA_DrawOptions blend = opts(PIXELICA_ALPHA_BLEND, 255, 0, NULL, 0);
    PIXELICA_MaterialLayers ml;
    PIXELICA_Mesh m;
    float persp[16];
    Target t;
    uint32_t checked, bad;
    int pass, x, y;

    if (!open_t(&t, 64, 64)) {
        return;
    }
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 1.5f, 100.0f) == PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    m.vertices = v; m.vertex_count = 4; m.indices = k_quad; m.index_count = 6;
    for (pass = 0; pass < 2; ++pass) {
        m.color = pass == 0 ? WHITE : RED;
        vc[0] = vc[1] = rgba(255, 255, 255, 255);
        vc[2] = vc[3] = pass == 0 ? rgba(0, 255, 255, 255) : rgba(255, 255, 255, 0);
        ml = ml_of(NULL, NULL, vc, NULL, 0);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_layers(t.ctx, &m, k_id, &ml, pass == 0 ? NULL : &blend) ==
              PIXELICA_OK);
        CHECK(pixelica_frame_stats(t.ctx).triangles_clipped > 0u);
        checked = bad = 0;
        for (y = 0; y < 64; ++y) {
            double ndc_y = 1.0 - 2.0 * (y + 0.5) / 64.0, tt, want;

            if (ndc_y > -1.0 / 9.0 - 0.01 || ndc_y < -1.0 / 1.5 + 0.01) {
                continue;
            }
            tt = (1.0 / ndc_y + 9.0) / 8.0;
            want = floor(31.0 * tt + 0.5);
            for (x = 24; x < 40; ++x, ++checked) {
                uint16_t c = px(&t, x, y);
                bad += abs(ch_r(c) - (int)want) > 1;
                if (pass == 0) {
                    bad += ch_g(c) != 63 || ch_b(c) != 31;
                }
            }
        }
        CHECK_EQ_LONG(bad, 0);
        CHECK(checked > 150u);
        CHECK(px(&t, 32, 63) == BG); /* below the near clip line */
    }
    close_t(&t);
}

/* Layer UVs are independent of the base UVs; opacity 0 leaves the base. */
static void test_independent_uvs(void)
{
    static const uint16_t base_px[2] = {RED, GREEN};
    static const uint16_t lay_px[2] = {BLUE, WHITE};
    static const float mirrored[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    PIXELICA_Texture base = tex_of(base_px, 2, 1), lay = tex_of(lay_px, 2, 1);
    PIXELICA_Vec2 luv[4];
    PIXELICA_TextureLayer l;
    PIXELICA_MaterialLayers ml;
    Quad q;
    Target t;
    int x, y, bad;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, BG, k_uv01);
    set_uv(luv, mirrored);

    l = layer_of(&lay, luv, PIXELICA_TEXTURE_LERP, 255);
    ml = ml_of(q.uv, &base, NULL, &l, 1);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_OK);
    bad = 0;
    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            bad += px(&t, x, y) != (x < 4 ? WHITE : BLUE);
        }
    }
    CHECK_EQ_LONG(bad, 0);

    l.opacity = 0;
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 32);
    CHECK_EQ_LONG(count(&t, GREEN), 32);
    CHECK(px(&t, 0, 0) == RED && px(&t, 7, 7) == GREEN);
    close_t(&t);
}

/* Every combine mode at opacity 0/128/255 over a flat base. */
static void test_combine_modes(void)
{
    static const uint16_t texel[1] = {0x3A69};
    static const uint8_t ops[3] = {0, 128, 255};
    PIXELICA_Texture tex = tex_of(texel, 1, 1);
    Quad q;
    Target t;
    int c, o;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, 0x7BCF, k_uv01);
    for (c = 0; c < 3; ++c) {
        for (o = 0; o < 3; ++o) {
            PIXELICA_TextureLayer l = layer_of(&tex, q.uv, (PIXELICA_TextureCombine)c, ops[o]);
            PIXELICA_MaterialLayers ml = ml_of(NULL, NULL, NULL, &l, 1);
            uint16_t want = ref_layer(0x7BCF, texel[0], (PIXELICA_TextureCombine)c, ops[o]);

            pixelica_begin_frame(t.ctx, BG);
            CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_OK);
            CHECK_EQ_LONG(count(&t, want), 64);
            if (ops[o] == 0) {
                CHECK_EQ_LONG(count(&t, 0x7BCF), 64);
            }
            if (c == PIXELICA_TEXTURE_LERP && ops[o] == 255) {
                CHECK_EQ_LONG(count(&t, texel[0]), 64);
            }
        }
    }
    /* ADD saturates per channel. */
    {
        static const uint16_t hi[1] = {0xFFE0 | 0x0010};
        PIXELICA_Texture h = tex_of(hi, 1, 1);
        PIXELICA_TextureLayer l = layer_of(&h, q.uv, PIXELICA_TEXTURE_ADD, 255);
        PIXELICA_MaterialLayers ml = ml_of(NULL, NULL, NULL, &l, 1);

        pixelica_begin_frame(t.ctx, BG);
        CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, WHITE), 64);
    }
    close_t(&t);
}

/* Three layers in order, then vertex color after all layers. */
static void test_stacked_layers_then_vertex_color(void)
{
    static const uint16_t b_px[1] = {0x6B4D};
    static const uint16_t l_px[3] = {0x4A69, 0x2104, 0x8C71};
    PIXELICA_Texture base = tex_of(b_px, 1, 1), lt[3];
    PIXELICA_TextureLayer l[3];
    PIXELICA_ColorRGBA8 vc[4];
    PIXELICA_MaterialLayers ml;
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    uint16_t want;
    Quad q;
    Target t;
    int i;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    for (i = 0; i < 3; ++i) {
        lt[i] = tex_of(&l_px[i], 1, 1);
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, BG, k_uv01);
    l[0] = layer_of(&lt[0], q.uv, PIXELICA_TEXTURE_MULTIPLY, 200);
    l[1] = layer_of(&lt[1], q.uv, PIXELICA_TEXTURE_ADD, 128);
    l[2] = layer_of(&lt[2], q.uv, PIXELICA_TEXTURE_LERP, 90);
    fill_vc(vc, rgba(200, 100, 255, 255));
    ml = ml_of(q.uv, &base, vc, l, 3);

    want = b_px[0];
    for (i = 0; i < 3; ++i) {
        want = ref_layer(want, l_px[i], l[i].combine, l[i].opacity);
    }
    want = ref_modulate(want, vc[0]);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, want), 64);
    close_t(&t);
}

/* Layer wrap modes: REPEAT wraps negative UVs, CLAMP clamps; both per axis. */
static void test_layer_wrap(void)
{
    static const struct { float u, v; PIXELICA_WrapMode wu, wv; int tx, ty; } cases[] = {
        {-0.25f, 0.5f, PIXELICA_WRAP_REPEAT, PIXELICA_WRAP_REPEAT, 2, 1},
        {-3.0f, 2.0f, PIXELICA_WRAP_REPEAT, PIXELICA_WRAP_REPEAT, 0, 0},
        {-0.25f, 0.5f, PIXELICA_WRAP_CLAMP, PIXELICA_WRAP_REPEAT, 0, 1},
        {-0.25f, -0.25f, PIXELICA_WRAP_REPEAT, PIXELICA_WRAP_CLAMP, 2, 0},
    };
    PIXELICA_Texture tex = tex_of(k_tex32, 3, 2);
    Quad q;
    Target t;
    size_t i;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, BG, k_uv01);
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        PIXELICA_Vec2 uv[4];
        PIXELICA_TextureLayer l;
        PIXELICA_MaterialLayers ml;
        int k;

        for (k = 0; k < 4; ++k) {
            uv[k].x = cases[i].u;
            uv[k].y = cases[i].v;
        }
        l = layer_of(&tex, uv, PIXELICA_TEXTURE_LERP, 255);
        l.wrap_u = cases[i].wu;
        l.wrap_v = cases[i].wv;
        ml = ml_of(NULL, NULL, NULL, &l, 1);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, k_tex32[cases[i].ty * 3 + cases[i].tx]), 64);
    }
    close_t(&t);
}

/* Alpha comes from the BASE texture's plane only; layers add no alpha. TEST
 * and BLEND keep the layered color, TEST writes depth, BLEND never does. */
static void test_base_alpha_with_layers(void)
{
    static const uint16_t base_px[1] = {RED};
    static const uint16_t lay_px[4] = {GREEN, GREEN, GREEN, GREEN};
    static const uint8_t plane[1] = {128};
    PIXELICA_Texture base = tex_of(base_px, 1, 1), lay = tex_of(lay_px, 2, 2);
    PIXELICA_TextureLayer l;
    PIXELICA_MaterialLayers ml;
    PIXELICA_DrawOptions o;
    Quad q;
    Target t;
    uint64_t before, depth_before;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, BG, k_uv01);
    l = layer_of(&lay, q.uv, PIXELICA_TEXTURE_LERP, 255);
    ml = ml_of(q.uv, &base, NULL, &l, 1);

    o = opts(PIXELICA_ALPHA_TEST, 255, 128, plane, 1); /* plane covers only base */
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, GREEN), 64);
    CHECK(pixelica_t_depth_at(t.ctx, 8, 3, 3) < PIXELICA_DEPTH_CLEAR);

    o.alpha_cutoff = 129;
    pixelica_begin_frame(t.ctx, BLUE);
    before = frame_hash(&t);
    CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
    CHECK(frame_hash(&t) == before);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 0);

    o = opts(PIXELICA_ALPHA_BLEND, 255, 0, plane, 1);
    pixelica_begin_frame(t.ctx, BLUE);
    depth_before = depth_hash(&t);
    CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, ref_blend(GREEN, BLUE, 128)), 64);
    CHECK(depth_hash(&t) == depth_before);
    close_t(&t);
}

/* Final alpha = near(near(texA * vertexA) * opacity). OPAQUE ignores it; TEST
 * keeps at cutoff == alpha and drops at +1; BLEND never writes depth. */
static void test_vertex_alpha(void)
{
    static const struct { uint8_t tex_a, vtx_a, opacity, textured; } cases[] = {
        {200, 0, 255, 1},   {200, 128, 255, 1}, {200, 255, 255, 1}, {200, 255, 150, 1},
        {200, 100, 150, 1}, {255, 128, 255, 0}, {255, 128, 100, 0}, {255, 255, 0, 0},
    };
    static const uint16_t texel[1] = {RED};
    PIXELICA_Texture tex = tex_of(texel, 1, 1);
    PIXELICA_ColorRGBA8 vc[4];
    Target t;
    size_t i;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const uint8_t plane = cases[i].tex_a;
        const size_t bytes = cases[i].textured ? 1u : 0u;
        const int a1 = near_div(cases[i].tex_a * cases[i].vtx_a);
        const int eff = near_div(a1 * cases[i].opacity);
        PIXELICA_MaterialLayers ml;
        PIXELICA_DrawOptions o;
        Quad q;
        uint64_t d_before, before;

        fill_vc(vc, rgba(255, 255, 255, cases[i].vtx_a));
        quad(&q, &t, 0, 0, 8, 8, 0.0f, RED, k_uv01);
        ml = ml_of(cases[i].textured ? q.uv : NULL, cases[i].textured ? &tex : NULL, vc, NULL, 0);

        /* BLEND over blue */
        o = opts(PIXELICA_ALPHA_BLEND, cases[i].opacity, 0, bytes ? &plane : NULL, bytes);
        pixelica_begin_frame(t.ctx, BLUE);
        d_before = depth_hash(&t);
        CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, ref_blend(RED, BLUE, eff)), 64);
        CHECK(depth_hash(&t) == d_before);

        /* TEST: keep at cutoff == eff, drop at eff + 1 */
        o = opts(PIXELICA_ALPHA_TEST, cases[i].opacity, (uint8_t)eff, bytes ? &plane : NULL, bytes);
        pixelica_begin_frame(t.ctx, BLUE);
        CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, RED), 64);
        CHECK(pixelica_t_depth_at(t.ctx, 8, 4, 4) < PIXELICA_DEPTH_CLEAR);
        if (eff < 255) {
            o.alpha_cutoff = (uint8_t)(eff + 1);
            pixelica_begin_frame(t.ctx, BLUE);
            before = frame_hash(&t);
            CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
            CHECK(frame_hash(&t) == before);
        }

        /* OPAQUE ignores vertex alpha, plane and opacity */
        o = opts(PIXELICA_ALPHA_OPAQUE, cases[i].opacity, 255, bytes ? &plane : NULL, bytes);
        pixelica_begin_frame(t.ctx, BLUE);
        CHECK(draw(&t, &q, &ml, &o) == PIXELICA_OK);
        CHECK_EQ_LONG(count(&t, RED), 64);
    }
    close_t(&t);
}

/* Bad layer descriptions are rejected before any pixel or counter changes. */
static void test_rejects(void)
{
    static const uint16_t texel[1] = {GREEN};
    PIXELICA_Texture good = tex_of(texel, 1, 1);
    PIXELICA_Texture no_pixels = tex_of(NULL, 1, 1), zero_w = tex_of(texel, 0, 1);
    PIXELICA_Vec2 uv[4], nan_uv[4], inf_uv[4], nan_base[4];
    PIXELICA_TextureLayer l[4], bad[1];
    PIXELICA_MaterialLayers ml;
    Quad q, bg;
    Target t;
    uint64_t before;
    uint32_t calls;
    size_t i;
    float nanv;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    nanv = pixelica_t_nan();
    quad(&q, &t, 0, 0, 8, 8, 0.0f, RED, k_uv01);
    quad(&bg, &t, 0, 0, 4, 8, 0.5f, BLUE, k_uv01);
    memcpy(uv, q.uv, sizeof uv);
    memcpy(nan_uv, q.uv, sizeof uv);
    memcpy(inf_uv, q.uv, sizeof uv);
    memcpy(nan_base, q.uv, sizeof uv);
    nan_uv[2].x = nanv;
    inf_uv[1].y = pixelica_t_inf();
    nan_base[3].y = nanv;
    for (i = 0; i < 4; ++i) {
        l[i] = layer_of(&good, uv, PIXELICA_TEXTURE_MULTIPLY, 255);
    }

    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw(&t, &bg, &(PIXELICA_MaterialLayers){0}, NULL) == PIXELICA_OK);
    before = frame_hash(&t);
    calls = pixelica_frame_stats(t.ctx).draw_calls;

    ml = ml_of(NULL, NULL, NULL, l, 4); /* too many layers */
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
    ml = ml_of(NULL, NULL, NULL, NULL, 1); /* count without array */
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);

    for (i = 0; i < 9; ++i) {
        bad[0] = layer_of(&good, uv, PIXELICA_TEXTURE_LERP, 255);
        switch (i) {
        case 0: bad[0].texture = NULL; break;
        case 1: bad[0].uvs = NULL; break;
        case 2: bad[0].texture = &no_pixels; break;
        case 3: bad[0].texture = &zero_w; break;
        case 4: bad[0].combine = (PIXELICA_TextureCombine)3; break;
        case 5: bad[0].combine = (PIXELICA_TextureCombine)-1; break;
        case 6: bad[0].wrap_v = (PIXELICA_WrapMode)2; break;
        case 7: bad[0].uvs = nan_uv; break;
        default: bad[0].uvs = inf_uv; break;
        }
        ml = ml_of(NULL, NULL, NULL, bad, 1);
        CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
    }

    /* a bad second layer poisons the whole draw even after a good first */
    l[1].combine = (PIXELICA_TextureCombine)7;
    ml = ml_of(NULL, NULL, NULL, l, 2);
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
    l[1].combine = PIXELICA_TEXTURE_ADD;

    /* base problems still apply: NaN UV, texture without UVs */
    ml = ml_of(nan_base, &good, NULL, l, 1);
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
    ml = ml_of(NULL, &good, NULL, l, 1);
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);

    /* Borrowed inputs cannot alias buffers written by this context. */
    {
        const uint16_t *owned = pixelica_color_buffer(t.ctx);
        PIXELICA_Texture aliased = good;
        PIXELICA_Mesh mesh = q.m;
        ml = ml_of(NULL, NULL, (const PIXELICA_ColorRGBA8 *)(const void *)owned, NULL, 0);
        CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
        ml = ml_of(NULL, NULL, NULL,
            (const PIXELICA_TextureLayer *)(const void *)owned, 1);
        CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
        ml = ml_of(q.uv, (const PIXELICA_Texture *)(const void *)owned, NULL, NULL, 0);
        CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
        aliased.pixels = owned;
        ml = ml_of(q.uv, &aliased, NULL, NULL, 0);
        CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
        bad[0] = layer_of(&good, (const PIXELICA_Vec2 *)(const void *)owned,
            PIXELICA_TEXTURE_LERP, 255);
        ml = ml_of(NULL, NULL, NULL, bad, 1);
        CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
        ml = ml_of(NULL, NULL, NULL, NULL, 0);
        mesh.vertices = (const PIXELICA_Vec3 *)(const void *)owned;
        CHECK(pixelica_draw_mesh_layers(t.ctx, &mesh, k_id, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
        mesh = q.m;
        mesh.indices = owned;
        CHECK(pixelica_draw_mesh_layers(t.ctx, &mesh, k_id, &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_draw_mesh_layers(t.ctx, &q.m, (const float *)(const void *)owned,
            &ml, NULL) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_draw_mesh_layers(t.ctx, &q.m, k_id,
            (const PIXELICA_MaterialLayers *)(const void *)owned, NULL) == PIXELICA_ERR_INVALID_ARG);
    }

    CHECK(frame_hash(&t) == before);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).draw_calls, calls);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).last_error, PIXELICA_ERR_INVALID_ARG);

    /* the maximum of three layers is accepted */
    ml = ml_of(NULL, NULL, NULL, l, 3);
    CHECK(draw(&t, &q, &ml, NULL) == PIXELICA_OK);
    CHECK(frame_hash(&t) != before);
    close_t(&t);
}

int main(void)
{
    CHECK_EQ_LONG(PIXELICA_MAX_TEXTURE_LAYERS, 3);
    CHECK_EQ_LONG(PIXELICA_TEXTURE_MULTIPLY, 0);
    CHECK_EQ_LONG(PIXELICA_TEXTURE_ADD, 1);
    CHECK_EQ_LONG(PIXELICA_TEXTURE_LERP, 2);
    CHECK_EQ_LONG(sizeof(PIXELICA_ColorRGBA8), 4);
    test_legacy_identical();
    test_constant_vertex_color();
    test_varying_vertex_color();
    test_clipped_perspective_vertex_color();
    test_independent_uvs();
    test_combine_modes();
    test_stacked_layers_then_vertex_color();
    test_layer_wrap();
    test_base_alpha_with_layers();
    test_vertex_alpha();
    test_rejects();
    return pixelica_t_finish("test_material_layers");
}

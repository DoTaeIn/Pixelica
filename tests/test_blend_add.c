/*
 * Pixelica - framebuffer additive blending in the 3D path
 * (PIXELICA_ALPHA_ADD): saturating RGB565 channels, effective alpha from
 * the alpha plane and opacity, depth semantics (tested, never written),
 * order independence, and the distinction from the material-layer
 * PIXELICA_TEXTURE_ADD, which combines a texture into the fragment colour
 * long before the framebuffer is touched.
 */
#include "test_util.h"

#define BG ((uint16_t)0x0000)
#define WHITE ((uint16_t)0xFFFF)
#define BAD PIXELICA_ERR_INVALID_ARG

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

static uint16_t px(const Target *t, int x, int y)
{
    return pixelica_t_color_at(t->ctx, t->w, x, y);
}
static uint16_t dpx(const Target *t, int x, int y)
{
    return pixelica_t_depth_at(t->ctx, t->w, x, y);
}

static int ch_r(uint16_t c) { return c >> 11; }
static int ch_g(uint16_t c) { return (c >> 5) & 63; }
static int ch_b(uint16_t c) { return c & 31; }
static uint16_t pack(int r, int g, int b)
{
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static int sat(int v, int m) { return v < m ? v : m; }
static int near_div(int v) { return (v + 127) / 255; }

/* The documented framebuffer ADD, per native channel. */
static uint16_t ref_add(uint16_t src, uint16_t dst, int a)
{
    return pack(sat(ch_r(dst) + near_div(ch_r(src) * a), 31),
                sat(ch_g(dst) + near_div(ch_g(src) * a), 63),
                sat(ch_b(dst) + near_div(ch_b(src) * a), 31));
}

static void quad(Quad *q, const Target *t, float x0, float y0, float x1,
                 float y1, float z, uint16_t color)
{
    const float nx0 = x0 / (float)t->w * 2.0f - 1.0f;
    const float nx1 = x1 / (float)t->w * 2.0f - 1.0f;
    const float ny0 = 1.0f - y0 / (float)t->h * 2.0f;
    const float ny1 = 1.0f - y1 / (float)t->h * 2.0f;
    int i;

    q->v[0].x = nx0; q->v[0].y = ny1;
    q->v[1].x = nx1; q->v[1].y = ny1;
    q->v[2].x = nx1; q->v[2].y = ny0;
    q->v[3].x = nx0; q->v[3].y = ny0;
    for (i = 0; i < 4; ++i) {
        q->v[i].z = z;
    }
    q->uv[0].x = 0.0f; q->uv[0].y = 1.0f;
    q->uv[1].x = 1.0f; q->uv[1].y = 1.0f;
    q->uv[2].x = 1.0f; q->uv[2].y = 0.0f;
    q->uv[3].x = 0.0f; q->uv[3].y = 0.0f;
    q->m.vertices = q->v;
    q->m.vertex_count = 4;
    q->m.indices = k_quad;
    q->m.index_count = 6;
    q->m.color = color;
}

static PIXELICA_DrawOptions add_opts(uint8_t opacity)
{
    PIXELICA_DrawOptions o = pixelica_draw_options_default();

    o.alpha_mode = PIXELICA_ALPHA_ADD;
    o.opacity = opacity;
    return o;
}

/* ---- saturating accumulation ------------------------------------------- */

static void test_add_saturates(void)
{
    Target t;
    Quad q;
    const uint16_t step = pack(8, 8, 8);
    PIXELICA_DrawOptions o = add_opts(255);
    uint16_t expect;
    int i;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0.0f, 0.0f, 8.0f, 8.0f, 0.0f, step);

    pixelica_begin_frame(t.ctx, BG);
    expect = BG;
    for (i = 0; i < 6; ++i) {
        CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, NULL, &o) ==
              PIXELICA_OK);
        expect = ref_add(step, expect, 255);
        CHECK(px(&t, 4, 4) == expect);
    }
    /* Six additions of 8 would be 48: the channels saturate at 31/63/31
     * instead of wrapping round to a dark colour. */
    CHECK(ch_r(px(&t, 4, 4)) == 31);
    CHECK(ch_g(px(&t, 4, 4)) == 48);
    CHECK(ch_b(px(&t, 4, 4)) == 31);
    CHECK(px(&t, 4, 4) == pack(31, 48, 31));

    /* Adding onto an already saturated pixel is a no-op, never a wrap. */
    CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(px(&t, 4, 4) == pack(31, 56, 31));
    CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(px(&t, 4, 4) == WHITE);
    CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(px(&t, 4, 4) == WHITE);
    close_t(&t);
}

/* ---- effective alpha ---------------------------------------------------- */

static void test_add_alpha_and_opacity(void)
{
    Target t;
    Quad q;
    const uint16_t src = pack(16, 32, 16);
    PIXELICA_DrawOptions o;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0.0f, 0.0f, 8.0f, 8.0f, 0.0f, src);

    /* Opacity scales the source before it is added. */
    o = add_opts(128);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(px(&t, 4, 4) == ref_add(src, BG, 128));

    /* Effective alpha 0 writes nothing at all - no colour, no depth, and
     * the pixel is not counted as written. */
    o = add_opts(0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(px(&t, 4, 4) == BG);
    CHECK(dpx(&t, 4, 4) == PIXELICA_DEPTH_CLEAR);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 0);
    CHECK(pixelica_frame_stats(t.ctx).fragments_covered == 64u);

    /* A texture alpha plane multiplies with the opacity the same way it
     * does for BLEND: a = round(round(plane) * opacity / 255). */
    {
        const uint16_t texels[4] = {src, src, src, src};
        const uint8_t plane[4] = {255u, 64u, 64u, 64u};
        PIXELICA_Texture tex;
        PIXELICA_Material mat;
        int a;

        tex.pixels = texels;
        tex.width = 2;
        tex.height = 2;
        mat.uvs = q.uv;
        mat.triangle_colors = NULL;
        mat.texture = &tex;
        o = add_opts(200);
        o.texture_alpha = plane;
        o.texture_alpha_bytes = sizeof plane;
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, &mat, &o) ==
              PIXELICA_OK);
        a = near_div(64 * 200);
        CHECK(px(&t, 6, 6) == ref_add(src, BG, a)); /* bottom-right texel */
        a = near_div(255 * 200);
        CHECK(px(&t, 1, 1) == ref_add(src, BG, a)); /* top-left texel */
    }
    close_t(&t);
}

/* ---- depth semantics ---------------------------------------------------- */

static void test_add_depth(void)
{
    Target t;
    Quad base, glow, late;
    const uint16_t src = pack(8, 8, 8);
    PIXELICA_DrawOptions o = add_opts(255);
    uint16_t depth_after_base;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&base, &t, 0.0f, 0.0f, 8.0f, 8.0f, 0.0f, pack(4, 4, 4));

    /* An opaque surface at z = 0 writes depth. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &base.m, k_id) == PIXELICA_OK);
    depth_after_base = dpx(&t, 4, 4);
    CHECK(depth_after_base != PIXELICA_DEPTH_CLEAR);

    /* Additive geometry BEHIND it is depth-rejected. */
    quad(&glow, &t, 0.0f, 0.0f, 8.0f, 8.0f, 0.5f, src);
    CHECK(pixelica_draw_mesh_options(t.ctx, &glow.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(px(&t, 4, 4) == pack(4, 4, 4));
    CHECK(dpx(&t, 4, 4) == depth_after_base);

    /* Additive geometry IN FRONT is added, but leaves depth alone. */
    quad(&glow, &t, 0.0f, 0.0f, 8.0f, 8.0f, -0.5f, src);
    CHECK(pixelica_draw_mesh_options(t.ctx, &glow.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(px(&t, 4, 4) == ref_add(src, pack(4, 4, 4), 255));
    CHECK(dpx(&t, 4, 4) == depth_after_base); /* unchanged */

    /* Because no depth was written, opaque geometry drawn afterwards at a
     * depth between the two is NOT occluded by the additive layer. */
    quad(&late, &t, 0.0f, 0.0f, 8.0f, 8.0f, -0.25f, pack(2, 2, 2));
    CHECK(pixelica_draw_mesh(t.ctx, &late.m, k_id) == PIXELICA_OK);
    CHECK(px(&t, 4, 4) == pack(2, 2, 2));
    CHECK(dpx(&t, 4, 4) < depth_after_base);
    close_t(&t);
}

/* ---- order independence -------------------------------------------------- */

static void test_add_order_independent(void)
{
    Target t;
    Quad a, b;
    PIXELICA_DrawOptions o = add_opts(255);
    uint16_t ab, ba;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    /* Different colours AND different depths: ADD neither sorts nor
     * occludes, so the result does not depend on submission order. */
    quad(&a, &t, 0.0f, 0.0f, 8.0f, 8.0f, -0.4f, pack(5, 9, 3));
    quad(&b, &t, 0.0f, 0.0f, 8.0f, 8.0f, 0.4f, pack(7, 2, 11));

    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_options(t.ctx, &a.m, k_id, NULL, &o) == PIXELICA_OK);
    CHECK(pixelica_draw_mesh_options(t.ctx, &b.m, k_id, NULL, &o) == PIXELICA_OK);
    ab = px(&t, 4, 4);

    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_options(t.ctx, &b.m, k_id, NULL, &o) == PIXELICA_OK);
    CHECK(pixelica_draw_mesh_options(t.ctx, &a.m, k_id, NULL, &o) == PIXELICA_OK);
    ba = px(&t, 4, 4);

    CHECK(ab == ba);
    CHECK(ab == pack(12, 11, 14));
    /* BLEND, by contrast, depends on the order. */
    {
        PIXELICA_DrawOptions ob = pixelica_draw_options_default();
        uint16_t first, second;

        ob.alpha_mode = PIXELICA_ALPHA_BLEND;
        ob.opacity = 128;
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_options(t.ctx, &a.m, k_id, NULL, &ob) ==
              PIXELICA_OK);
        CHECK(pixelica_draw_mesh_options(t.ctx, &b.m, k_id, NULL, &ob) ==
              PIXELICA_OK);
        first = px(&t, 4, 4);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_options(t.ctx, &b.m, k_id, NULL, &ob) ==
              PIXELICA_OK);
        CHECK(pixelica_draw_mesh_options(t.ctx, &a.m, k_id, NULL, &ob) ==
              PIXELICA_OK);
        second = px(&t, 4, 4);
        CHECK(first != second);
    }
    close_t(&t);
}

/* ---- framebuffer ADD is not texture-layer ADD --------------------------- */

static void test_add_is_not_texture_add(void)
{
    Target t;
    Quad q;
    const uint16_t layer_texels[1] = {pack(8, 8, 8)};
    const uint16_t base_texels[1] = {pack(4, 4, 4)};
    PIXELICA_Texture layer_tex, base_tex;
    PIXELICA_TextureLayer layer;
    PIXELICA_MaterialLayers ml;
    PIXELICA_DrawOptions o;
    uint16_t dst_before;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0.0f, 0.0f, 8.0f, 8.0f, 0.0f, pack(0, 0, 0));
    base_tex.pixels = base_texels;
    base_tex.width = 1;
    base_tex.height = 1;
    layer_tex.pixels = layer_texels;
    layer_tex.width = 1;
    layer_tex.height = 1;
    layer.texture = &layer_tex;
    layer.uvs = q.uv;
    layer.wrap_u = PIXELICA_WRAP_CLAMP;
    layer.wrap_v = PIXELICA_WRAP_CLAMP;
    layer.combine = PIXELICA_TEXTURE_ADD;
    layer.opacity = 255u;
    memset(&ml, 0, sizeof ml);
    ml.base.uvs = q.uv;
    ml.base.texture = &base_tex;
    ml.layers = &layer;
    ml.layer_count = 1;

    /* Start from a non-black framebuffer so the two meanings differ. */
    pixelica_begin_frame(t.ctx, pack(20, 20, 20));
    dst_before = px(&t, 4, 4);
    CHECK(dst_before == pack(20, 20, 20));

    /* Texture-layer ADD combines base + layer into the FRAGMENT colour and
     * then, being OPAQUE, REPLACES the destination and writes depth. */
    CHECK(pixelica_draw_mesh_layers(t.ctx, &q.m, k_id, &ml, NULL) ==
          PIXELICA_OK);
    CHECK(px(&t, 4, 4) == pack(12, 12, 12)); /* 4 + 8, not 20 + anything */
    CHECK(dpx(&t, 4, 4) != PIXELICA_DEPTH_CLEAR);

    /* Framebuffer ADD adds the fragment colour ONTO the destination and
     * leaves depth alone. Same layered material, different destination
     * rule. */
    pixelica_begin_frame(t.ctx, pack(20, 20, 20));
    o = add_opts(255);
    CHECK(pixelica_draw_mesh_layers(t.ctx, &q.m, k_id, &ml, &o) == PIXELICA_OK);
    CHECK(px(&t, 4, 4) == pack(31, 32, 31)); /* 20 + 12, red/blue saturate */
    CHECK(dpx(&t, 4, 4) == PIXELICA_DEPTH_CLEAR);
    close_t(&t);
}

/* ---- validation ---------------------------------------------------------- */

static void test_add_validation(void)
{
    Target t;
    Quad q;
    PIXELICA_DrawOptions o;
    uint64_t before;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0.0f, 0.0f, 8.0f, 8.0f, 0.0f, WHITE);
    pixelica_begin_frame(t.ctx, BG);

    /* ADD is a known mode everywhere PIXELICA_AlphaMode is accepted. */
    o = add_opts(255);
    CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(pixelica_draw_mesh_layers(t.ctx, &q.m, k_id, NULL, &o) ==
          PIXELICA_OK);
    CHECK(pixelica_draw_mesh_effects(t.ctx, &q.m, k_id, NULL, &o, NULL) ==
          PIXELICA_OK);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, &o, NULL, NULL) ==
          PIXELICA_OK);

    /* One past ADD is still rejected, and rejects change nothing. */
    before = pixelica_t_hash16(pixelica_color_buffer(t.ctx), 64);
    o.alpha_mode = (PIXELICA_AlphaMode)(PIXELICA_ALPHA_ADD + 1);
    CHECK(pixelica_draw_mesh_options(t.ctx, &q.m, k_id, NULL, &o) == BAD);
    CHECK(pixelica_t_hash16(pixelica_color_buffer(t.ctx), 64) == before);
    close_t(&t);
}

int main(void)
{
    test_add_saturates();
    test_add_alpha_and_opacity();
    test_add_depth();
    test_add_order_independent();
    test_add_is_not_texture_add();
    test_add_validation();
    return pixelica_t_finish("test_blend_add");
}

/*
 * Pixelica2D - framebuffer additive blending (PIXELICA_ALPHA_ADD) in
 * pixelica2d_draw_sprite_ex: saturating RGB565 channels, effective alpha
 * from the alpha plane, the sprite opacity and the option opacity, depth
 * semantics (tested, never written), the colour key running first, and
 * order independence.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pixelica/renderer2d.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define W 4u
#define H 2u
#define BAD PIXELICA_ERR_INVALID_ARG

typedef struct Rig {
    uint16_t color[W * H];
    uint16_t depth[W * H];
    PIXELICA2D_Context ctx;
} Rig;

static int ch_r(uint16_t c) { return c >> 11; }
static int ch_g(uint16_t c) { return (c >> 5) & 63; }
static int ch_b(uint16_t c) { return c & 31; }
static uint16_t pack(int r, int g, int b)
{
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static int sat(int v, int m) { return v < m ? v : m; }
static uint32_t mul255(uint32_t v, uint32_t a) { return (v * a + 127u) / 255u; }

static uint16_t ref_add(uint16_t src, uint16_t dst, uint32_t a)
{
    return pack(sat(ch_r(dst) + (int)mul255((uint32_t)ch_r(src), a), 31),
                sat(ch_g(dst) + (int)mul255((uint32_t)ch_g(src), a), 63),
                sat(ch_b(dst) + (int)mul255((uint32_t)ch_b(src), a), 31));
}

/* 1x1 texture of one colour, so every covered pixel takes the same texel. */
static uint16_t g_texel = 0;
static const PIXELICA_Texture k_texture = {&g_texel, 1u, 1u};

static void rig_init(Rig *r, uint16_t fill, uint16_t depth_fill)
{
    PIXELICA2D_Surface s;
    size_t i;

    s.pixels = r->color;
    s.width = (uint16_t)W;
    s.height = (uint16_t)H;
    s.stride = W;
    s.depth = r->depth;
    s.depth_stride = W;
    CHECK(pixelica2d_init(&r->ctx, &s) == PIXELICA_OK);
    for (i = 0; i < W * H; ++i) {
        r->color[i] = fill;
        r->depth[i] = depth_fill;
    }
}

/* A sprite covering the whole surface. */
static PIXELICA2D_Sprite full_sprite(void)
{
    PIXELICA2D_Sprite s;

    CHECK(pixelica2d_sprite_init(&s, &k_texture) == PIXELICA_OK);
    s.scale.x = (float)W;
    s.scale.y = (float)H;
    return s;
}

static PIXELICA_DrawOptions add_opts(uint8_t opacity)
{
    PIXELICA_DrawOptions o = pixelica_draw_options_default();

    o.alpha_mode = PIXELICA_ALPHA_ADD;
    o.opacity = opacity;
    return o;
}

static void test_saturates(void)
{
    Rig r;
    PIXELICA2D_Sprite s = full_sprite();
    PIXELICA_DrawOptions o = add_opts(255);
    uint16_t expect = 0;
    int i;

    g_texel = pack(8, 8, 8);
    rig_init(&r, 0, PIXELICA2D_DEPTH_CLEAR);
    for (i = 0; i < 6; ++i) {
        CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
        expect = ref_add(g_texel, expect, 255u);
        CHECK(r.color[0] == expect);
    }
    /* Six additions of 8 saturate instead of wrapping. */
    CHECK(r.color[0] == pack(31, 48, 31));
    CHECK(ch_r(r.color[0]) == 31);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == pack(31, 56, 31));
}

static void test_alpha_sources(void)
{
    Rig r;
    PIXELICA2D_Sprite s;
    PIXELICA_DrawOptions o;
    const uint8_t plane[1] = {64u};
    uint32_t a;

    g_texel = pack(16, 32, 16);

    /* The option opacity alone. */
    rig_init(&r, 0, PIXELICA2D_DEPTH_CLEAR);
    s = full_sprite();
    o = add_opts(128);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == ref_add(g_texel, 0, 128u));

    /* Sprite opacity multiplies with it, exactly as for BLEND. */
    rig_init(&r, 0, PIXELICA2D_DEPTH_CLEAR);
    s = full_sprite();
    s.opacity = 200u;
    o = add_opts(128);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    a = mul255(mul255(255u, 128u), 200u);
    CHECK(r.color[0] == ref_add(g_texel, 0, a));

    /* And so does the texture alpha plane. */
    rig_init(&r, 0, PIXELICA2D_DEPTH_CLEAR);
    s = full_sprite();
    s.opacity = 200u;
    o = add_opts(128);
    o.texture_alpha = plane;
    o.texture_alpha_bytes = sizeof plane;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    a = mul255(mul255(64u, 128u), 200u);
    CHECK(r.color[0] == ref_add(g_texel, 0, a));

    /* Effective alpha 0 writes nothing. */
    rig_init(&r, 0x1234u, PIXELICA2D_DEPTH_CLEAR);
    s = full_sprite();
    o = add_opts(0);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == 0x1234u);
    CHECK(r.ctx.stats.pixels_written == 0u);

    /* The colour key is applied before alpha and blending. */
    rig_init(&r, 0x1234u, PIXELICA2D_DEPTH_CLEAR);
    s = full_sprite();
    s.color_key_enabled = true;
    s.color_key = g_texel;
    o = add_opts(255);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == 0x1234u);
    CHECK(r.ctx.stats.pixels_written == 0u);
}

static void test_depth(void)
{
    Rig r;
    PIXELICA2D_Sprite s;
    PIXELICA_DrawOptions o = add_opts(255);

    g_texel = pack(8, 8, 8);

    /* depth_test honoured: a sprite behind the stored depth is rejected. */
    rig_init(&r, 0, 100u);
    s = full_sprite();
    s.depth_test = true;
    s.depth = 200u;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == 0u);
    CHECK(r.depth[0] == 100u);

    /* In front: added, but depth is never written even when depth_write
     * was requested (ADD forces it off, like BLEND). */
    rig_init(&r, 0, 100u);
    s = full_sprite();
    s.depth_test = true;
    s.depth_write = true;
    s.depth = 50u;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == ref_add(g_texel, 0, 255u));
    CHECK(r.depth[0] == 100u);
}

static void test_order_independent(void)
{
    Rig r;
    PIXELICA2D_Sprite s;
    PIXELICA_DrawOptions o = add_opts(255);
    uint16_t ab, ba;

    rig_init(&r, 0, PIXELICA2D_DEPTH_CLEAR);
    s = full_sprite();
    g_texel = pack(5, 9, 3);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    g_texel = pack(7, 2, 11);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    ab = r.color[0];

    rig_init(&r, 0, PIXELICA2D_DEPTH_CLEAR);
    g_texel = pack(7, 2, 11);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    g_texel = pack(5, 9, 3);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    ba = r.color[0];

    CHECK(ab == ba);
    CHECK(ab == pack(12, 11, 14));
}

static void test_enum_range(void)
{
    Rig r;
    PIXELICA2D_Sprite s;
    PIXELICA_DrawOptions o = add_opts(255);

    g_texel = pack(8, 8, 8);
    rig_init(&r, 0x2222u, PIXELICA2D_DEPTH_CLEAR);
    s = full_sprite();
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);

    /* One past ADD is rejected and changes nothing. */
    rig_init(&r, 0x2222u, PIXELICA2D_DEPTH_CLEAR);
    o.alpha_mode = (PIXELICA_AlphaMode)(PIXELICA_ALPHA_ADD + 1);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == BAD);
    CHECK(r.color[0] == 0x2222u);
    CHECK(r.ctx.stats.draw_calls == 0u);
}

int main(void)
{
    test_saturates();
    test_alpha_sources();
    test_depth();
    test_order_independent();
    test_enum_range();
    if (g_failures != 0) {
        fprintf(stderr, "test_blend_add2d: %d of %d checks FAILED\n", g_failures,
                g_checks);
        return 1;
    }
    printf("test_blend_add2d: all %d checks passed\n", g_checks);
    return 0;
}

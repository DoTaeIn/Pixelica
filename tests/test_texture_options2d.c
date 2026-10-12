/* Tests for pixelica2d_draw_sprite_ex: REPEAT/CLAMP sampling and per-texel alpha. */
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

#define W 8u
#define H 4u
#define BG 0x1111u
#define A_ 0xF800u
#define B_ 0x07E0u
#define C_ 0x001Fu
#define D_ 0xFFFFu
#define E_ 0x8410u
#define F_ 0x2104u

typedef struct Rig {
    uint16_t color[W * H];
    uint16_t depth[W * H];
    PIXELICA2D_Context ctx;
} Rig;

static void rig_init(Rig *r, uint16_t depth_fill)
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
        r->color[i] = BG;
        r->depth[i] = depth_fill;
    }
}

/* 3x2 non-power-of-two texture: row0 = A B C, row1 = D E F. */
static const uint16_t k_tex[6] = {A_, B_, C_, D_, E_, F_};
static const PIXELICA_Texture k_texture = {k_tex, 3u, 2u};

static PIXELICA2D_Sprite sprite(int32_t x, int32_t y, int32_t w, int32_t h)
{
    PIXELICA2D_Sprite s;

    CHECK(pixelica2d_sprite_init(&s, &k_texture) == PIXELICA_OK);
    s.source.x = x;
    s.source.y = y;
    s.source.width = w;
    s.source.height = h;
    return s;
}

static uint16_t ref_blend(uint16_t src, uint16_t dst, uint32_t a)
{
    uint32_t ia = 255u - a;
    uint32_t r = ((src >> 11) * a + (dst >> 11) * ia + 127u) / 255u;
    uint32_t g = (((src >> 5) & 0x3Fu) * a + ((dst >> 5) & 0x3Fu) * ia + 127u) / 255u;
    uint32_t b = ((src & 0x1Fu) * a + (dst & 0x1Fu) * ia + 127u) / 255u;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void test_defaults_match_legacy(void)
{
    Rig a, b, c;
    PIXELICA2D_Sprite s = sprite(0, 0, 3, 2);
    PIXELICA_DrawOptions o = pixelica_draw_options_default();

    s.scale.x = 2.0f;
    s.scale.y = -1.5f;
    s.rotation = 0.3f;
    s.position.x = 3.0f;
    s.position.y = 2.0f;
    s.pivot.x = 1.0f;
    s.pivot.y = 1.0f;
    rig_init(&a, 100u);
    rig_init(&b, 100u);
    rig_init(&c, 100u);
    s.depth = 5u;
    s.depth_test = true;
    s.depth_write = true;
    s.color_key_enabled = true;
    s.color_key = E_;
    s.opacity = 200u;
    CHECK(pixelica2d_draw_sprite(&a.ctx, &s) == PIXELICA_OK);
    CHECK(pixelica2d_draw_sprite_ex(&b.ctx, &s, NULL) == PIXELICA_OK);
    CHECK(pixelica2d_draw_sprite_ex(&c.ctx, &s, &o) == PIXELICA_OK);
    CHECK(memcmp(a.color, b.color, sizeof a.color) == 0 && memcmp(a.depth, b.depth, sizeof a.depth) == 0);
    CHECK(memcmp(a.color, c.color, sizeof a.color) == 0 && memcmp(a.depth, c.depth, sizeof a.depth) == 0);
    CHECK(a.ctx.stats.pixels_written == c.ctx.stats.pixels_written && a.ctx.stats.pixels_written > 0u);
    /* legacy rect validation stays for NULL, ex with options relaxes it. */
    s = sprite(-1, 0, 3, 2);
    CHECK(pixelica2d_draw_sprite_ex(&a.ctx, &s, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_sprite_ex(&a.ctx, &s, &o) == PIXELICA_OK);
}

static void test_repeat_and_clamp(void)
{
    static const uint16_t rep[5] = {C_, A_, B_, C_, A_};
    static const uint16_t clp[4] = {A_, A_, B_, C_};
    Rig r;
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    PIXELICA2D_Sprite s;
    size_t i;

    /* negative x, wider than the texture, REPEAT U */
    rig_init(&r, 0u);
    o.wrap_u = PIXELICA_WRAP_REPEAT;
    s = sprite(-1, 0, 5, 1);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    for (i = 0; i < 5u; ++i) {
        CHECK(r.color[i] == rep[i]);
    }
    CHECK(r.color[5] == BG && r.color[W] == BG);

    /* CLAMP U leaves the out-of-range coordinate on the edge texel */
    rig_init(&r, 0u);
    o = pixelica_draw_options_default();
    s = sprite(-1, 0, 4, 1);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    for (i = 0; i < 4u; ++i) {
        CHECK(r.color[i] == clp[i]);
    }

    /* independent V: REPEAT with y = -1 -> row 1, U stays clamped */
    rig_init(&r, 0u);
    o.wrap_v = PIXELICA_WRAP_REPEAT;
    s = sprite(0, -1, 3, 3);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == D_ && r.color[1] == E_ && r.color[2] == F_);
    CHECK(r.color[W] == A_ && r.color[W + 2u] == C_);
    CHECK(r.color[2u * W] == D_ && r.color[2u * W + 2u] == F_);

    /* very large negative coordinates wrap without overflow: -3 * 100000 is a multiple of 3 */
    rig_init(&r, 0u);
    o.wrap_u = PIXELICA_WRAP_REPEAT;
    s = sprite(INT32_MIN, INT32_MIN, 3, 2);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    /* INT32_MIN mod 3 == 1, mod 2 == 0 */
    CHECK(r.color[0] == B_ && r.color[1] == C_ && r.color[2] == A_);

    /* 2x scale + rotation + repeat is accepted and writes inside the surface */
    rig_init(&r, 0u);
    s = sprite(-7, 5, 6, 4);
    s.scale.x = 1.0f;
    s.rotation = 0.5f;
    s.position.x = 4.0f;
    s.position.y = 2.0f;
    s.pivot.x = 3.0f;
    s.pivot.y = 2.0f;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.ctx.stats.pixels_written > 0u);
}

static void test_alpha_test(void)
{
    static const uint8_t map[6] = {255u, 0u, 255u, 10u, 200u, 255u};
    Rig r;
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    PIXELICA2D_Sprite s = sprite(0, 0, 3, 2);

    o.alpha_mode = PIXELICA_ALPHA_TEST;
    o.texture_alpha = map;
    o.texture_alpha_bytes = sizeof map;
    rig_init(&r, 50u);
    s.depth = 7u;
    s.depth_test = true;
    s.depth_write = true;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == A_ && r.color[1] == BG && r.color[2] == C_);
    CHECK(r.color[W] == BG && r.color[W + 1u] == E_ && r.color[W + 2u] == F_);
    CHECK(r.depth[0] == 7u && r.depth[1] == 50u && r.depth[W] == 50u && r.depth[W + 1u] == 7u);
    CHECK(r.ctx.stats.pixels_written == 4u);

    /* depth test rejects farther sprite; options.opacity multiplies into the cutoff */
    s.depth = 60u;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.ctx.stats.pixels_written == 4u);

    rig_init(&r, 0u);
    s.depth_test = false;
    s.depth_write = false;
    o.opacity = 128u; /* 255->128, 200->100 (<128), 10->5 */
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == A_ && r.color[2] == C_ && r.color[W + 1u] == BG && r.color[W + 2u] == F_);
    CHECK(r.color[0] == A_); /* survivors are opaque, not blended */

    /* sprite.opacity multiplies too: 255*128/255=128, *128 -> 64 < 128 */
    rig_init(&r, 0u);
    s.opacity = 128u;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.ctx.stats.pixels_written == 0u);
}

static void test_alpha_blend(void)
{
    static const uint8_t map[6] = {255u, 0u, 128u, 64u, 255u, 255u};
    Rig r;
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    PIXELICA2D_Sprite s = sprite(0, 0, 3, 1);
    uint32_t a;

    o.alpha_mode = PIXELICA_ALPHA_BLEND;
    o.texture_alpha = map;
    o.texture_alpha_bytes = sizeof map;
    rig_init(&r, 40u);
    s.depth = 9u;
    s.depth_test = true;
    s.depth_write = true;
    s.opacity = 200u;
    o.opacity = 220u;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    a = (255u * 220u + 127u) / 255u;
    a = (a * 200u + 127u) / 255u;
    CHECK(r.color[0] == ref_blend(A_, BG, a));
    CHECK(r.color[1] == BG);
    a = (128u * 220u + 127u) / 255u;
    a = (a * 200u + 127u) / 255u;
    CHECK(r.color[2] == ref_blend(C_, BG, a));
    CHECK(r.depth[0] == 40u && r.depth[1] == 40u && r.depth[2] == 40u); /* never writes depth */
    /* Ignored depth_write does not require a depth buffer for blending. */
    rig_init(&r, 0u);
    r.ctx.surface.depth = NULL;
    s.depth_test = false;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.ctx.stats.pixels_written == 2u);
    s.depth_test = true;

    /* depth test still applies */
    rig_init(&r, 5u);
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == BG && r.ctx.stats.pixels_written == 0u);

    /* alpha 255 is an exact copy; color key still skips */
    rig_init(&r, 0u);
    s = sprite(0, 0, 3, 2);
    s.color_key_enabled = true;
    s.color_key = E_;
    o.opacity = 255u;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[0] == A_ && r.color[W + 1u] == BG && r.color[W + 2u] == F_);

    /* OPAQUE ignores options.opacity and the map, keeps sprite.opacity */
    rig_init(&r, 0u);
    o.alpha_mode = PIXELICA_ALPHA_OPAQUE;
    o.opacity = 1u;
    s.color_key_enabled = false;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.color[1] == B_ && r.color[0] == A_);
}

static void test_invalid(void)
{
    static const uint8_t map[6] = {255u, 255u, 255u, 255u, 255u, 255u};
    Rig r;
    Rig ref;
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    PIXELICA2D_Sprite s = sprite(0, 0, 3, 2);
    PIXELICA2D_Context ctx_before;

    rig_init(&r, 3u);
    rig_init(&ref, 3u);
    pixelica2d_draw_sprite(&r.ctx, &s);
    pixelica2d_draw_sprite(&ref.ctx, &s);
    ctx_before = r.ctx;

#define EXPECT_REJECT()                                                                         \
    do {                                                                                         \
        CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_ERR_INVALID_ARG);            \
        CHECK(memcmp(r.color, ref.color, sizeof r.color) == 0);                                  \
        CHECK(memcmp(r.depth, ref.depth, sizeof r.depth) == 0);                                  \
        CHECK(memcmp(&r.ctx, &ctx_before, sizeof r.ctx) == 0);                                   \
    } while (0)

    o.wrap_u = (PIXELICA_WrapMode)-1;
    EXPECT_REJECT();
    o = pixelica_draw_options_default();
    o.wrap_v = (PIXELICA_WrapMode)2;
    EXPECT_REJECT();
    o = pixelica_draw_options_default();
    o.alpha_mode = (PIXELICA_AlphaMode)-1;
    EXPECT_REJECT();
    o.alpha_mode = (PIXELICA_AlphaMode)3;
    EXPECT_REJECT();

    o = pixelica_draw_options_default();
    o.alpha_mode = PIXELICA_ALPHA_TEST;
    o.texture_alpha = map;
    o.texture_alpha_bytes = 5u; /* < 3*2 */
    EXPECT_REJECT();
    o.texture_alpha = NULL;
    o.texture_alpha_bytes = 6u;
    EXPECT_REJECT();

    /* alpha plane aliasing the color or depth buffer */
    o.texture_alpha = (const uint8_t *)r.color;
    o.texture_alpha_bytes = sizeof r.color;
    EXPECT_REJECT();
    o.texture_alpha = (const uint8_t *)r.depth;
    o.texture_alpha_bytes = sizeof r.depth;
    EXPECT_REJECT();

    /* non-positive source size, invalid sprite fields */
    o = pixelica_draw_options_default();
    s.source.width = 0;
    EXPECT_REJECT();
    s = sprite(0, 0, 3, 2);
    s.depth_test = true;
    s.depth = UINT16_MAX;
    EXPECT_REJECT();
    CHECK(pixelica2d_draw_sprite_ex(NULL, &s, &o) == PIXELICA_ERR_INVALID_ARG);
#undef EXPECT_REJECT
}

static void test_extreme(void)
{
    Rig r;
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    PIXELICA2D_Sprite s = sprite(INT32_MAX - 1, INT32_MAX - 1, INT32_MAX, INT32_MAX);

    o.wrap_u = PIXELICA_WRAP_REPEAT;
    o.wrap_v = PIXELICA_WRAP_REPEAT;
    rig_init(&r, 0u);
    s.position.x = -3.0e38f;
    s.position.y = 1.0e30f;
    s.scale.x = 3.0e38f;
    s.scale.y = -1.0e-30f;
    s.pivot.x = 1.0e30f;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    s.rotation = 1.0f;
    CHECK(pixelica2d_draw_sprite_ex(&r.ctx, &s, &o) == PIXELICA_OK);
    CHECK(r.ctx.stats.draw_calls == 2u);
}

int main(void)
{
    test_defaults_match_legacy();
    test_repeat_and_clamp();
    test_alpha_test();
    test_alpha_blend();
    test_invalid();
    test_extreme();
    printf("texture_options2d: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

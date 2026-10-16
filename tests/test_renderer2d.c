/*
 * Pixelica 2D renderer tests. Standalone: includes only the 2D header (and
 * through it the common headers), never pixelica.h, so it builds in a 2D-only
 * tree. CHECK never uses assert(), so every check runs in Release.
 *
 * Expected pixels are written out by hand from the documented math; the
 * surfaces carry canary values in their row padding and tail so any write
 * outside the visible area is caught.
 */
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pixelica/renderer2d.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(cond)) {                                                                     \
            ++g_failures;                                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        long long t2d_a_ = (long long)(actual);                                            \
        long long t2d_e_ = (long long)(expected);                                          \
        ++g_checks;                                                                        \
        if (t2d_a_ != t2d_e_) {                                                            \
            ++g_failures;                                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s == %s (%lld vs %lld)\n", __FILE__,     \
                    __LINE__, #actual, #expected, t2d_a_, t2d_e_);                         \
        }                                                                                  \
    } while (0)

#define BG 0x1111u
#define CANARY 0xCAFEu
#define RED 0xF800u
#define GRN 0x07E0u
#define BLU 0x001Fu
#define WHT 0xFFFFu
#define KEY 0xF81Fu
#define C_E 0x2222u
#define C_F 0x3333u
#define C_G 0x4444u
#define C_H 0x5555u
#define FX_CAP 256u

static float t2d_inf(void)
{
    volatile float zero = 0.0f;
    return 1.0f / zero;
}

static float t2d_nan(void)
{
    volatile float zero = 0.0f;
    return zero / zero;
}

/* ---- fixture: padded surface filled with canaries, then cleared ------- */

typedef struct Fixture {
    uint16_t color[FX_CAP];
    uint16_t depth[FX_CAP];
    PIXELICA2D_Context ctx;
    uint32_t w, h, stride;
    int has_depth;
} Fixture;

typedef struct Snapshot {
    uint16_t color[FX_CAP];
    uint16_t depth[FX_CAP];
    unsigned char ctx[sizeof(PIXELICA2D_Context)];
} Snapshot;

static void fx_init(Fixture *f, uint16_t w, uint16_t h, uint32_t stride, int with_depth)
{
    PIXELICA2D_Surface s;
    size_t i;

    for (i = 0; i < FX_CAP; ++i) {
        f->color[i] = CANARY;
        f->depth[i] = CANARY;
    }
    f->w = w;
    f->h = h;
    f->stride = stride;
    f->has_depth = with_depth;
    s.pixels = f->color;
    s.width = w;
    s.height = h;
    s.stride = stride;
    s.depth = with_depth ? f->depth : NULL;
    s.depth_stride = stride;
    CHECK(pixelica2d_init(&f->ctx, &s) == PIXELICA_OK);
    CHECK(pixelica2d_clear(&f->ctx, (uint16_t)BG) == PIXELICA_OK);
}

static uint16_t fx_px(const Fixture *f, uint32_t x, uint32_t y)
{
    return f->color[(size_t)y * f->stride + x];
}

static uint16_t fx_dp(const Fixture *f, uint32_t x, uint32_t y)
{
    return f->depth[(size_t)y * f->stride + x];
}

/* Every non-visible element (row padding, tail, unused depth) is a canary. */
static int fx_padding_ok(const Fixture *f)
{
    size_t i;

    for (i = 0; i < FX_CAP; ++i) {
        int visible = (i % f->stride) < f->w && (i / f->stride) < f->h;

        if ((!visible && f->color[i] != CANARY) || ((!visible || !f->has_depth) && f->depth[i] != CANARY)) {
            fprintf(stderr, "padding element %u overwritten\n", (unsigned)i);
            return 0;
        }
    }
    return 1;
}

/* Visible color equals the w*h row-major reference and padding is intact. */
static int fx_matches(const Fixture *f, const uint16_t *expected)
{
    uint32_t x, y;

    for (y = 0; y < f->h; ++y) {
        for (x = 0; x < f->w; ++x) {
            if (fx_px(f, x, y) != expected[y * f->w + x]) {
                fprintf(stderr, "pixel (%u,%u) = 0x%04X, expected 0x%04X\n", (unsigned)x, (unsigned)y,
                        (unsigned)fx_px(f, x, y), (unsigned)expected[y * f->w + x]);
                return 0;
            }
        }
    }
    return fx_padding_ok(f);
}

static void fx_snap(const Fixture *f, Snapshot *s)
{
    memcpy(s->color, f->color, sizeof s->color);
    memcpy(s->depth, f->depth, sizeof s->depth);
    memcpy(s->ctx, &f->ctx, sizeof s->ctx);
}

static int fx_unchanged(const Fixture *f, const Snapshot *s)
{
    return memcmp(s->color, f->color, sizeof s->color) == 0 && memcmp(s->depth, f->depth, sizeof s->depth) == 0 &&
           memcmp(s->ctx, &f->ctx, sizeof s->ctx) == 0;
}

static PIXELICA_Texture make_tex(const uint16_t *pixels, uint16_t w, uint16_t h)
{
    PIXELICA_Texture t;

    t.pixels = pixels;
    t.width = w;
    t.height = h;
    return t;
}

static PIXELICA2D_Sprite make_sprite(const PIXELICA_Texture *tex, float x, float y)
{
    PIXELICA2D_Sprite s;

    memset(&s, 0, sizeof s);
    CHECK(pixelica2d_sprite_init(&s, tex) == PIXELICA_OK);
    s.position.x = x;
    s.position.y = y;
    return s;
}

/* 2x2: RED GRN / BLU WHT */
static const uint16_t k_tex2[4] = { RED, GRN, BLU, WHT };

/* ---- context ------------------------------------------------------------- */

static void test_init_and_setters(void)
{
    uint16_t buf[18], dbuf[18];
    union {
        PIXELICA2D_Context ctx;
        uint16_t px[128];
    } inside;
    PIXELICA2D_Surface s, bad;
    PIXELICA2D_Context ctx, before;
    PIXELICA2D_Camera cam;
    PIXELICA2D_Rect r;
    size_t i, untouched = 0;

    for (i = 0; i < 18u; ++i) {
        buf[i] = 0x1234u;
    }
    s.pixels = buf;
    s.width = 4;
    s.height = 3;
    s.stride = 6;
    s.depth = NULL;
    s.depth_stride = 0;
    memset(&ctx, 0xAB, sizeof ctx);
    CHECK(pixelica2d_init(&ctx, &s) == PIXELICA_OK);
    CHECK(ctx.surface.pixels == buf && ctx.surface.stride == 6u && ctx.surface.depth == NULL);
    CHECK(ctx.camera.position.x == 0.0f && ctx.camera.position.y == 0.0f && ctx.camera.zoom == 1.0f);
    CHECK(ctx.clip.x == 0 && ctx.clip.y == 0 && ctx.clip.width == 4 && ctx.clip.height == 3);
    CHECK(ctx.stats.draw_calls == 0u && ctx.stats.pixels_written == 0u);
    for (i = 0; i < 18u; ++i) {
        if (buf[i] == 0x1234u) {
            ++untouched;
        }
    }
    CHECK_EQ(untouched, 18); /* init never clears */

    memcpy(&before, &ctx, sizeof ctx);
    CHECK(pixelica2d_init(NULL, &s) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_init(&ctx, NULL) == PIXELICA_ERR_INVALID_ARG);
    bad = s;
    bad.pixels = NULL;
    CHECK(pixelica2d_init(&ctx, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = s;
    bad.width = 0;
    CHECK(pixelica2d_init(&ctx, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = s;
    bad.height = 4097;
    bad.stride = 4097;
    CHECK(pixelica2d_init(&ctx, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = s;
    bad.stride = 3; /* < width */
    CHECK(pixelica2d_init(&ctx, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = s;
    bad.depth = buf + 2; /* overlaps color */
    bad.depth_stride = 6;
    CHECK(pixelica2d_init(&ctx, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = s;
    bad.depth = dbuf;
    bad.depth_stride = 3; /* < width */
    CHECK(pixelica2d_init(&ctx, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = s;
    bad.pixels = (uint16_t *)(uintptr_t)(UINTPTR_MAX - 15u); /* 16 bytes left, 32 needed */
    bad.width = 16;
    bad.height = 1;
    bad.stride = 16;
    CHECK(pixelica2d_init(&ctx, &bad) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&ctx, &before, sizeof ctx) == 0);

    /* The context may not live inside its own color buffer. */
    bad = s;
    bad.pixels = inside.px;
    bad.width = 128;
    bad.height = 1;
    bad.stride = 128;
    CHECK(pixelica2d_init(&inside.ctx, &bad) == PIXELICA_ERR_INVALID_ARG);

    cam.position.x = 3.0f;
    cam.position.y = -2.0f;
    cam.zoom = 2.0f;
    CHECK(pixelica2d_set_camera(&ctx, &cam) == PIXELICA_OK);
    CHECK(ctx.camera.position.x == 3.0f && ctx.camera.position.y == -2.0f && ctx.camera.zoom == 2.0f);
    cam.zoom = 0.0f;
    CHECK(pixelica2d_set_camera(&ctx, &cam) == PIXELICA_ERR_INVALID_ARG);
    cam.zoom = -1.0f;
    CHECK(pixelica2d_set_camera(&ctx, &cam) == PIXELICA_ERR_INVALID_ARG);
    cam.zoom = t2d_inf();
    CHECK(pixelica2d_set_camera(&ctx, &cam) == PIXELICA_ERR_INVALID_ARG);
    cam.zoom = 1.0f;
    cam.position.y = t2d_nan();
    CHECK(pixelica2d_set_camera(&ctx, &cam) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_set_camera(NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(ctx.camera.position.x == 3.0f && ctx.camera.zoom == 2.0f);
    CHECK(pixelica2d_set_camera(&ctx, NULL) == PIXELICA_OK);
    CHECK(ctx.camera.position.x == 0.0f && ctx.camera.position.y == 0.0f && ctx.camera.zoom == 1.0f);

    r.x = -2;
    r.y = 1;
    r.width = 4;
    r.height = 10;
    CHECK(pixelica2d_set_clip(&ctx, &r) == PIXELICA_OK);
    CHECK(ctx.clip.x == 0 && ctx.clip.y == 1 && ctx.clip.width == 2 && ctx.clip.height == 2);
    r.x = r.y = r.width = r.height = INT32_MAX;
    CHECK(pixelica2d_set_clip(&ctx, &r) == PIXELICA_OK);
    CHECK(ctx.clip.x == 4 && ctx.clip.y == 3 && ctx.clip.width == 0 && ctx.clip.height == 0);
    r.x = r.y = INT32_MIN;
    r.width = r.height = INT32_MAX; /* ends at -1: empty */
    CHECK(pixelica2d_set_clip(&ctx, &r) == PIXELICA_OK);
    CHECK(ctx.clip.x == 0 && ctx.clip.y == 0 && ctx.clip.width == 0 && ctx.clip.height == 0);
    r.x = 1;
    r.y = 1;
    r.width = -1;
    r.height = 1;
    CHECK(pixelica2d_set_clip(&ctx, &r) == PIXELICA_ERR_INVALID_ARG);
    CHECK(ctx.clip.width == 0 && ctx.clip.height == 0);
    CHECK(pixelica2d_set_clip(&ctx, NULL) == PIXELICA_OK);
    CHECK(ctx.clip.x == 0 && ctx.clip.y == 0 && ctx.clip.width == 4 && ctx.clip.height == 3);

    /* Directly corrupted public fields are revalidated by every call. */
    ctx.camera.zoom = t2d_nan();
    CHECK(pixelica2d_clear(&ctx, 0) == PIXELICA_ERR_INVALID_ARG);
    CHECK(buf[0] == 0x1234u);
    CHECK(pixelica2d_set_camera(&ctx, NULL) == PIXELICA_OK);
    ctx.clip.width = 5; /* wider than the surface */
    CHECK(pixelica2d_clear(&ctx, 0) == PIXELICA_ERR_INVALID_ARG);
    CHECK(buf[0] == 0x1234u);
}

static void test_clear_padding(void)
{
    Fixture f;

    fx_init(&f, 3, 2, 5, 1);
    f.ctx.stats.draw_calls = 7u;
    f.ctx.stats.pixels_written = 9u;
    CHECK(pixelica2d_clear(&f.ctx, 0x00F0u) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == 0u && f.ctx.stats.pixels_written == 0u);
    CHECK(fx_px(&f, 0, 0) == 0x00F0u && fx_px(&f, 2, 1) == 0x00F0u);
    CHECK(fx_dp(&f, 0, 0) == PIXELICA2D_DEPTH_CLEAR && fx_dp(&f, 2, 1) == PIXELICA2D_DEPTH_CLEAR);
    CHECK(f.color[3] == CANARY && f.color[4] == CANARY && f.color[8] == CANARY);
    CHECK(f.depth[3] == CANARY && f.depth[9] == CANARY);
    CHECK(fx_padding_ok(&f));
}

/* ---- sprites ------------------------------------------------------------- */

static void test_sprite_transforms(void)
{
    static const uint16_t tex21[2] = { RED, GRN };
    static const uint16_t plain[16] = { BG, BG, BG, BG, BG, RED, GRN, BG, BG, BLU, WHT, BG, BG, BG, BG, BG };
    static const uint16_t flip_x[16] = { BG, BG, BG, BG, BG, GRN, RED, BG, BG, WHT, BLU, BG, BG, BG, BG, BG };
    static const uint16_t flip_y[16] = { BG, BG, BG, BG, BG, BLU, WHT, BG, BG, RED, GRN, BG, BG, BG, BG, BG };
    static const uint16_t crop[16] = { GRN, BG, BG, BG, WHT, BG, BG, BG, BG, BG, BG, BG, BG, BG, BG, BG };
    static const uint16_t scale2[16] = { RED, RED, GRN, GRN, RED, RED, GRN, GRN,
                                         BLU, BLU, WHT, WHT, BLU, BLU, WHT, WHT };
    static const uint16_t camera[16] = { BG, BG, BG, BG, BG, BG, BG, BG, RED, RED, GRN, GRN, RED, RED, GRN, GRN };
    static const uint16_t rot90[16] = { BG, BG, BG, BG, BG, RED, BG, BG, BG, GRN, BG, BG, BG, BG, BG, BG };
    static const uint16_t rot180[16] = { BG, BG, BG, BG, BG, WHT, BLU, BG, BG, GRN, RED, BG, BG, BG, BG, BG };
    PIXELICA_Texture tex = make_tex(k_tex2, 2, 2), strip = make_tex(tex21, 2, 1);
    PIXELICA2D_Camera cam;
    PIXELICA2D_Sprite s;
    Fixture f;

    fx_init(&f, 4, 4, 6, 0);
    s = make_sprite(&tex, 1.0f, 1.0f);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, plain));
    CHECK(f.ctx.stats.draw_calls == 1u && f.ctx.stats.pixels_written == 4u);

    /* Negative scale mirrors around the pivot; pivot at the far edge keeps
     * the footprint in place. */
    fx_init(&f, 4, 4, 6, 0);
    s.scale.x = -1.0f;
    s.pivot.x = 2.0f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, flip_x));

    fx_init(&f, 4, 4, 6, 0);
    s = make_sprite(&tex, 1.0f, 1.0f);
    s.scale.y = -1.0f;
    s.pivot.y = 2.0f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, flip_y));

    fx_init(&f, 4, 4, 6, 0);
    s = make_sprite(&tex, 0.0f, 0.0f);
    s.source.x = 1;
    s.source.width = 1;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, crop));
    CHECK(f.ctx.stats.pixels_written == 2u);

    fx_init(&f, 4, 4, 6, 0);
    s = make_sprite(&tex, 0.0f, 0.0f);
    s.scale.x = s.scale.y = 2.0f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, scale2));
    CHECK(f.ctx.stats.pixels_written == 16u);

    /* World [1,3)x[1,3) seen from (1,0) at zoom 2 -> screen [0,4)x[2,6). */
    fx_init(&f, 4, 4, 6, 0);
    cam.position.x = 1.0f;
    cam.position.y = 0.0f;
    cam.zoom = 2.0f;
    CHECK(pixelica2d_set_camera(&f.ctx, &cam) == PIXELICA_OK);
    s = make_sprite(&tex, 1.0f, 1.0f);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, camera));
    CHECK(f.ctx.stats.pixels_written == 8u);

    /* +pi/2 is clockwise on a y-down screen: a horizontal strip points down. */
    fx_init(&f, 4, 4, 6, 0);
    s = make_sprite(&strip, 2.0f, 1.0f);
    s.rotation = 1.57079632679489662f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, rot90));
    CHECK(f.ctx.stats.pixels_written == 2u);

    fx_init(&f, 4, 4, 6, 0);
    s = make_sprite(&tex, 2.0f, 2.0f);
    s.pivot.x = s.pivot.y = 1.0f;
    s.rotation = 3.14159265358979324f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, rot180));
}

static void test_sprite_extremes(void)
{
    static const uint16_t at_origin[16] = { RED, GRN, BG, BG, BLU, WHT, BG, BG, BG, BG, BG, BG, BG, BG, BG, BG };
    PIXELICA_Texture tex = make_tex(k_tex2, 2, 2);
    PIXELICA2D_Camera cam;
    PIXELICA2D_Sprite s;
    Fixture f;
    uint32_t x, y, n;

    /* Equal huge camera and position cancel exactly: drawn at the origin. */
    fx_init(&f, 4, 4, 6, 0);
    cam.position.x = cam.position.y = FLT_MAX;
    cam.zoom = 1.0f;
    CHECK(pixelica2d_set_camera(&f.ctx, &cam) == PIXELICA_OK);
    s = make_sprite(&tex, FLT_MAX, FLT_MAX);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, at_origin));

    /* Opposite extremes: 2*FLT_MAX away, nothing drawn, still one draw. */
    fx_init(&f, 4, 4, 6, 0);
    cam.position.y = 0.0f;
    CHECK(pixelica2d_set_camera(&f.ctx, &cam) == PIXELICA_OK);
    s = make_sprite(&tex, -FLT_MAX, 0.0f);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == 1u && f.ctx.stats.pixels_written == 0u);
    CHECK(fx_padding_ok(&f));

    /* FLT_MAX scale around pivot (1,1): every visible pixel maps to texel
     * (1,1) = WHT. */
    fx_init(&f, 4, 4, 6, 0);
    s = make_sprite(&tex, 0.0f, 0.0f);
    s.scale.x = s.scale.y = FLT_MAX;
    s.pivot.x = s.pivot.y = 1.0f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    for (n = 0, y = 0; y < 4u; ++y) {
        for (x = 0; x < 4u; ++x) {
            if (fx_px(&f, x, y) == WHT) {
                ++n;
            }
        }
    }
    CHECK_EQ(n, 16);
    CHECK(fx_padding_ok(&f));

    /* Tiny scale covers no pixel center. */
    fx_init(&f, 4, 4, 6, 0);
    s = make_sprite(&tex, 1.0f, 1.0f);
    s.scale.x = s.scale.y = 1e-30f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(f.ctx.stats.pixels_written == 0u && fx_padding_ok(&f));

    /* FLT_MAX zoom: the first texel fills the screen. */
    fx_init(&f, 4, 4, 6, 0);
    cam.position.x = cam.position.y = 0.0f;
    cam.zoom = FLT_MAX;
    CHECK(pixelica2d_set_camera(&f.ctx, &cam) == PIXELICA_OK);
    s = make_sprite(&tex, 0.0f, 0.0f);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(f.ctx.stats.pixels_written == 16u && fx_px(&f, 3, 3) == RED && fx_padding_ok(&f));

    /* Rotated with enormous and tiny values: must stay in bounds. */
    fx_init(&f, 4, 4, 6, 0);
    cam.zoom = FLT_MIN;
    CHECK(pixelica2d_set_camera(&f.ctx, &cam) == PIXELICA_OK);
    s = make_sprite(&tex, 1e30f, -1e30f);
    s.rotation = 1e30f;
    s.pivot.x = FLT_MAX;
    s.pivot.y = -FLT_MAX;
    s.scale.x = FLT_MAX;
    s.scale.y = -FLT_MIN;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    cam.zoom = FLT_MAX;
    CHECK(pixelica2d_set_camera(&f.ctx, &cam) == PIXELICA_OK);
    s = make_sprite(&tex, 0.0f, 0.0f);
    s.rotation = 0.5f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    s.rotation = -FLT_MAX;
    s.scale.x = -FLT_MAX;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == 3u && fx_padding_ok(&f));

    /* Stats saturate instead of wrapping. */
    fx_init(&f, 4, 4, 6, 0);
    f.ctx.stats.draw_calls = UINT32_MAX;
    f.ctx.stats.pixels_written = UINT32_MAX - 1u;
    s = make_sprite(&tex, 0.0f, 0.0f);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == UINT32_MAX && f.ctx.stats.pixels_written == UINT32_MAX);
}

static void test_sprite_init_and_frames(void)
{
    uint16_t atlas_px[24];
    PIXELICA_Texture atlas, bad;
    PIXELICA2D_Sprite s, before;
    Fixture f;
    size_t i;

    for (i = 0; i < 24u; ++i) {
        atlas_px[i] = (uint16_t)(0x100u + i); /* 6x4, value = 0x100 + y*6 + x */
    }
    atlas = make_tex(atlas_px, 6, 4);
    memset(&s, 0x5A, sizeof s);
    CHECK(pixelica2d_sprite_init(&s, &atlas) == PIXELICA_OK);
    CHECK(s.texture == &atlas && s.source.x == 0 && s.source.y == 0 && s.source.width == 6 &&
          s.source.height == 4);
    CHECK(s.position.x == 0.0f && s.position.y == 0.0f && s.scale.x == 1.0f && s.scale.y == 1.0f);
    CHECK(s.pivot.x == 0.0f && s.pivot.y == 0.0f && s.rotation == 0.0f && s.depth == 0u);
    CHECK(s.opacity == 255u && !s.color_key_enabled && !s.depth_test && !s.depth_write);

    memcpy(&before, &s, sizeof s);
    CHECK(pixelica2d_sprite_init(&s, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_sprite_init(NULL, &atlas) == PIXELICA_ERR_INVALID_ARG);
    bad = make_tex(NULL, 2, 2);
    CHECK(pixelica2d_sprite_init(&s, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = make_tex(atlas_px, 0, 2);
    CHECK(pixelica2d_sprite_init(&s, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = make_tex(atlas_px, 2, 4097);
    CHECK(pixelica2d_sprite_init(&s, &bad) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&s, &before, sizeof s) == 0);

    /* 6x4 atlas of 2x2 cells: 3 columns, 2 rows. */
    CHECK(pixelica2d_sprite_frame(&s, 4u, 2, 2) == PIXELICA_OK);
    CHECK(s.source.x == 2 && s.source.y == 2 && s.source.width == 2 && s.source.height == 2);
    CHECK(pixelica2d_sprite_frame(&s, 5u, 2, 2) == PIXELICA_OK);
    CHECK(s.source.x == 4 && s.source.y == 2);
    CHECK(pixelica2d_sprite_frame(&s, 1u, 3, 4) == PIXELICA_OK);
    CHECK(s.source.x == 3 && s.source.y == 0 && s.source.width == 3 && s.source.height == 4);
    memcpy(&before, &s, sizeof s);
    CHECK(pixelica2d_sprite_frame(&s, 6u, 2, 2) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_sprite_frame(&s, 2u, 3, 4) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_sprite_frame(&s, 0u, 4, 2) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_sprite_frame(&s, 0u, 0, 2) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_sprite_frame(&s, UINT32_MAX, 1, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_sprite_frame(NULL, 0u, 2, 2) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&s, &before, sizeof s) == 0);

    fx_init(&f, 4, 4, 6, 0);
    CHECK(pixelica2d_sprite_frame(&s, 4u, 2, 2) == PIXELICA_OK);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 0) == 0x100u + 14u && fx_px(&f, 1, 0) == 0x100u + 15u);
    CHECK(fx_px(&f, 0, 1) == 0x100u + 20u && fx_px(&f, 1, 1) == 0x100u + 21u);
    CHECK(fx_px(&f, 2, 0) == BG && fx_px(&f, 0, 2) == BG && fx_padding_ok(&f));
}

static void test_sprite_invalid(void)
{
    PIXELICA_Texture tex = make_tex(k_tex2, 2, 2), inside;
    PIXELICA2D_Sprite good, s;
    Snapshot snap;
    Fixture f, nodepth;

    fx_init(&f, 4, 4, 6, 1);
    good = make_sprite(&tex, 1.0f, 1.0f);
    fx_snap(&f, &snap);

    CHECK(pixelica2d_draw_sprite(NULL, &good) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_sprite(&f.ctx, NULL) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.texture = NULL;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.source.x = -1;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.source.width = 0;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.source.x = 1; /* 1 + 2 > 2 */
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.source.x = INT32_MAX;
    s.source.width = INT32_MAX;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.position.x = t2d_nan();
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.scale.x = 0.0f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.scale.y = t2d_inf();
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.pivot.y = -t2d_inf();
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.rotation = t2d_nan();
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    s = good;
    s.depth_test = true;
    s.depth = PIXELICA2D_DEPTH_CLEAR; /* 65535 is reserved for "cleared" */
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    /* Texels inside the destination color buffer. */
    inside = make_tex(f.color, 2, 2);
    s = good;
    s.texture = &inside;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));

    fx_init(&nodepth, 4, 4, 6, 0);
    fx_snap(&nodepth, &snap);
    s = good;
    s.depth_write = true;
    CHECK(pixelica2d_draw_sprite(&nodepth.ctx, &s) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&nodepth, &snap));
}

/* ---- blending, color key and depth ---------------------------------------- */

static void test_alpha_key_depth(void)
{
    static const uint16_t cyan[1] = { 0x07FFu };
    static const uint16_t keyed_red[2] = { KEY, RED };
    static const uint16_t keyed_blue[2] = { KEY, BLU };
    static const uint16_t green2[2] = { GRN, GRN };
    static const uint16_t white2[2] = { WHT, WHT };
    PIXELICA_Texture t_cyan = make_tex(cyan, 1, 1), t_kr = make_tex(keyed_red, 2, 1), t_kb = make_tex(keyed_blue, 2, 1);
    PIXELICA_Texture t_g = make_tex(green2, 2, 1), t_w = make_tex(white2, 2, 1);
    PIXELICA2D_Rect all = { 0, 0, 4, 2 }, one = { 1, 0, 1, 1 };
    PIXELICA2D_Sprite s;
    Snapshot snap;
    Fixture f;

    /* fill_rect at 255 is an exact copy; cyan over red at 128:
     * r (0*128+31*127+127)/255=15, g (63*128+0+127)/255=32,
     * b (31*128+0+127)/255=16 -> 0x7C10. */
    fx_init(&f, 4, 2, 5, 1);
    CHECK(pixelica2d_fill_rect(&f.ctx, &all, (uint16_t)RED, 255u) == PIXELICA_OK);
    CHECK(fx_px(&f, 3, 1) == RED && f.ctx.stats.pixels_written == 8u);
    s = make_sprite(&t_cyan, 0.0f, 0.0f);
    s.opacity = 128u;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 0) == 0x7C10u && fx_px(&f, 1, 0) == RED);
    CHECK(pixelica2d_fill_rect(&f.ctx, &one, 0x07FFu, 128u) == PIXELICA_OK);
    CHECK(fx_px(&f, 1, 0) == 0x7C10u);

    /* +127 rounding: white over black at 4 -> g (63*4+127)/255=1, r/b 0. */
    CHECK(pixelica2d_clear(&f.ctx, 0u) == PIXELICA_OK);
    CHECK(pixelica2d_fill_rect(&f.ctx, &one, (uint16_t)WHT, 4u) == PIXELICA_OK);
    CHECK(fx_px(&f, 1, 0) == 0x0020u && fx_px(&f, 0, 0) == 0u);

    /* Keyed texel: no color and no depth write. */
    CHECK(pixelica2d_clear(&f.ctx, (uint16_t)BG) == PIXELICA_OK);
    s = make_sprite(&t_kr, 0.0f, 0.0f);
    s.color_key_enabled = true;
    s.color_key = KEY;
    s.depth_test = s.depth_write = true;
    s.depth = 100u;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 0) == BG && fx_dp(&f, 0, 0) == PIXELICA2D_DEPTH_CLEAR);
    CHECK(fx_px(&f, 1, 0) == RED && fx_dp(&f, 1, 0) == 100u);
    CHECK(f.ctx.stats.pixels_written == 1u);

    /* Strict LESS: equal depth fails; test without write keeps depth. */
    s = make_sprite(&t_g, 0.0f, 0.0f);
    s.depth_test = true;
    s.depth = 100u;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 0) == GRN && fx_dp(&f, 0, 0) == PIXELICA2D_DEPTH_CLEAR);
    CHECK(fx_px(&f, 1, 0) == RED && fx_dp(&f, 1, 0) == 100u);
    s.depth = 99u;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_px(&f, 1, 0) == GRN && fx_dp(&f, 1, 0) == 100u);

    /* Translucent keyed sprite writes depth only where it shaded.
     * BLU over BG(0x1111 = r2 g8 b17) at 128: r (2*127+127)/255=1,
     * g (8*127+127)/255=4, b (31*128+17*127+127)/255=24 -> 0x0898. */
    s = make_sprite(&t_kb, 0.0f, 1.0f);
    s.opacity = 128u;
    s.color_key_enabled = true;
    s.color_key = KEY;
    s.depth_test = s.depth_write = true;
    s.depth = 50u;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 1) == BG && fx_dp(&f, 0, 1) == PIXELICA2D_DEPTH_CLEAR);
    CHECK(fx_px(&f, 1, 1) == 0x0898u && fx_dp(&f, 1, 1) == 50u);

    /* Opacity 0 writes nothing, not even depth, but counts one draw. */
    fx_snap(&f, &snap);
    s = make_sprite(&t_w, 0.0f, 0.0f);
    s.opacity = 0u;
    s.depth_write = true;
    s.depth = 0u;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(memcmp(snap.color, f.color, sizeof snap.color) == 0 && memcmp(snap.depth, f.depth, sizeof snap.depth) == 0);
    CHECK(f.ctx.stats.draw_calls == 5u);

    /* Write without test overwrites any stored depth. */
    s.opacity = 255u;
    s.depth = 200u;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 0) == WHT && fx_px(&f, 1, 0) == WHT);
    CHECK(fx_dp(&f, 0, 0) == 200u && fx_dp(&f, 1, 0) == 200u && fx_dp(&f, 1, 1) == 50u);
    CHECK(fx_padding_ok(&f));
}

static void test_clipping(void)
{
    static const uint16_t white16[16] = { WHT, WHT, WHT, WHT, WHT, WHT, WHT, WHT,
                                          WHT, WHT, WHT, WHT, WHT, WHT, WHT, WHT };
    static const uint16_t clipped[16] = { BG, BG, BG, BG, BG, WHT, WHT, BG, BG, WHT, WHT, BG, BG, BG, BG, BG };
    static const uint16_t corner[16] = { RED, RED, BG, BG, RED, RED, BG, BG, BG, BG, BG, BG, BG, BG, BG, BG };
    PIXELICA_Texture tex = make_tex(white16, 4, 4);
    PIXELICA2D_Rect clip = { 1, 1, 2, 2 }, r;
    PIXELICA2D_Sprite s;
    Snapshot snap;
    Fixture f;

    fx_init(&f, 4, 4, 6, 0);
    CHECK(pixelica2d_set_clip(&f.ctx, &clip) == PIXELICA_OK);
    s = make_sprite(&tex, 0.0f, 0.0f);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(fx_matches(&f, clipped));
    CHECK(f.ctx.stats.pixels_written == 4u);
    s = make_sprite(&tex, -1.0f, -1.0f);
    s.scale.x = s.scale.y = 3.0f;
    s.rotation = 0.3f;
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    r.x = r.y = -100;
    r.width = r.height = 1000;
    CHECK(pixelica2d_fill_rect(&f.ctx, &r, (uint16_t)WHT, 255u) == PIXELICA_OK);
    CHECK(fx_matches(&f, clipped));

    /* Zero-size clip: accepted, draws nothing, still counts. */
    clip.width = clip.height = 0;
    CHECK(pixelica2d_set_clip(&f.ctx, &clip) == PIXELICA_OK);
    fx_snap(&f, &snap);
    CHECK(pixelica2d_draw_sprite(&f.ctx, &s) == PIXELICA_OK);
    CHECK(memcmp(snap.color, f.color, sizeof snap.color) == 0);
    CHECK(f.ctx.stats.draw_calls == 4u);

    /* fill_rect: screen space, clipped, overflow-safe extents. */
    fx_init(&f, 4, 4, 6, 0);
    r.x = r.y = -5;
    r.width = r.height = 7;
    CHECK(pixelica2d_fill_rect(&f.ctx, &r, (uint16_t)RED, 255u) == PIXELICA_OK);
    CHECK(fx_matches(&f, corner));
    CHECK(f.ctx.stats.pixels_written == 4u);
    r.x = r.y = r.width = r.height = INT32_MAX;
    CHECK(pixelica2d_fill_rect(&f.ctx, &r, (uint16_t)BLU, 255u) == PIXELICA_OK);
    r.x = r.y = INT32_MIN;
    CHECK(pixelica2d_fill_rect(&f.ctx, &r, (uint16_t)BLU, 255u) == PIXELICA_OK);
    r.width = 0;
    CHECK(pixelica2d_fill_rect(&f.ctx, &r, (uint16_t)BLU, 255u) == PIXELICA_OK);
    CHECK(fx_matches(&f, corner));
    CHECK(f.ctx.stats.draw_calls == 4u && f.ctx.stats.pixels_written == 4u);
    fx_snap(&f, &snap);
    r.width = -1;
    CHECK(pixelica2d_fill_rect(&f.ctx, &r, (uint16_t)BLU, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_fill_rect(&f.ctx, NULL, (uint16_t)BLU, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));
}

/* ---- lists --------------------------------------------------------------- */

static void test_draw_list(void)
{
    static const uint16_t c_red[1] = { RED }, c_grn[1] = { GRN }, c_blu[1] = { BLU }, c_wht[1] = { WHT };
    static const uint16_t expect[4] = { RED, BLU, GRN, BG };
    PIXELICA_Texture tr = make_tex(c_red, 1, 1), tg = make_tex(c_grn, 1, 1);
    PIXELICA_Texture tb = make_tex(c_blu, 1, 1), tw = make_tex(c_wht, 1, 1), inside;
    union {
        PIXELICA2D_Item items[6];
        size_t raw[1];
    } shared;
    PIXELICA2D_Item items[6], bad[6];
    size_t order[6], i, order_before[6];
    Snapshot snap;
    Fixture f;

    /* item: texture, x, layer, sort_y */
    items[0].sprite = make_sprite(&tr, 0.0f, 0.0f);
    items[0].layer = 1;
    items[0].sort_y = 0.0f;
    items[1].sprite = make_sprite(&tg, 1.0f, 0.0f);
    items[1].layer = 0;
    items[1].sort_y = 5.0f;
    items[2].sprite = make_sprite(&tb, 1.0f, 0.0f); /* ties item 1: later index wins */
    items[2].layer = 0;
    items[2].sort_y = 5.0f;
    items[3].sprite = make_sprite(&tw, 0.0f, 0.0f); /* under item 0 (lower layer) */
    items[3].layer = 0;
    items[3].sort_y = -1.0f;
    items[4].sprite = make_sprite(&tg, 2.0f, 0.0f);
    items[4].layer = 0;
    items[4].sort_y = 2.0f;
    items[5].sprite = make_sprite(&tb, 2.0f, 0.0f); /* lowest layer despite large y */
    items[5].layer = -3;
    items[5].sort_y = 100.0f;

    fx_init(&f, 4, 1, 5, 0);
    CHECK(pixelica2d_draw_list(&f.ctx, items, 6, order, 6) == PIXELICA_OK);
    CHECK(order[0] == 5u && order[1] == 3u && order[2] == 4u && order[3] == 1u && order[4] == 2u &&
          order[5] == 0u);
    CHECK(fx_matches(&f, expect));
    CHECK(f.ctx.stats.draw_calls == 6u && f.ctx.stats.pixels_written == 6u);

    /* Failure transaction: an invalid LAST item rejects everything. */
    fx_init(&f, 4, 1, 5, 0);
    for (i = 0; i < 6u; ++i) {
        order[i] = 77u;
    }
    memcpy(order_before, order, sizeof order);
    fx_snap(&f, &snap);
    memcpy(bad, items, sizeof bad);
    bad[5].sort_y = t2d_nan();
    CHECK(pixelica2d_draw_list(&f.ctx, bad, 6, order, 6) == PIXELICA_ERR_INVALID_ARG);
    memcpy(bad, items, sizeof bad);
    bad[5].sprite.texture = NULL;
    CHECK(pixelica2d_draw_list(&f.ctx, bad, 6, order, 6) == PIXELICA_ERR_INVALID_ARG);
    memcpy(bad, items, sizeof bad);
    bad[5].sprite.position.x = t2d_inf();
    CHECK(pixelica2d_draw_list(&f.ctx, bad, 6, order, 6) == PIXELICA_ERR_INVALID_ARG);
    inside = make_tex(f.color, 1, 1);
    memcpy(bad, items, sizeof bad);
    bad[5].sprite.texture = &inside;
    CHECK(pixelica2d_draw_list(&f.ctx, bad, 6, order, 6) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_list(&f.ctx, items, 6, order, 5) == PIXELICA_ERR_NO_MEMORY);
    CHECK(pixelica2d_draw_list(&f.ctx, NULL, 1, order, 6) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_list(&f.ctx, items, 1, NULL, 6) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_list(&f.ctx, items, (size_t)PIXELICA2D_MAX_ITEMS + 1u, order, 6) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_list(NULL, items, 6, order, 6) == PIXELICA_ERR_INVALID_ARG);
    /* Scratch overlapping the items or the context. */
    memcpy(shared.items, items, sizeof items);
    CHECK(pixelica2d_draw_list(&f.ctx, shared.items, 6, shared.raw, 6) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(shared.items, items, sizeof items) == 0);
    CHECK(pixelica2d_draw_list(&f.ctx, items, 1, (size_t *)(void *)&f.ctx, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));
    CHECK(memcmp(order, order_before, sizeof order) == 0);

    /* count 0: NULL inputs allowed, nothing touched. */
    CHECK(pixelica2d_draw_list(&f.ctx, NULL, 0, NULL, 0) == PIXELICA_OK);
    CHECK(fx_unchanged(&f, &snap));
}

/* ---- tilemaps ------------------------------------------------------------ */

static uint16_t g_big_map[1024u * 1024u];

static void test_tilemap(void)
{
    /* 4x2 atlas, 2x2 tiles: tile 0 = RED GRN / BLU WHT, tile 1 = E F / G H. */
    static const uint16_t atlas_px[8] = { RED, GRN, C_E, C_F, BLU, WHT, C_G, C_H };
    /* 2x2 cells, stride 3; the padding column holds an invalid id (9). */
    static const uint16_t cells[6] = { 1, 0, 9, PIXELICA2D_EMPTY_TILE, 1, 9 };
    static const uint16_t bad_last[6] = { 1, 0, 9, PIXELICA2D_EMPTY_TILE, 2, 9 };
    static const uint16_t expect[30] = {
        BG, C_E, C_F, RED, GRN, BG,
        BG, C_G, C_H, BLU, WHT, BG,
        BG, BG,  BG,  C_E, C_F, BG,
        BG, BG,  BG,  C_G, C_H, BG,
        BG, BG,  BG,  BG,  BG,  BG,
    };
    static const uint16_t scaled[30] = {
        RED, GRN, GRN, BG, BG, BG,
        RED, GRN, GRN, BG, BG, BG,
        BLU, WHT, WHT, BG, BG, BG,
        BLU, WHT, WHT, BG, BG, BG,
        BG,  BG,  BG,  BG, BG, BG,
    };
    static const uint16_t one_white[1] = { WHT };
    static const uint16_t tile0[1] = { 0 };
    PIXELICA_Texture atlas = make_tex(atlas_px, 4, 2), white = make_tex(one_white, 1, 1);
    PIXELICA2D_Tilemap m, b;
    PIXELICA2D_Rect clip;
    PIXELICA2D_Camera cam;
    Snapshot snap;
    Fixture f;

    memset(&m, 0, sizeof m);
    m.atlas = &atlas;
    m.tiles = cells;
    m.cols = 2;
    m.rows = 2;
    m.tile_width = 2;
    m.tile_height = 2;
    m.stride_tiles = 3;
    m.position.x = 1.0f;
    m.position.y = 0.0f;
    m.scale = 1.0f;
    m.opacity = 255u;

    fx_init(&f, 6, 5, 7, 0);
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &m) == PIXELICA_OK);
    CHECK(fx_matches(&f, expect));
    CHECK(f.ctx.stats.draw_calls == 3u && f.ctx.stats.pixels_written == 12u);

    /* Color key applies per texel. */
    fx_init(&f, 6, 5, 7, 0);
    b = m;
    b.color_key_enabled = true;
    b.color_key = RED;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_OK);
    CHECK(fx_px(&f, 3, 0) == BG && fx_px(&f, 4, 0) == GRN && f.ctx.stats.pixels_written == 11u);

    /* Only cells inside the visible cell rectangle count as draws. */
    fx_init(&f, 6, 5, 7, 0);
    clip.x = 1;
    clip.y = 0;
    clip.width = clip.height = 2;
    CHECK(pixelica2d_set_clip(&f.ctx, &clip) == PIXELICA_OK);
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &m) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == 1u && f.ctx.stats.pixels_written == 4u);
    fx_init(&f, 6, 5, 7, 0);
    clip.x = 2;
    clip.y = 1;
    CHECK(pixelica2d_set_clip(&f.ctx, &clip) == PIXELICA_OK);
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &m) == PIXELICA_OK);
    CHECK(fx_px(&f, 2, 1) == C_H && fx_px(&f, 3, 1) == BLU && fx_px(&f, 2, 2) == BG && fx_px(&f, 3, 2) == C_E);
    CHECK(fx_px(&f, 1, 1) == BG && fx_px(&f, 4, 1) == BG && fx_px(&f, 3, 0) == BG && fx_padding_ok(&f));
    CHECK(f.ctx.stats.draw_calls == 3u && f.ctx.stats.pixels_written == 3u);

    /* An invalid id in an offscreen last cell still rejects the whole map. */
    fx_init(&f, 6, 5, 7, 0);
    clip.x = clip.y = 0;
    CHECK(pixelica2d_set_clip(&f.ctx, &clip) == PIXELICA_OK);
    fx_snap(&f, &snap);
    b = m;
    b.tiles = bad_last;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b = m;
    b.tile_width = 3; /* atlas width 4 not a multiple */
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b = m;
    b.scale = 0.0f;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b.scale = t2d_nan();
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b = m;
    b.stride_tiles = 1;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b = m;
    b.cols = 0;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b = m;
    b.tiles = NULL;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b = m;
    b.tiles = f.color; /* tiles inside the color buffer */
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b = m;
    b.cols = 1025;
    b.rows = 1024; /* > PIXELICA2D_MAX_TILES, rejected before reading tiles */
    b.stride_tiles = 1025;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    b = m;
    b.depth_test = true; /* no depth buffer */
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_tilemap(&f.ctx, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));

    /* Scale 2 seen from camera (1,0): pixel x samples map texel
     * floor((x + 0.5 + 1) / 2). */
    fx_init(&f, 6, 5, 7, 0);
    cam.position.x = 1.0f;
    cam.position.y = 0.0f;
    cam.zoom = 1.0f;
    CHECK(pixelica2d_set_camera(&f.ctx, &cam) == PIXELICA_OK);
    b = m;
    b.tiles = tile0;
    b.cols = b.rows = 1;
    b.stride_tiles = 1;
    b.position.x = 0.0f;
    b.scale = 2.0f;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_OK);
    CHECK(fx_matches(&f, scaled));
    CHECK(f.ctx.stats.draw_calls == 1u && f.ctx.stats.pixels_written == 12u);
    b.opacity = 0u;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == 2u && f.ctx.stats.pixels_written == 12u);

    /* 1024x1024 cells (the maximum): only visible cells are drawn/counted. */
    memset(&b, 0, sizeof b);
    b.atlas = &white;
    b.tiles = g_big_map;
    b.cols = b.rows = 1024;
    b.stride_tiles = 1024;
    b.tile_width = b.tile_height = 1;
    b.scale = 1.0f;
    b.opacity = 255u;
    fx_init(&f, 6, 5, 7, 0);
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == 30u && f.ctx.stats.pixels_written == 30u);
    cam.position.x = 1020.0f;
    cam.position.y = 1021.0f;
    CHECK(pixelica2d_set_camera(&f.ctx, &cam) == PIXELICA_OK);
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_OK); /* 4 cols x 3 rows left */
    CHECK(f.ctx.stats.draw_calls == 42u && f.ctx.stats.pixels_written == 42u);
    b.position.x = -1e9f;
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == 42u && f.ctx.stats.pixels_written == 42u);
    g_big_map[1024u * 1024u - 1u] = 1u; /* only id 0 exists */
    CHECK(pixelica2d_draw_tilemap(&f.ctx, &b) == PIXELICA_ERR_INVALID_ARG);
    g_big_map[1024u * 1024u - 1u] = 0u;
    CHECK(fx_padding_ok(&f));
}

/* ---- text ---------------------------------------------------------------- */

static void test_text(void)
{
    /* 3x3 glyphs: 'A' = 111/101/111, 'B' = 100/110/111. */
    static const uint8_t rows[6] = { 0xE0u, 0xA0u, 0xE0u, 0x80u, 0xC0u, 0xE0u };
    static const struct { uint8_t x, y; } ink[22] = {
        { 1, 0 }, { 2, 0 }, { 3, 0 }, { 1, 1 }, { 3, 1 }, { 1, 2 }, { 2, 2 }, { 3, 2 }, /* A */
        { 5, 0 }, { 5, 1 }, { 6, 1 }, { 5, 2 }, { 6, 2 }, { 7, 2 },                     /* B */
        { 1, 4 }, { 2, 4 }, { 3, 4 }, { 1, 5 }, { 3, 5 }, { 1, 6 }, { 2, 6 }, { 3, 6 }, /* A */
    };
    uint16_t expect[80];
    PIXELICA2D_Font font, b;
    Snapshot snap;
    Fixture f;
    size_t i;

    font.rows = rows;
    font.first_char = 'A';
    font.glyph_count = 2;
    font.width = 3;
    font.height = 3;

    for (i = 0; i < 80u; ++i) {
        expect[i] = BG;
    }
    for (i = 0; i < 22u; ++i) {
        expect[ink[i].y * 10u + ink[i].x] = WHT;
    }
    /* "AB\nA?": advance 4 per byte, newline back to x=1 and down 4; '?' has
     * no glyph and draws nothing. */
    fx_init(&f, 10, 8, 10, 0);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, "AB\nA?", 5, 1, 0, (uint16_t)WHT, 255u) == PIXELICA_OK);
    CHECK(fx_matches(&f, expect));
    CHECK(f.ctx.stats.draw_calls == 1u && f.ctx.stats.pixels_written == 22u);

    /* Partly off the top-left: rows 101 / 111 at y 0..1, x -1..1. */
    fx_init(&f, 10, 8, 10, 0);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, "A", 1, -1, -1, (uint16_t)RED, 255u) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 0) == BG && fx_px(&f, 1, 0) == RED && fx_px(&f, 0, 1) == RED && fx_px(&f, 1, 1) == RED);
    CHECK(f.ctx.stats.pixels_written == 3u);

    /* Translucent text uses the same integer blend ('B' has 6 set bits). */
    CHECK(pixelica2d_clear(&f.ctx, (uint16_t)RED) == PIXELICA_OK);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, "B", 1, 0, 0, 0x07FFu, 128u) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 0) == 0x7C10u && fx_px(&f, 1, 0) == RED && fx_px(&f, 2, 2) == 0x7C10u);

    /* Empty, fully clipped and invisible text still count one draw each. */
    CHECK(pixelica2d_draw_text(&f.ctx, &font, NULL, 0, 0, 0, (uint16_t)WHT, 255u) == PIXELICA_OK);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, "AAAA\nAAAA", 9, INT32_MAX, INT32_MAX, (uint16_t)WHT, 255u) == PIXELICA_OK);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, "AAAA", 4, INT32_MIN, INT32_MIN, (uint16_t)WHT, 255u) == PIXELICA_OK);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, "A", 1, 0, 4, (uint16_t)WHT, 0u) == PIXELICA_OK);
    CHECK(f.ctx.stats.draw_calls == 5u && f.ctx.stats.pixels_written == 6u);
    CHECK(fx_px(&f, 0, 4) == RED);
    CHECK(fx_padding_ok(&f));

    fx_snap(&f, &snap);
    CHECK(pixelica2d_draw_text(&f.ctx, NULL, "A", 1, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, NULL, 1, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, "A", (size_t)PIXELICA2D_MAX_TEXT_BYTES + 1u, 0, 0, 0, 255u) ==
          PIXELICA_ERR_INVALID_ARG);
    b = font;
    b.width = 9;
    CHECK(pixelica2d_draw_text(&f.ctx, &b, "A", 1, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    b = font;
    b.height = 33;
    CHECK(pixelica2d_draw_text(&f.ctx, &b, "A", 1, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    b = font;
    b.glyph_count = 0;
    CHECK(pixelica2d_draw_text(&f.ctx, &b, NULL, 0, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    b = font;
    b.first_char = 255;
    CHECK(pixelica2d_draw_text(&f.ctx, &b, "A", 1, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    b = font;
    b.rows = NULL;
    CHECK(pixelica2d_draw_text(&f.ctx, &b, "A", 1, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    b = font;
    b.rows = (const uint8_t *)f.color; /* font rows inside the color buffer */
    CHECK(pixelica2d_draw_text(&f.ctx, &b, "A", 1, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_text(&f.ctx, &font, (const char *)f.color, 2, 0, 0, 0, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));
}

/* ---- isometric ----------------------------------------------------------- */

static bool unicode_test_glyph(void *userdata, uint32_t cp, uint32_t rows[32])
{
    (void)userdata;
    if (cp != 0xD55Cu && cp != 0x1F600u && cp != 'A') return false;
    rows[0] = 0xE0000000u; rows[1] = 0xA0000000u; rows[2] = 0xE0000000u;
    return true;
}

static void test_text_utf8(void)
{
    PIXELICA_Font font = {3u, 3u, 1u, unicode_test_glyph, NULL}, bad;
    Fixture f;
    Snapshot snap;
    uint16_t expected[80];
    const char text[] = "\xED\x95\x9C\xF0\x9F\x98\x80\n?";
    size_t x, y;
    fx_init(&f, 10, 8, 12, 1);
    for (y = 0u; y < 8u; ++y) for (x = 0u; x < 10u; ++x) {
        int ink = (y < 3u && (x < 3u || (x >= 4u && x < 7u)) &&
            (y != 1u || (x % 4u) != 1u)) ||
            (y >= 4u && y < 7u && x < 3u && (y != 5u || x != 1u));
        expected[y*10u+x] = (uint16_t)(ink ? WHT : BG);
    }
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, text, sizeof text-1u, 0, 0, WHT, 255u) == PIXELICA_OK);
    CHECK(fx_matches(&f, expected));
    CHECK(fx_padding_ok(&f));
    CHECK(fx_dp(&f, 0, 0) == UINT16_MAX);
    fx_snap(&f, &snap);
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, "A\xC0\x80", 3u, 0, 0, WHT, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, "\xED\x95", 2u, 0, 0, WHT, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));
    bad = font; bad.scale = 0u;
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &bad, "A", 1u, 0, 0, WHT, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, (const char *)f.color, 1u, 0, 0, WHT, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(fx_unchanged(&f, &snap));
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, NULL, 1u, 0, 0, WHT, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, "", (size_t)PIXELICA2D_MAX_TEXT_BYTES+1u, 0, 0, WHT, 255u) == PIXELICA_ERR_INVALID_ARG);
    fx_init(&f, 10, 8, 12, 0);
    font.scale = 2u;
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, "A", 1u, -2, -2, RED, 255u) == PIXELICA_OK);
    CHECK(fx_px(&f, 0, 0) == BG && fx_px(&f, 2, 0) == RED && fx_px(&f, 0, 2) == RED);
    CHECK(fx_padding_ok(&f));
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, text, sizeof text-1u, INT32_MIN, INT32_MAX, RED, 128u) == PIXELICA_OK);
    CHECK(pixelica2d_draw_text_utf8(&f.ctx, &font, NULL, 0u, 0, 0, RED, 0u) == PIXELICA_OK);
}

static void test_iso(void)
{
    PIXELICA2D_Iso iso, b;
    PIXELICA_Vec3 map;
    PIXELICA_Vec2 out, screen;

    iso.tile_width = 32.0f;
    iso.tile_height = 16.0f;
    iso.height_scale = 8.0f;
    iso.origin.x = 100.0f;
    iso.origin.y = 50.0f;

    /* (3,1,2): x = 100 + 2*16 = 132, y = 50 + 4*8 - 2*8 = 66. */
    map.x = 3.0f;
    map.y = 1.0f;
    map.z = 2.0f;
    CHECK(pixelica2d_iso_project(&iso, map, &out) == PIXELICA_OK);
    CHECK(out.x == 132.0f && out.y == 66.0f);
    CHECK(pixelica2d_iso_unproject(&iso, out, 2.0f, &screen) == PIXELICA_OK);
    CHECK(screen.x == 3.0f && screen.y == 1.0f);

    /* (-2.5,4.25,1.5): x = 100 - 6.75*16 = -8, y = 50 + 1.75*8 - 12 = 52. */
    map.x = -2.5f;
    map.y = 4.25f;
    map.z = 1.5f;
    CHECK(pixelica2d_iso_project(&iso, map, &out) == PIXELICA_OK);
    CHECK(out.x == -8.0f && out.y == 52.0f);
    CHECK(pixelica2d_iso_unproject(&iso, out, 1.5f, &screen) == PIXELICA_OK);
    CHECK(screen.x == -2.5f && screen.y == 4.25f);

    b = iso;
    b.height_scale = 0.0f; /* allowed: flat map */
    CHECK(pixelica2d_iso_project(&b, map, &out) == PIXELICA_OK);
    CHECK(out.x == -8.0f && out.y == 64.0f);

    out.x = out.y = 7.0f;
    CHECK(pixelica2d_iso_project(NULL, map, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_iso_project(&iso, map, NULL) == PIXELICA_ERR_INVALID_ARG);
    b = iso;
    b.tile_width = 0.0f;
    CHECK(pixelica2d_iso_project(&b, map, &out) == PIXELICA_ERR_INVALID_ARG);
    b = iso;
    b.tile_height = -1.0f;
    CHECK(pixelica2d_iso_unproject(&b, screen, 0.0f, &out) == PIXELICA_ERR_INVALID_ARG);
    b = iso;
    b.height_scale = -0.5f;
    CHECK(pixelica2d_iso_project(&b, map, &out) == PIXELICA_ERR_INVALID_ARG);
    b = iso;
    b.origin.x = t2d_nan();
    CHECK(pixelica2d_iso_project(&b, map, &out) == PIXELICA_ERR_INVALID_ARG);
    map.z = t2d_inf();
    CHECK(pixelica2d_iso_project(&iso, map, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_iso_unproject(&iso, screen, t2d_nan(), &out) == PIXELICA_ERR_INVALID_ARG);
    /* Finite inputs whose result overflows float. */
    b = iso;
    b.tile_width = FLT_MAX;
    map.x = FLT_MAX;
    map.y = -FLT_MAX;
    map.z = 0.0f;
    CHECK(pixelica2d_iso_project(&b, map, &out) == PIXELICA_ERR_INVALID_ARG);
    b = iso;
    b.tile_width = 1e-30f;
    screen.x = 1e30f;
    screen.y = 0.0f;
    CHECK(pixelica2d_iso_unproject(&b, screen, 0.0f, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(out.x == 7.0f && out.y == 7.0f);
}

int main(void)
{
    test_init_and_setters();
    test_clear_padding();
    test_sprite_transforms();
    test_sprite_extremes();
    test_sprite_init_and_frames();
    test_sprite_invalid();
    test_alpha_key_depth();
    test_clipping();
    test_draw_list();
    test_tilemap();
    test_text();
    test_text_utf8();
    test_iso();
    if (g_failures != 0) {
        fprintf(stderr, "test_renderer2d: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_renderer2d: all %d checks passed\n", g_checks);
    return 0;
}

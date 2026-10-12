/*
 * Hybrid 2.5D tests: 2D view over the 3D buffers, projection, billboards.
 *
 * Scene used by most tests: a 32x24 3D context with an orthographic camera at
 * the origin looking down -Z (1 world unit == 1 pixel, world (0,0) == screen
 * (16,12), +Y up in the world, +Y down on the screen). A WALL quad sits at
 * view distance 5 covering world x 0..8, y -6..6, i.e. pixels x 16..23,
 * y 6..17. Sprites are 4x4 texels drawn at 1 texel == 1 pixel.
 *
 * Depth values with near 1 / far 9 are exact in binary (m10 = -1/4,
 * m14 = -5/4): view distance 3 -> 16384, 5 -> 32767, 7 -> 49151.
 */
#include <stdbool.h>

#include "pixelica/hybrid.h"
#include "core/context_internal.h"
#include "test_util.h"

#define W 32
#define H 24

#define BG PIXELICA_RGB565(16, 16, 48)
#define WALL PIXELICA_RGB565(200, 64, 64)
#define SPR PIXELICA_RGB565(64, 224, 64)
#define KEY PIXELICA_RGB565(248, 0, 248)
#define HUD PIXELICA_RGB565(248, 248, 0)

#define DEPTH_NEAR_3 16384u
#define DEPTH_MID_5 32767u
#define DEPTH_FAR_7 49151u

typedef struct TestPal {
    uint32_t presents;
    const uint16_t *last_pixels;
    uint16_t width, height;
    uint32_t stride;
} TestPal;

static uint64_t pal_time(void *userdata)
{
    (void)userdata;
    return 0u;
}

static uint32_t pal_buttons(void *userdata)
{
    (void)userdata;
    return 0u;
}

static bool pal_present(void *userdata, const uint16_t *pixels, uint16_t width,
                        uint16_t height, uint32_t stride_pixels)
{
    TestPal *pal = userdata;

    pal->presents += 1u;
    pal->last_pixels = pixels;
    pal->width = width;
    pal->height = height;
    pal->stride = stride_pixels;
    return true;
}

static PIXELICA_PlatformOps make_ops(TestPal *pal)
{
    PIXELICA_PlatformOps ops;

    memset(&ops, 0, sizeof ops);
    ops.userdata = pal;
    ops.time_us = pal_time;
    ops.read_buttons = pal_buttons;
    ops.present_rgb565 = pal_present;
    return ops;
}

static PIXELICA_Context *make_ctx(TestPal *pal)
{
    PIXELICA_Config cfg = pixelica_t_config(W, H);
    PIXELICA_PlatformOps ops = make_ops(pal);
    PIXELICA_Context *ctx = NULL;

    memset(pal, 0, sizeof *pal);
    if (pixelica_create(&cfg, &ops, &ctx) != PIXELICA_OK) {
        return NULL;
    }
    return ctx;
}

static uint16_t g_solid[16];
static uint16_t g_keyed[16];
static PIXELICA_Texture g_solid_tex = {g_solid, 4, 4};
static PIXELICA_Texture g_keyed_tex = {g_keyed, 4, 4};

/* Keyed texture: SPR border, a 2x2 KEY hole in the middle. */
static void fill_textures(void)
{
    int x, y;

    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            const bool hole = x >= 1 && x <= 2 && y >= 1 && y <= 2;

            g_solid[y * 4 + x] = SPR;
            g_keyed[y * 4 + x] = hole ? (uint16_t)KEY : (uint16_t)SPR;
        }
    }
}

/*
 * Asymmetric texture for mirroring tests: every texel is unique (0x0100 + n),
 * except texel (0,0) which is KEY, so a color-keyed draw shows where the
 * top-left corner ended up.
 */
static uint16_t g_asym[16];
static PIXELICA_Texture g_asym_tex = {g_asym, 4, 4};

static void fill_asym(void)
{
    int n;

    for (n = 0; n < 16; ++n) {
        g_asym[n] = n == 0 ? (uint16_t)KEY : (uint16_t)(0x0100u + (unsigned)n);
    }
}

static void ortho_vp(float vp[16])
{
    CHECK(pixelica_mat4_orthographic(vp, -16.0f, 16.0f, -12.0f, 12.0f, 1.0f, 9.0f) == PIXELICA_OK);
}

static PIXELICA_Status draw_wall(PIXELICA_Context *ctx, float z)
{
    const PIXELICA_Vec3 v[4] = {{0.0f, -6.0f, z}, {8.0f, -6.0f, z}, {8.0f, 6.0f, z}, {0.0f, 6.0f, z}};
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    PIXELICA_Mesh mesh;
    float model[16];

    mesh.vertices = v;
    mesh.vertex_count = 4u;
    mesh.indices = idx;
    mesh.index_count = 6u;
    mesh.color = WALL;
    pixelica_mat4_identity(model);
    return pixelica_draw_mesh(ctx, &mesh, model);
}

static void begin_scene(PIXELICA_Context *ctx, const float vp[16])
{
    pixelica_begin_frame(ctx, BG);
    pixelica_set_view_projection(ctx, vp);
}

static PIXELICA2D_Sprite make_sprite(const PIXELICA_Texture *tex)
{
    PIXELICA2D_Sprite s;

    memset(&s, 0, sizeof s);
    CHECK(pixelica2d_sprite_init(&s, tex) == PIXELICA_OK);
    s.pivot.x = 2.0f;
    s.pivot.y = 2.0f;
    s.depth_write = true;
    return s;
}

static uint16_t px(const PIXELICA_Context *ctx, int x, int y)
{
    return pixelica_t_color_at(ctx, W, x, y);
}

static uint16_t dp(const PIXELICA_Context *ctx, int x, int y)
{
    return pixelica_t_depth_at(ctx, W, x, y);
}

static uint64_t color_hash(const PIXELICA_Context *ctx)
{
    return pixelica_t_hash16(pixelica_color_buffer(ctx), (size_t)W * H);
}

static uint64_t depth_hash(const PIXELICA_Context *ctx)
{
    return pixelica_t_hash16(pixelica_depth_buffer(ctx), (size_t)W * H);
}

/* Same arithmetic as the 2D contract: (src*a + dst*(255-a) + 127) / 255. */
static uint16_t blend565(uint16_t s, uint16_t d, uint8_t a)
{
    const uint32_t ia = 255u - a;
    const uint32_t r = (((uint32_t)s >> 11) * a + ((uint32_t)d >> 11) * ia + 127u) / 255u;
    const uint32_t g = ((((uint32_t)s >> 5) & 63u) * a + (((uint32_t)d >> 5) & 63u) * ia + 127u) / 255u;
    const uint32_t b = (((uint32_t)s & 31u) * a + ((uint32_t)d & 31u) * ia + 127u) / 255u;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/*
 * One scene: wall at z=-5, then a billboard at world (0,0,sprite_z) of
 * 4x4 world units on the bound canvas. The frame is left open so tests can
 * read the buffers.
 */
static PIXELICA_Status render_wall_and_sprite(PIXELICA_Context *ctx, float sprite_z,
                                         const PIXELICA2D_Sprite *appearance,
                                         PIXELICA2D_Context *canvas)
{
    float vp[16];
    PIXELICA_Vec3 pos;
    PIXELICA_Vec2 size;

    ortho_vp(vp);
    begin_scene(ctx, vp);
    CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
    CHECK(pixelica2d_bind_3d(canvas, ctx) == PIXELICA_OK);
    pos.x = 0.0f;
    pos.y = 0.0f;
    pos.z = sprite_z;
    size.x = 4.0f;
    size.y = 4.0f;
    return pixelica25d_draw_billboard(canvas, vp, appearance, pos, size);
}

static void test_bind(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context view;
    unsigned char before[sizeof view];
    float vp[16];
    PIXELICA2D_Rect rect = {1, 1, 2, 2};
    uint64_t hash_before;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    memset(&view, 0xA5, sizeof view);
    memcpy(before, &view, sizeof view);

    /* No active frame yet: refused, output untouched. */
    CHECK(pixelica2d_bind_3d(&view, ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(before, &view, sizeof view) == 0);
    CHECK(pixelica2d_bind_3d(NULL, ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_bind_3d(&view, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(before, &view, sizeof view) == 0);

    ortho_vp(vp);
    begin_scene(ctx, vp);
    CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
    hash_before = color_hash(ctx);

    CHECK(pixelica2d_bind_3d(&view, ctx) == PIXELICA_OK);
    CHECK(view.surface.pixels == pixelica_color_buffer(ctx));
    CHECK(view.surface.depth == pixelica_depth_buffer(ctx));
    CHECK_EQ_LONG(view.surface.width, W);
    CHECK_EQ_LONG(view.surface.height, H);
    CHECK_EQ_LONG(view.surface.stride, W);
    CHECK_EQ_LONG(view.surface.depth_stride, W);
    CHECK(view.camera.position.x == 0.0f && view.camera.position.y == 0.0f);
    CHECK(view.camera.zoom == 1.0f);
    CHECK_EQ_LONG(view.clip.x, 0);
    CHECK_EQ_LONG(view.clip.y, 0);
    CHECK_EQ_LONG(view.clip.width, W);
    CHECK_EQ_LONG(view.clip.height, H);
    CHECK_EQ_LONG(view.stats.draw_calls, 0);
    CHECK_EQ_LONG(view.stats.pixels_written, 0);

    /* Bind neither clears nor presents. */
    CHECK(color_hash(ctx) == hash_before);
    CHECK_EQ_LONG(pal.presents, 0);

    /* The view really is the 3D buffer. */
    CHECK(pixelica2d_fill_rect(&view, &rect, HUD, 255) == PIXELICA_OK);
    CHECK(px(ctx, 1, 1) == HUD && px(ctx, 2, 2) == HUD);
    CHECK(px(ctx, 3, 3) == BG);

    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK_EQ_LONG(pal.presents, 1);

    /* After end_frame the old view must not be reused; binding is refused. */
    memset(&view, 0xA5, sizeof view);
    memcpy(before, &view, sizeof view);
    CHECK(pixelica2d_bind_3d(&view, ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(before, &view, sizeof view) == 0);

    /* The next frame can rebind. */
    begin_scene(ctx, vp);
    CHECK(pixelica2d_bind_3d(&view, ctx) == PIXELICA_OK);
    CHECK(view.surface.pixels == pixelica_color_buffer(ctx));
    CHECK_EQ_LONG(pal.presents, 1);
    pixelica_destroy(ctx);
}

static unsigned char g_ctx_memory[65536];

static void test_bind_destroyed(void)
{
    PIXELICA_Config cfg = pixelica_t_config(W, H);
    TestPal pal;
    PIXELICA_PlatformOps ops = make_ops(&pal);
    PIXELICA_Context *ctx = NULL;
    PIXELICA2D_Context view;
    unsigned char before[sizeof view];
    size_t need = 0;

    memset(&pal, 0, sizeof pal);
    CHECK(pixelica_memory_requirement(&cfg, &need) == PIXELICA_OK);
    CHECK(need <= sizeof g_ctx_memory);
    if (need > sizeof g_ctx_memory) {
        return;
    }
    CHECK(pixelica_create_in_buffer(&cfg, &ops, g_ctx_memory, sizeof g_ctx_memory, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        return;
    }
    pixelica_begin_frame(ctx, BG);
    pixelica_destroy(ctx); /* caller-owned memory stays readable; magic is cleared */

    memset(&view, 0xA5, sizeof view);
    memcpy(before, &view, sizeof view);
    CHECK(pixelica2d_bind_3d(&view, ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(before, &view, sizeof view) == 0);
}

/* Via uintptr_t: the buffers are const, and some probes are not real pointers. */
#define AS_VIEW(p) ((PIXELICA2D_Context *)(void *)(uintptr_t)(p))
#define LEAD_BYTES 256u

static unsigned char g_ctx_snapshot[sizeof g_ctx_memory];

/*
 * A bind whose output overlaps the engine-owned block is refused and changes
 * nothing: the snapshot covers the whole caller-owned memory, i.e. the context
 * struct, guards, color, depth and arena.
 */
static void expect_bind_rejected(PIXELICA2D_Context *out, PIXELICA_Context *ctx)
{
    CHECK(pixelica2d_bind_3d(out, ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(g_ctx_memory, g_ctx_snapshot, sizeof g_ctx_memory) == 0);
    CHECK(pixelica_memory_guards_intact(ctx) != 0);
}

static void test_bind_output_must_be_disjoint(void)
{
    PIXELICA_Config cfg = pixelica_t_config(W, H);
    TestPal pal;
    PIXELICA_PlatformOps ops = make_ops(&pal);
    PIXELICA_Context *ctx = NULL;
    PIXELICA2D_Context view;
    PIXELICA2D_Context *below, *above;
    PIXELICA2D_Rect rect = {2, 2, 3, 3};
    const size_t plane = (size_t)W * H * sizeof(uint16_t);
    const size_t word = sizeof(void *);
    size_t need = 0, span, ctx_off;
    unsigned char *ctx_bytes, *color, *depth;
    float vp[16];

    memset(&pal, 0, sizeof pal);
    memset(g_ctx_memory, 0x3C, sizeof g_ctx_memory);
    CHECK(pixelica_memory_requirement(&cfg, &need) == PIXELICA_OK);
    span = (need + 15u) & ~(size_t)15u;
    _Static_assert(LEAD_BYTES >= sizeof(PIXELICA2D_Context), "space for a disjoint view before the context");
    CHECK(LEAD_BYTES + span + sizeof view <= sizeof g_ctx_memory);
    if (LEAD_BYTES + span + sizeof view > sizeof g_ctx_memory) {
        return;
    }
    CHECK(pixelica_create_in_buffer(&cfg, &ops, g_ctx_memory + LEAD_BYTES,
                               sizeof g_ctx_memory - LEAD_BYTES, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        return;
    }
    ctx_bytes = (unsigned char *)(void *)ctx;
    color = (unsigned char *)(uintptr_t)pixelica_color_buffer(ctx);
    depth = (unsigned char *)(uintptr_t)pixelica_depth_buffer(ctx);
    ctx_off = (size_t)(ctx_bytes - g_ctx_memory);
    CHECK(ctx_off >= LEAD_BYTES && ctx_off + span + sizeof view <= sizeof g_ctx_memory);

    /* Non-trivial color and depth, so any stray write shows up. */
    ortho_vp(vp);
    begin_scene(ctx, vp);
    CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
    memcpy(g_ctx_snapshot, g_ctx_memory, sizeof g_ctx_memory);

    /* Core context struct (start, interior, straddling its start from below). */
    expect_bind_rejected(AS_VIEW(ctx_bytes), ctx);
    expect_bind_rejected(AS_VIEW(ctx_bytes + word), ctx);
    expect_bind_rejected(AS_VIEW(ctx_bytes - word), ctx);
    /* Color buffer (the reported reproduction), guard0 -> color, color -> guard1. */
    expect_bind_rejected(AS_VIEW(color), ctx);
    expect_bind_rejected(AS_VIEW(color + plane / 2u), ctx);
    expect_bind_rejected(AS_VIEW(color - word), ctx);
    expect_bind_rejected(AS_VIEW(color + plane - word), ctx);
    /* Depth buffer, depth -> guard2, guard2 itself. */
    expect_bind_rejected(AS_VIEW(depth), ctx);
    expect_bind_rejected(AS_VIEW(depth + plane / 2u), ctx);
    expect_bind_rejected(AS_VIEW(depth + plane - word), ctx);
    expect_bind_rejected(AS_VIEW(depth + plane), ctx);
    /* Frame arena (follows guard2; the arena is 5 KiB, so +64 is well inside it). */
    expect_bind_rejected(AS_VIEW(depth + plane + 64u), ctx);
    /* The new geometric coverage bitmap is also context-owned storage. */
    expect_bind_rejected(AS_VIEW(ctx->coverage), ctx);
    expect_bind_rejected(AS_VIEW(ctx->guards[PIXELICA_GUARD_COUNT - 1u]), ctx);
    /* An address range that would wrap around is refused without any access. */
    expect_bind_rejected(AS_VIEW((uintptr_t)UINTPTR_MAX - sizeof view / 2u), ctx);
    CHECK(pixelica2d_bind_3d(AS_VIEW(ctx_bytes), NULL) == PIXELICA_ERR_INVALID_ARG);

    /* The same context is still fully usable and untouched. */
    CHECK(pixelica2d_bind_3d(&view, ctx) == PIXELICA_OK);
    CHECK(view.surface.pixels == pixelica_color_buffer(ctx));
    CHECK(view.surface.depth == pixelica_depth_buffer(ctx));
    CHECK(pixelica2d_fill_rect(&view, &rect, HUD, 255) == PIXELICA_OK);
    CHECK(px(ctx, 2, 2) == HUD && px(ctx, 4, 4) == HUD && px(ctx, 5, 5) == BG);
    CHECK(px(ctx, 20, 8) == WALL);
    CHECK_EQ_LONG(dp(ctx, 20, 8), DEPTH_MID_5);
    CHECK(pixelica_memory_guards_intact(ctx) != 0);

    /* Memory immediately below the block is fine, and nothing inside changes. */
    memcpy(g_ctx_snapshot, g_ctx_memory, sizeof g_ctx_memory);
    below = AS_VIEW(ctx_bytes - sizeof *below);
    CHECK(pixelica2d_bind_3d(below, ctx) == PIXELICA_OK);
    CHECK(below->surface.pixels == pixelica_color_buffer(ctx));
    CHECK(below->surface.depth == pixelica_depth_buffer(ctx));
    CHECK_EQ_LONG(below->surface.width, W);
    CHECK_EQ_LONG(below->stats.draw_calls, 0);
    CHECK(memcmp(g_ctx_memory + ctx_off, g_ctx_snapshot + ctx_off, span) == 0);
    CHECK(memcmp(g_ctx_memory, g_ctx_snapshot, ctx_off - sizeof *below) == 0);

    /* Memory at or past the end of the block (need >= block size) is fine too. */
    above = AS_VIEW(ctx_bytes + span);
    CHECK(pixelica2d_bind_3d(above, ctx) == PIXELICA_OK);
    CHECK(above->surface.pixels == pixelica_color_buffer(ctx));
    CHECK(above->surface.depth == pixelica_depth_buffer(ctx));
    CHECK_EQ_LONG(above->surface.height, H);
    CHECK(memcmp(g_ctx_memory + ctx_off, g_ctx_snapshot + ctx_off, span) == 0);
    CHECK(pixelica_memory_guards_intact(ctx) != 0);

    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK_EQ_LONG(pal.presents, 1);
    CHECK(pixelica_memory_guards_intact(ctx) != 0);
    pixelica_destroy(ctx);
}

static void test_bind_output_must_be_disjoint_heap(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context view;
    uint16_t color_before[W * H];
    uint16_t depth_before[W * H];
    PIXELICA_FrameStats stats_before;
    float vp[16];

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    ortho_vp(vp);
    begin_scene(ctx, vp);
    CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
    memcpy(color_before, pixelica_color_buffer(ctx), sizeof color_before);
    memcpy(depth_before, pixelica_depth_buffer(ctx), sizeof depth_before);
    stats_before = pixelica_frame_stats(ctx);

    CHECK(pixelica2d_bind_3d(AS_VIEW(pixelica_color_buffer(ctx)), ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(color_before, pixelica_color_buffer(ctx), sizeof color_before) == 0);
    CHECK(pixelica2d_bind_3d(AS_VIEW(pixelica_depth_buffer(ctx) + 8), ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(depth_before, pixelica_depth_buffer(ctx), sizeof depth_before) == 0);
    CHECK(pixelica2d_bind_3d(AS_VIEW(ctx), ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_memory_guards_intact(ctx) != 0);
    CHECK_EQ_LONG(pixelica_frame_stats(ctx).draw_calls, stats_before.draw_calls);
    CHECK_EQ_LONG(pixelica_frame_stats(ctx).pixels_written, stats_before.pixels_written);

    /* Still a live context in an active frame: a disjoint view works. */
    CHECK(pixelica2d_bind_3d(&view, ctx) == PIXELICA_OK);
    CHECK(view.surface.pixels == pixelica_color_buffer(ctx));
    CHECK(memcmp(color_before, pixelica_color_buffer(ctx), sizeof color_before) == 0);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK_EQ_LONG(pal.presents, 1);
    CHECK(pixelica_memory_guards_intact(ctx) != 0);
    pixelica_destroy(ctx);
}

static void expect_project_invalid(const float vp[16], PIXELICA_Vec3 p, uint16_t w, uint16_t h)
{
    PIXELICA25D_Projection out;
    unsigned char before[sizeof out];

    memset(&out, 0x5A, sizeof out);
    memcpy(before, &out, sizeof out);
    CHECK(pixelica25d_project(vp, p, w, h, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(before, &out, sizeof out) == 0);
}

static void test_project_standard(void)
{
    float identity[16];
    float vp[16];
    PIXELICA25D_Projection out;
    PIXELICA_Vec3 p;

    pixelica_mat4_identity(identity);

    /* Identity: NDC maps straight to the viewport, y flips. */
    p.x = 0.0f; p.y = 0.0f; p.z = 0.0f;
    CHECK(pixelica25d_project(identity, p, W, H, &out) == PIXELICA_OK);
    CHECK(out.drawable);
    CHECK_NEAR(out.screen.x, 16.0, 1e-4);
    CHECK_NEAR(out.screen.y, 12.0, 1e-4);
    CHECK_EQ_LONG(out.depth, 32767);
    CHECK_NEAR(out.pixels_per_unit_x, 16.0, 1e-4);
    CHECK_NEAR(out.pixels_per_unit_y, 12.0, 1e-4);

    p.x = -1.0f; p.y = 1.0f; p.z = -1.0f;
    CHECK(pixelica25d_project(identity, p, W, H, &out) == PIXELICA_OK);
    CHECK(out.drawable);
    CHECK_NEAR(out.screen.x, 0.0, 1e-4);
    CHECK_NEAR(out.screen.y, 0.0, 1e-4);
    CHECK_EQ_LONG(out.depth, 0);

    p.x = 1.0f; p.y = -1.0f; p.z = 1.0f;
    CHECK(pixelica25d_project(identity, p, W, H, &out) == PIXELICA_OK);
    CHECK(out.drawable);
    CHECK_NEAR(out.screen.x, 32.0, 1e-4);
    CHECK_NEAR(out.screen.y, 24.0, 1e-4);
    CHECK_EQ_LONG(out.depth, 65534); /* never the 65535 clear value */

    p.x = 0.0f; p.y = 0.0f; p.z = 1.01f;
    memset(&out, 0x5A, sizeof out);
    CHECK(pixelica25d_project(identity, p, W, H, &out) == PIXELICA_OK);
    CHECK(!out.drawable);
    CHECK(out.screen.x == 0.0f && out.screen.y == 0.0f);
    CHECK(out.pixels_per_unit_x == 0.0f && out.pixels_per_unit_y == 0.0f);
    CHECK_EQ_LONG(out.depth, 0);

    /* Orthographic: 1 world unit == 1 pixel, exact depth at view distance 5. */
    ortho_vp(vp);
    p.x = 4.0f; p.y = 3.0f; p.z = -5.0f;
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK);
    CHECK(out.drawable);
    CHECK_NEAR(out.screen.x, 20.0, 1e-3);
    CHECK_NEAR(out.screen.y, 9.0, 1e-3);
    CHECK_EQ_LONG(out.depth, DEPTH_MID_5);
    CHECK_NEAR(out.pixels_per_unit_x, 1.0, 1e-4);
    CHECK_NEAR(out.pixels_per_unit_y, 1.0, 1e-4);
    p.z = -3.0f;
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK);
    CHECK_EQ_LONG(out.depth, DEPTH_NEAR_3);
    p.z = -7.0f;
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK);
    CHECK_EQ_LONG(out.depth, DEPTH_FAR_7);

    /* Before near, beyond far, behind the camera: OK but not drawable. */
    p.z = -0.5f;
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK && !out.drawable);
    p.z = -9.5f;
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK && !out.drawable);
    p.z = 5.0f;
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK && !out.drawable);

    /* A far-off-screen anchor is still drawable (a big sprite may reach in). */
    p.x = 1000.0f; p.y = -700.0f; p.z = -5.0f;
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK);
    CHECK(out.drawable);
    CHECK_NEAR(out.screen.x, 1016.0, 0.05);
    CHECK_NEAR(out.screen.y, 712.0, 0.05);
}

static void test_project_perspective(void)
{
    float proj[16];
    float vp[16];
    PIXELICA25D_Projection out;
    PIXELICA_Vec3 p;

    /* 90 degree vertical fov, square pixels on a 32x24 viewport. */
    CHECK(pixelica_mat4_perspective(vp, 1.57079632679f, 32.0f / 24.0f, 1.0f, 9.0f) == PIXELICA_OK);

    p.x = 0.0f; p.y = 0.0f; p.z = -4.0f;
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK);
    CHECK(out.drawable);
    CHECK_NEAR(out.screen.x, 16.0, 1e-3);
    CHECK_NEAR(out.screen.y, 12.0, 1e-3);
    CHECK_NEAR(out.pixels_per_unit_x, 3.0, 1e-3); /* 12 / w */
    CHECK_NEAR(out.pixels_per_unit_y, 3.0, 1e-3);
    /* ndc z = 0.6875 -> round(1.6875 * 32767) = 55294 (float matrix: +-1). */
    CHECK(out.depth >= 55293u && out.depth <= 55295u);

    p.x = 2.0f; p.y = 0.0f; p.z = -4.0f; /* ndc x = 0.375 */
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK);
    CHECK_NEAR(out.screen.x, 22.0, 1e-3);

    p.x = 0.0f; p.y = 0.0f; p.z = -0.5f; /* before near */
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK && !out.drawable);
    p.z = -10.0f; /* beyond far */
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK && !out.drawable);
    p.z = 3.0f; /* behind */
    CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK && !out.drawable);

    /* A rotated (orthonormal) camera keeps the size: put a point on its axis. */
    {
        float view[16], inverse[16], target[4];
        PIXELICA_Vec3 axis;

        pixelica_mat4_rotation_y(view, 0.7f);
        pixelica_mat4_rotation_y(inverse, -0.7f);
        axis.x = 0.0f; axis.y = 0.0f; axis.z = -4.0f;
        pixelica_mat4_transform_point(target, inverse, axis);
        p.x = target[0]; p.y = target[1]; p.z = target[2];
        CHECK(pixelica_mat4_perspective(proj, 1.57079632679f, 32.0f / 24.0f, 1.0f, 9.0f) == PIXELICA_OK);
        pixelica_mat4_multiply(vp, proj, view);
        CHECK(pixelica25d_project(vp, p, W, H, &out) == PIXELICA_OK);
        CHECK(out.drawable);
        CHECK_NEAR(out.screen.x, 16.0, 1e-2);
        CHECK_NEAR(out.screen.y, 12.0, 1e-2);
        CHECK_NEAR(out.pixels_per_unit_x, 3.0, 1e-2);
        CHECK_NEAR(out.pixels_per_unit_y, 3.0, 1e-2);
    }
}

static void test_project_invalid(void)
{
    float identity[16];
    float bad[16];
    float huge[16];
    float zero[16];
    PIXELICA_Vec3 p = {0.0f, 0.0f, 0.0f};
    PIXELICA_Vec3 bad_p;
    PIXELICA25D_Projection out;
    unsigned char before[sizeof out];
    size_t i;

    pixelica_mat4_identity(identity);

    memset(&out, 0x5A, sizeof out);
    memcpy(before, &out, sizeof out);
    CHECK(pixelica25d_project(identity, p, W, H, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica25d_project(NULL, p, W, H, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(before, &out, sizeof out) == 0);

    memcpy(bad, identity, sizeof bad);
    bad[5] = pixelica_t_nan();
    expect_project_invalid(bad, p, W, H);
    memcpy(bad, identity, sizeof bad);
    bad[12] = pixelica_t_inf();
    expect_project_invalid(bad, p, W, H);

    bad_p = p;
    bad_p.y = pixelica_t_nan();
    expect_project_invalid(identity, bad_p, W, H);
    bad_p = p;
    bad_p.z = pixelica_t_inf();
    expect_project_invalid(identity, bad_p, W, H);

    expect_project_invalid(identity, p, 0, H);
    expect_project_invalid(identity, p, W, 0);
    expect_project_invalid(identity, p, 4097, H);
    expect_project_invalid(identity, p, W, 4097);

    /* Degenerate x/y scale. */
    for (i = 0; i < 16; ++i) {
        zero[i] = 0.0f;
    }
    zero[15] = 1.0f;
    expect_project_invalid(zero, p, W, H);
    memcpy(bad, identity, sizeof bad);
    bad[0] = 0.0f;
    expect_project_invalid(bad, p, W, H);

    /* Finite inputs whose screen position does not fit a float. */
    memcpy(huge, identity, sizeof huge);
    huge[0] = 3.0e38f;
    bad_p = p;
    bad_p.x = 3.0e38f;
    expect_project_invalid(huge, bad_p, W, H);
}

static void test_occlusion(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA2D_Sprite s;
    int x, y;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    fill_textures();

    /* In front of the wall (distance 3 < 5): the whole sprite shows. The
     * appearance asks for no depth test; the billboard forces it on. */
    s = make_sprite(&g_solid_tex);
    s.depth_test = false;
    CHECK(render_wall_and_sprite(ctx, -3.0f, &s, &canvas) == PIXELICA_OK);
    for (y = 10; y < 14; ++y) {
        for (x = 14; x < 18; ++x) {
            CHECK(px(ctx, x, y) == SPR);
            CHECK_EQ_LONG(dp(ctx, x, y), DEPTH_NEAR_3);
        }
    }
    CHECK(px(ctx, 13, 11) == BG && px(ctx, 18, 11) == WALL);
    CHECK(px(ctx, 20, 8) == WALL && px(ctx, 23, 17) == WALL);
    CHECK(px(ctx, 24, 8) == BG && px(ctx, 20, 5) == BG);
    CHECK_EQ_LONG(dp(ctx, 20, 8), DEPTH_MID_5);
    CHECK_EQ_LONG(canvas.stats.draw_calls, 1);
    CHECK_EQ_LONG(canvas.stats.pixels_written, 16);

    /* Behind the wall (distance 7): only the part outside the wall shows. */
    s = make_sprite(&g_solid_tex);
    s.depth_test = false;
    CHECK(render_wall_and_sprite(ctx, -7.0f, &s, &canvas) == PIXELICA_OK);
    for (y = 10; y < 14; ++y) {
        CHECK(px(ctx, 14, y) == SPR && px(ctx, 15, y) == SPR);
        CHECK_EQ_LONG(dp(ctx, 14, y), DEPTH_FAR_7);
        CHECK(px(ctx, 16, y) == WALL && px(ctx, 17, y) == WALL);
        CHECK_EQ_LONG(dp(ctx, 16, y), DEPTH_MID_5);
    }
    CHECK_EQ_LONG(canvas.stats.pixels_written, 8);

    /* Equal depth: strict LESS keeps the wall. Distance 5 is exact in float and
     * double here (see the header comment); the header makes no such promise
     * for arbitrary coplanar geometry. */
    s = make_sprite(&g_solid_tex);
    CHECK(render_wall_and_sprite(ctx, -5.0f, &s, &canvas) == PIXELICA_OK);
    for (y = 10; y < 14; ++y) {
        CHECK(px(ctx, 15, y) == SPR);
        CHECK(px(ctx, 16, y) == WALL && px(ctx, 17, y) == WALL);
    }
    CHECK_EQ_LONG(canvas.stats.pixels_written, 8);

    /* depth_write = false: color shows, depth stays that of the wall/clear. */
    s = make_sprite(&g_solid_tex);
    s.depth_write = false;
    CHECK(render_wall_and_sprite(ctx, -3.0f, &s, &canvas) == PIXELICA_OK);
    CHECK(px(ctx, 16, 11) == SPR);
    CHECK_EQ_LONG(dp(ctx, 16, 11), DEPTH_MID_5);
    CHECK_EQ_LONG(dp(ctx, 14, 11), PIXELICA_DEPTH_CLEAR);

    pixelica_destroy(ctx);
}

static void test_key_holes_and_alpha(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA2D_Sprite s;
    float vp[16];
    PIXELICA_Vec3 pos = {4.0f, 0.0f, -3.0f}; /* screen (20,12): fully over the wall */
    PIXELICA_Vec2 size = {4.0f, 4.0f};
    uint64_t color_wall_only, depth_wall_only;
    int x, y;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    fill_textures();
    ortho_vp(vp);

    begin_scene(ctx, vp);
    CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
    color_wall_only = color_hash(ctx);
    depth_wall_only = depth_hash(ctx);

    /* Keyed holes: wall color AND wall depth show through, border is drawn. */
    s = make_sprite(&g_keyed_tex);
    s.color_key_enabled = true;
    s.color_key = KEY;
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, pos, size) == PIXELICA_OK);
    for (y = 10; y < 14; ++y) {
        for (x = 18; x < 22; ++x) {
            const bool hole = x >= 19 && x <= 20 && y >= 11 && y <= 12;

            if (hole) {
                CHECK(px(ctx, x, y) == WALL);
                CHECK_EQ_LONG(dp(ctx, x, y), DEPTH_MID_5);
            } else {
                CHECK(px(ctx, x, y) == SPR);
                CHECK_EQ_LONG(dp(ctx, x, y), DEPTH_NEAR_3);
            }
        }
    }
    CHECK_EQ_LONG(canvas.stats.draw_calls, 1);
    CHECK_EQ_LONG(canvas.stats.pixels_written, 12);

    /* Holes over empty background leave the depth buffer cleared. */
    pixelica_begin_frame(ctx, BG);
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, pos, size) == PIXELICA_OK);
    CHECK(px(ctx, 19, 11) == BG);
    CHECK_EQ_LONG(dp(ctx, 19, 11), PIXELICA_DEPTH_CLEAR);
    CHECK(px(ctx, 18, 10) == SPR);
    CHECK_EQ_LONG(dp(ctx, 18, 10), DEPTH_NEAR_3);

    /* Opacity 0 writes neither color nor depth, but is one accepted draw. */
    begin_scene(ctx, vp);
    CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
    s = make_sprite(&g_solid_tex);
    s.opacity = 0u;
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, pos, size) == PIXELICA_OK);
    CHECK(color_hash(ctx) == color_wall_only);
    CHECK(depth_hash(ctx) == depth_wall_only);
    CHECK_EQ_LONG(canvas.stats.draw_calls, 1);
    CHECK_EQ_LONG(canvas.stats.pixels_written, 0);

    /* Half opacity blends over the wall and, as a depth write, updates depth. */
    s = make_sprite(&g_solid_tex);
    s.opacity = 128u;
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, pos, size) == PIXELICA_OK);
    CHECK(px(ctx, 19, 11) == blend565(SPR, WALL, 128u));
    CHECK_EQ_LONG(dp(ctx, 19, 11), DEPTH_NEAR_3);
    CHECK_EQ_LONG(canvas.stats.draw_calls, 2);
    CHECK_EQ_LONG(canvas.stats.pixels_written, 16);

    pixelica_destroy(ctx);
}

static void test_billboard_rules(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA2D_Sprite s;
    PIXELICA2D_Sprite good;
    PIXELICA_Texture no_pixels = {NULL, 4, 4};
    float vp[16];
    float persp[16];
    PIXELICA_Vec3 center = {0.0f, 0.0f, -3.0f};
    PIXELICA_Vec2 size = {4.0f, 4.0f};
    PIXELICA_Vec2 bad_size;
    uint64_t color_before, depth_before;
    PIXELICA_FrameStats stats3d;
    unsigned char sprite_before[sizeof s];
    int x, y;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    fill_textures();
    ortho_vp(vp);
    CHECK(pixelica_mat4_perspective(persp, 1.57079632679f, 32.0f / 24.0f, 1.0f, 9.0f) == PIXELICA_OK);

    begin_scene(ctx, vp);
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    good = make_sprite(&g_solid_tex);
    color_before = color_hash(ctx);
    depth_before = depth_hash(ctx);

    /* NULL arguments and a bad matrix/size change nothing. */
    CHECK(pixelica25d_draw_billboard(NULL, vp, &good, center, size) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica25d_draw_billboard(&canvas, NULL, &good, center, size) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica25d_draw_billboard(&canvas, vp, NULL, center, size) == PIXELICA_ERR_INVALID_ARG);
    {
        float bad_vp[16];

        memcpy(bad_vp, vp, sizeof bad_vp);
        bad_vp[0] = pixelica_t_nan();
        CHECK(pixelica25d_draw_billboard(&canvas, bad_vp, &good, center, size) == PIXELICA_ERR_INVALID_ARG);
    }
    bad_size = size; bad_size.x = 0.0f;
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &good, center, bad_size) == PIXELICA_ERR_INVALID_ARG);
    bad_size = size; bad_size.y = -1.0f;
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &good, center, bad_size) == PIXELICA_ERR_INVALID_ARG);
    bad_size = size; bad_size.x = pixelica_t_nan();
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &good, center, bad_size) == PIXELICA_ERR_INVALID_ARG);
    bad_size = size; bad_size.y = pixelica_t_inf();
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &good, center, bad_size) == PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(canvas.stats.draw_calls, 0);
    CHECK(color_hash(ctx) == color_before && depth_hash(ctx) == depth_before);

    /* A canvas without a depth buffer cannot host depth-tested billboards. */
    {
        static uint16_t flat_color[16 * 16];
        PIXELICA2D_Surface surface;
        PIXELICA2D_Context flat;

        surface.pixels = flat_color;
        surface.width = 16;
        surface.height = 16;
        surface.stride = 16;
        surface.depth = NULL;
        surface.depth_stride = 0;
        CHECK(pixelica2d_init(&flat, &surface) == PIXELICA_OK);
        CHECK(pixelica25d_draw_billboard(&flat, vp, &good, center, size) == PIXELICA_ERR_INVALID_ARG);
        CHECK_EQ_LONG(flat.stats.draw_calls, 0);
    }

    /* The appearance is validated even when the point is behind the camera. */
    {
        PIXELICA_Vec3 behind = {0.0f, 0.0f, 5.0f};

        CHECK(pixelica25d_draw_billboard(&canvas, persp, &good, behind, size) == PIXELICA_OK);
        CHECK_EQ_LONG(canvas.stats.draw_calls, 0);
        CHECK(color_hash(ctx) == color_before && depth_hash(ctx) == depth_before);

        s = good;
        s.texture = NULL;
        CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_ERR_INVALID_ARG);
        s = good;
        s.texture = &no_pixels;
        CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_ERR_INVALID_ARG);
        s = good;
        s.source.width = 99;
        CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_ERR_INVALID_ARG);
        s = good;
        s.rotation = pixelica_t_nan();
        CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_ERR_INVALID_ARG);
        s = good;
        s.pivot.x = pixelica_t_inf();
        CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_ERR_INVALID_ARG);

        /* Position / depth / depth_test of the appearance are overridden, so ignored. */
        s = good;
        s.position.x = pixelica_t_nan();
        s.depth_test = false;
        CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_OK);
        CHECK_EQ_LONG(canvas.stats.draw_calls, 0);
    }
    CHECK(color_hash(ctx) == color_before && depth_hash(ctx) == depth_before);

    /* Camera is ignored and preserved, the clip is honoured. */
    {
        PIXELICA2D_Camera camera;
        PIXELICA2D_Rect clip = {0, 0, 16, 24};

        camera.position.x = 100.0f;
        camera.position.y = 100.0f;
        camera.zoom = 2.0f;
        CHECK(pixelica2d_set_camera(&canvas, &camera) == PIXELICA_OK);
        CHECK(pixelica2d_set_clip(&canvas, &clip) == PIXELICA_OK);
        memcpy(sprite_before, &good, sizeof good);
        CHECK(pixelica25d_draw_billboard(&canvas, vp, &good, center, size) == PIXELICA_OK);
        CHECK(memcmp(sprite_before, &good, sizeof good) == 0);
        CHECK(canvas.camera.position.x == 100.0f && canvas.camera.position.y == 100.0f);
        CHECK(canvas.camera.zoom == 2.0f);
        CHECK_EQ_LONG(canvas.clip.width, 16);
        CHECK_EQ_LONG(canvas.clip.height, 24);
        for (y = 10; y < 14; ++y) {
            CHECK(px(ctx, 14, y) == SPR && px(ctx, 15, y) == SPR);
            CHECK(px(ctx, 16, y) == BG && px(ctx, 17, y) == BG);
        }
        CHECK_EQ_LONG(canvas.stats.draw_calls, 1);
        CHECK_EQ_LONG(canvas.stats.pixels_written, 8);
        CHECK(pixelica2d_set_camera(&canvas, NULL) == PIXELICA_OK);
        CHECK(pixelica2d_set_clip(&canvas, NULL) == PIXELICA_OK);
    }

    /* Billboards add to the 2D counters only. */
    stats3d = pixelica_frame_stats(ctx);
    CHECK_EQ_LONG(stats3d.draw_calls, 0);
    CHECK_EQ_LONG(stats3d.pixels_written, 0);
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &good, center, size) == PIXELICA_OK);
    CHECK_EQ_LONG(canvas.stats.draw_calls, 2);
    CHECK_EQ_LONG(pixelica_frame_stats(ctx).draw_calls, 0);
    CHECK_EQ_LONG(pixelica_frame_stats(ctx).pixels_written, 0);

    /*
     * Anchor off-screen (screen x = -8) but a 40x40 world-unit sprite reaches
     * in: columns 0..11 are fully covered, the rest is untouched.
     */
    begin_scene(ctx, vp);
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    {
        PIXELICA_Vec3 left = {-24.0f, 0.0f, -3.0f};
        PIXELICA_Vec2 big = {40.0f, 40.0f};
        PIXELICA25D_Projection pr;

        CHECK(pixelica25d_project(vp, left, W, H, &pr) == PIXELICA_OK);
        CHECK(pr.drawable && pr.screen.x < 0.0f);
        CHECK(pixelica25d_draw_billboard(&canvas, vp, &good, left, big) == PIXELICA_OK);
        for (y = 0; y < H; ++y) {
            for (x = 0; x < W; ++x) {
                CHECK(px(ctx, x, y) == (x < 12 ? (uint16_t)SPR : (uint16_t)BG));
            }
        }
        CHECK_EQ_LONG(canvas.stats.pixels_written, 12 * 24);
    }

    pixelica_destroy(ctx);
}

/*
 * The sign of appearance.scale picks the mirroring, the magnitude is ignored
 * (it comes from world_size). The billboard at world (0,0,-3), 4x4 world units,
 * covers pixels x 14..17, y 10..13 with the pivot (2,2) in its middle, so a
 * mirror around the pivot maps the footprint onto itself.
 */
static void test_billboard_mirroring(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA2D_Sprite s;
    unsigned char sprite_before[sizeof s];
    PIXELICA_Vec3 center = {0.0f, 0.0f, -3.0f};
    PIXELICA_Vec3 over_wall = {4.0f, 0.0f, -3.0f}; /* pixels x 18..21, y 10..13 */
    PIXELICA_Vec2 size = {4.0f, 4.0f};
    float vp[16];
    int variant, i, j;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    fill_asym();
    ortho_vp(vp);

    /* All four sign combinations, with a color key that marks texel (0,0). */
    for (variant = 0; variant < 4; ++variant) {
        const bool flip_x = (variant & 1) != 0;
        const bool flip_y = (variant & 2) != 0;

        begin_scene(ctx, vp);
        CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
        s = make_sprite(&g_asym_tex);
        s.color_key_enabled = true;
        s.color_key = KEY;
        s.scale.x = flip_x ? -3.0f : 0.5f; /* magnitudes must not matter */
        s.scale.y = flip_y ? -0.25f : 7.0f;
        memcpy(sprite_before, &s, sizeof s);
        CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, center, size) == PIXELICA_OK);
        CHECK(memcmp(sprite_before, &s, sizeof s) == 0);

        for (j = 0; j < 4; ++j) {
            for (i = 0; i < 4; ++i) {
                const int sx = flip_x ? 3 - i : i;
                const int sy = flip_y ? 3 - j : j;
                const uint16_t texel = g_asym[sy * 4 + sx];

                if (texel == (uint16_t)KEY) {
                    CHECK(px(ctx, 14 + i, 10 + j) == BG);
                    CHECK_EQ_LONG(dp(ctx, 14 + i, 10 + j), PIXELICA_DEPTH_CLEAR);
                } else {
                    CHECK(px(ctx, 14 + i, 10 + j) == texel);
                    CHECK_EQ_LONG(dp(ctx, 14 + i, 10 + j), DEPTH_NEAR_3);
                }
            }
        }
        /* Same footprint whatever the sign. */
        CHECK(px(ctx, 13, 10) == BG && px(ctx, 18, 13) == BG);
        CHECK(px(ctx, 14, 9) == BG && px(ctx, 17, 14) == BG);
        CHECK_EQ_LONG(canvas.stats.draw_calls, 1);
        CHECK_EQ_LONG(canvas.stats.pixels_written, 15);
    }

    /* Mirrored in X and Y over the wall with half opacity: blend, depth write. */
    begin_scene(ctx, vp);
    CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    s = make_sprite(&g_asym_tex);
    s.scale.x = -1.0f;
    s.scale.y = -1.0f;
    s.opacity = 128u;
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, over_wall, size) == PIXELICA_OK);
    for (j = 0; j < 4; ++j) {
        for (i = 0; i < 4; ++i) {
            const uint16_t texel = g_asym[(3 - j) * 4 + (3 - i)];

            CHECK(px(ctx, 18 + i, 10 + j) == blend565(texel, WALL, 128u));
            CHECK_EQ_LONG(dp(ctx, 18 + i, 10 + j), DEPTH_NEAR_3);
        }
    }
    CHECK_EQ_LONG(canvas.stats.draw_calls, 1);
    CHECK_EQ_LONG(canvas.stats.pixels_written, 16);

    /* A mirrored billboard is still depth tested against the wall (distance 7). */
    begin_scene(ctx, vp);
    CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    s = make_sprite(&g_asym_tex);
    s.scale.x = -1.0f;
    over_wall.z = -7.0f;
    CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, over_wall, size) == PIXELICA_OK);
    CHECK(px(ctx, 19, 11) == WALL);
    CHECK_EQ_LONG(dp(ctx, 19, 11), DEPTH_MID_5);
    CHECK_EQ_LONG(canvas.stats.pixels_written, 0);

    pixelica_destroy(ctx);
}

static void test_billboard_scale_invalid(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA2D_Sprite good, s;
    PIXELICA_Vec3 center = {0.0f, 0.0f, -3.0f};
    PIXELICA_Vec3 behind = {0.0f, 0.0f, 5.0f};
    PIXELICA_Vec2 size = {4.0f, 4.0f};
    float vp[16];
    float persp[16];
    float bad[5];
    uint64_t color_before, depth_before;
    size_t k;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    fill_asym();
    ortho_vp(vp);
    CHECK(pixelica_mat4_perspective(persp, 1.57079632679f, 32.0f / 24.0f, 1.0f, 9.0f) == PIXELICA_OK);
    bad[0] = 0.0f;
    bad[1] = -0.0f;
    bad[2] = pixelica_t_nan();
    bad[3] = pixelica_t_inf();
    bad[4] = -pixelica_t_inf();

    begin_scene(ctx, vp);
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    good = make_sprite(&g_asym_tex);
    color_before = color_hash(ctx);
    depth_before = depth_hash(ctx);

    for (k = 0; k < sizeof bad / sizeof bad[0]; ++k) {
        /* Visible point and a point behind the camera are refused alike. */
        s = good;
        s.scale.x = bad[k];
        CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, center, size) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_ERR_INVALID_ARG);
        s = good;
        s.scale.y = bad[k];
        CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, center, size) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_ERR_INVALID_ARG);
        CHECK_EQ_LONG(canvas.stats.draw_calls, 0);
        CHECK_EQ_LONG(canvas.stats.pixels_written, 0);
        CHECK(color_hash(ctx) == color_before && depth_hash(ctx) == depth_before);
    }

    /* Negative (mirroring) scales are valid even when nothing is visible. */
    s = good;
    s.scale.x = -1.0f;
    s.scale.y = -1.0f;
    CHECK(pixelica25d_draw_billboard(&canvas, persp, &s, behind, size) == PIXELICA_OK);
    CHECK_EQ_LONG(canvas.stats.draw_calls, 0);
    CHECK(color_hash(ctx) == color_before && depth_hash(ctx) == depth_before);

    pixelica_destroy(ctx);
}

static void test_frame_with_hud_single_present(void)
{
    static const uint8_t box_rows[4] = {0xF0u, 0x90u, 0x90u, 0xF0u};
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA2D_Sprite s;
    PIXELICA2D_Font font;
    PIXELICA2D_Rect hud_rect = {20, 8, 2, 2};
    PIXELICA_Vec3 pos = {0.0f, 0.0f, -3.0f};
    PIXELICA_Vec2 size = {4.0f, 4.0f};
    float vp[16];
    uint32_t frame;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    fill_textures();
    ortho_vp(vp);
    font.rows = box_rows;
    font.first_char = (uint16_t)'A';
    font.glyph_count = 1u;
    font.width = 4u;
    font.height = 4u;
    s = make_sprite(&g_solid_tex);

    for (frame = 1u; frame <= 2u; ++frame) {
        begin_scene(ctx, vp);
        CHECK(draw_wall(ctx, -5.0f) == PIXELICA_OK);
        CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
        CHECK(pixelica25d_draw_billboard(&canvas, vp, &s, pos, size) == PIXELICA_OK);
        CHECK_EQ_LONG(canvas.stats.draw_calls, 1);

        /* HUD: screen space, depth ignored even over the wall. */
        CHECK(pixelica2d_fill_rect(&canvas, &hud_rect, HUD, 255) == PIXELICA_OK);
        CHECK(pixelica2d_draw_text(&canvas, &font, "A", 1u, 1, 1, SPR, 255) == PIXELICA_OK);
        CHECK(px(ctx, 20, 8) == HUD && px(ctx, 21, 9) == HUD);
        CHECK_EQ_LONG(dp(ctx, 20, 8), DEPTH_MID_5);
        CHECK(px(ctx, 1, 1) == SPR && px(ctx, 4, 4) == SPR);
        CHECK(px(ctx, 2, 2) == BG && px(ctx, 3, 3) == BG);
        CHECK_EQ_LONG(dp(ctx, 1, 1), PIXELICA_DEPTH_CLEAR);
        CHECK(canvas.stats.draw_calls >= 2u);

        /* The 3D counters only know about the wall. */
        CHECK_EQ_LONG(pixelica_frame_stats(ctx).draw_calls, 1);

        CHECK_EQ_LONG(pal.presents, frame - 1u);
        CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
        CHECK_EQ_LONG(pal.presents, frame);
        CHECK(pal.last_pixels == pixelica_color_buffer(ctx));
        CHECK_EQ_LONG(pal.width, W);
        CHECK_EQ_LONG(pal.height, H);
        CHECK_EQ_LONG(pal.stride, W);
    }
    CHECK(pixelica_memory_guards_intact(ctx) != 0);
    pixelica_destroy(ctx);
}

int main(void)
{
    test_bind();
    test_bind_destroyed();
    test_bind_output_must_be_disjoint();
    test_bind_output_must_be_disjoint_heap();
    test_project_standard();
    test_project_perspective();
    test_project_invalid();
    test_occlusion();
    test_key_holes_and_alpha();
    test_billboard_rules();
    test_billboard_mirroring();
    test_billboard_scale_invalid();
    test_frame_with_hud_single_present();
    return pixelica_t_finish("test_hybrid");
}

/*
 * Pixelica - context lifecycle, initialization failures, caller-owned memory
 * and guard canaries, PAL present contract, and the fixed timestep.
 */
#include <stdlib.h>

#include "test_util.h"

#define RED ((uint16_t)0xF800)

typedef struct MockPal {
    int present_calls;
    int read_calls;
    int log_calls;
    int time_calls;
    bool present_result;
    uint32_t buttons;
    const uint16_t *last_pixels;
    uint16_t last_w, last_h;
    uint32_t last_stride;
    uint64_t hash_at_present;
} MockPal;

static uint64_t mock_time(void *ud)
{
    ((MockPal *)ud)->time_calls++;
    return 0;
}

static uint32_t mock_buttons(void *ud)
{
    MockPal *m = (MockPal *)ud;
    m->read_calls++;
    return m->buttons;
}

static bool mock_present(void *ud, const uint16_t *pixels, uint16_t w,
                         uint16_t h, uint32_t stride)
{
    MockPal *m = (MockPal *)ud;
    m->present_calls++;
    m->last_pixels = pixels;
    m->last_w = w;
    m->last_h = h;
    m->last_stride = stride;
    m->hash_at_present = pixelica_t_hash16(pixels, (size_t)w * (size_t)h);
    return m->present_result;
}

static void mock_log(void *ud, const char *message)
{
    (void)message;
    ((MockPal *)ud)->log_calls++;
}

static PIXELICA_PlatformOps mock_ops(MockPal *m)
{
    PIXELICA_PlatformOps ops;
    memset(m, 0, sizeof *m);
    m->present_result = true;
    ops.userdata = m;
    ops.time_us = mock_time;
    ops.read_buttons = mock_buttons;
    ops.present_rgb565 = mock_present;
    ops.log = mock_log;
    return ops;
}

typedef struct UpdateLog {
    int calls;
    int bad_dt;
    uint32_t last_buttons;
    float position; /* tiny deterministic "game state" */
} UpdateLog;

static void on_update(void *ud, float dt, uint32_t buttons)
{
    UpdateLog *u = (UpdateLog *)ud;
    u->calls++;
    if (dt != 1.0f / 60.0f) {
        u->bad_dt++;
    }
    u->last_buttons = buttons;
    u->position += ((buttons & PIXELICA_BTN_RIGHT) ? 3.0f : 1.0f) * dt;
}

/* ------------------------------------------------------------------------ */

static void test_create_validation(void)
{
    int dummy = 0;
    PIXELICA_Context *const poison = (PIXELICA_Context *)(void *)&dummy;
    PIXELICA_Context *ctx;
    PIXELICA_Config cfg;
    PIXELICA_PlatformOps ops;
    MockPal pal;
    size_t bytes = 123;
    int i;

    cfg = pixelica_t_config(64, 48);
    CHECK(pixelica_create(&cfg, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    ctx = poison;
    CHECK(pixelica_create(NULL, NULL, &ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(ctx == NULL);

    for (i = 0; i < 8; ++i) {
        cfg = pixelica_t_config(64, 48);
        switch (i) {
        case 0: cfg.width = 0; break;
        case 1: cfg.height = 0; break;
        case 2: cfg.width = (uint16_t)(PIXELICA_MAX_DIMENSION + 1u); break;
        case 3: cfg.height = (uint16_t)(PIXELICA_MAX_DIMENSION + 1u); break;
        case 4: cfg.max_draw_commands = 0; break;
        case 5: cfg.max_vertices_per_mesh = 0; break;
        case 6:
            cfg.frame_arena_bytes =
                (uint32_t)cfg.max_vertices_per_mesh * PIXELICA_ARENA_BYTES_PER_VERTEX - 1u;
            break;
        default: cfg.frame_arena_bytes = 0; break;
        }
        ctx = poison;
        CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_ERR_INVALID_ARG);
        CHECK(ctx == NULL);
        bytes = 123;
        CHECK(pixelica_memory_requirement(&cfg, &bytes) == PIXELICA_ERR_INVALID_ARG);
        CHECK(bytes == 0);
    }
    CHECK(pixelica_memory_requirement(NULL, &bytes) == PIXELICA_ERR_INVALID_ARG);
    cfg = pixelica_t_config(64, 48);
    CHECK(pixelica_memory_requirement(&cfg, NULL) == PIXELICA_ERR_INVALID_ARG);

    /* Boundary values are accepted. */
    cfg = pixelica_t_config((uint16_t)PIXELICA_MAX_DIMENSION, 1);
    cfg.frame_arena_bytes =
        (uint32_t)cfg.max_vertices_per_mesh * PIXELICA_ARENA_BYTES_PER_VERTEX;
    CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_OK);
    CHECK(ctx != NULL);
    pixelica_destroy(ctx);

    /* Incomplete PAL tables are rejected (and the failure is logged);
     * `log` itself is optional. */
    cfg = pixelica_t_config(64, 48);
    ops = mock_ops(&pal);
    ops.present_rgb565 = NULL;
    ctx = poison;
    CHECK(pixelica_create(&cfg, &ops, &ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(ctx == NULL);
    CHECK(pal.log_calls > 0);
    ops = mock_ops(&pal);
    ops.read_buttons = NULL;
    CHECK(pixelica_create(&cfg, &ops, &ctx) == PIXELICA_ERR_INVALID_ARG);
    ops = mock_ops(&pal);
    ops.time_us = NULL;
    CHECK(pixelica_create(&cfg, &ops, &ctx) == PIXELICA_ERR_INVALID_ARG);
    ops = mock_ops(&pal);
    ops.log = NULL;
    CHECK(pixelica_create(&cfg, &ops, &ctx) == PIXELICA_OK);
    pixelica_destroy(ctx);
}

static void test_create_in_buffer_and_guards(void)
{
    PIXELICA_Config cfg = pixelica_t_config(40, 30);
    const size_t pixels = 40u * 30u;
    size_t req = 0;
    unsigned char *block;
    unsigned char *mem;
    PIXELICA_Context *ctx = NULL;
    size_t color_off, depth_off;
    size_t i;
    int all_zero = 1, all_clear = 1;

    CHECK(pixelica_memory_requirement(&cfg, &req) == PIXELICA_OK);
    CHECK(req >= pixels * 4u + cfg.frame_arena_bytes);
    block = (unsigned char *)malloc(req + 1u);
    CHECK(block != NULL);
    if (block == NULL) {
        return;
    }
    mem = block + 1; /* deliberately misaligned */

    /* Too small -> NO_MEMORY, nothing created; NULL memory -> invalid. */
    ctx = (PIXELICA_Context *)(void *)block;
    CHECK(pixelica_create_in_buffer(&cfg, NULL, mem, req - 1u, &ctx) ==
          PIXELICA_ERR_NO_MEMORY);
    CHECK(ctx == NULL);
    CHECK(pixelica_create_in_buffer(&cfg, NULL, NULL, req, &ctx) ==
          PIXELICA_ERR_INVALID_ARG);

    CHECK(pixelica_create_in_buffer(&cfg, NULL, mem, req, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        free(block);
        return;
    }
    color_off = (size_t)((const unsigned char *)(const void *)pixelica_color_buffer(ctx) - block);
    depth_off = (size_t)((const unsigned char *)(const void *)pixelica_depth_buffer(ctx) - block);
    CHECK(color_off >= 1u && color_off + pixels * 2u <= req + 1u);
    CHECK(depth_off >= 1u && depth_off + pixels * 2u <= req + 1u);
    CHECK(color_off + pixels * 2u <= depth_off); /* non-overlapping */

    /* Deterministic initial contents. */
    for (i = 0; i < pixels; ++i) {
        if (pixelica_color_buffer(ctx)[i] != 0) {
            all_zero = 0;
        }
        if (pixelica_depth_buffer(ctx)[i] != PIXELICA_DEPTH_CLEAR) {
            all_clear = 0;
        }
    }
    CHECK(all_zero && all_clear);
    CHECK(pixelica_memory_guards_intact(ctx) == 1);

    /* The canary really detects a one-byte overrun / underrun. */
    block[color_off + pixels * 2u] ^= 0xFFu;
    CHECK(pixelica_memory_guards_intact(ctx) == 0);
    block[color_off + pixels * 2u] ^= 0xFFu;
    CHECK(pixelica_memory_guards_intact(ctx) == 1);
    block[color_off - 1u] ^= 0xFFu;
    CHECK(pixelica_memory_guards_intact(ctx) == 0);
    block[color_off - 1u] ^= 0xFFu;
    block[depth_off + pixels * 2u] ^= 0x01u;
    CHECK(pixelica_memory_guards_intact(ctx) == 0);
    block[depth_off + pixels * 2u] ^= 0x01u;
    CHECK(pixelica_memory_guards_intact(ctx) == 1);

    /* Full frame inside caller memory. */
    pixelica_begin_frame(ctx, RED);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK(pixelica_color_buffer(ctx)[pixels - 1u] == RED);
    CHECK(pixelica_memory_guards_intact(ctx) == 1);

    /* Destroy invalidates without freeing caller memory. */
    pixelica_destroy(ctx);
    CHECK(pixelica_color_buffer(ctx) == NULL);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_ERR_INVALID_ARG);
    pixelica_destroy(ctx); /* double destroy of caller-memory context is harmless */
    free(block);
}

static void test_frame_protocol_and_present(void)
{
    static uint16_t expected[32 * 24];
    PIXELICA_Config cfg = pixelica_t_config(32, 24);
    PIXELICA_PlatformOps ops;
    MockPal pal;
    PIXELICA_Context *ctx = NULL;
    size_t i;
    int f;

    ops = mock_ops(&pal);
    CHECK(pixelica_create(&cfg, &ops, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        return;
    }
    for (i = 0; i < 32u * 24u; ++i) {
        expected[i] = RED;
    }

    /* end without begin: rejected, nothing presented. */
    CHECK(pixelica_end_frame(ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pal.present_calls == 0);

    /* One frame -> exactly one synchronous present of the color buffer. */
    pixelica_begin_frame(ctx, RED);
    CHECK(pal.present_calls == 0);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK(pal.present_calls == 1);
    CHECK(pal.last_pixels == pixelica_color_buffer(ctx));
    CHECK(pal.last_w == 32 && pal.last_h == 24 && pal.last_stride == 32u);
    CHECK(pal.hash_at_present == pixelica_t_hash16(expected, 32u * 24u));

    /* Present failure surfaces as PIXELICA_ERR_PLATFORM; the frame is closed
     * and the next one works. */
    pal.present_result = false;
    pixelica_begin_frame(ctx, 0);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_ERR_PLATFORM);
    CHECK(pixelica_frame_stats(ctx).last_error == PIXELICA_ERR_PLATFORM);
    CHECK(pal.present_calls == 2);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pal.present_calls == 2);
    pal.present_result = true;
    for (f = 0; f < 10; ++f) {
        pixelica_begin_frame(ctx, (uint16_t)f);
        CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    }
    CHECK(pal.present_calls == 12);

    /* Restarting a frame with begin_frame does not present. */
    pixelica_begin_frame(ctx, 1);
    pixelica_begin_frame(ctx, 2);
    CHECK(pal.present_calls == 12);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK(pal.present_calls == 13);

    /* The core never reads the clock itself in G0 (driver's job). */
    CHECK(pal.time_calls == 0);
    CHECK(pixelica_memory_guards_intact(ctx) == 1);
    pixelica_destroy(ctx);
}

static void test_stats_reset(void)
{
    static const PIXELICA_Vec3 v[3] = {
        {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}};
    static const uint16_t idx[3] = {0, 1, 2};
    float id[16];
    PIXELICA_Config cfg = pixelica_t_config(16, 16);
    PIXELICA_Context *ctx = NULL;
    PIXELICA_Mesh m;
    PIXELICA_FrameStats s;

    CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        return;
    }
    pixelica_mat4_identity(id);
    m.vertices = v;
    m.vertex_count = 3;
    m.indices = idx;
    m.index_count = 3;
    m.color = RED;
    pixelica_begin_frame(ctx, 0);
    CHECK(pixelica_draw_mesh(ctx, &m, id) == PIXELICA_OK);
    s = pixelica_frame_stats(ctx);
    CHECK(s.draw_calls == 1 && s.triangles_submitted == 1);
    CHECK(s.triangles_rasterized == 1 && s.pixels_written > 0);
    /* 3 transformed vertices of scratch, within the documented budget. */
    CHECK(s.arena_high_water >= 3u * 16u &&
          s.arena_high_water <= 3u * PIXELICA_ARENA_BYTES_PER_VERTEX);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK); /* headless: no PAL, still OK */
    pixelica_begin_frame(ctx, 0);
    s = pixelica_frame_stats(ctx);
    CHECK(s.draw_calls == 0 && s.pixels_written == 0 &&
          s.last_error == PIXELICA_OK);
    pixelica_destroy(ctx);
}

static void test_null_safety(void)
{
    float m[16];
    PIXELICA_FrameStats s;
    UpdateLog u;

    memset(&u, 0, sizeof u);
    pixelica_mat4_identity(m);
    pixelica_destroy(NULL);
    pixelica_begin_frame(NULL, 0);
    pixelica_set_view_projection(NULL, m);
    pixelica_set_cull_mode(NULL, PIXELICA_CULL_NONE);
    pixelica_update(NULL, 1000000u, on_update, &u);
    CHECK(u.calls == 0);
    CHECK(pixelica_end_frame(NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_color_buffer(NULL) == NULL);
    CHECK(pixelica_depth_buffer(NULL) == NULL);
    CHECK(pixelica_memory_guards_intact(NULL) == 0);
    s = pixelica_frame_stats(NULL);
    CHECK(s.draw_calls == 0 && s.pixels_written == 0);
}

/* Creates a context with a mock PAL for one timestep scenario. */
static PIXELICA_Context *timestep_ctx(MockPal *pal, UpdateLog *u)
{
    PIXELICA_Config cfg = pixelica_t_config(8, 8);
    PIXELICA_PlatformOps ops = mock_ops(pal);
    PIXELICA_Context *ctx = NULL;

    memset(u, 0, sizeof *u);
    CHECK(pixelica_create(&cfg, &ops, &ctx) == PIXELICA_OK);
    return ctx;
}

static void test_fixed_timestep(void)
{
    MockPal pal;
    UpdateLog u;
    PIXELICA_Context *ctx;
    int i;

    /* 16667 us is a hair over 1/60 s: exactly one step per call. */
    ctx = timestep_ctx(&pal, &u);
    if (ctx == NULL) {
        return;
    }
    pal.buttons = PIXELICA_BTN_A | PIXELICA_BTN_LEFT;
    for (i = 0; i < 60; ++i) {
        pixelica_update(ctx, 16667u, on_update, &u);
    }
    CHECK(u.calls == 60);
    CHECK(u.bad_dt == 0);
    CHECK(pal.read_calls == 60); /* input sampled once per call */
    CHECK(u.last_buttons == (PIXELICA_BTN_A | PIXELICA_BTN_LEFT));
    CHECK(pal.present_calls == 0);
    pixelica_destroy(ctx);

    /* 16666 us is a hair under: exact integer accounting, no drift.
     * 60 calls -> floor(59.996) = 59 steps; 600 calls -> 599. */
    ctx = timestep_ctx(&pal, &u);
    pixelica_update(ctx, 16666u, on_update, &u);
    CHECK(u.calls == 0);
    CHECK(pal.read_calls == 1); /* sampled even when no step runs */
    for (i = 1; i < 60; ++i) {
        pixelica_update(ctx, 16666u, on_update, &u);
    }
    CHECK(u.calls == 59);
    for (i = 60; i < 600; ++i) {
        pixelica_update(ctx, 16666u, on_update, &u);
    }
    CHECK(u.calls == 599);
    pixelica_destroy(ctx);

    /* Spiral-of-death cap: 1 s -> 4 steps, excess dropped. */
    ctx = timestep_ctx(&pal, &u);
    pixelica_update(ctx, 1000000u, on_update, &u);
    CHECK(u.calls == 4);
    pixelica_update(ctx, 0u, on_update, &u);
    CHECK(u.calls == 4);
    pixelica_destroy(ctx);

    /* Overflow safety: UINT64_MAX is clamped, then accounting resumes. */
    ctx = timestep_ctx(&pal, &u);
    pixelica_update(ctx, UINT64_MAX, on_update, &u);
    CHECK(u.calls == 4);
    pixelica_update(ctx, 0u, on_update, &u);
    CHECK(u.calls == 4);
    pixelica_update(ctx, 16667u, on_update, &u);
    CHECK(u.calls == 5);
    pixelica_destroy(ctx);

    /* Exactly the cap runs in full; above it whole steps are dropped but
     * the sub-step remainder is kept: 83334 us = 5 steps + 40 units, so
     * a following 16666 us (999960 units) completes exactly one step. */
    ctx = timestep_ctx(&pal, &u);
    pixelica_update(ctx, 66667u, on_update, &u);
    CHECK(u.calls == 4);
    pixelica_destroy(ctx);
    ctx = timestep_ctx(&pal, &u);
    pixelica_update(ctx, 83334u, on_update, &u);
    CHECK(u.calls == 4);
    pixelica_update(ctx, 16666u, on_update, &u);
    CHECK(u.calls == 5);
    pixelica_destroy(ctx);

    /* Buttons are re-sampled every call and shared by that call's steps. */
    ctx = timestep_ctx(&pal, &u);
    pal.buttons = PIXELICA_BTN_UP;
    pixelica_update(ctx, 33334u, on_update, &u);
    CHECK(u.calls == 2 && u.last_buttons == PIXELICA_BTN_UP);
    pal.buttons = PIXELICA_BTN_START;
    pixelica_update(ctx, 16667u, on_update, &u);
    CHECK(u.calls == 3 && u.last_buttons == PIXELICA_BTN_START);
    CHECK(pal.read_calls == 2);

    /* NULL callback: no input read and no time accumulated. */
    pixelica_update(ctx, 1000000u, NULL, NULL);
    CHECK(pal.read_calls == 2);
    pixelica_update(ctx, 0u, on_update, &u);
    CHECK(u.calls == 3);
    pixelica_destroy(ctx);
}

static void test_timestep_frame_rate_independence(void)
{
    MockPal pal;
    UpdateLog a, b;
    PIXELICA_Context *ctx;
    int i;

    /* Same total time (2,000,040 us) and inputs at 60 Hz vs 20 Hz frame
     * pacing: identical step count and bit-identical game state. */
    ctx = timestep_ctx(&pal, &a);
    pal.buttons = PIXELICA_BTN_RIGHT;
    for (i = 0; i < 120; ++i) {
        pixelica_update(ctx, 16667u, on_update, &a);
    }
    pixelica_destroy(ctx);

    ctx = timestep_ctx(&pal, &b);
    pal.buttons = PIXELICA_BTN_RIGHT;
    for (i = 0; i < 40; ++i) {
        pixelica_update(ctx, 50001u, on_update, &b);
    }
    pixelica_destroy(ctx);

    CHECK(a.calls == 120 && b.calls == 120);
    CHECK(memcmp(&a.position, &b.position, sizeof a.position) == 0);
    CHECK_NEAR(a.position, 6.0, 1e-3);
}

static void test_headless_update(void)
{
    PIXELICA_Config cfg = pixelica_t_config(8, 8);
    PIXELICA_Context *ctx = NULL;
    UpdateLog u;

    memset(&u, 0, sizeof u);
    u.last_buttons = 0xFFFFFFFFu;
    CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_OK);
    pixelica_update(ctx, 16667u, on_update, &u);
    CHECK(u.calls == 1);
    CHECK(u.last_buttons == 0u); /* no PAL: no buttons */
    pixelica_destroy(ctx);
}

int main(void)
{
    test_create_validation();
    test_create_in_buffer_and_guards();
    test_frame_protocol_and_present();
    test_stats_reset();
    test_null_safety();
    test_fixed_timestep();
    test_timestep_frame_rate_independence();
    test_headless_update();
    return pixelica_t_finish("test_core");
}

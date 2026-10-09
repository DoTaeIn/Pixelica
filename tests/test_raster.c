/*
 * Pixelica - rasterizer, depth, culling and clipping behaviour checked on
 * real color/depth buffers (headless, no PAL).
 *
 * Most tests use the identity view-projection, so mesh coordinates are NDC
 * directly and scr() converts screen pixel coordinates (y down) to NDC. With
 * power-of-two targets these conversions are exact in float, which lets the
 * tests assert exact pixel sets for the top-left fill rule.
 */
#include <float.h>

#include "test_util.h"

#define BG ((uint16_t)0x0000)
#define RED ((uint16_t)0xF800)
#define GREEN ((uint16_t)0x07E0)
#define BLUE ((uint16_t)0x001F)
#define PI_F 3.14159265358979323846f

#define MAX_PIXELS (256 * 192)

typedef struct Target {
    PIXELICA_Context *ctx;
    uint16_t w, h;
} Target;

static uint16_t g_snap_a[MAX_PIXELS];
static uint16_t g_snap_b[MAX_PIXELS];
static uint16_t g_snap_c[MAX_PIXELS];
static uint16_t g_snap_d[MAX_PIXELS];

static const float k_identity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};

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
    /* Canary: no write landed outside color/depth/arena. */
    CHECK(pixelica_memory_guards_intact(t->ctx) == 1);
    pixelica_destroy(t->ctx);
    t->ctx = NULL;
}

static size_t npix(const Target *t)
{
    return (size_t)t->w * (size_t)t->h;
}

static void snap_color(const Target *t, uint16_t *dst)
{
    memcpy(dst, pixelica_color_buffer(t->ctx), npix(t) * sizeof(uint16_t));
}

static void snap_depth(const Target *t, uint16_t *dst)
{
    memcpy(dst, pixelica_depth_buffer(t->ctx), npix(t) * sizeof(uint16_t));
}

static uint32_t count(const Target *t, uint16_t color)
{
    return pixelica_t_count_color(t->ctx, t->w, t->h, color);
}

static uint16_t color_at(const Target *t, int x, int y)
{
    return pixelica_t_color_at(t->ctx, t->w, x, y);
}

static uint16_t depth_at(const Target *t, int x, int y)
{
    return pixelica_t_depth_at(t->ctx, t->w, x, y);
}

static uint64_t frame_hash(const Target *t)
{
    return pixelica_t_hash16(pixelica_color_buffer(t->ctx), npix(t)) ^
           (pixelica_t_hash16(pixelica_depth_buffer(t->ctx), npix(t)) * 31u);
}

/* Screen pixel coordinates (y down) -> NDC for the identity camera. */
static PIXELICA_Vec3 scr(const Target *t, float sx, float sy, float z)
{
    PIXELICA_Vec3 v;
    v.x = sx / (float)t->w * 2.0f - 1.0f;
    v.y = 1.0f - sy / (float)t->h * 2.0f;
    v.z = z;
    return v;
}

static PIXELICA_Vec3 vec3(float x, float y, float z)
{
    PIXELICA_Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static PIXELICA_Status draw_mesh(const Target *t, const PIXELICA_Vec3 *v, uint32_t vc,
                            const uint16_t *idx, uint32_t ic, uint16_t color,
                            const float model[16])
{
    PIXELICA_Mesh m;
    m.vertices = v;
    m.vertex_count = vc;
    m.indices = idx;
    m.index_count = ic;
    m.color = color;
    return pixelica_draw_mesh(t->ctx, &m, model);
}

static PIXELICA_Status draw_tri(const Target *t, PIXELICA_Vec3 a, PIXELICA_Vec3 b,
                           PIXELICA_Vec3 c, uint16_t color)
{
    static const uint16_t idx[3] = {0, 1, 2};
    PIXELICA_Vec3 v[3];
    v[0] = a;
    v[1] = b;
    v[2] = c;
    return draw_mesh(t, v, 3, idx, 3, color, k_identity);
}

static PIXELICA_Status draw_tri_px(const Target *t, float ax, float ay, float bx,
                              float by, float cx, float cy, float z,
                              uint16_t color)
{
    return draw_tri(t, scr(t, ax, ay, z), scr(t, bx, by, z),
                    scr(t, cx, cy, z), color);
}

/* Reorders to front-facing: visually counter-clockwise, which in y-down
 * screen coordinates is a negative signed area. */
static PIXELICA_Status draw_tri_px_front(const Target *t, float ax, float ay,
                                    float bx, float by, float cx, float cy,
                                    float z, uint16_t color)
{
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area > 0.0f) {
        return draw_tri_px(t, ax, ay, cx, cy, bx, by, z, color);
    }
    return draw_tri_px(t, ax, ay, bx, by, cx, cy, z, color);
}

static void draw_rect_px(const Target *t, float x0, float y0, float x1,
                         float y1, float z, uint16_t color)
{
    CHECK(draw_tri_px_front(t, x0, y0, x1, y1, x1, y0, z, color) == PIXELICA_OK);
    CHECK(draw_tri_px_front(t, x0, y0, x0, y1, x1, y1, z, color) == PIXELICA_OK);
}

/* World-space square in the plane z, counter-clockwise seen from +Z. */
static PIXELICA_Status draw_square_world(const Target *t, float half, float z,
                                    uint16_t color)
{
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    PIXELICA_Vec3 v[4];
    v[0] = vec3(-half, -half, z);
    v[1] = vec3(half, -half, z);
    v[2] = vec3(half, half, z);
    v[3] = vec3(-half, half, z);
    return draw_mesh(t, v, 4, idx, 6, color, k_identity);
}

/* ------------------------------------------------------------------------ */

static void test_top_left_rule(void)
{
    Target t;
    const float x0 = 2.5f, y0 = 4.5f, x1 = 10.5f, y1 = 8.5f;
    uint32_t overlap = 0, mismatch = 0;
    uint32_t ca, cb;
    int x, y;

    if (!open_target(&t, 32, 32)) {
        return;
    }

    /* Rectangle whose edges pass exactly through pixel centers, split along
     * a diagonal that also passes through centers. Top/left edges own the
     * centers, bottom/right do not: exactly cols 2..9, rows 4..7. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri_px_front(&t, x0, y0, x1, y1, x1, y0, 0.0f, RED) == PIXELICA_OK);
    snap_color(&t, g_snap_a);
    ca = count(&t, RED);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri_px_front(&t, x0, y0, x0, y1, x1, y1, 0.0f, RED) == PIXELICA_OK);
    snap_color(&t, g_snap_b);
    cb = count(&t, RED);

    for (y = 0; y < 32; ++y) {
        for (x = 0; x < 32; ++x) {
            size_t i = (size_t)y * 32u + (size_t)x;
            int a = g_snap_a[i] == RED;
            int b = g_snap_b[i] == RED;
            int inside = x >= 2 && x <= 9 && y >= 4 && y <= 7;
            if (a && b) {
                ++overlap;
            }
            if ((a || b) != inside) {
                ++mismatch;
            }
        }
    }
    CHECK_EQ_LONG(overlap, 0);
    CHECK_EQ_LONG(mismatch, 0);
    CHECK_EQ_LONG(ca + cb, 32);
    CHECK(ca > 0 && cb > 0);

    /* A rectangle exactly matching the target covers every pixel once. */
    pixelica_begin_frame(t.ctx, BG);
    draw_rect_px(&t, 0.0f, 0.0f, 32.0f, 32.0f, 0.0f, GREEN);
    CHECK_EQ_LONG(count(&t, GREEN), 32 * 32);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 32 * 32);

    close_target(&t);
}

static void test_fan_watertight(void)
{
    Target t;
    const float cx = 16.3f, cy = 15.7f, radius = 11.0f;
    float px[8], py[8];
    uint32_t sum = 0, uni, bad_inside = 0, bad_outside = 0;
    int k, x, y;

    if (!open_target(&t, 32, 32)) {
        return;
    }
    for (k = 0; k < 8; ++k) {
        float ang = (float)(k * 45 + 10) * PI_F / 180.0f;
        px[k] = cx + radius * cosf(ang);
        py[k] = cy + radius * sinf(ang);
    }

    /* Each fan triangle alone; then all together. If no pixel is shared,
     * the individual counts sum to the union count. */
    for (k = 0; k < 8; ++k) {
        int n = (k + 1) % 8;
        uint32_t c;
        pixelica_begin_frame(t.ctx, BG);
        CHECK(draw_tri_px_front(&t, cx, cy, px[k], py[k], px[n], py[n], 0.0f,
                                RED) == PIXELICA_OK);
        c = count(&t, RED);
        CHECK(c > 0);
        sum += c;
    }
    pixelica_begin_frame(t.ctx, BG);
    for (k = 0; k < 8; ++k) {
        int n = (k + 1) % 8;
        CHECK(draw_tri_px_front(&t, cx, cy, px[k], py[k], px[n], py[n], 0.0f,
                                RED) == PIXELICA_OK);
    }
    uni = count(&t, RED);
    CHECK_EQ_LONG(sum, uni);

    /* No gaps: every center clearly inside the octagon is covered, every
     * center clearly outside is not (0.1 px margin absorbs snapping). */
    for (y = 0; y < 32; ++y) {
        for (x = 0; x < 32; ++x) {
            float sx = (float)x + 0.5f, sy = (float)y + 0.5f;
            int all_in = 1, any_out = 0;
            for (k = 0; k < 8; ++k) {
                int n = (k + 1) % 8;
                float ex = px[n] - px[k], ey = py[n] - py[k];
                float len = sqrtf(ex * ex + ey * ey);
                /* Center of the octagon gives the inside sign. */
                float s_c = ex * (cy - py[k]) - ey * (cx - px[k]);
                float s_p = (ex * (sy - py[k]) - ey * (sx - px[k])) / len;
                if (s_c < 0.0f) {
                    s_p = -s_p;
                }
                if (s_p < 0.1f) {
                    all_in = 0;
                }
                if (s_p < -0.1f) {
                    any_out = 1;
                }
            }
            if (all_in && color_at(&t, x, y) != RED) {
                ++bad_inside;
            }
            if (any_out && color_at(&t, x, y) != BG) {
                ++bad_outside;
            }
        }
    }
    CHECK_EQ_LONG(bad_inside, 0);
    CHECK_EQ_LONG(bad_outside, 0);
    close_target(&t);
}

static void test_degenerate(void)
{
    Target t;
    PIXELICA_FrameStats s;

    if (!open_target(&t, 32, 32)) {
        return;
    }
    pixelica_begin_frame(t.ctx, BG);
    /* Collinear, single point, and a sliver between pixel-center rows. */
    CHECK(draw_tri_px(&t, 2.0f, 2.0f, 10.0f, 10.0f, 20.0f, 20.0f, 0.0f, RED) ==
          PIXELICA_OK);
    CHECK(draw_tri_px(&t, 7.3f, 7.3f, 7.3f, 7.3f, 7.3f, 7.3f, 0.0f, RED) ==
          PIXELICA_OK);
    CHECK(draw_tri_px_front(&t, 2.6f, 2.1f, 9.4f, 2.1f, 9.4f, 2.4f, 0.0f,
                            RED) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 0);
    s = pixelica_frame_stats(t.ctx);
    CHECK_EQ_LONG(s.pixels_written, 0);
    CHECK_EQ_LONG(s.triangles_submitted, 3);

    /* A sub-pixel triangle containing exactly one center. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri_px_front(&t, 5.2f, 5.2f, 5.9f, 5.3f, 5.3f, 5.9f, 0.0f,
                            RED) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 1);
    CHECK(color_at(&t, 5, 5) == RED);
    close_target(&t);
}

static void test_offscreen_and_huge(void)
{
    Target t;
    PIXELICA_FrameStats s;

    if (!open_target(&t, 256, 192)) {
        return;
    }

    /* Entirely left of the screen, and entirely beyond the far plane. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri(&t, vec3(-3.0f, -0.5f, 0.0f), vec3(-2.0f, -0.5f, 0.0f),
                   vec3(-2.5f, 0.5f, 0.0f), RED) == PIXELICA_OK);
    CHECK(draw_tri(&t, vec3(-0.5f, -0.5f, 1.5f), vec3(0.5f, -0.5f, 1.5f),
                   vec3(0.0f, 0.5f, 1.5f), RED) == PIXELICA_OK);
    s = pixelica_frame_stats(t.ctx);
    CHECK_EQ_LONG(s.triangles_rejected, 2);
    CHECK_EQ_LONG(s.pixels_written, 0);
    CHECK_EQ_LONG(count(&t, RED), 0);

    /* Triangle 100x larger than the screen: clipped by all four side planes,
     * covers every pixel exactly. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri(&t, vec3(-100.0f, -100.0f, 0.0f),
                   vec3(300.0f, -100.0f, 0.0f), vec3(-100.0f, 300.0f, 0.0f),
                   RED) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 256 * 192);
    s = pixelica_frame_stats(t.ctx);
    CHECK_EQ_LONG(s.triangles_clipped, 1);
    CHECK_EQ_LONG(s.pixels_written, 256 * 192);

    /* +-1e6 wide band: rows with NDC |y| < 0.5 (rows 48..143) fully covered,
     * nothing else. Exercises the fixed-point overflow contract. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri(&t, vec3(-1.0e6f, -0.5f, 0.0f), vec3(1.0e6f, -0.5f, 0.0f),
                   vec3(0.0f, 0.5f, 0.0f), RED) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 96 * 256);
    CHECK(color_at(&t, 0, 48) == RED && color_at(&t, 255, 143) == RED);
    CHECK(color_at(&t, 128, 47) == BG && color_at(&t, 128, 144) == BG);
    close_target(&t);
}

/* Finite vertices up to +-FLT_MAX must clip correctly: plane distances such
 * as x + w and their differences exceed the float range, so a float clipper
 * produces inf/NaN intersections and draws nothing. */
static void test_flt_max_clipping(void)
{
    static const float sizes[3] = {100.0f, 1.0e20f, FLT_MAX};
    Target t;
    PIXELICA_FrameStats s;
    int i;

    if (!open_target(&t, 64, 48)) {
        return;
    }

    /* CCW triangle far larger than the screen: covers every pixel. */
    for (i = 0; i < 3; ++i) {
        const float h = sizes[i];

        pixelica_begin_frame(t.ctx, BG);
        CHECK(draw_tri(&t, vec3(-h, -h, 0.0f), vec3(h, -h, 0.0f),
                       vec3(0.0f, h, 0.0f), RED) == PIXELICA_OK);
        s = pixelica_frame_stats(t.ctx);
        CHECK_EQ_LONG(count(&t, RED), 64 * 48);
        CHECK_EQ_LONG(s.pixels_written, 64 * 48);
        CHECK_EQ_LONG(s.triangles_clipped, 1);
        CHECK(depth_at(&t, 0, 0) <= PIXELICA_DEPTH_MAX_DRAWABLE);
        CHECK(depth_at(&t, 63, 47) <= PIXELICA_DEPTH_MAX_DRAWABLE);
    }

    /* +-FLT_MAX wide band: exactly rows with NDC |y| < 0.5 (rows 12..35),
     * same pixel set as a moderately wide band. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri(&t, vec3(-FLT_MAX, -0.5f, 0.0f), vec3(FLT_MAX, -0.5f, 0.0f),
                   vec3(0.0f, 0.5f, 0.0f), RED) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 24 * 64);
    CHECK(color_at(&t, 0, 12) == RED && color_at(&t, 63, 35) == RED);
    CHECK(color_at(&t, 32, 11) == BG && color_at(&t, 32, 36) == BG);

    /* Back-facing (CW) extreme triangle is still culled, not drawn. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri(&t, vec3(-FLT_MAX, -FLT_MAX, 0.0f), vec3(0.0f, FLT_MAX, 0.0f),
                   vec3(FLT_MAX, -FLT_MAX, 0.0f), RED) == PIXELICA_OK);
    s = pixelica_frame_stats(t.ctx);
    CHECK_EQ_LONG(s.triangles_culled, 1);
    CHECK_EQ_LONG(count(&t, RED), 0);
    close_target(&t);
}

static void test_winding_and_culling(void)
{
    Target t;
    uint32_t front_count;

    if (!open_target(&t, 32, 32)) {
        return;
    }
    /* (4,4)->(4,20)->(20,20) is visually counter-clockwise: front. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri_px(&t, 4.0f, 4.0f, 4.0f, 20.0f, 20.0f, 20.0f, 0.0f, RED) ==
          PIXELICA_OK);
    front_count = count(&t, RED);
    CHECK(front_count > 100);
    snap_color(&t, g_snap_a);

    /* Reversed winding is back-facing: culled by default. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri_px(&t, 4.0f, 4.0f, 20.0f, 20.0f, 4.0f, 20.0f, 0.0f, RED) ==
          PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 0);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).triangles_culled, 1);

    /* Culling disabled: both windings give the identical pixel set. */
    pixelica_set_cull_mode(t.ctx, PIXELICA_CULL_NONE);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri_px(&t, 4.0f, 4.0f, 20.0f, 20.0f, 4.0f, 20.0f, 0.0f, RED) ==
          PIXELICA_OK);
    snap_color(&t, g_snap_b);
    CHECK(memcmp(g_snap_a, g_snap_b, npix(&t) * sizeof(uint16_t)) == 0);

    /* Invalid modes are ignored. */
    pixelica_set_cull_mode(t.ctx, PIXELICA_CULL_BACK);
    pixelica_set_cull_mode(t.ctx, (PIXELICA_CullMode)42);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_tri_px(&t, 4.0f, 4.0f, 20.0f, 20.0f, 4.0f, 20.0f, 0.0f, RED) ==
          PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 0);
    close_target(&t);
}

static void test_depth_ordering(void)
{
    Target t;
    int order;
    float persp[16];

    if (!open_target(&t, 32, 32)) {
        return;
    }

    /* Overlapping flat rectangles, both draw orders: identical results,
     * nearer (smaller NDC z) wins, depth encodes (z+1)/2 * 65534. */
    for (order = 0; order < 2; ++order) {
        pixelica_begin_frame(t.ctx, BG);
        if (order == 0) {
            draw_rect_px(&t, 4.0f, 4.0f, 20.0f, 20.0f, -0.5f, RED);
            draw_rect_px(&t, 12.0f, 12.0f, 28.0f, 28.0f, 0.5f, BLUE);
            snap_color(&t, g_snap_a);
            snap_depth(&t, g_snap_c);
        } else {
            draw_rect_px(&t, 12.0f, 12.0f, 28.0f, 28.0f, 0.5f, BLUE);
            draw_rect_px(&t, 4.0f, 4.0f, 20.0f, 20.0f, -0.5f, RED);
            snap_color(&t, g_snap_b);
            snap_depth(&t, g_snap_d);
        }
        CHECK(color_at(&t, 16, 16) == RED);
        CHECK(color_at(&t, 8, 8) == RED);
        CHECK(color_at(&t, 24, 24) == BLUE);
        CHECK(color_at(&t, 2, 2) == BG);
        CHECK_EQ_LONG(depth_at(&t, 16, 16), 16384);
        CHECK_EQ_LONG(depth_at(&t, 24, 24), 49151);
        CHECK_EQ_LONG(depth_at(&t, 2, 2), PIXELICA_DEPTH_CLEAR);
    }
    CHECK(memcmp(g_snap_a, g_snap_b, npix(&t) * sizeof(uint16_t)) == 0);
    CHECK(memcmp(g_snap_c, g_snap_d, npix(&t) * sizeof(uint16_t)) == 0);

    /* Equal depth: strict less-than keeps the first fragment. */
    pixelica_begin_frame(t.ctx, BG);
    draw_rect_px(&t, 4.0f, 4.0f, 20.0f, 20.0f, 0.0f, RED);
    draw_rect_px(&t, 4.0f, 4.0f, 20.0f, 20.0f, 0.0f, GREEN);
    CHECK_EQ_LONG(count(&t, RED), 16 * 16);
    CHECK_EQ_LONG(count(&t, GREEN), 0);

    /* Interpenetrating: red tilts from z=-0.5 (left) to +0.5 (right) through
     * flat blue z=0, so the visible color switches at x=16 in both orders. */
    for (order = 0; order < 2; ++order) {
        int i;
        pixelica_begin_frame(t.ctx, BG);
        for (i = 0; i < 2; ++i) {
            if ((i == 0) == (order == 0)) {
                CHECK(draw_tri(&t, scr(&t, 4.0f, 8.0f, -0.5f),
                               scr(&t, 4.0f, 24.0f, -0.5f),
                               scr(&t, 28.0f, 24.0f, 0.5f), RED) == PIXELICA_OK);
                CHECK(draw_tri(&t, scr(&t, 4.0f, 8.0f, -0.5f),
                               scr(&t, 28.0f, 24.0f, 0.5f),
                               scr(&t, 28.0f, 8.0f, 0.5f), RED) == PIXELICA_OK);
            } else {
                draw_rect_px(&t, 4.0f, 8.0f, 28.0f, 24.0f, 0.0f, BLUE);
            }
        }
        CHECK(color_at(&t, 10, 16) == RED);
        CHECK(color_at(&t, 14, 12) == RED);
        CHECK(color_at(&t, 18, 20) == BLUE);
        CHECK(color_at(&t, 22, 16) == BLUE);
    }

    /* Perspective camera: square at distance 2 in front of one at 4. */
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 0.1f, 100.0f) ==
          PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    for (order = 0; order < 2; ++order) {
        pixelica_begin_frame(t.ctx, BG);
        if (order == 0) {
            CHECK(draw_square_world(&t, 0.5f, -2.0f, RED) == PIXELICA_OK);
            CHECK(draw_square_world(&t, 2.0f, -4.0f, BLUE) == PIXELICA_OK);
            snap_color(&t, g_snap_a);
            snap_depth(&t, g_snap_c);
        } else {
            CHECK(draw_square_world(&t, 2.0f, -4.0f, BLUE) == PIXELICA_OK);
            CHECK(draw_square_world(&t, 0.5f, -2.0f, RED) == PIXELICA_OK);
            snap_color(&t, g_snap_b);
            snap_depth(&t, g_snap_d);
        }
        /* Red projects to [12,20]^2, blue to [8,24]^2 on 32x32. */
        CHECK(color_at(&t, 16, 16) == RED);
        CHECK(color_at(&t, 10, 10) == BLUE);
        CHECK(color_at(&t, 4, 4) == BG);
        CHECK(depth_at(&t, 16, 16) < depth_at(&t, 10, 10));
    }
    CHECK(memcmp(g_snap_a, g_snap_b, npix(&t) * sizeof(uint16_t)) == 0);
    CHECK(memcmp(g_snap_c, g_snap_d, npix(&t) * sizeof(uint16_t)) == 0);
    close_target(&t);
}

static void test_near_plane_crossing(void)
{
    Target t;
    float persp[16];
    static const uint16_t idx[3] = {0, 2, 1}; /* A, C, B: front-facing */
    PIXELICA_Vec3 v[3];
    uint32_t above = 0;
    int x, y;

    if (!open_target(&t, 64, 64)) {
        return;
    }
    /* Floor triangle y=-1: A, B 2 units ahead, C 2 units BEHIND the camera
     * (w < 0). near = 1.5, so the visible part is the strip between the
     * A-B edge (NDC y = -0.5, screen row 48.0) and the near-plane cut
     * (NDC y = -1/1.5, screen row 53.33). An unclipped projection of C
     * would land at NDC (0, +0.5) and cover the screen center instead. */
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 1.5f, 100.0f) ==
          PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    v[0] = vec3(-1.0f, -1.0f, -2.0f);
    v[1] = vec3(1.0f, -1.0f, -2.0f);
    v[2] = vec3(0.0f, -1.0f, 2.0f);

    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_mesh(&t, v, 3, idx, 3, RED, k_identity) == PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).triangles_clipped, 1);
    CHECK(color_at(&t, 32, 48) == RED);
    CHECK(color_at(&t, 32, 52) == RED);
    CHECK(color_at(&t, 16, 50) == RED);
    CHECK(color_at(&t, 32, 47) == BG);  /* above the far edge */
    CHECK(color_at(&t, 32, 53) == BG);  /* cut by the near plane */
    CHECK(color_at(&t, 32, 60) == BG);
    CHECK(color_at(&t, 32, 40) == BG);  /* naive projection would cover */
    CHECK(color_at(&t, 12, 50) == BG);
    for (y = 0; y < 48; ++y) {
        for (x = 0; x < 64; ++x) {
            if (color_at(&t, x, y) != BG) {
                ++above;
            }
        }
    }
    CHECK_EQ_LONG(above, 0);
    /* Closer floor (lower on screen) has smaller depth. */
    CHECK(depth_at(&t, 32, 52) < depth_at(&t, 32, 48));
    CHECK(depth_at(&t, 32, 48) <= PIXELICA_DEPTH_MAX_DRAWABLE);
    close_target(&t);
}

static void test_behind_camera(void)
{
    Target t;
    float persp[16];
    float neg_w[16];
    static const uint16_t idx[3] = {0, 1, 2};
    PIXELICA_Vec3 v[3];

    if (!open_target(&t, 64, 64)) {
        return;
    }
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 0.1f, 100.0f) ==
          PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    pixelica_set_cull_mode(t.ctx, PIXELICA_CULL_NONE);

    /* Entirely behind the camera (all w < 0): nothing. */
    pixelica_begin_frame(t.ctx, BG);
    v[0] = vec3(-1.0f, -1.0f, 2.0f);
    v[1] = vec3(1.0f, -1.0f, 2.0f);
    v[2] = vec3(0.0f, 1.0f, 2.0f);
    CHECK(draw_mesh(&t, v, 3, idx, 3, RED, k_identity) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 0);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).triangles_rejected, 1);

    /* One vertex exactly at the eye plane (w == 0): clipped, bounded. */
    pixelica_begin_frame(t.ctx, BG);
    v[0] = vec3(0.0f, -1.0f, 0.0f);
    v[1] = vec3(1.0f, -1.0f, -3.0f);
    v[2] = vec3(-1.0f, -1.0f, -3.0f);
    CHECK(draw_mesh(&t, v, 3, idx, 3, RED, k_identity) == PIXELICA_OK);
    CHECK(count(&t, RED) > 0);
    CHECK(color_at(&t, 32, 10) == BG); /* floor stays below the horizon */
    CHECK(pixelica_memory_guards_intact(t.ctx) == 1);

    /* A custom matrix producing w = -1 for every vertex: a naive divide
     * would draw a mirrored triangle; the w-epsilon plane rejects it. */
    pixelica_mat4_identity(neg_w);
    neg_w[15] = -1.0f;
    pixelica_set_view_projection(t.ctx, neg_w);
    v[0] = vec3(-0.5f, -0.5f, 0.0f);
    v[1] = vec3(0.5f, -0.5f, 0.0f);
    v[2] = vec3(0.0f, 0.5f, 0.0f);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_mesh(&t, v, 3, idx, 3, RED, k_identity) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 0);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).triangles_rejected, 1);
    /* Same triangle with w = +1 is visible (sanity). */
    pixelica_set_view_projection(t.ctx, k_identity);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_mesh(&t, v, 3, idx, 3, RED, k_identity) == PIXELICA_OK);
    CHECK(count(&t, RED) > 0);
    close_target(&t);
}

static void test_near_far_planes_identity(void)
{
    Target t;
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    PIXELICA_Vec3 v[4];
    int x;

    if (!open_target(&t, 64, 64)) {
        return;
    }
    /* Full-screen quad with z = 2x: the part with |x| > 0.5 lies outside
     * z in [-1, 1] and must be clipped (near on the left, far on the
     * right). Columns 16..47 remain. */
    v[0] = vec3(-1.0f, -1.0f, -2.0f);
    v[1] = vec3(1.0f, -1.0f, 2.0f);
    v[2] = vec3(1.0f, 1.0f, 2.0f);
    v[3] = vec3(-1.0f, 1.0f, -2.0f);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_mesh(&t, v, 4, idx, 6, RED, k_identity) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 32 * 64);
    CHECK(color_at(&t, 15, 32) == BG);
    CHECK(color_at(&t, 16, 32) == RED);
    CHECK(color_at(&t, 47, 32) == RED);
    CHECK(color_at(&t, 48, 32) == BG);
    for (x = 16; x < 47; ++x) {
        CHECK(depth_at(&t, x, 32) < depth_at(&t, x + 1, 32));
    }
    CHECK(depth_at(&t, 16, 32) < 2000);
    CHECK(depth_at(&t, 47, 32) > 63000);
    close_target(&t);
}

static void test_shared_edge_after_clipping(void)
{
    Target t;
    float persp[16];
    static const uint16_t tri_a[3] = {0, 1, 2};
    static const uint16_t tri_b[3] = {0, 2, 3};
    PIXELICA_Vec3 v[4];
    uint32_t ca, cb, uni;

    if (!open_target(&t, 64, 64)) {
        return;
    }
    /* Floor quad from 3 units ahead to 2 units behind the camera, split
     * along a diagonal that crosses the near plane and the side planes.
     * Visible part: every row below NDC y = -1/3 (rows 43..63) fully. */
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 0.5f, 100.0f) ==
          PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    pixelica_set_cull_mode(t.ctx, PIXELICA_CULL_NONE);
    v[0] = vec3(-3.0f, -1.0f, -3.0f);
    v[1] = vec3(3.0f, -1.0f, -3.0f);
    v[2] = vec3(3.0f, -1.0f, 2.0f);
    v[3] = vec3(-3.0f, -1.0f, 2.0f);

    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_mesh(&t, v, 4, tri_a, 3, RED, k_identity) == PIXELICA_OK);
    ca = count(&t, RED);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_mesh(&t, v, 4, tri_b, 3, RED, k_identity) == PIXELICA_OK);
    cb = count(&t, RED);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_mesh(&t, v, 4, tri_a, 3, RED, k_identity) == PIXELICA_OK);
    CHECK(draw_mesh(&t, v, 4, tri_b, 3, RED, k_identity) == PIXELICA_OK);
    uni = count(&t, RED);
    CHECK(ca > 0 && cb > 0);
    CHECK_EQ_LONG(ca + cb, uni);         /* no double coverage */
    CHECK_EQ_LONG(uni, 21 * 64);         /* no gaps */
    CHECK(color_at(&t, 0, 43) == RED && color_at(&t, 63, 63) == RED);
    CHECK(color_at(&t, 32, 42) == BG);
    close_target(&t);
}

static void test_invalid_inputs(void)
{
    Target t;
    static PIXELICA_Vec3 many[300];
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    static const uint16_t bad_idx[6] = {0, 1, 2, 0, 2, 4};
    PIXELICA_Vec3 v[4];
    PIXELICA_Mesh m;
    float model[16];
    uint64_t before;

    if (!open_target(&t, 32, 32)) {
        return;
    }
    v[0] = vec3(-0.8f, -0.8f, 0.0f);
    v[1] = vec3(0.8f, -0.8f, 0.0f);
    v[2] = vec3(0.8f, 0.8f, 0.0f);
    v[3] = vec3(-0.8f, 0.8f, 0.0f);

    pixelica_begin_frame(t.ctx, BG);
    draw_rect_px(&t, 2.0f, 2.0f, 9.0f, 9.0f, 0.0f, GREEN);
    before = frame_hash(&t);

    /* Every rejected draw must leave color and depth untouched. */
    CHECK(pixelica_draw_mesh(t.ctx, NULL, k_identity) == PIXELICA_ERR_INVALID_ARG);
    m.vertices = v;
    m.vertex_count = 4;
    m.indices = idx;
    m.index_count = 6;
    m.color = RED;
    CHECK(pixelica_draw_mesh(t.ctx, &m, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_draw_mesh(NULL, &m, k_identity) == PIXELICA_ERR_INVALID_ARG);

    /* Atomic: first triangle valid, second has index 4 >= vertex_count. */
    CHECK(draw_mesh(&t, v, 4, bad_idx, 6, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).last_error, PIXELICA_ERR_INVALID_ARG);
    CHECK(draw_mesh(&t, v, 4, idx, 4, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG); /* not a multiple of 3 */
    CHECK(draw_mesh(&t, NULL, 4, idx, 6, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(draw_mesh(&t, v, 4, NULL, 6, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(draw_mesh(&t, v, 0, idx, 6, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG); /* indices into an empty vertex array */

    /* Non-finite vertex (only in the last vertex), NaN and infinity. */
    v[3].y = pixelica_t_nan();
    CHECK(draw_mesh(&t, v, 4, idx, 6, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG);
    v[3].y = pixelica_t_inf();
    CHECK(draw_mesh(&t, v, 4, idx, 6, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG);
    v[3].y = 0.8f;

    /* Finite inputs whose transform overflows to infinity. */
    pixelica_mat4_scale(model, 10.0f, 10.0f, 10.0f);
    v[3].x = 3.0e38f;
    CHECK(draw_mesh(&t, v, 4, idx, 6, RED, model) == PIXELICA_ERR_INVALID_ARG);
    v[3].x = -0.8f;

    /* Non-finite model matrix. */
    pixelica_mat4_identity(model);
    model[13] = pixelica_t_nan();
    CHECK(draw_mesh(&t, v, 4, idx, 6, RED, model) == PIXELICA_ERR_INVALID_ARG);

    /* Non-finite or NULL camera: draws fail until a valid one is set. */
    pixelica_mat4_identity(model);
    model[0] = pixelica_t_inf();
    pixelica_set_view_projection(t.ctx, model);
    CHECK(draw_mesh(&t, v, 4, idx, 6, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG);
    pixelica_set_view_projection(t.ctx, k_identity);
    pixelica_set_view_projection(t.ctx, NULL);
    CHECK(draw_mesh(&t, v, 4, idx, 6, RED, k_identity) ==
          PIXELICA_ERR_INVALID_ARG);

    /* Mesh larger than max_vertices_per_mesh (256). */
    pixelica_set_view_projection(t.ctx, k_identity);
    CHECK(draw_mesh(&t, many, 257, idx, 6, RED, k_identity) ==
          PIXELICA_ERR_CAPACITY);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).last_error, PIXELICA_ERR_CAPACITY);

    CHECK(frame_hash(&t) == before);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).draw_calls, 2);

    /* Empty meshes are accepted and draw nothing. */
    CHECK(draw_mesh(&t, NULL, 0, NULL, 0, RED, k_identity) == PIXELICA_OK);
    CHECK(draw_mesh(&t, many, 256, idx, 0, RED, k_identity) == PIXELICA_OK);
    CHECK(frame_hash(&t) == before);

    /* The camera recovered: a valid draw works again. */
    CHECK(draw_mesh(&t, v, 4, idx, 6, RED, k_identity) == PIXELICA_OK);
    CHECK(count(&t, RED) > 0);

    /* Drawing outside begin/end is rejected. */
    CHECK(pixelica_end_frame(t.ctx) == PIXELICA_OK);
    before = frame_hash(&t);
    CHECK(draw_mesh(&t, v, 4, idx, 6, BLUE, k_identity) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(frame_hash(&t) == before);
    close_target(&t);
}

static void test_draw_capacity(void)
{
    Target t;
    PIXELICA_Config cfg = pixelica_t_config(16, 16);
    int i;

    cfg.max_draw_commands = 3;
    t.w = 16;
    t.h = 16;
    t.ctx = NULL;
    CHECK(pixelica_create(&cfg, NULL, &t.ctx) == PIXELICA_OK);
    if (t.ctx == NULL) {
        return;
    }
    for (i = 0; i < 2; ++i) {
        pixelica_begin_frame(t.ctx, BG);
        CHECK(draw_tri_px_front(&t, 1.0f, 1.0f, 8.0f, 1.0f, 1.0f, 8.0f, 0.0f,
                                RED) == PIXELICA_OK);
        CHECK(draw_tri_px_front(&t, 1.0f, 1.0f, 8.0f, 1.0f, 1.0f, 8.0f, 0.0f,
                                RED) == PIXELICA_OK);
        CHECK(draw_tri_px_front(&t, 1.0f, 1.0f, 8.0f, 1.0f, 1.0f, 8.0f, 0.0f,
                                RED) == PIXELICA_OK);
        CHECK(draw_tri_px_front(&t, 9.0f, 9.0f, 15.0f, 9.0f, 9.0f, 15.0f,
                                0.0f, BLUE) == PIXELICA_ERR_CAPACITY);
        CHECK_EQ_LONG(count(&t, BLUE), 0);
        CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).draw_calls, 3);
        CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).last_error, PIXELICA_ERR_CAPACITY);
        /* The next frame starts from zero again. */
        CHECK(pixelica_end_frame(t.ctx) == PIXELICA_OK);
    }
    close_target(&t);
}

/* Cube scene used for the determinism check (6 single-color faces). */
static void render_cube(const Target *t, float angle)
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
    int f;

    CHECK(pixelica_mat4_perspective(proj, 0.8726646f, 256.0f / 192.0f, 0.1f,
                               50.0f) == PIXELICA_OK);
    pixelica_mat4_translation(view, 0.0f, 0.0f, -3.0f);
    pixelica_mat4_multiply(vp, proj, view);
    pixelica_mat4_rotation_x(rx, angle * 0.6f);
    pixelica_mat4_rotation_y(ry, angle);
    pixelica_mat4_multiply(model, ry, rx);

    pixelica_set_view_projection(t->ctx, vp);
    pixelica_begin_frame(t->ctx, 0x1084);
    for (f = 0; f < 6; ++f) {
        CHECK(draw_mesh(t, verts, 8, faces[f], 6, colors[f], model) ==
              PIXELICA_OK);
    }
    CHECK(pixelica_end_frame(t->ctx) == PIXELICA_OK);
}

static void test_determinism_256x192(void)
{
    static const float angles[5] = {0.0f, 0.37f, 1.1f, 2.0f, 4.4f};
    uint64_t first[5];
    Target a, b;
    int i;

    if (!open_target(&a, 256, 192)) {
        return;
    }
    if (!open_target(&b, 256, 192)) {
        close_target(&a);
        return;
    }
    for (i = 0; i < 5; ++i) {
        PIXELICA_FrameStats s;
        uint32_t bg;

        render_cube(&a, angles[i]);
        first[i] = frame_hash(&a);
        s = pixelica_frame_stats(a.ctx);
        bg = count(&a, 0x1084);
        /* Non-trivial frame: the cube covers a real part of the screen and
         * the background is still visible around it. */
        CHECK(s.pixels_written > 2000u);
        CHECK(bg > 10000u && bg < 256u * 192u);
        CHECK_EQ_LONG(s.triangles_submitted, 12);
        /* Informational only: float results may legitimately differ
         * between compilers/architectures, so no golden value is asserted. */
        printf("cube angle %.2f: frame hash %08lx%08lx, %lu px written\n",
               (double)angles[i], (unsigned long)(first[i] >> 32),
               (unsigned long)(first[i] & 0xFFFFFFFFu),
               (unsigned long)s.pixels_written);
    }
    /* A fresh context and a re-render after other frames both reproduce
     * the exact same color+depth hash. */
    for (i = 0; i < 5; ++i) {
        render_cube(&b, angles[i]);
        CHECK(frame_hash(&b) == first[i]);
    }
    render_cube(&a, angles[2]);
    CHECK(frame_hash(&a) == first[2]);
    CHECK(first[0] != first[2]);

    /* At an oblique angle more than one face is visible. */
    {
        int distinct = 0;
        static const uint16_t colors[6] = {0xF800, 0x07E0, 0x001F,
                                           0xFFE0, 0xF81F, 0x07FF};
        for (i = 0; i < 6; ++i) {
            if (count(&a, colors[i]) > 100u) {
                ++distinct;
            }
        }
        CHECK(distinct >= 2);
    }
    close_target(&a);
    close_target(&b);
}

int main(void)
{
    test_top_left_rule();
    test_fan_watertight();
    test_degenerate();
    test_offscreen_and_huge();
    test_flt_max_clipping();
    test_winding_and_culling();
    test_depth_ordering();
    test_near_plane_crossing();
    test_behind_camera();
    test_near_far_planes_identity();
    test_shared_edge_after_clipping();
    test_invalid_inputs();
    test_draw_capacity();
    test_determinism_256x192();
    return pixelica_t_finish("test_raster");
}

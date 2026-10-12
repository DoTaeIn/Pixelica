/*
 * Particle rendering adapter tests: 2D adapter (always) and 2.5D adapter (only
 * when pixelica/particles25d.h is on the include path, i.e. the particles25d
 * target is linked).
 *
 * Scene for the 2D tests: a 32x24 surface, camera at the origin (1 world pixel
 * == 1 screen pixel). A 4x4 particle centered on (10,8) covers pixels
 * x 8..11, y 6..9 (pixel centers at +0.5 fall inside the half-open footprint).
 *
 * Scene for the 2.5D tests (same numbers as tests/test_hybrid.c): a 32x24 3D
 * context with an orthographic camera at the origin looking down -Z, 1 world
 * unit == 1 pixel, world (0,0) == screen (16,12), near 1 / far 9. View distance
 * 3 -> depth 16384, 5 -> 32767, 7 -> 49151. A WALL quad at z=-5 covers pixels
 * x 16..23, y 6..17.
 */
#include <stdbool.h>

#include "pixelica/particles.h"
#include "pixelica/particles2d.h"
#include "test_util.h"

#if defined(PIXELICA_TEST_PARTICLES25D)
#define HAVE_PARTICLES25D 1
#endif

#ifdef HAVE_PARTICLES25D
#include "pixelica/particles25d.h"
#endif

#define SW 32
#define SH 24

#define BG PIXELICA_RGB565(16, 16, 48)
#define RED PIXELICA_RGB565(248, 0, 0)
#define GREEN PIXELICA_RGB565(0, 248, 0)
#define BLUE PIXELICA_RGB565(0, 0, 248)
#define KEY PIXELICA_RGB565(248, 0, 248)

#define PI_4 0.7853982f

/* Same arithmetic as the 2D renderer: (src*a + dst*(255-a) + 127) / 255 per channel. */
static uint16_t blend565(uint16_t s, uint16_t d, uint8_t a)
{
    const uint32_t ia = 255u - a;
    const uint32_t r = (((uint32_t)s >> 11) * a + ((uint32_t)d >> 11) * ia + 127u) / 255u;
    const uint32_t g = ((((uint32_t)s >> 5) & 63u) * a + (((uint32_t)d >> 5) & 63u) * ia + 127u) / 255u;
    const uint32_t b = (((uint32_t)s & 31u) * a + ((uint32_t)d & 31u) * ia + 127u) / 255u;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Same rounding as the adapters use to multiply two 8-bit opacities. */
static uint8_t mul_opacity(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a * (uint32_t)b + 127u) / 255u);
}

static uint32_t count_color(const uint16_t *pixels, uint16_t color)
{
    uint32_t n = 0u;
    size_t i;

    for (i = 0u; i < (size_t)SW * SH; ++i) {
        if (pixels[i] == color) {
            ++n;
        }
    }
    return n;
}

/* ---- emitters: a quiet pool (no births, no motion) with exactly known values ---- */

static PIXELICA_ParticleConfig quiet_config(uint16_t color, uint8_t opacity, float size)
{
    PIXELICA_ParticleConfig cfg;

    pixelica_particles_config_default(&cfg);
    cfg.rate = 0.0f;
    cfg.lifetime_min = 10.0f;
    cfg.lifetime_max = 10.0f;
    cfg.size_start = size;
    cfg.size_end = size;
    cfg.color_start = color;
    cfg.color_end = color;
    cfg.opacity_start = opacity;
    cfg.opacity_end = opacity;
    return cfg;
}

static void make_emitter(PIXELICA_ParticleEmitter *e, PIXELICA_Particle *store, uint32_t capacity,
                         const PIXELICA_ParticleConfig *cfg)
{
    memset(store, 0, sizeof *store * capacity);
    CHECK(pixelica_particles_init(e, store, capacity, cfg, 1u) == PIXELICA_OK);
}

static void spawn_at(PIXELICA_ParticleEmitter *e, float x, float y, float z)
{
    PIXELICA_Vec3 position;
    uint32_t spawned = 0u;

    position.x = x;
    position.y = y;
    position.z = z;
    CHECK(pixelica_particles_move(e, position) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(e, 1u, &spawned) == PIXELICA_OK);
    CHECK_EQ_LONG(spawned, 1);
}

/*
 * Two particles in one pool with different colors: the first one is aged by one
 * second so its color has moved from RED toward BLUE, the second is newborn RED.
 */
static void make_pair(PIXELICA_ParticleEmitter *e, PIXELICA_Particle *store, uint8_t opacity,
                      PIXELICA_Vec3 first, PIXELICA_Vec3 second)
{
    PIXELICA_ParticleConfig cfg = quiet_config(RED, opacity, 4.0f);

    cfg.color_end = BLUE;
    make_emitter(e, store, 4u, &cfg);
    spawn_at(e, first.x, first.y, first.z);
    CHECK(pixelica_particles_update(e, 1.0f) == PIXELICA_OK);
    spawn_at(e, second.x, second.y, second.z);
    CHECK_EQ_LONG(e->count, 2);
    CHECK(e->particles[0].color != e->particles[1].color);
}

/* ---- 2D surface ---- */

static uint16_t g_color[SW * SH];
static uint16_t g_depth[SW * SH];
static PIXELICA2D_Context g_ctx;

static void reset2d(bool with_depth)
{
    PIXELICA2D_Surface surface;

    memset(&surface, 0, sizeof surface);
    surface.pixels = g_color;
    surface.width = SW;
    surface.height = SH;
    surface.stride = SW;
    surface.depth = with_depth ? g_depth : NULL;
    surface.depth_stride = SW;
    CHECK(pixelica2d_init(&g_ctx, &surface) == PIXELICA_OK);
    CHECK(pixelica2d_clear(&g_ctx, BG) == PIXELICA_OK);
}

static uint16_t px2d(int x, int y)
{
    return g_color[(size_t)y * SW + (size_t)x];
}

static uint64_t hash_color2d(void)
{
    return pixelica_t_hash16(g_color, (size_t)SW * SH);
}

static uint64_t hash_depth2d(void)
{
    return pixelica_t_hash16(g_depth, (size_t)SW * SH);
}

/* 4x4 texture, every texel unique (0x0100 + n) except texel (0,0) which is the color key. */
static uint16_t g_asym[16];
static PIXELICA_Texture g_asym_tex = {g_asym, 4, 4};
static uint16_t g_green[16];
static uint16_t g_blue[16];
static PIXELICA_Texture g_green_tex = {g_green, 4, 4};
static PIXELICA_Texture g_blue_tex = {g_blue, 4, 4};

static void fill_textures(void)
{
    int n;

    for (n = 0; n < 16; ++n) {
        g_asym[n] = n == 0 ? (uint16_t)KEY : (uint16_t)(0x0100u + (unsigned)n);
        g_green[n] = GREEN;
        g_blue[n] = BLUE;
    }
}

static PIXELICA2D_Sprite make_appearance(const PIXELICA_Texture *tex)
{
    PIXELICA2D_Sprite s;

    memset(&s, 0, sizeof s);
    CHECK(pixelica2d_sprite_init(&s, tex) == PIXELICA_OK);
    s.pivot.x = 2.0f;
    s.pivot.y = 2.0f;
    return s;
}

/* The call must fail with INVALID_ARG and leave color, depth and stats untouched. */
static void expect_rejected_2d(const PIXELICA_ParticleEmitter *e, const PIXELICA2D_Sprite *a)
{
    const uint64_t color = hash_color2d();
    const uint64_t depth = hash_depth2d();
    const PIXELICA2D_Stats stats = g_ctx.stats;

    CHECK(pixelica2d_particles_draw(&g_ctx, e, a) == PIXELICA_ERR_INVALID_ARG);
    CHECK(hash_color2d() == color);
    CHECK(hash_depth2d() == depth);
    CHECK_EQ_LONG(g_ctx.stats.draw_calls, stats.draw_calls);
    CHECK_EQ_LONG(g_ctx.stats.pixels_written, stats.pixels_written);
}

static void test_square_geometry(void)
{
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[4];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 4.0f);
    int x, y;

    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 10.0f, 8.0f, 99.0f); /* Z is ignored in 2D */
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(count_color(g_color, RED), 16);
    for (y = 6; y <= 9; ++y) {
        for (x = 8; x <= 11; ++x) {
            CHECK(px2d(x, y) == RED);
        }
    }
    CHECK(px2d(7, 6) == BG && px2d(12, 9) == BG && px2d(8, 5) == BG && px2d(11, 10) == BG);
    CHECK_EQ_LONG(g_ctx.stats.draw_calls, 1);
    CHECK_EQ_LONG(g_ctx.stats.pixels_written, 16);

    /* The camera moves the particle exactly like a sprite. */
    {
        const PIXELICA2D_Camera cam = {{5.0f, 2.0f}, 1.0f};

        reset2d(false);
        CHECK(pixelica2d_set_camera(&g_ctx, &cam) == PIXELICA_OK);
        CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
        CHECK_EQ_LONG(count_color(g_color, RED), 16);
        CHECK(px2d(3, 4) == RED && px2d(6, 7) == RED && px2d(2, 4) == BG && px2d(7, 7) == BG);
    }
    /* Zoom scales the size: 4 world pixels at zoom 2 are 8 screen pixels around (20,16). */
    {
        const PIXELICA2D_Camera cam = {{0.0f, 0.0f}, 2.0f};

        reset2d(false);
        CHECK(pixelica2d_set_camera(&g_ctx, &cam) == PIXELICA_OK);
        CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
        CHECK_EQ_LONG(count_color(g_color, RED), 64);
        CHECK(px2d(16, 12) == RED && px2d(23, 19) == RED && px2d(15, 12) == BG && px2d(24, 19) == BG);
    }
    /* The clip rectangle is honoured. */
    {
        const PIXELICA2D_Rect clip = {0, 0, 10, SH};

        reset2d(false);
        CHECK(pixelica2d_set_clip(&g_ctx, &clip) == PIXELICA_OK);
        CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
        CHECK_EQ_LONG(count_color(g_color, RED), 8);
        CHECK(px2d(9, 6) == RED && px2d(10, 6) == BG);
    }
}

static void test_rotation(void)
{
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[2];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 8.0f);

    make_emitter(&e, store, 2u, &cfg);
    spawn_at(&e, 16.0f, 12.0f, 0.0f);
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(count_color(g_color, RED), 64);
    CHECK(px2d(12, 8) == RED && px2d(20, 12) == BG);

    /* A 45 degree square: the axis-aligned corner is empty, the diamond tip is filled. */
    cfg.rotation_min = PI_4;
    cfg.rotation_max = PI_4;
    make_emitter(&e, store, 2u, &cfg);
    spawn_at(&e, 16.0f, 12.0f, 0.0f);
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    CHECK(px2d(16, 12) == RED);
    CHECK(px2d(12, 8) == BG);
    CHECK(px2d(20, 12) == RED);
}

static void test_opacity_and_skips(void)
{
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[4];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 128, 4.0f);

    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 10.0f, 8.0f, 0.0f);
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(count_color(g_color, blend565(RED, BG, 128)), 16);
    CHECK_EQ_LONG(count_color(g_color, RED), 0);

    /* Zero opacity and zero size are skipped without even a draw call. */
    cfg = quiet_config(RED, 0, 4.0f);
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 10.0f, 8.0f, 0.0f);
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(g_ctx.stats.draw_calls, 0);
    CHECK_EQ_LONG(count_color(g_color, BG), SW * SH);

    cfg = quiet_config(RED, 255, 0.0f);
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 10.0f, 8.0f, 0.0f);
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(g_ctx.stats.draw_calls, 0);
    CHECK_EQ_LONG(count_color(g_color, BG), SW * SH);

    /* No live particles: success, the context is not touched. */
    cfg = quiet_config(RED, 255, 4.0f);
    make_emitter(&e, store, 4u, &cfg);
    CHECK_EQ_LONG(e.count, 0);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(g_ctx.stats.draw_calls, 0);
}

static void test_pool_order(void)
{
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[4];
    PIXELICA_Vec3 a = {10.0f, 8.0f, 0.0f};
    PIXELICA_Vec3 b = {12.0f, 8.0f, 0.0f};

    /* Pool order [aged, newborn RED]: the later particle is drawn over the earlier one. */
    make_pair(&e, store, 255u, a, b);
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    CHECK(px2d(8, 7) == e.particles[0].color);
    CHECK(px2d(9, 7) == e.particles[0].color);
    CHECK(px2d(10, 7) == RED && px2d(11, 7) == RED); /* overlap */
    CHECK(px2d(13, 7) == RED);
    CHECK(px2d(8, 7) != RED && px2d(8, 7) != BG);
    CHECK_EQ_LONG(g_ctx.stats.draw_calls, 2);
}

static void test_textured(void)
{
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[4];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 4.0f);
    PIXELICA2D_Sprite app;
    int tx, ty;

    fill_textures();
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 10.0f, 8.0f, 0.0f);

    /* 1:1: texel (tx,ty) lands on pixel (8+tx, 6+ty); the keyed texel is skipped; no RED (no tint). */
    app = make_appearance(&g_asym_tex);
    app.color_key_enabled = true;
    app.color_key = KEY;
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, &app) == PIXELICA_OK);
    for (ty = 0; ty < 4; ++ty) {
        for (tx = 0; tx < 4; ++tx) {
            const uint16_t want = (tx == 0 && ty == 0) ? (uint16_t)BG : g_asym[ty * 4 + tx];

            CHECK(px2d(8 + tx, 6 + ty) == want);
        }
    }
    CHECK_EQ_LONG(count_color(g_color, RED), 0);

    /* Negative scale.x mirrors around the pivot; only the sign counts, not the magnitude. */
    app.scale.x = -3.0f;
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, &app) == PIXELICA_OK);
    CHECK(px2d(11, 6) == BG && px2d(8, 6) == g_asym[3] && px2d(11, 9) == g_asym[12]);
    app.scale.x = 1.0f;

    /* Stretched to size 8: every texel covers 2x2 pixels, origin (12,8) around (16,12). */
    cfg = quiet_config(RED, 255, 8.0f);
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 16.0f, 12.0f, 0.0f);
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, &app) == PIXELICA_OK);
    CHECK(px2d(12, 8) == BG && px2d(13, 9) == BG);
    CHECK(px2d(14, 8) == g_asym[1] && px2d(15, 9) == g_asym[1]);
    CHECK(px2d(18, 12) == g_asym[11] && px2d(19, 15) == g_asym[15]);

    /* appearance.opacity multiplies the particle opacity (128 * 128 / 255 -> 64). */
    cfg = quiet_config(RED, 128, 4.0f);
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 10.0f, 8.0f, 0.0f);
    app.opacity = 128u;
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, &app) == PIXELICA_OK);
    CHECK_EQ_LONG(mul_opacity(128u, 128u), 64);
    CHECK(px2d(9, 6) == blend565(g_asym[1], BG, mul_opacity(128u, 128u)));
    app.opacity = 0u;
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, &app) == PIXELICA_OK);
    CHECK_EQ_LONG(g_ctx.stats.draw_calls, 0);
}

static void test_depth_options(void)
{
    PIXELICA_ParticleEmitter first, second, third;
    PIXELICA_Particle s1[2], s2[2], s3[2];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 4.0f);
    PIXELICA2D_Sprite a, b, c;

    fill_textures();
    make_emitter(&first, s1, 2u, &cfg);
    make_emitter(&second, s2, 2u, &cfg);
    make_emitter(&third, s3, 2u, &cfg);
    spawn_at(&first, 10.0f, 8.0f, 0.0f);
    spawn_at(&second, 12.0f, 8.0f, 0.0f);
    spawn_at(&third, 11.0f, 8.0f, 0.0f);

    a = make_appearance(&g_green_tex);
    a.depth = 10u;
    a.depth_write = true;
    b = make_appearance(&g_blue_tex);
    b.depth = 20u;
    b.depth_test = true;
    c = make_appearance(&g_blue_tex);
    c.depth = 5u;
    c.depth_test = true;

    reset2d(true);
    CHECK(pixelica2d_particles_draw(&g_ctx, &first, &a) == PIXELICA_OK);
    CHECK(px2d(9, 7) == GREEN && g_depth[7 * SW + 9] == 10u);
    /* Behind the green particle where they overlap (x 10,11), visible beside it. */
    CHECK(pixelica2d_particles_draw(&g_ctx, &second, &b) == PIXELICA_OK);
    CHECK(px2d(10, 7) == GREEN && px2d(11, 7) == GREEN);
    CHECK(px2d(12, 7) == BLUE && px2d(13, 7) == BLUE);
    CHECK_EQ_LONG(g_depth[7 * SW + 12], PIXELICA2D_DEPTH_CLEAR); /* b does not write depth */
    /* In front of it. */
    CHECK(pixelica2d_particles_draw(&g_ctx, &third, &c) == PIXELICA_OK);
    CHECK(px2d(9, 7) == BLUE && px2d(10, 7) == BLUE);

    /* Depth flags on a surface without a depth buffer are rejected, nothing is drawn. */
    reset2d(false);
    expect_rejected_2d(&first, &a);
    expect_rejected_2d(&first, &b);
    /* A depth value above the 2D limit is rejected as well. */
    reset2d(true);
    a.depth = PIXELICA2D_DEPTH_CLEAR;
    expect_rejected_2d(&first, &a);
}

static void test_invalid_2d(void)
{
    PIXELICA_ParticleEmitter e, bad;
    PIXELICA_Particle store[4], bad_store[4];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 4.0f);
    PIXELICA2D_Sprite app;
    PIXELICA_Texture empty_tex = {NULL, 4, 4};
    unsigned k;

    fill_textures();
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 10.0f, 8.0f, 0.0f);
    spawn_at(&e, 20.0f, 8.0f, 0.0f);
    reset2d(true);
    CHECK(pixelica2d_particles_draw(NULL, &e, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica2d_particles_draw(&g_ctx, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);

    /* A valid first particle must not be drawn when a later one is bad. */
    for (k = 0u; k < 7u; ++k) {
        bad = e;
        memcpy(bad_store, store, sizeof bad_store);
        bad.particles = bad_store;
        switch (k) {
        case 0u: bad_store[1].position.x = pixelica_t_nan(); break;
        case 1u: bad_store[1].position.y = pixelica_t_inf(); break;
        case 2u: bad_store[1].rotation = pixelica_t_nan(); break;
        case 3u: bad_store[1].rotation = -pixelica_t_inf(); break;
        case 4u: bad_store[1].size = -1.0f; break;
        case 5u: bad_store[1].size = 2.0f * PIXELICA_PARTICLES_MAX_VALUE; break;
        default: bad_store[1].size = pixelica_t_nan(); break;
        }
        expect_rejected_2d(&bad, NULL);
    }

    /* Malformed emitter descriptors. */
    bad = e;
    bad.capacity = 0u;
    expect_rejected_2d(&bad, NULL);
    bad = e;
    bad.capacity = PIXELICA_PARTICLES_MAX_CAPACITY + 1u;
    expect_rejected_2d(&bad, NULL);
    bad = e;
    bad.count = bad.capacity + 1u;
    expect_rejected_2d(&bad, NULL);
    bad = e;
    bad.particles = NULL;
    expect_rejected_2d(&bad, NULL);

    /* Malformed appearances. */
    for (k = 0u; k < 9u; ++k) {
        app = make_appearance(&g_asym_tex);
        switch (k) {
        case 0u: app.texture = NULL; break;
        case 1u: app.scale.x = 0.0f; break;
        case 2u: app.scale.y = pixelica_t_nan(); break;
        case 3u: app.source.width = 5; break;
        case 4u: app.source.x = -1; break;
        case 5u: app.source.height = 0; break;
        case 6u: app.pivot.x = pixelica_t_inf(); break;
        case 7u: app.rotation = pixelica_t_nan(); break;
        default: app.texture = &empty_tex; break; /* texture without pixels */
        }
        expect_rejected_2d(&e, &app);
    }
}

/* ---- presets ---- */

static void check_preset(PIXELICA_ParticlePreset preset, bool y_down, float vy_min, float vy_max, float gy,
                         float rate, float life_min, float life_max, float size0, float size1)
{
    PIXELICA_ParticleConfig cfg;

    memset(&cfg, 0, sizeof cfg);
    CHECK(pixelica_particles_preset(preset, y_down, &cfg) == PIXELICA_OK);
    CHECK_NEAR(cfg.velocity_min.y, vy_min, 1e-5);
    CHECK_NEAR(cfg.velocity_max.y, vy_max, 1e-5);
    CHECK_NEAR(cfg.gravity.y, gy, 1e-5);
    CHECK_NEAR(cfg.rate, rate, 1e-5);
    CHECK_NEAR(cfg.lifetime_min, life_min, 1e-5);
    CHECK_NEAR(cfg.lifetime_max, life_max, 1e-5);
    CHECK_NEAR(cfg.size_start, size0, 1e-5);
    CHECK_NEAR(cfg.size_end, size1, 1e-5);
    CHECK_NEAR(cfg.position.x, 0.0, 0.0);
}

static void test_preset_values(void)
{
    PIXELICA_ParticleConfig cfg;
    PIXELICA_ParticleConfig before;

    /* +Y up values, then the same preset mirrored for a +Y down screen. */
    check_preset(PIXELICA_PARTICLES_SPARKS, false, 20.0f, 120.0f, -240.0f, 60.0f, 0.4f, 0.9f, 2.0f, 1.0f);
    check_preset(PIXELICA_PARTICLES_SPARKS, true, -120.0f, -20.0f, 240.0f, 60.0f, 0.4f, 0.9f, 2.0f, 1.0f);
    check_preset(PIXELICA_PARTICLES_SMOKE, false, 15.0f, 35.0f, 6.0f, 12.0f, 1.5f, 3.0f, 4.0f, 14.0f);
    check_preset(PIXELICA_PARTICLES_SMOKE, true, -35.0f, -15.0f, -6.0f, 12.0f, 1.5f, 3.0f, 4.0f, 14.0f);
    check_preset(PIXELICA_PARTICLES_RAIN, false, -260.0f, -200.0f, -60.0f, 120.0f, 0.6f, 1.0f, 1.0f, 1.0f);
    check_preset(PIXELICA_PARTICLES_RAIN, true, 200.0f, 260.0f, 60.0f, 120.0f, 0.6f, 1.0f, 1.0f, 1.0f);

    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_SPARKS, true, &cfg) == PIXELICA_OK);
    CHECK(cfg.color_start == 0xFFE0u && cfg.color_end == 0xF800u);
    CHECK(cfg.opacity_start == 255u && cfg.opacity_end == 0u);
    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_RAIN, true, &cfg) == PIXELICA_OK);
    CHECK(cfg.color_start == 0x9DFFu && cfg.color_end == 0x5C1Fu);
    CHECK(cfg.opacity_start == 200u && cfg.opacity_end == 120u);

    /* Unknown preset or NULL output: rejected, output untouched. */
    memset(&cfg, 0x5A, sizeof cfg);
    before = cfg;
    CHECK(pixelica_particles_preset((PIXELICA_ParticlePreset)3, true, &cfg) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_particles_preset((PIXELICA_ParticlePreset)-1, false, &cfg) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&before, &cfg, sizeof cfg) == 0);
    CHECK(pixelica_particles_preset(PIXELICA_PARTICLES_SPARKS, true, NULL) == PIXELICA_ERR_INVALID_ARG);
}

/* Runs a y-down preset for a few ticks and draws it; reports what it produced. */
static void run_preset(PIXELICA_ParticlePreset preset, float spawn_y, uint64_t *hash, uint32_t *written)
{
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[64];
    PIXELICA_ParticleConfig cfg;
    uint32_t i;
    int t;

    CHECK(pixelica_particles_preset(preset, true, &cfg) == PIXELICA_OK);
    cfg.position.x = 16.0f;
    cfg.position.y = spawn_y;
    CHECK(pixelica_particles_init(&e, store, 64u, &cfg, 7u) == PIXELICA_OK);
    for (t = 0; t < 6; ++t) {
        CHECK(pixelica_particles_update(&e, 1.0f / 60.0f) == PIXELICA_OK);
    }
    CHECK(e.count > 0u);
    for (i = 0u; i < e.count; ++i) {
        if (preset == PIXELICA_PARTICLES_RAIN) {
            CHECK(e.particles[i].position.y >= spawn_y); /* falls down the screen */
        } else {
            CHECK(e.particles[i].position.y <= spawn_y); /* rises up the screen */
        }
    }
    reset2d(false);
    CHECK(pixelica2d_particles_draw(&g_ctx, &e, NULL) == PIXELICA_OK);
    *hash = hash_color2d();
    *written = g_ctx.stats.pixels_written;
}

static void test_presets_render(void)
{
    static const PIXELICA_ParticlePreset presets[3] = {PIXELICA_PARTICLES_SPARKS, PIXELICA_PARTICLES_SMOKE,
                                                       PIXELICA_PARTICLES_RAIN};
    static const float spawn_y[3] = {14.0f, 14.0f, 2.0f};
    uint64_t blank;
    unsigned k;

    reset2d(false);
    blank = hash_color2d();
    for (k = 0u; k < 3u; ++k) {
        uint64_t h1 = 0u, h2 = 0u;
        uint32_t w1 = 0u, w2 = 0u;

        run_preset(presets[k], spawn_y[k], &h1, &w1);
        run_preset(presets[k], spawn_y[k], &h2, &w2);
        CHECK(h1 != blank);
        CHECK(w1 > 0u);
        CHECK(h1 == h2); /* same seed and calls: the same image */
        CHECK_EQ_LONG(w1, w2);
    }
}

#ifdef HAVE_PARTICLES25D

/* ---- 2.5D adapter ---- */

typedef struct TestPal {
    uint32_t presents;
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

static bool pal_present(void *userdata, const uint16_t *pixels, uint16_t width, uint16_t height,
                        uint32_t stride_pixels)
{
    TestPal *pal = userdata;

    (void)pixels;
    (void)width;
    (void)height;
    (void)stride_pixels;
    pal->presents += 1u;
    return true;
}

static PIXELICA_Context *make_ctx(TestPal *pal)
{
    PIXELICA_Config cfg;
    PIXELICA_PlatformOps ops;
    PIXELICA_Context *ctx = NULL;

    memset(pal, 0, sizeof *pal);
    memset(&cfg, 0, sizeof cfg);
    cfg.width = SW;
    cfg.height = SH;
    cfg.max_draw_commands = 64;
    cfg.max_vertices_per_mesh = 256;
    cfg.frame_arena_bytes = 256u * PIXELICA_ARENA_BYTES_PER_VERTEX;
    memset(&ops, 0, sizeof ops);
    ops.userdata = pal;
    ops.time_us = pal_time;
    ops.read_buttons = pal_buttons;
    ops.present_rgb565 = pal_present;
    if (pixelica_create(&cfg, &ops, &ctx) != PIXELICA_OK) {
        return NULL;
    }
    return ctx;
}

static void ortho_vp(float vp[16])
{
    CHECK(pixelica_mat4_orthographic(vp, -16.0f, 16.0f, -12.0f, 12.0f, 1.0f, 9.0f) == PIXELICA_OK);
}

static void draw_wall(PIXELICA_Context *ctx, float z)
{
    const PIXELICA_Vec3 v[4] = {{0.0f, -6.0f, z}, {8.0f, -6.0f, z}, {8.0f, 6.0f, z}, {0.0f, 6.0f, z}};
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    PIXELICA_Mesh mesh;
    float model[16];

    mesh.vertices = v;
    mesh.vertex_count = 4u;
    mesh.indices = idx;
    mesh.index_count = 6u;
    mesh.color = GREEN;
    pixelica_mat4_identity(model);
    CHECK(pixelica_draw_mesh(ctx, &mesh, model) == PIXELICA_OK);
}

static uint16_t px3d(const PIXELICA_Context *ctx, int x, int y)
{
    return pixelica_color_buffer(ctx)[(size_t)y * SW + (size_t)x];
}

static uint16_t dp3d(const PIXELICA_Context *ctx, int x, int y)
{
    return pixelica_depth_buffer(ctx)[(size_t)y * SW + (size_t)x];
}

static uint64_t hash_color3d(const PIXELICA_Context *ctx)
{
    return pixelica_t_hash16(pixelica_color_buffer(ctx), (size_t)SW * SH);
}

static uint64_t hash_depth3d(const PIXELICA_Context *ctx)
{
    return pixelica_t_hash16(pixelica_depth_buffer(ctx), (size_t)SW * SH);
}

static uint32_t red_count3d(const PIXELICA_Context *ctx)
{
    return count_color(pixelica_color_buffer(ctx), RED);
}

/* Starts a frame with the orthographic camera and binds a 2D view over it. */
static void begin_scene(PIXELICA_Context *ctx, PIXELICA2D_Context *canvas, float vp[16], bool wall)
{
    ortho_vp(vp);
    pixelica_begin_frame(ctx, BG);
    pixelica_set_view_projection(ctx, vp);
    if (wall) {
        draw_wall(ctx, -5.0f);
    }
    CHECK(pixelica2d_bind_3d(canvas, ctx) == PIXELICA_OK);
}

static void expect_rejected_3d(PIXELICA_Context *ctx, PIXELICA2D_Context *canvas, const float vp[16],
                               const PIXELICA_ParticleEmitter *e, const PIXELICA25D_ParticleOptions *opt,
                               PIXELICA25D_ParticleSortItem *scratch, size_t scratch_count, PIXELICA_Status want)
{
    const uint64_t color = hash_color3d(ctx);
    const uint64_t depth = hash_depth3d(ctx);
    const PIXELICA2D_Stats stats = canvas->stats;

    CHECK(pixelica25d_particles_draw(canvas, vp, e, opt, scratch, scratch_count) == want);
    CHECK(hash_color3d(ctx) == color);
    CHECK(hash_depth3d(ctx) == depth);
    CHECK_EQ_LONG(canvas->stats.draw_calls, stats.draw_calls);
    CHECK_EQ_LONG(canvas->stats.pixels_written, stats.pixels_written);
}

static void test_billboards(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[4];
    PIXELICA25D_ParticleSortItem scratch[4];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 4.0f);
    PIXELICA25D_ParticleOptions opt;
    float vp[16];
    int x, y;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    memset(&opt, 0, sizeof opt);
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 0.0f, 0.0f, -5.0f);

    /* Defaults: a 4x4 square around screen (16,12); depth is NOT written. */
    begin_scene(ctx, &canvas, vp, false);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, NULL, scratch, 4u) == PIXELICA_OK);
    CHECK_EQ_LONG(red_count3d(ctx), 16);
    for (y = 10; y <= 13; ++y) {
        for (x = 14; x <= 17; ++x) {
            CHECK(px3d(ctx, x, y) == RED);
            CHECK_EQ_LONG(dp3d(ctx, x, y), PIXELICA2D_DEPTH_CLEAR);
        }
    }
    CHECK(px3d(ctx, 13, 10) == BG && px3d(ctx, 18, 13) == BG);
    CHECK_EQ_LONG(canvas.stats.draw_calls, 1);

    /* The canvas camera is ignored. */
    {
        const PIXELICA2D_Camera cam = {{7.0f, 9.0f}, 3.0f};

        begin_scene(ctx, &canvas, vp, false);
        CHECK(pixelica2d_set_camera(&canvas, &cam) == PIXELICA_OK);
        CHECK(pixelica25d_particles_draw(&canvas, vp, &e, &opt, scratch, 4u) == PIXELICA_OK);
        CHECK_EQ_LONG(red_count3d(ctx), 16);
        CHECK(px3d(ctx, 14, 10) == RED && px3d(ctx, 17, 13) == RED);
    }

    /* depth_write on: the particle's projected depth (view distance 5 -> 32767) is stored. */
    begin_scene(ctx, &canvas, vp, false);
    opt.depth_write = true;
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, &opt, scratch, 4u) == PIXELICA_OK);
    CHECK_EQ_LONG(dp3d(ctx, 15, 11), 32767);
    CHECK_EQ_LONG(dp3d(ctx, 13, 11), PIXELICA2D_DEPTH_CLEAR);

    /* Behind the camera and beyond far: success, nothing drawn, no draw call. */
    opt.depth_write = false;
    {
        PIXELICA_ParticleEmitter out_of_range;
        PIXELICA_Particle out_store[4];
        const float zs[2] = {5.0f, -20.0f};
        unsigned k;

        for (k = 0u; k < 2u; ++k) {
            make_emitter(&out_of_range, out_store, 4u, &cfg);
            spawn_at(&out_of_range, 0.0f, 0.0f, zs[k]);
            begin_scene(ctx, &canvas, vp, false);
            CHECK(pixelica25d_particles_draw(&canvas, vp, &out_of_range, &opt, scratch, 4u) == PIXELICA_OK);
            CHECK_EQ_LONG(red_count3d(ctx), 0);
            CHECK_EQ_LONG(canvas.stats.draw_calls, 0);
        }
    }

    /* Opacity: blended over the background. */
    cfg = quiet_config(RED, 128, 4.0f);
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 0.0f, 0.0f, -5.0f);
    begin_scene(ctx, &canvas, vp, false);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, &opt, scratch, 4u) == PIXELICA_OK);
    CHECK_EQ_LONG(count_color(pixelica_color_buffer(ctx), blend565(RED, BG, 128)), 16);

    pixelica_destroy(ctx);
}

static void test_occlusion(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[4];
    PIXELICA25D_ParticleSortItem scratch[4];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 4.0f);
    float vp[16];

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    /* Behind the wall (distance 7 vs 5): only the two columns left of the wall x=16 stay visible. */
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 0.0f, 0.0f, -7.0f);
    begin_scene(ctx, &canvas, vp, true);
    CHECK(px3d(ctx, 16, 11) == GREEN);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, NULL, scratch, 4u) == PIXELICA_OK);
    CHECK_EQ_LONG(red_count3d(ctx), 8);
    CHECK(px3d(ctx, 14, 11) == RED && px3d(ctx, 15, 11) == RED);
    CHECK(px3d(ctx, 16, 11) == GREEN && px3d(ctx, 17, 11) == GREEN);

    /* In front of the wall (distance 3): fully visible, over the wall. */
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 0.0f, 0.0f, -3.0f);
    begin_scene(ctx, &canvas, vp, true);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, NULL, scratch, 4u) == PIXELICA_OK);
    CHECK_EQ_LONG(red_count3d(ctx), 16);
    CHECK(px3d(ctx, 16, 11) == RED);

    /* An opaque particle that writes depth hides a farther one drawn later. */
    {
        PIXELICA_ParticleEmitter far_e;
        PIXELICA_Particle far_store[4];
        PIXELICA_ParticleConfig far_cfg = quiet_config(BLUE, 255, 4.0f);
        PIXELICA25D_ParticleOptions opt;

        memset(&opt, 0, sizeof opt);
        opt.depth_write = true;
        make_emitter(&far_e, far_store, 4u, &far_cfg);
        spawn_at(&far_e, 1.0f, 0.0f, -7.0f);
        begin_scene(ctx, &canvas, vp, false);
        CHECK(pixelica25d_particles_draw(&canvas, vp, &e, &opt, scratch, 4u) == PIXELICA_OK);
        CHECK(pixelica25d_particles_draw(&canvas, vp, &far_e, NULL, scratch, 4u) == PIXELICA_OK);
        CHECK(px3d(ctx, 16, 11) == RED);  /* overlap: near particle kept */
        CHECK(px3d(ctx, 18, 11) == BLUE); /* far-only pixel */
    }
    pixelica_destroy(ctx);
}

/* Translucent particles of one emitter must blend far to near, whatever the pool order. */
static void test_sorting(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[4];
    PIXELICA25D_ParticleSortItem scratch[4];
    const PIXELICA_Vec3 near_pos = {0.0f, 0.0f, -3.0f};
    const PIXELICA_Vec3 far_pos = {2.0f, 0.0f, -7.0f}; /* covers pixels x 16..19 */
    float vp[16];
    unsigned order;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    for (order = 0u; order < 2u; ++order) {
        uint16_t near_color, far_color;
        const uint8_t a = 128u;

        if (order == 0u) {
            make_pair(&e, store, a, near_pos, far_pos); /* pool: [near, far] */
            near_color = e.particles[0].color;
            far_color = e.particles[1].color;
        } else {
            make_pair(&e, store, a, far_pos, near_pos); /* pool: [far, near] */
            near_color = e.particles[1].color;
            far_color = e.particles[0].color;
        }
        begin_scene(ctx, &canvas, vp, false);
        CHECK(pixelica25d_particles_draw(&canvas, vp, &e, NULL, scratch, 4u) == PIXELICA_OK);
        CHECK(px3d(ctx, 14, 11) == blend565(near_color, BG, a));
        CHECK(px3d(ctx, 19, 11) == blend565(far_color, BG, a));
        /* Overlap x 16..17: the far particle first, the near one over it. */
        CHECK(px3d(ctx, 16, 11) == blend565(near_color, blend565(far_color, BG, a), a));
        CHECK(px3d(ctx, 17, 12) == blend565(near_color, blend565(far_color, BG, a), a));
        CHECK_EQ_LONG(canvas.stats.draw_calls, 2);
    }

    /*
     * Eight translucent particles stacked on one pixel, each with its own color
     * (different age) and its own distance, spawned in two different pool orders.
     * The pixel must be the far-to-near blend either way.
     */
    {
        static const unsigned spawn_order[2][8] = {{0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u},
                                                   {3u, 0u, 6u, 2u, 7u, 1u, 5u, 4u}};
        PIXELICA_ParticleEmitter big;
        PIXELICA_Particle big_store[8];
        PIXELICA25D_ParticleSortItem big_scratch[8];
        PIXELICA_ParticleConfig cfg = quiet_config(RED, 100, 4.0f);
        const uint8_t a = 100u;

        cfg.color_end = BLUE;
        for (order = 0u; order < 2u; ++order) {
            uint16_t color_at_distance[8];
            uint16_t expected = BG;
            unsigned k;
            int d;

            make_emitter(&big, big_store, 8u, &cfg);
            for (k = 0u; k < 8u; ++k) {
                spawn_at(&big, 0.0f, 0.0f, -1.5f - 0.5f * (float)spawn_order[order][k]);
                CHECK(pixelica_particles_update(&big, 1.0f) == PIXELICA_OK);
            }
            CHECK_EQ_LONG(big.count, 8);
            for (k = 0u; k < 8u; ++k) {
                const unsigned distance_index =
                    (unsigned)((-big.particles[k].position.z - 1.5f) / 0.5f + 0.5f);

                color_at_distance[distance_index] = big.particles[k].color;
            }
            for (d = 7; d >= 0; --d) {
                expected = blend565(color_at_distance[d], expected, a);
            }
            begin_scene(ctx, &canvas, vp, false);
            CHECK(pixelica25d_particles_draw(&canvas, vp, &big, NULL, big_scratch, 8u) == PIXELICA_OK);
            CHECK_EQ_LONG(canvas.stats.draw_calls, 8);
            CHECK(px3d(ctx, 16, 12) == expected);
        }
    }
    pixelica_destroy(ctx);
}

static void test_textured_billboards(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA_ParticleEmitter e;
    PIXELICA_Particle store[4];
    PIXELICA25D_ParticleSortItem scratch[4];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 4.0f);
    PIXELICA2D_Sprite app;
    PIXELICA25D_ParticleOptions opt;
    float vp[16];

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    fill_textures();
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 0.0f, 0.0f, -5.0f);
    app = make_appearance(&g_asym_tex);
    app.color_key_enabled = true;
    app.color_key = KEY;
    app.depth_write = true; /* the appearance flag is overridden by the options */
    memset(&opt, 0, sizeof opt);
    opt.appearance = &app;

    /* Texel (tx,ty) -> pixel (14+tx, 10+ty); keyed texel skipped; no tint. */
    begin_scene(ctx, &canvas, vp, false);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, &opt, scratch, 4u) == PIXELICA_OK);
    CHECK(px3d(ctx, 14, 10) == BG);
    CHECK(px3d(ctx, 15, 10) == g_asym[1] && px3d(ctx, 17, 13) == g_asym[15]);
    CHECK_EQ_LONG(red_count3d(ctx), 0);
    CHECK_EQ_LONG(dp3d(ctx, 15, 10), PIXELICA2D_DEPTH_CLEAR);

    opt.depth_write = true;
    begin_scene(ctx, &canvas, vp, false);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, &opt, scratch, 4u) == PIXELICA_OK);
    CHECK_EQ_LONG(dp3d(ctx, 15, 10), 32767);
    CHECK_EQ_LONG(dp3d(ctx, 14, 10), PIXELICA2D_DEPTH_CLEAR); /* keyed texel writes no depth */

    /* Mirror in X by the sign of scale.x. */
    opt.depth_write = false;
    app.scale.x = -1.0f;
    begin_scene(ctx, &canvas, vp, false);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, &opt, scratch, 4u) == PIXELICA_OK);
    CHECK(px3d(ctx, 17, 10) == BG && px3d(ctx, 14, 10) == g_asym[3]);
    app.scale.x = 1.0f;

    /* appearance.opacity multiplies the particle opacity. */
    cfg = quiet_config(RED, 128, 4.0f);
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, 0.0f, 0.0f, -5.0f);
    app.opacity = 128u;
    begin_scene(ctx, &canvas, vp, false);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &e, &opt, scratch, 4u) == PIXELICA_OK);
    CHECK(px3d(ctx, 15, 10) == blend565(g_asym[1], BG, mul_opacity(128u, 128u)));
    pixelica_destroy(ctx);
}

static void test_perspective_size(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA_ParticleEmitter near_e, far_e;
    PIXELICA_Particle near_store[2], far_store[2];
    PIXELICA25D_ParticleSortItem scratch[2];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 2.0f);
    float vp[16];
    uint32_t near_count, far_count;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    CHECK(pixelica_mat4_perspective(vp, 1.5707964f, (float)SW / (float)SH, 0.5f, 50.0f) == PIXELICA_OK);
    make_emitter(&near_e, near_store, 2u, &cfg);
    make_emitter(&far_e, far_store, 2u, &cfg);
    spawn_at(&near_e, 0.0f, 0.0f, -3.0f);
    spawn_at(&far_e, 0.0f, 0.0f, -9.0f);

    pixelica_begin_frame(ctx, BG);
    pixelica_set_view_projection(ctx, vp);
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &far_e, NULL, scratch, 2u) == PIXELICA_OK);
    far_count = red_count3d(ctx);
    pixelica_begin_frame(ctx, BG);
    pixelica_set_view_projection(ctx, vp);
    CHECK(pixelica2d_bind_3d(&canvas, ctx) == PIXELICA_OK);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &near_e, NULL, scratch, 2u) == PIXELICA_OK);
    near_count = red_count3d(ctx);
    CHECK(far_count > 0u);
    CHECK(near_count > far_count); /* nearer particles cover more pixels */
    pixelica_destroy(ctx);
}

static void test_invalid_25d(void)
{
    TestPal pal;
    PIXELICA_Context *ctx = make_ctx(&pal);
    PIXELICA2D_Context canvas;
    PIXELICA2D_Context flat;
    PIXELICA_ParticleEmitter e, bad;
    PIXELICA_Particle store[4], bad_store[4];
    PIXELICA25D_ParticleSortItem scratch[4];
    PIXELICA_ParticleConfig cfg = quiet_config(RED, 255, 4.0f);
    PIXELICA25D_ParticleOptions opt;
    PIXELICA2D_Sprite app;
    float vp[16];
    float bad_vp[16];
    unsigned k;

    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    fill_textures();
    memset(&opt, 0, sizeof opt);
    make_emitter(&e, store, 4u, &cfg);
    spawn_at(&e, -4.0f, 0.0f, -5.0f);
    spawn_at(&e, 4.0f, 0.0f, -5.0f);
    begin_scene(ctx, &canvas, vp, false);

    /* NULL arguments. */
    expect_rejected_3d(ctx, &canvas, vp, NULL, NULL, scratch, 4u, PIXELICA_ERR_INVALID_ARG);
    expect_rejected_3d(ctx, &canvas, NULL, &e, NULL, scratch, 4u, PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica25d_particles_draw(NULL, vp, &e, NULL, scratch, 4u) == PIXELICA_ERR_INVALID_ARG);
    memcpy(bad_vp, vp, sizeof bad_vp);
    bad_vp[0] = pixelica_t_nan();
    expect_rejected_3d(ctx, &canvas, bad_vp, &e, NULL, scratch, 4u, PIXELICA_ERR_INVALID_ARG);

    /* A canvas without a depth buffer cannot depth-test. */
    memset(&flat, 0, sizeof flat);
    reset2d(false);
    flat = g_ctx;
    CHECK(pixelica25d_particles_draw(&flat, vp, &e, NULL, scratch, 4u) == PIXELICA_ERR_INVALID_ARG);

    /* Scratch: too small, missing, overlapping the particle pool. */
    expect_rejected_3d(ctx, &canvas, vp, &e, NULL, scratch, 1u, PIXELICA_ERR_NO_MEMORY);
    expect_rejected_3d(ctx, &canvas, vp, &e, NULL, NULL, 4u, PIXELICA_ERR_NO_MEMORY);
    expect_rejected_3d(ctx, &canvas, vp, &e, NULL, (PIXELICA25D_ParticleSortItem *)(void *)store, 4u,
                       PIXELICA_ERR_INVALID_ARG);
    /* With nothing to sort, scratch is not needed. */
    bad = e;
    bad.count = 0u;
    CHECK(pixelica25d_particles_draw(&canvas, vp, &bad, NULL, NULL, 0u) == PIXELICA_OK);

    /* A valid first particle must not be drawn when the second one is bad. */
    for (k = 0u; k < 8u; ++k) {
        bad = e;
        memcpy(bad_store, store, sizeof bad_store);
        bad.particles = bad_store;
        switch (k) {
        case 0u: bad_store[1].position.x = pixelica_t_nan(); break;
        case 1u: bad_store[1].position.y = pixelica_t_inf(); break;
        case 2u: bad_store[1].position.z = pixelica_t_nan(); break;
        case 3u: bad_store[1].rotation = pixelica_t_nan(); break;
        case 4u: bad_store[1].size = -1.0f; break;
        case 5u: bad_store[1].size = 2.0f * PIXELICA_PARTICLES_MAX_VALUE; break;
        case 6u: bad_store[1].size = pixelica_t_nan(); break;
        default: bad.count = bad.capacity + 1u; break;
        }
        expect_rejected_3d(ctx, &canvas, vp, &bad, NULL, scratch, 4u, PIXELICA_ERR_INVALID_ARG);
    }

    /* Bad appearances. */
    for (k = 0u; k < 6u; ++k) {
        app = make_appearance(&g_asym_tex);
        switch (k) {
        case 0u: app.texture = NULL; break;
        case 1u: app.scale.x = 0.0f; break;
        case 2u: app.source.width = 9; break;
        case 3u: app.pivot.y = pixelica_t_nan(); break;
        case 4u: app.rotation = pixelica_t_inf(); break;
        default: app.scale.y = pixelica_t_nan(); break;
        }
        opt.appearance = &app;
        expect_rejected_3d(ctx, &canvas, vp, &e, &opt, scratch, 4u, PIXELICA_ERR_INVALID_ARG);
    }
    opt.appearance = NULL;

    /* A projected size beyond PIXELICA25D_PARTICLES_MAX_PROJECTED_SIZE (1e6 units * 1600 px/unit). */
    {
        PIXELICA_ParticleEmitter huge;
        PIXELICA_Particle huge_store[2];
        PIXELICA_ParticleConfig huge_cfg = quiet_config(RED, 255, PIXELICA_PARTICLES_MAX_VALUE);
        float zoom_vp[16];

        CHECK(pixelica_mat4_orthographic(zoom_vp, -0.01f, 0.01f, -0.0075f, 0.0075f, 1.0f, 9.0f) == PIXELICA_OK);
        make_emitter(&huge, huge_store, 2u, &huge_cfg);
        spawn_at(&huge, 0.0f, 0.0f, -5.0f);
        expect_rejected_3d(ctx, &canvas, zoom_vp, &huge, NULL, scratch, 4u, PIXELICA_ERR_INVALID_ARG);
    }
    pixelica_destroy(ctx);
}

#endif /* HAVE_PARTICLES25D */

int main(void)
{
    fill_textures();
    test_square_geometry();
    test_rotation();
    test_opacity_and_skips();
    test_pool_order();
    test_textured();
    test_depth_options();
    test_invalid_2d();
    test_preset_values();
    test_presets_render();
#ifdef HAVE_PARTICLES25D
    test_billboards();
    test_occlusion();
    test_sorting();
    test_textured_billboards();
    test_perspective_size();
    test_invalid_25d();
#else
    printf("test_particles_render: 2.5D adapter not built, 2D checks only\n");
#endif
    return pixelica_t_finish("test_particles_render");
}

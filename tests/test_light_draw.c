/*
 * Light patches, contact shadows and the emissive-mask hook through the real
 * 3D pipeline (pixelica_draw_light, pixelica_draw_contact_shadow,
 * pixelica_emissive_mask_pixel/_clear/_resolve; src/renderer/light_draw.c).
 */
#include "test_util.h"

#define SIZE 8u
#define PIXELS (SIZE * SIZE)
#define MASK_W 4u
#define MASK_H 4u
#define MASK_TEXELS (MASK_W * MASK_H)

static uint8_t g_mask_alpha[MASK_TEXELS];
static uint16_t g_tint_pixels[MASK_TEXELS];
static uint8_t g_emissive_alpha[PIXELS];
static uint16_t g_emissive_depth[PIXELS];
/* Aligned scratch reused as alpha / depth / counter storage by the overlap
 * tests, so no test ever builds a misaligned pointer. */
static uint32_t g_words_a[PIXELS];
static uint32_t g_words_b[PIXELS];
static uint32_t g_words_c[PIXELS];

static const uint16_t k_tint = 0x4208u;   /* r=8 g=16 b=8 */
static const uint16_t k_shadow = 0x0000u;

/* A full-viewport quad: with the identity camera, world == NDC. */
static PIXELICA_Status make_screen_quad(PIXELICA_LightQuad *out, float z)
{
    PIXELICA_LightPlacement p;

    memset(&p, 0, sizeof p);
    p.origin.x = -1.0f;
    p.origin.y = -1.0f;
    p.origin.z = z;
    p.u_axis.x = 2.0f;
    p.v_axis.y = 2.0f;
    p.normal.z = 1.0f; /* facing the camera */
    return pixelica_light_quad(&p, out);
}

static void build_mask(PIXELICA_LightMask *mask, PIXELICA_Texture *tint, uint8_t peak,
                       uint16_t color)
{
    PIXELICA_LightMaskDesc desc;

    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_RECT, &desc) == PIXELICA_OK);
    desc.width = (uint16_t)MASK_W;
    desc.height = (uint16_t)MASK_H;
    desc.softness = 0.0f;
    desc.fade_v = 0.0f;
    desc.peak = peak;
    desc.panes_x = 1u;
    desc.panes_y = 1u;
    desc.bar = 0.0f;
    CHECK(pixelica_light_mask_build(&desc, g_mask_alpha, sizeof g_mask_alpha, mask) ==
          PIXELICA_OK);
    CHECK(pixelica_light_tint_fill(color, (uint16_t)MASK_W, (uint16_t)MASK_H, g_tint_pixels,
                                   MASK_TEXELS, tint) == PIXELICA_OK);
}

static PIXELICA_Context *make_ctx(uint16_t draws)
{
    PIXELICA_Config cfg = pixelica_t_config((uint16_t)SIZE, (uint16_t)SIZE);
    PIXELICA_Context *ctx = NULL;
    float identity[16];

    cfg.max_draw_commands = draws;
    CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_OK);
    if (ctx != NULL) {
        pixelica_mat4_identity(identity);
        pixelica_set_view_projection(ctx, identity);
    }
    return ctx;
}

/* An opaque quad through the ordinary path, to act as a receiver/occluder. */
static PIXELICA_Status draw_opaque(PIXELICA_Context *ctx, float x0, float x1, float z,
                                   uint16_t color)
{
    PIXELICA_Vec3 v[4];
    static const uint16_t indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    PIXELICA_Mesh mesh;
    float model[16];

    v[0].x = x0;
    v[0].y = -1.0f;
    v[0].z = z;
    v[1].x = x1;
    v[1].y = -1.0f;
    v[1].z = z;
    v[2].x = x1;
    v[2].y = 1.0f;
    v[2].z = z;
    v[3].x = x0;
    v[3].y = 1.0f;
    v[3].z = z;
    mesh.vertices = v;
    mesh.vertex_count = 4u;
    mesh.indices = indices;
    mesh.index_count = 6u;
    mesh.color = color;
    pixelica_mat4_identity(model);
    return pixelica_draw_mesh(ctx, &mesh, model);
}

static void test_additive_patch_and_depth(void)
{
    PIXELICA_Context *ctx = make_ctx(8u);
    PIXELICA_LightDraw draw;
    uint32_t i;

    if (ctx == NULL) {
        return;
    }
    memset(&draw, 0, sizeof draw);
    CHECK(make_screen_quad(&draw.quad, 0.0f) == PIXELICA_OK);
    build_mask(&draw.mask, &draw.tint, 255u, k_tint);
    draw.mode = PIXELICA_ALPHA_ADD;
    draw.opacity = 255u;

    pixelica_begin_frame(ctx, 0x0000u);
    CHECK(pixelica_draw_light(ctx, &draw, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_frame_stats(ctx).draw_calls, 1); /* one draw command */
    /* Full mask at full opacity on black: the tint lands unchanged ... */
    for (i = 0; i < PIXELS; ++i) {
        CHECK_EQ_LONG(pixelica_color_buffer(ctx)[i], k_tint);
    }
    /* ... and a light patch NEVER writes depth. */
    for (i = 0; i < PIXELS; ++i) {
        CHECK_EQ_LONG(pixelica_depth_buffer(ctx)[i], PIXELICA_DEPTH_CLEAR);
    }
    /* Additive and order-independent: twice the patch is twice the light. */
    CHECK(pixelica_draw_light(ctx, &draw, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG((pixelica_color_buffer(ctx)[0] >> 11) & 31u, 16);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* Half opacity halves the contribution. */
    pixelica_begin_frame(ctx, 0x0000u);
    draw.opacity = 128u;
    CHECK(pixelica_draw_light(ctx, &draw, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG((pixelica_color_buffer(ctx)[0] >> 11) & 31u, 4); /* (8*128+127)/255 */
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* Saturation instead of wrap-around. */
    pixelica_begin_frame(ctx, 0xFFFFu);
    draw.opacity = 255u;
    CHECK(pixelica_draw_light(ctx, &draw, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_color_buffer(ctx)[0], 0xFFFF);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    pixelica_destroy(ctx);
}

/* The receiver semantics: nearer geometry hides the patch. */
static void test_depth_occlusion(void)
{
    PIXELICA_Context *ctx = make_ctx(8u);
    PIXELICA_LightDraw draw;
    const uint16_t occluder = 0x07E0u;
    uint32_t x, y;

    if (ctx == NULL) {
        return;
    }
    memset(&draw, 0, sizeof draw);
    CHECK(make_screen_quad(&draw.quad, 0.0f) == PIXELICA_OK);
    build_mask(&draw.mask, &draw.tint, 255u, k_tint);
    draw.mode = PIXELICA_ALPHA_ADD;
    draw.opacity = 255u;

    pixelica_begin_frame(ctx, 0x0000u);
    /* Left half: opaque, NEARER than the patch (z = -0.5 vs 0). */
    CHECK(draw_opaque(ctx, -1.0f, 0.0f, -0.5f, occluder) == PIXELICA_OK);
    CHECK(pixelica_draw_light(ctx, &draw, NULL) == PIXELICA_OK);
    for (y = 0; y < SIZE; ++y) {
        for (x = 0; x < SIZE; ++x) {
            uint16_t c = pixelica_t_color_at(ctx, (uint16_t)SIZE, (int)x, (int)y);

            if (x < SIZE / 2u) {
                CHECK_EQ_LONG(c, occluder); /* the patch is behind: nothing added */
            } else {
                CHECK_EQ_LONG(c, k_tint);
            }
        }
    }
    /* Geometry drawn AFTER the patch is not occluded by it (no depth write). */
    CHECK(draw_opaque(ctx, 0.0f, 1.0f, 0.5f, 0x001Fu) == PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_t_color_at(ctx, (uint16_t)SIZE, (int)(SIZE - 1u), 0), 0x001F);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    pixelica_destroy(ctx);
}

static void test_contact_shadow(void)
{
    PIXELICA_Context *ctx = make_ctx(8u);
    PIXELICA_ContactShadow shadow;
    PIXELICA_LightMaskDesc desc;
    unsigned int center_red, corner_red;

    if (ctx == NULL) {
        return;
    }
    memset(&shadow, 0, sizeof shadow);
    /* An ellipse mask: soft at the rim, opaque in the middle. */
    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_ELLIPSE, &desc) == PIXELICA_OK);
    desc.width = (uint16_t)MASK_W;
    desc.height = (uint16_t)MASK_H;
    desc.softness = 0.5f;
    desc.peak = 255u;
    CHECK(pixelica_light_mask_build(&desc, g_mask_alpha, sizeof g_mask_alpha,
                                    &shadow.mask) == PIXELICA_OK);
    CHECK(pixelica_light_tint_fill(k_shadow, (uint16_t)MASK_W, (uint16_t)MASK_H,
                                   g_tint_pixels, MASK_TEXELS, &shadow.tint) ==
          PIXELICA_OK);
    shadow.center.z = 0.0f;
    shadow.normal.z = 1.0f;
    shadow.u_axis.x = 1.0f;
    shadow.radius_u = 1.0f;
    shadow.radius_v = 1.0f;
    shadow.opacity = 255u;

    pixelica_begin_frame(ctx, 0xFFFFu);
    CHECK(pixelica_draw_contact_shadow(ctx, &shadow, NULL) == PIXELICA_OK);
    center_red = (pixelica_t_color_at(ctx, (uint16_t)SIZE, (int)(SIZE / 2u),
                                      (int)(SIZE / 2u)) >> 11) & 31u;
    corner_red = (pixelica_t_color_at(ctx, (uint16_t)SIZE, 0, 0) >> 11) & 31u;
    CHECK(center_red < 31u);        /* darkened */
    CHECK(corner_red > center_red); /* the rim is softer than the middle */
    /* A shadow is a blend, so it does not write depth either. */
    CHECK_EQ_LONG(pixelica_t_depth_at(ctx, (uint16_t)SIZE, (int)(SIZE / 2u),
                                      (int)(SIZE / 2u)),
                  PIXELICA_DEPTH_CLEAR);
    /* opacity scales the whole shadow. */
    {
        unsigned int light_red;

        pixelica_begin_frame(ctx, 0xFFFFu);
        shadow.opacity = 64u;
        CHECK(pixelica_draw_contact_shadow(ctx, &shadow, NULL) == PIXELICA_OK);
        light_red = (pixelica_t_color_at(ctx, (uint16_t)SIZE, (int)(SIZE / 2u),
                                         (int)(SIZE / 2u)) >> 11) & 31u;
        CHECK(light_red > center_red);
        CHECK(light_red < 31u);
    }
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    pixelica_destroy(ctx);
}

/* Every rejection must leave colour, depth and the stats untouched. */
static void test_rejects_preserve_state(void)
{
    PIXELICA_Context *ctx = make_ctx(8u);
    PIXELICA_LightDraw good, bad;
    PIXELICA_ContactShadow shadow;
    uint16_t color_before[PIXELS], depth_before[PIXELS];
    PIXELICA_FrameStats stats_before;
    unsigned int i;

    if (ctx == NULL) {
        return;
    }
    memset(&good, 0, sizeof good);
    CHECK(make_screen_quad(&good.quad, 0.0f) == PIXELICA_OK);
    build_mask(&good.mask, &good.tint, 255u, k_tint);
    good.mode = PIXELICA_ALPHA_ADD;
    good.opacity = 255u;

    pixelica_begin_frame(ctx, 0x1234u);
    CHECK(draw_opaque(ctx, -1.0f, 0.0f, -0.5f, 0x07E0u) == PIXELICA_OK);
    memcpy(color_before, pixelica_color_buffer(ctx), sizeof color_before);
    memcpy(depth_before, pixelica_depth_buffer(ctx), sizeof depth_before);
    stats_before = pixelica_frame_stats(ctx);

    for (i = 0; i < 11u; ++i) {
        PIXELICA_Status status;

        bad = good;
        switch (i) {
        case 0: bad.mode = PIXELICA_ALPHA_OPAQUE; break;
        case 1: bad.mode = PIXELICA_ALPHA_TEST; break;
        case 2: bad.mode = (PIXELICA_AlphaMode)9; break;
        case 3: bad.mask.alpha = NULL; break;
        case 4: bad.mask.width = (uint16_t)(MASK_W + 1u); break;  /* != tint */
        case 5: bad.mask.height = 1u; break;
        case 6: bad.mask.stride = MASK_W + 1u; break;             /* not tight */
        case 7: bad.tint.pixels = NULL; break;
        case 8: bad.tint.width = 0u; break;
        case 9: bad.quad.vertices[2].x = pixelica_t_nan(); break;
        default: bad.quad.indices[0] = 4u; break;                 /* out of range */
        }
        status = pixelica_draw_light(ctx, &bad, NULL);
        CHECK(status == PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(color_before, pixelica_color_buffer(ctx), sizeof color_before) == 0);
        CHECK(memcmp(depth_before, pixelica_depth_buffer(ctx), sizeof depth_before) == 0);
        CHECK_EQ_LONG(pixelica_frame_stats(ctx).draw_calls, stats_before.draw_calls);
    }
    CHECK(pixelica_draw_light(ctx, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_draw_light(NULL, &good, NULL) == PIXELICA_ERR_INVALID_ARG);

    /* Context-owned memory may not be used as the mask or the tint. */
    bad = good;
    bad.mask.alpha = (const uint8_t *)(const void *)pixelica_color_buffer(ctx);
    CHECK(pixelica_draw_light(ctx, &bad, NULL) == PIXELICA_ERR_INVALID_ARG);
    bad = good;
    bad.tint.pixels = pixelica_color_buffer(ctx);
    CHECK(pixelica_draw_light(ctx, &bad, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(color_before, pixelica_color_buffer(ctx), sizeof color_before) == 0);

    /* Contact-shadow placement errors come back the same way. */
    memset(&shadow, 0, sizeof shadow);
    shadow.mask = good.mask;
    shadow.tint = good.tint;
    shadow.normal.z = 1.0f;
    shadow.u_axis.x = 1.0f;
    shadow.radius_u = 1.0f;
    shadow.radius_v = 1.0f;
    shadow.opacity = 255u;
    CHECK(pixelica_draw_contact_shadow(ctx, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_draw_contact_shadow(NULL, &shadow, NULL) == PIXELICA_ERR_INVALID_ARG);
    {
        PIXELICA_ContactShadow s = shadow;

        s.radius_u = 0.0f;
        CHECK(pixelica_draw_contact_shadow(ctx, &s, NULL) == PIXELICA_ERR_INVALID_ARG);
        s = shadow;
        s.normal.z = 0.0f; /* zero normal */
        CHECK(pixelica_draw_contact_shadow(ctx, &s, NULL) == PIXELICA_ERR_INVALID_ARG);
        s = shadow;
        s.u_axis.x = 0.0f;
        s.u_axis.z = 1.0f; /* parallel to the normal */
        CHECK(pixelica_draw_contact_shadow(ctx, &s, NULL) == PIXELICA_ERR_INVALID_ARG);
        s = shadow;
        s.center.x = pixelica_t_inf();
        CHECK(pixelica_draw_contact_shadow(ctx, &s, NULL) == PIXELICA_ERR_INVALID_ARG);
        s = shadow;
        s.mask.stride = MASK_W + 1u;
        CHECK(pixelica_draw_contact_shadow(ctx, &s, NULL) == PIXELICA_ERR_INVALID_ARG);
    }
    CHECK(memcmp(color_before, pixelica_color_buffer(ctx), sizeof color_before) == 0);
    CHECK(memcmp(depth_before, pixelica_depth_buffer(ctx), sizeof depth_before) == 0);
    CHECK_EQ_LONG(pixelica_frame_stats(ctx).draw_calls, stats_before.draw_calls);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* Outside a frame, and over the draw-command budget. */
    CHECK(pixelica_draw_light(ctx, &good, NULL) == PIXELICA_ERR_INVALID_ARG);
    pixelica_destroy(ctx);

    ctx = make_ctx(2u);
    if (ctx != NULL) {
        pixelica_begin_frame(ctx, 0u);
        CHECK(pixelica_draw_light(ctx, &good, NULL) == PIXELICA_OK);
        CHECK(pixelica_draw_light(ctx, &good, NULL) == PIXELICA_OK);
        CHECK(pixelica_draw_light(ctx, &good, NULL) == PIXELICA_ERR_CAPACITY);
        CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
        pixelica_destroy(ctx);
    }
}

static void emissive_mask_init(PIXELICA_EmissiveMask *mask)
{
    memset(mask, 0, sizeof *mask);
    mask->alpha = g_emissive_alpha;
    mask->depth = g_emissive_depth;
    mask->width = (uint16_t)SIZE;
    mask->height = (uint16_t)SIZE;
    mask->stride = SIZE;
}

static uint32_t count_recorded(const PIXELICA_EmissiveMask *mask)
{
    uint32_t count = 0u, i;

    for (i = 0; i < PIXELS; ++i) {
        if (mask->alpha[i] != 0u) {
            ++count;
        }
    }
    return count;
}

static void test_emissive_capture(void)
{
    PIXELICA_Context *ctx = make_ctx(8u);
    PIXELICA_EmissiveMask mask;
    PIXELICA_MeshEffects effects;
    PIXELICA_LightDraw draw;
    uint16_t opaque_depth;

    if (ctx == NULL) {
        return;
    }
    emissive_mask_init(&mask);
    memset(&effects, 0, sizeof effects);
    effects.pixel = pixelica_emissive_mask_pixel;
    effects.userdata = &mask;

    memset(&draw, 0, sizeof draw);
    CHECK(make_screen_quad(&draw.quad, 0.0f) == PIXELICA_OK);
    build_mask(&draw.mask, &draw.tint, 255u, k_tint);
    draw.mode = PIXELICA_ALPHA_ADD;
    draw.opacity = 255u;

    /* The depth an opaque draw at z = 0 writes, for comparison below. */
    pixelica_begin_frame(ctx, 0u);
    CHECK(draw_opaque(ctx, -1.0f, 1.0f, 0.0f, 0x07E0u) == PIXELICA_OK);
    opaque_depth = pixelica_t_depth_at(ctx, (uint16_t)SIZE, 0, 0);
    CHECK(opaque_depth != PIXELICA_DEPTH_CLEAR);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* Recording the full-coverage patch marks every pixel with its alpha. */
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    CHECK_EQ_LONG(count_recorded(&mask), 0);
    CHECK_EQ_LONG(g_emissive_depth[0], PIXELICA_DEPTH_CLEAR);
    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_OK);
    CHECK_EQ_LONG(count_recorded(&mask), PIXELS);
    CHECK_EQ_LONG(mask.alpha[0], 255);
    CHECK_EQ_LONG(mask.written, PIXELS);
    CHECK_EQ_LONG(mask.depth[0], opaque_depth); /* the fragment's own depth */
    /* The hook changes nothing about the image. */
    CHECK_EQ_LONG(pixelica_color_buffer(ctx)[0], k_tint);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* A constant value instead of the fragment alpha. */
    mask.value = 77u;
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_OK);
    CHECK_EQ_LONG(mask.alpha[0], 77);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    mask.value = 0u;

    /* Fully transparent fragments never emit: the hook runs BEFORE the alpha
     * stage, so this is what keeps an invisible glow out of the bloom. */
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    pixelica_begin_frame(ctx, 0u);
    draw.opacity = 0u;
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_OK);
    CHECK_EQ_LONG(count_recorded(&mask), 0);
    CHECK_EQ_LONG(mask.written, 0);
    CHECK_EQ_LONG(pixelica_color_buffer(ctx)[0], 0); /* and nothing was drawn */
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    /* Same with a constant `value`: alpha 0 is still skipped. */
    mask.value = 200u;
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_OK);
    CHECK_EQ_LONG(count_recorded(&mask), 0);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    mask.value = 0u;
    draw.opacity = 255u;

    /* min_alpha models an alpha-test cutoff the hook cannot see itself. */
    draw.opacity = 128u;
    mask.min_alpha = 200u;
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_OK);
    CHECK_EQ_LONG(count_recorded(&mask), 0);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    mask.min_alpha = 100u;
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_OK);
    CHECK_EQ_LONG(count_recorded(&mask), PIXELS);
    CHECK_EQ_LONG(mask.alpha[0], 128);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    mask.min_alpha = 0u;
    draw.opacity = 255u;

    /* An unusable mask records nothing and still draws correctly. */
    {
        PIXELICA_EmissiveMask broken = mask;
        PIXELICA_MeshEffects broken_effects = effects;

        broken.alpha = NULL;
        broken_effects.userdata = &broken;
        pixelica_begin_frame(ctx, 0u);
        CHECK(pixelica_draw_light(ctx, &draw, &broken_effects) == PIXELICA_OK);
        CHECK_EQ_LONG(pixelica_color_buffer(ctx)[0], k_tint);
        CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
        CHECK(pixelica_emissive_mask_clear(&broken) == PIXELICA_ERR_INVALID_ARG);
        broken = mask;
        broken.stride = SIZE - 1u;
        CHECK(pixelica_emissive_mask_clear(&broken) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_emissive_mask_clear(NULL) == PIXELICA_ERR_INVALID_ARG);
    }
    pixelica_destroy(ctx);
}

/*
 * The hidden-emitter case the draw order cannot always avoid: an emissive
 * patch recorded first, then opaque geometry drawn over it. Resolving against
 * the finished depth buffer removes exactly the covered half.
 */
static void test_emissive_resolve(void)
{
    PIXELICA_Context *ctx = make_ctx(8u);
    PIXELICA_EmissiveMask mask;
    PIXELICA_MeshEffects effects;
    PIXELICA_LightDraw draw;
    PIXELICA_FrameView frame;
    uint32_t removed = 0u, x, y;

    if (ctx == NULL) {
        return;
    }
    emissive_mask_init(&mask);
    memset(&effects, 0, sizeof effects);
    effects.pixel = pixelica_emissive_mask_pixel;
    effects.userdata = &mask;
    memset(&draw, 0, sizeof draw);
    CHECK(make_screen_quad(&draw.quad, 0.0f) == PIXELICA_OK);
    build_mask(&draw.mask, &draw.tint, 255u, k_tint);
    draw.mode = PIXELICA_ALPHA_ADD;
    draw.opacity = 255u;

    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_OK);
    CHECK_EQ_LONG(count_recorded(&mask), PIXELS);
    /* Now cover the left half with nearer opaque geometry. */
    CHECK(draw_opaque(ctx, -1.0f, 0.0f, -0.5f, 0x07E0u) == PIXELICA_OK);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    CHECK(pixelica_frame_view(ctx, &frame) == PIXELICA_OK);
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, &removed) == PIXELICA_OK);
    CHECK_EQ_LONG(removed, PIXELS / 2u);
    for (y = 0; y < SIZE; ++y) {
        for (x = 0; x < SIZE; ++x) {
            uint8_t a = mask.alpha[(size_t)y * SIZE + x];

            if (x < SIZE / 2u) {
                CHECK_EQ_LONG(a, 0); /* hidden emitter: no glow */
            } else {
                CHECK_EQ_LONG(a, 255);
            }
        }
    }
    /* Resolving again changes nothing. */
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, &removed) == PIXELICA_OK);
    CHECK_EQ_LONG(removed, 0);
    /* out_removed is optional. */
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, NULL) == PIXELICA_OK);

    /* Rejections. */
    {
        PIXELICA_EmissiveMask bad = mask;
        PIXELICA_FrameView bad_frame = frame;
        uint8_t copy[PIXELS];

        memcpy(copy, g_emissive_alpha, sizeof copy);
        bad.depth = NULL;
        CHECK(pixelica_emissive_mask_resolve(&bad, &frame, NULL) == PIXELICA_ERR_INVALID_ARG);
        bad = mask;
        bad.width = (uint16_t)(SIZE - 1u);
        CHECK(pixelica_emissive_mask_resolve(&bad, &frame, NULL) == PIXELICA_ERR_INVALID_ARG);
        bad_frame.depth = NULL;
        CHECK(pixelica_emissive_mask_resolve(&mask, &bad_frame, NULL) ==
              PIXELICA_ERR_INVALID_ARG);
        bad_frame = frame;
        bad_frame.height = (uint16_t)(SIZE - 1u);
        CHECK(pixelica_emissive_mask_resolve(&mask, &bad_frame, NULL) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_emissive_mask_resolve(&mask, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_emissive_mask_resolve(NULL, &frame, NULL) == PIXELICA_ERR_INVALID_ARG);
        /* The written alpha plane must not be the depth it reads. */
        bad = mask;
        bad.alpha = (uint8_t *)(void *)g_emissive_depth;
        CHECK(pixelica_emissive_mask_resolve(&bad, &frame, NULL) == PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(copy, g_emissive_alpha, sizeof copy) == 0);
    }
    pixelica_destroy(ctx);
}

/*
 * The two writable planes of an emissive mask are validated as ADDRESS RANGES
 * before anything is written: a plane may not contain the descriptor that
 * describes it, the planes may not overlap each other, and _resolve may not
 * write its counter into anything it reads.
 */
static void test_emissive_overlap_rules(void)
{
    union {
        PIXELICA_EmissiveMask mask;
        uint8_t bytes[256];
    } holder;
    PIXELICA_EmissiveMask mask;
    PIXELICA_FrameView frame;
    uint8_t *alpha_a = (uint8_t *)(void *)g_words_a;
    uint16_t *depth_b = (uint16_t *)(void *)g_words_b;
    uint16_t *depth_c = (uint16_t *)(void *)g_words_c;
    uint32_t removed = 0xBEEFu;
    size_t i;

    /* (1) the alpha plane containing its own descriptor (memset would have
     *     destroyed the pointer it is iterating with). */
    memset(&holder, 0, sizeof holder);
    holder.mask.alpha = holder.bytes;
    holder.mask.width = 4u;
    holder.mask.height = 4u;
    holder.mask.stride = 4u;
    CHECK(pixelica_emissive_mask_clear(&holder.mask) == PIXELICA_ERR_INVALID_ARG);
    CHECK(holder.mask.alpha == holder.bytes); /* descriptor intact */
    CHECK_EQ_LONG(holder.mask.width, 4);
    CHECK_EQ_LONG(holder.mask.stride, 4);

    /* (2) the depth plane containing the descriptor. */
    memset(&holder, 0, sizeof holder);
    holder.mask.alpha = g_emissive_alpha;
    holder.mask.depth = (uint16_t *)(void *)holder.bytes;
    holder.mask.width = 4u;
    holder.mask.height = 4u;
    holder.mask.stride = 4u;
    memset(g_emissive_alpha, 0xAB, 16u);
    CHECK(pixelica_emissive_mask_clear(&holder.mask) == PIXELICA_ERR_INVALID_ARG);
    CHECK(holder.mask.alpha == g_emissive_alpha);
    CHECK_EQ_LONG(g_emissive_alpha[0], 0xAB); /* no partial mutation */

    /* (3) the two planes overlapping each other. */
    memset(&mask, 0, sizeof mask);
    mask.alpha = alpha_a;
    mask.depth = (uint16_t *)(void *)g_words_a;
    mask.width = 4u;
    mask.height = 4u;
    mask.stride = 4u;
    memset(g_words_a, 0xAB, sizeof g_words_a);
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(alpha_a[0], 0xAB);
    CHECK_EQ_LONG(alpha_a[15], 0xAB);

    /* Disjoint planes are accepted and both are cleared. */
    mask.depth = depth_b;
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    for (i = 0; i < 16u; ++i) {
        CHECK_EQ_LONG(alpha_a[i], 0);
        CHECK_EQ_LONG(depth_b[i], PIXELICA_DEPTH_CLEAR);
    }

    /* (4) _resolve: the counter may not live in anything the pass reads or
     *     writes, and a rejection leaves both the mask and the counter alone. */
    mask.width = (uint16_t)SIZE;
    mask.height = (uint16_t)SIZE;
    mask.stride = SIZE;
    CHECK(pixelica_emissive_mask_clear(&mask) == PIXELICA_OK);
    alpha_a[0] = 200u; /* one recorded emitter, in front of everything */
    depth_b[0] = 10u;
    frame.color = NULL;
    frame.depth = depth_c;
    frame.width = (uint16_t)SIZE;
    frame.height = (uint16_t)SIZE;
    frame.stride = SIZE;
    for (i = 0; i < PIXELS; ++i) {
        depth_c[i] = PIXELICA_DEPTH_CLEAR;
    }
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, &removed) == PIXELICA_OK);
    CHECK_EQ_LONG(removed, 0);
    CHECK_EQ_LONG(alpha_a[0], 200);

    removed = 0xBEEFu;
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, &g_words_a[0]) ==
          PIXELICA_ERR_INVALID_ARG); /* counter inside the alpha plane */
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, &g_words_b[0]) ==
          PIXELICA_ERR_INVALID_ARG); /* counter inside the mask depth plane */
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, &g_words_c[0]) ==
          PIXELICA_ERR_INVALID_ARG); /* counter inside the frame depth plane */
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, (uint32_t *)(void *)&mask) ==
          PIXELICA_ERR_INVALID_ARG); /* counter inside the mask descriptor */
    CHECK(pixelica_emissive_mask_resolve(&mask, &frame, (uint32_t *)(void *)&frame) ==
          PIXELICA_ERR_INVALID_ARG); /* counter inside the frame descriptor */
    CHECK_EQ_LONG(removed, 0xBEEF); /* the caller's counter is untouched */
    CHECK_EQ_LONG(alpha_a[0], 200); /* and so is the mask */

    /* The written alpha plane may not be the depth the pass reads either. */
    {
        PIXELICA_EmissiveMask aliased = mask;

        aliased.alpha = (uint8_t *)(void *)g_words_c;
        CHECK(pixelica_emissive_mask_resolve(&aliased, &frame, NULL) ==
              PIXELICA_ERR_INVALID_ARG);
    }
}

/* A patch mask can be filtered like any other texture. */
static void test_bilinear_filter(void)
{
    PIXELICA_Context *ctx = make_ctx(8u);
    PIXELICA_LightDraw draw;
    PIXELICA_MeshEffects effects;
    PIXELICA_LightMaskDesc desc;
    uint64_t nearest_hash, bilinear_hash;

    if (ctx == NULL) {
        return;
    }
    memset(&draw, 0, sizeof draw);
    CHECK(make_screen_quad(&draw.quad, 0.0f) == PIXELICA_OK);
    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_ELLIPSE, &desc) == PIXELICA_OK);
    desc.width = (uint16_t)MASK_W;
    desc.height = (uint16_t)MASK_H;
    desc.softness = 1.0f;
    desc.peak = 255u;
    CHECK(pixelica_light_mask_build(&desc, g_mask_alpha, sizeof g_mask_alpha, &draw.mask) ==
          PIXELICA_OK);
    CHECK(pixelica_light_tint_fill(0xFFFFu, (uint16_t)MASK_W, (uint16_t)MASK_H,
                                   g_tint_pixels, MASK_TEXELS, &draw.tint) == PIXELICA_OK);
    draw.mode = PIXELICA_ALPHA_ADD;
    draw.opacity = 255u;

    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, NULL) == PIXELICA_OK);
    nearest_hash = pixelica_t_hash16(pixelica_color_buffer(ctx), PIXELS);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    memset(&effects, 0, sizeof effects);
    effects.filter = PIXELICA_FILTER_BILINEAR;
    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_OK);
    bilinear_hash = pixelica_t_hash16(pixelica_color_buffer(ctx), PIXELS);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK(nearest_hash != bilinear_hash); /* the filter really is applied */

    /* An invalid effects descriptor is rejected by the usual path. */
    effects.filter = (PIXELICA_FilterMode)7;
    pixelica_begin_frame(ctx, 0u);
    CHECK(pixelica_draw_light(ctx, &draw, &effects) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    pixelica_destroy(ctx);
}

int main(void)
{
    test_additive_patch_and_depth();
    test_depth_occlusion();
    test_contact_shadow();
    test_rejects_preserve_state();
    test_emissive_capture();
    test_emissive_resolve();
    test_emissive_overlap_rules();
    test_bilinear_filter();
    return pixelica_t_finish("test_light_draw");
}

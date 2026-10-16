/*
 * Shape masks, flat tint images and receiver-plane quads
 * (pixelica/light.h, src/light/light_mask.c).
 *
 * Common-library test: no renderer, no context.
 */
#include "pixelica/light.h"
#include "test_util.h"

#define W 16u
#define H 16u

static uint8_t g_alpha[64u * 64u];
static uint16_t g_tint[64u * 64u];

/* A mask descriptor sharing storage with the bytes it would be written into. */
union DescHolder {
    PIXELICA_LightMaskDesc desc;
    uint8_t bytes[512];
};

union MaskHolder {
    PIXELICA_LightMask mask;
    uint8_t bytes[512];
};

union TextureHolder {
    PIXELICA_Texture texture;
    uint16_t pixels[256];
};

static uint8_t at(uint32_t x, uint32_t y, uint32_t width)
{
    return g_alpha[(size_t)y * width + x];
}

static void test_defaults(void)
{
    PIXELICA_LightMaskDesc rect, ellipse, before;
    PIXELICA_LightMask built;

    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_RECT, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    memset(&before, 0xAB, sizeof before);
    rect = before;
    CHECK(pixelica_light_mask_default((PIXELICA_LightShape)7, &rect) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&rect, &before, sizeof rect) == 0); /* untouched on failure */

    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_RECT, &rect) == PIXELICA_OK);
    CHECK(rect.shape == PIXELICA_LIGHT_SHAPE_RECT);
    CHECK_EQ_LONG(rect.width, 48);
    CHECK_EQ_LONG(rect.height, 32);
    CHECK_EQ_LONG(rect.panes_x, 2);
    CHECK_EQ_LONG(rect.panes_y, 3);
    CHECK(rect.bar > 0.0f);
    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_ELLIPSE, &ellipse) == PIXELICA_OK);
    CHECK(ellipse.shape == PIXELICA_LIGHT_SHAPE_ELLIPSE);
    CHECK_EQ_LONG(ellipse.panes_x, 1);
    CHECK(ellipse.bar == 0.0f);
    /* Both defaults must be accepted as-is. */
    CHECK(pixelica_light_mask_build(&rect, g_alpha, sizeof g_alpha, &built) == PIXELICA_OK);
    CHECK(pixelica_light_mask_build(&ellipse, g_alpha, sizeof g_alpha, &built) == PIXELICA_OK);
}

static void test_rect_shape(void)
{
    PIXELICA_LightMaskDesc d;
    PIXELICA_LightMask mask;
    uint32_t x, y;

    /* Hard rectangle: softness 0 and no bars cover every texel fully. */
    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_RECT, &d) == PIXELICA_OK);
    d.width = (uint16_t)W;
    d.height = (uint16_t)H;
    d.softness = 0.0f;
    d.fade_v = 0.0f;
    d.peak = 255u;
    d.panes_x = 1u;
    d.panes_y = 1u;
    d.bar = 0.0f;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    CHECK(mask.alpha == g_alpha);
    CHECK_EQ_LONG(mask.width, W);
    CHECK_EQ_LONG(mask.stride, W); /* tight, as texture_alpha requires */
    for (y = 0; y < H; ++y) {
        for (x = 0; x < W; ++x) {
            CHECK_EQ_LONG(at(x, y, W), 255);
        }
    }

    /* Soft edges: the centre stays at the peak and coverage never increases
     * toward the border. */
    d.softness = 0.5f;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    CHECK_EQ_LONG(at(W / 2u, H / 2u, W), 255);
    CHECK(at(0, H / 2u, W) < 60u);
    for (x = 1; x <= W / 2u - 1u; ++x) {
        CHECK(at(x, H / 2u, W) >= at(x - 1u, H / 2u, W));
    }
    /* Symmetric. */
    for (x = 0; x < W; ++x) {
        CHECK_EQ_LONG(at(x, H / 2u, W), at(W - 1u - x, H / 2u, W));
    }

    /* fade_v dims the last row relative to the first. */
    d.softness = 0.0f;
    d.fade_v = 0.5f;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    CHECK_EQ_LONG(at(W / 2u, 0, W), 251);           /* 255 * (1 - 0.5*0.03125) */
    CHECK_EQ_LONG(at(W / 2u, H - 1u, W), 131);      /* 255 * (1 - 0.5*0.96875) */

    /* peak 0 is a legal, fully transparent mask. */
    d.fade_v = 0.0f;
    d.peak = 0u;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    for (x = 0; x < W * H; ++x) {
        CHECK_EQ_LONG(g_alpha[x], 0);
    }
}

static void test_rect_bars(void)
{
    PIXELICA_LightMaskDesc d;
    PIXELICA_LightMask mask;
    uint32_t y;

    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_RECT, &d) == PIXELICA_OK);
    d.width = 20u;
    d.height = 4u;
    d.softness = 0.0f;
    d.fade_v = 0.0f;
    d.peak = 255u;
    d.panes_x = 2u;
    d.panes_y = 1u;
    d.bar = 0.2f;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    for (y = 0; y < 4u; ++y) {
        /* The internal boundary sits at u = 0.5, i.e. between texel 9 and 10. */
        CHECK_EQ_LONG(at(9u, y, 20u), 0);
        CHECK_EQ_LONG(at(10u, y, 20u), 0);
        CHECK_EQ_LONG(at(0u, y, 20u), 255);  /* the outer border has no bar */
        CHECK_EQ_LONG(at(19u, y, 20u), 255);
        CHECK_EQ_LONG(at(4u, y, 20u), 255);  /* middle of a pane */
    }
    /* bar 0 leaves no line at all. */
    d.bar = 0.0f;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    CHECK_EQ_LONG(at(9u, 0, 20u), 255);
    CHECK_EQ_LONG(at(10u, 0, 20u), 255);
}

static void test_ellipse_shape(void)
{
    PIXELICA_LightMaskDesc d;
    PIXELICA_LightMask mask;
    uint32_t x;

    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_ELLIPSE, &d) == PIXELICA_OK);
    d.width = (uint16_t)W;
    d.height = (uint16_t)H;
    d.softness = 1.0f;
    d.peak = 200u;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    /* Corners are outside the unit ellipse, the centre is at the peak. */
    CHECK_EQ_LONG(at(0, 0, W), 0);
    CHECK_EQ_LONG(at(W - 1u, H - 1u, W), 0);
    /* The texel centre nearest the middle is not exactly r = 0, so smoothstep
     * gives 0.978 of the peak here. */
    CHECK_EQ_LONG(at(W / 2u, H / 2u, W), 196);
    /* Monotonically decreasing from the centre outward along a row. */
    for (x = W / 2u; x + 1u < W; ++x) {
        CHECK(at(x + 1u, H / 2u, W) <= at(x, H / 2u, W));
    }
    /* A hard ellipse: softness 0 fills the inscribed ellipse completely. */
    d.softness = 0.0f;
    d.peak = 255u;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    CHECK_EQ_LONG(at(W / 2u, H / 2u, W), 255);
    CHECK_EQ_LONG(at(0, 0, W), 0);
}

static void test_one_pixel_mask(void)
{
    PIXELICA_LightMaskDesc d;
    PIXELICA_LightMask mask;

    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_ELLIPSE, &d) == PIXELICA_OK);
    d.width = 1u;
    d.height = 1u;
    d.peak = 123u;
    g_alpha[1] = 0x5Au;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    CHECK_EQ_LONG(g_alpha[0], 123); /* the single texel centre is r = 0 */
    CHECK_EQ_LONG(g_alpha[1], 0x5A); /* nothing beyond width*height is touched */
    CHECK_EQ_LONG(mask.stride, 1);

    d.shape = PIXELICA_LIGHT_SHAPE_RECT;
    d.panes_x = 2u;
    d.panes_y = 2u;
    d.bar = 0.5f;
    d.softness = 0.0f;
    CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) == PIXELICA_OK);
    CHECK_EQ_LONG(g_alpha[0], 0); /* 1x1 with 2 panes: the texel IS the bar */
}

static void test_mask_rejects(void)
{
    PIXELICA_LightMaskDesc d, good;
    PIXELICA_LightMask mask, before;
    union DescHolder desc_holder;
    union MaskHolder mask_holder;
    size_t i;

    CHECK(pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_RECT, &good) == PIXELICA_OK);
    good.width = 8u;
    good.height = 8u;

    CHECK(pixelica_light_mask_build(NULL, g_alpha, sizeof g_alpha, &mask) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_mask_build(&good, NULL, sizeof g_alpha, &mask) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_mask_build(&good, g_alpha, sizeof g_alpha, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    /* Capacity is reported separately from invalid arguments. */
    CHECK(pixelica_light_mask_build(&good, g_alpha, 63u, &mask) == PIXELICA_ERR_CAPACITY);
    CHECK(pixelica_light_mask_build(&good, g_alpha, 64u, &mask) == PIXELICA_OK);

    /* Every field is range-checked, for every shape. */
    for (i = 0; i < 12u; ++i) {
        d = good;
        switch (i) {
        case 0: d.width = 0u; break;
        case 1: d.height = 0u; break;
        case 2: d.width = (uint16_t)(PIXELICA_MAX_TEXTURE_DIMENSION + 1u); break;
        case 3: d.softness = -0.1f; break;
        case 4: d.softness = 1.1f; break;
        case 5: d.softness = pixelica_t_nan(); break;
        case 6: d.fade_v = 1.5f; break;
        case 7: d.fade_v = pixelica_t_inf(); break;
        case 8: d.bar = 0.6f; break;
        case 9: d.panes_x = 0u; break;
        case 10: d.panes_y = (uint8_t)(PIXELICA_LIGHT_MAX_PANES + 1u); break;
        default: d.shape = (PIXELICA_LightShape)9; break;
        }
        memset(g_alpha, 0xAB, 64u);
        memset(&before, 0xCD, sizeof before);
        mask = before;
        CHECK(pixelica_light_mask_build(&d, g_alpha, sizeof g_alpha, &mask) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(&mask, &before, sizeof mask) == 0);
        CHECK_EQ_LONG(g_alpha[0], 0xAB); /* nothing written on failure */
        CHECK_EQ_LONG(g_alpha[63], 0xAB);
    }

    /* The descriptor must not live inside the bytes we are about to write. */
    memset(&desc_holder, 0, sizeof desc_holder);
    desc_holder.desc = good;
    CHECK(pixelica_light_mask_build(&desc_holder.desc, desc_holder.bytes,
                                    sizeof desc_holder.bytes, &mask) ==
          PIXELICA_ERR_INVALID_ARG);
    /* Neither must the output descriptor. */
    memset(&mask_holder, 0, sizeof mask_holder);
    CHECK(pixelica_light_mask_build(&good, mask_holder.bytes, sizeof mask_holder.bytes,
                                    &mask_holder.mask) == PIXELICA_ERR_INVALID_ARG);
}

static void test_tint_fill(void)
{
    PIXELICA_Texture texture, before;
    union TextureHolder holder;
    size_t i;

    CHECK(pixelica_light_tint_fill(0x1234u, 4u, 3u, g_tint, sizeof g_tint / sizeof g_tint[0],
                                   &texture) == PIXELICA_OK);
    CHECK(texture.pixels == g_tint);
    CHECK_EQ_LONG(texture.width, 4);
    CHECK_EQ_LONG(texture.height, 3);
    for (i = 0; i < 12u; ++i) {
        CHECK_EQ_LONG(g_tint[i], 0x1234);
    }

    memset(&before, 0xCD, sizeof before);
    texture = before;
    CHECK(pixelica_light_tint_fill(0u, 4u, 3u, g_tint, 11u, &texture) ==
          PIXELICA_ERR_CAPACITY);
    CHECK(memcmp(&texture, &before, sizeof texture) == 0);
    CHECK_EQ_LONG(g_tint[0], 0x1234); /* unchanged */
    CHECK(pixelica_light_tint_fill(0u, 0u, 3u, g_tint, 64u, &texture) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_tint_fill(0u, 4u, 3u, NULL, 64u, &texture) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_tint_fill(0u, 4u, 3u, g_tint, 64u, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    /* The output descriptor must not live in the filled pixels. */
    memset(&holder, 0, sizeof holder);
    CHECK(pixelica_light_tint_fill(0u, 4u, 4u, holder.pixels,
                                   sizeof holder.pixels / sizeof holder.pixels[0],
                                   &holder.texture) == PIXELICA_ERR_INVALID_ARG);
}

static PIXELICA_Vec3 v3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static int indices_are(const PIXELICA_LightQuad *q, int forward)
{
    static const uint16_t fwd[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    static const uint16_t rev[6] = {0u, 2u, 1u, 0u, 3u, 2u};
    const uint16_t *want = forward ? fwd : rev;
    int i;

    for (i = 0; i < 6; ++i) {
        if (q->indices[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

static void test_quad(void)
{
    PIXELICA_LightPlacement p;
    PIXELICA_LightQuad quad, before;

    memset(&p, 0, sizeof p);
    p.origin = v3(1.0f, 0.0f, 2.0f);
    p.u_axis = v3(2.0f, 0.0f, 0.0f);
    p.v_axis = v3(0.0f, 0.0f, -3.0f);
    p.normal = v3(0.0f, 2.0f, 0.0f); /* not unit length on purpose */
    p.bias = 0.25f;

    CHECK(pixelica_light_quad(&p, &quad) == PIXELICA_OK);
    /* bias follows the UNIT normal. */
    CHECK_NEAR(quad.vertices[0].x, 1.0, 1e-6);
    CHECK_NEAR(quad.vertices[0].y, 0.25, 1e-6);
    CHECK_NEAR(quad.vertices[0].z, 2.0, 1e-6);
    CHECK_NEAR(quad.vertices[1].x, 3.0, 1e-6);
    CHECK_NEAR(quad.vertices[2].x, 3.0, 1e-6);
    CHECK_NEAR(quad.vertices[2].z, -1.0, 1e-6);
    CHECK_NEAR(quad.vertices[3].z, -1.0, 1e-6);
    CHECK_NEAR(quad.uvs[0].x, 0.0, 0.0);
    CHECK_NEAR(quad.uvs[2].x, 1.0, 0.0);
    CHECK_NEAR(quad.uvs[2].y, 1.0, 0.0);
    /* cross(u, v) = (0, +, 0) points along the normal: front order. */
    CHECK(indices_are(&quad, 1));

    /* Mirroring one axis flips the winding so the patch still faces +normal. */
    p.v_axis = v3(0.0f, 0.0f, 3.0f);
    CHECK(pixelica_light_quad(&p, &quad) == PIXELICA_OK);
    CHECK(indices_are(&quad, 0));

    memset(&before, 0xCD, sizeof before);
    quad = before;
    p.v_axis = v3(4.0f, 0.0f, 0.0f); /* parallel to u_axis */
    CHECK(pixelica_light_quad(&p, &quad) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&quad, &before, sizeof quad) == 0);
    p.v_axis = v3(0.0f, 0.0f, 3.0f);
    p.normal = v3(0.0f, 0.0f, 0.0f);
    CHECK(pixelica_light_quad(&p, &quad) == PIXELICA_ERR_INVALID_ARG);
    p.normal = v3(1.0f, 0.0f, 0.0f); /* lies in the u/v plane: no front side */
    CHECK(pixelica_light_quad(&p, &quad) == PIXELICA_ERR_INVALID_ARG);
    p.normal = v3(0.0f, 1.0f, 0.0f);
    p.bias = pixelica_t_nan();
    CHECK(pixelica_light_quad(&p, &quad) == PIXELICA_ERR_INVALID_ARG);
    p.bias = 0.0f;
    p.u_axis = v3(pixelica_t_inf(), 0.0f, 0.0f);
    CHECK(pixelica_light_quad(&p, &quad) == PIXELICA_ERR_INVALID_ARG);
    /* Finite but overflowing: 1e38 + 1e38 does not fit in a float. */
    p.origin = v3(3.0e38f, 0.0f, 0.0f);
    p.u_axis = v3(3.0e38f, 0.0f, 0.0f);
    CHECK(pixelica_light_quad(&p, &quad) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&quad, &before, sizeof quad) == 0);
    CHECK(pixelica_light_quad(NULL, &quad) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_quad(&p, NULL) == PIXELICA_ERR_INVALID_ARG);
}

static void test_quad_centered(void)
{
    PIXELICA_LightQuad quad, before;
    float cx, cy, cz;
    int i;

    CHECK(pixelica_light_quad_centered(v3(0.0f, 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f),
                                       v3(1.0f, 0.0f, 0.0f), 2.0f, 3.0f, 0.01f,
                                       &quad) == PIXELICA_OK);
    cx = 0.0f;
    cy = 0.0f;
    cz = 0.0f;
    for (i = 0; i < 4; ++i) {
        cx += quad.vertices[i].x;
        cy += quad.vertices[i].y;
        cz += quad.vertices[i].z;
    }
    CHECK_NEAR(cx / 4.0, 0.0, 1e-6);
    CHECK_NEAR(cy / 4.0, 0.01, 1e-6); /* the whole quad is lifted by bias */
    CHECK_NEAR(cz / 4.0, 0.0, 1e-6);
    /* 2 * radius per axis. */
    CHECK_NEAR(quad.vertices[1].x - quad.vertices[0].x, 4.0, 1e-6);
    CHECK_NEAR(quad.vertices[3].z - quad.vertices[0].z, -6.0, 1e-6);
    CHECK(indices_are(&quad, 1));

    /* An off-plane u_axis hint is projected into the plane. */
    {
        PIXELICA_LightQuad tilted;

        CHECK(pixelica_light_quad_centered(v3(0.0f, 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f),
                                           v3(1.0f, 5.0f, 0.0f), 2.0f, 3.0f, 0.01f,
                                           &tilted) == PIXELICA_OK);
        for (i = 0; i < 4; ++i) {
            CHECK_NEAR(tilted.vertices[i].x, quad.vertices[i].x, 1e-5);
            CHECK_NEAR(tilted.vertices[i].y, quad.vertices[i].y, 1e-5);
            CHECK_NEAR(tilted.vertices[i].z, quad.vertices[i].z, 1e-5);
        }
    }

    memset(&before, 0xCD, sizeof before);
    quad = before;
    CHECK(pixelica_light_quad_centered(v3(0.0f, 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f),
                                       v3(0.0f, 1.0f, 0.0f), 1.0f, 1.0f, 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG); /* u_axis parallel to the normal */
    CHECK(pixelica_light_quad_centered(v3(0.0f, 0.0f, 0.0f), v3(0.0f, 0.0f, 0.0f),
                                       v3(1.0f, 0.0f, 0.0f), 1.0f, 1.0f, 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_quad_centered(v3(0.0f, 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f),
                                       v3(1.0f, 0.0f, 0.0f), 0.0f, 1.0f, 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG); /* radius must be > 0 */
    CHECK(pixelica_light_quad_centered(v3(0.0f, 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f),
                                       v3(1.0f, 0.0f, 0.0f), 1.0f, -1.0f, 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_quad_centered(v3(pixelica_t_nan(), 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f),
                                       v3(1.0f, 0.0f, 0.0f), 1.0f, 1.0f, 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_quad_centered(v3(0.0f, 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f),
                                       v3(1.0f, 0.0f, 0.0f), 1.0f, 1.0f, 0.0f, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&quad, &before, sizeof quad) == 0);
}

static void test_quad_project(void)
{
    PIXELICA_LightPlacement window;
    PIXELICA_LightQuad quad, before;

    /* The Pixel Room window: north wall, x in [0.5, 2.5], y in [1.0, 2.3]. */
    memset(&window, 0, sizeof window);
    window.origin = v3(0.5f, 1.0f, -3.95f);
    window.u_axis = v3(2.0f, 0.0f, 0.0f);
    window.v_axis = v3(0.0f, 1.3f, 0.0f);
    window.normal = v3(0.0f, 0.0f, 1.0f);

    /* Sunlight going down and into the room lands as a parallelogram. */
    CHECK(pixelica_light_quad_project(&window, v3(0.0f, -0.8f, 0.6f),
                                      v3(0.0f, 0.0f, 0.0f), v3(0.0f, 1.0f, 0.0f),
                                      0.002f, &quad) == PIXELICA_OK);
    CHECK_NEAR(quad.vertices[0].x, 0.5, 1e-5);
    CHECK_NEAR(quad.vertices[0].y, 0.002, 1e-6);   /* on the floor plus bias */
    CHECK_NEAR(quad.vertices[0].z, -3.95 + 0.75, 1e-5);
    CHECK_NEAR(quad.vertices[1].x, 2.5, 1e-5);
    CHECK_NEAR(quad.vertices[3].z, -3.95 + 1.725, 1e-4);
    CHECK_NEAR(quad.vertices[3].y, 0.002, 1e-6);
    CHECK(indices_are(&quad, 0));

    memset(&before, 0xCD, sizeof before);
    quad = before;
    /* Straight down: a vertical window collapses to a line. */
    CHECK(pixelica_light_quad_project(&window, v3(0.0f, -1.0f, 0.0f), v3(0.0f, 0.0f, 0.0f),
                                      v3(0.0f, 1.0f, 0.0f), 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    /* Light travelling away from the receiver (t <= 0). */
    CHECK(pixelica_light_quad_project(&window, v3(0.0f, 1.0f, 0.6f), v3(0.0f, 0.0f, 0.0f),
                                      v3(0.0f, 1.0f, 0.0f), 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    /* Parallel to the receiver plane. */
    CHECK(pixelica_light_quad_project(&window, v3(1.0f, 0.0f, 0.0f), v3(0.0f, 0.0f, 0.0f),
                                      v3(0.0f, 1.0f, 0.0f), 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_quad_project(&window, v3(0.0f, 0.0f, 0.0f), v3(0.0f, 0.0f, 0.0f),
                                      v3(0.0f, 1.0f, 0.0f), 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_quad_project(&window, v3(0.0f, -1.0f, 0.6f), v3(0.0f, 0.0f, 0.0f),
                                      v3(0.0f, 0.0f, 0.0f), 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_quad_project(&window, v3(0.0f, -1.0f, 0.6f), v3(0.0f, 0.0f, 0.0f),
                                      v3(0.0f, 1.0f, 0.0f), pixelica_t_nan(), &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_light_quad_project(NULL, v3(0.0f, -1.0f, 0.6f), v3(0.0f, 0.0f, 0.0f),
                                      v3(0.0f, 1.0f, 0.0f), 0.0f, &quad) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&quad, &before, sizeof quad) == 0);

    /* A wall receiver works the same way: project onto x = -4 (normal +X). */
    CHECK(pixelica_light_quad_project(&window, v3(-0.7f, -0.3f, 0.4f),
                                      v3(-4.0f, 0.0f, 0.0f), v3(1.0f, 0.0f, 0.0f),
                                      0.004f, &quad) == PIXELICA_OK);
    CHECK_NEAR(quad.vertices[0].x, -4.0 + 0.004, 1e-5);
    CHECK_NEAR(quad.vertices[1].x, -4.0 + 0.004, 1e-5);
}

int main(void)
{
    test_defaults();
    test_rect_shape();
    test_rect_bars();
    test_ellipse_shape();
    test_one_pixel_mask();
    test_mask_rejects();
    test_tint_fill();
    test_quad();
    test_quad_centered();
    test_quad_project();
    return pixelica_t_finish("test_light_mask");
}

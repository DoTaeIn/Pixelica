/*
 * Selective bloom (pixelica/light.h, src/light/bloom.c).
 *
 * Common-library test: it runs on a plain caller-owned RGB565 surface, which
 * is what both a 2D render target and a snapshot of a 3D frame are.
 */
#include "pixelica/light.h"
#include "test_util.h"

#define TW 16u
#define TH 12u
#define PIXELS (TW * TH)

static uint16_t g_color[PIXELS];
static uint16_t g_emissive[PIXELS];
static uint8_t g_mask[PIXELS];
static uint16_t g_scratch[6u * PIXELS];

static const uint16_t k_background = 0x2104u; /* r=4 g=8 b=4 */
static const uint16_t k_lamp = 0xFFE0u;       /* bright warm (r=31 g=63 b=0) */

static void surface_init(PIXELICA_LightSurface *s)
{
    s->pixels = g_color;
    s->width = (uint16_t)TW;
    s->height = (uint16_t)TH;
    s->stride = TW;
}

static void fill_background(void)
{
    size_t i;

    for (i = 0; i < PIXELS; ++i) {
        g_color[i] = k_background;
    }
}

static unsigned int red_of(uint16_t c)
{
    return ((unsigned int)c >> 11) & 31u;
}

static unsigned int green_of(uint16_t c)
{
    return ((unsigned int)c >> 5) & 63u;
}

static unsigned int blue_of(uint16_t c)
{
    return (unsigned int)c & 31u;
}

/* A 4x3 block of emissive pixels in the middle of the frame. */
static void mark_block(void)
{
    uint32_t x, y;

    memset(g_mask, 0, sizeof g_mask);
    for (y = 4; y < 7u; ++y) {
        for (x = 6; x < 10u; ++x) {
            g_mask[(size_t)y * TW + x] = 255u;
            g_color[(size_t)y * TW + x] = k_lamp;
        }
    }
}

static void test_defaults_and_scratch(void)
{
    PIXELICA_LightSurface surface;
    PIXELICA_LightMask mask;
    PIXELICA_BloomDesc desc;

    CHECK(pixelica_bloom_default(NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    CHECK(desc.mask == NULL && desc.emissive == NULL);
    CHECK_EQ_LONG(desc.downsample, 4);
    CHECK_EQ_LONG(desc.passes, 2);
    CHECK_EQ_LONG(desc.threshold, 0);

    surface_init(&surface);
    mask.alpha = g_mask;
    mask.width = (uint16_t)TW;
    mask.height = (uint16_t)TH;
    mask.stride = TW;

    /* An explicit selection is REQUIRED: the default descriptor has none. */
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 0);
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_ERR_INVALID_ARG);

    desc.mask = &mask;
    /* 16/4 = 4, 12/4 = 3 -> 6 * 4 * 3 */
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 6u * 4u * 3u);
    desc.downsample = 1u;
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 6u * PIXELS);
    /* Non-divisible: ceil(16/3) = 6, ceil(12/3) = 4. */
    desc.downsample = 3u;
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 6u * 6u * 4u);
    /* 5 does not divide either axis: ceil(16/5) = 4, ceil(12/5) = 3. */
    desc.downsample = 5u;
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 6u * 4u * 3u);

    /* scratch_pixels agrees with apply about what is valid: a mask of the
     * wrong size is rejected by BOTH. */
    mask.width = (uint16_t)(TW - 1u);
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 0);
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_ERR_INVALID_ARG);
    mask.width = (uint16_t)TW;
    mask.stride = TW - 1u; /* stride < width */
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 0);
    mask.stride = TW;
    mask.alpha = NULL;
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 0);
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(NULL, &desc), 0);
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, NULL), 0);
}

/*
 * The point of "selective": a non-emissive pixel the glow does not reach
 * keeps its EXACT value, and the glow is added, never blurred over the frame.
 */
static void test_selectivity(void)
{
    PIXELICA_LightSurface surface;
    PIXELICA_LightMask mask;
    PIXELICA_BloomDesc desc;
    uint16_t sharp_edge[PIXELS];
    size_t i;
    uint32_t changed = 0u, y;

    fill_background();
    mark_block();
    /* A hard black/white edge far from the lamp must stay hard. */
    for (y = 0; y < TH; ++y) {
        g_color[(size_t)y * TW + 0u] = 0xFFFFu;
        g_color[(size_t)y * TW + 1u] = 0x0000u;
    }
    memcpy(sharp_edge, g_color, sizeof sharp_edge);

    surface_init(&surface);
    mask.alpha = g_mask;
    mask.width = (uint16_t)TW;
    mask.height = (uint16_t)TH;
    mask.stride = TW;
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    desc.mask = &mask;
    desc.downsample = 2u;
    desc.radius = 2u;
    desc.passes = 1u;
    desc.intensity = 255u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);

    for (i = 0; i < PIXELS; ++i) {
        if (g_color[i] != sharp_edge[i]) {
            ++changed;
        }
        /* Additive only: no channel may ever decrease. */
        CHECK(red_of(g_color[i]) >= red_of(sharp_edge[i]));
        CHECK(green_of(g_color[i]) >= green_of(sharp_edge[i]));
        CHECK(blue_of(g_color[i]) >= blue_of(sharp_edge[i]));
    }
    CHECK(changed > 0u);
    CHECK(changed < PIXELS); /* not a full-frame effect */
    /* The far edge column keeps its exact black; the white stays white. */
    CHECK_EQ_LONG(g_color[(size_t)(TH - 1u) * TW + 1u], 0x0000);
    CHECK_EQ_LONG(g_color[(size_t)(TH - 1u) * TW + 0u], 0xFFFF);
    /* A ring around the lamp got brighter than the background. */
    CHECK(red_of(g_color[(size_t)3u * TW + 8u]) > red_of(k_background));
    CHECK(red_of(g_color[(size_t)7u * TW + 8u]) > red_of(k_background));
    /* The lamp is warm, so the glow adds red and green but no blue. */
    CHECK(green_of(g_color[(size_t)3u * TW + 8u]) > green_of(k_background));
    CHECK_EQ_LONG(blue_of(g_color[(size_t)3u * TW + 8u]), blue_of(k_background));
}

static void test_mask_gates_everything(void)
{
    PIXELICA_LightSurface surface;
    PIXELICA_LightMask mask;
    PIXELICA_BloomDesc desc;
    uint16_t before[PIXELS];

    /* A bright frame with an all-zero mask must not glow at all: brightness
     * alone never selects. */
    fill_background();
    mark_block();
    memset(g_mask, 0, sizeof g_mask);
    memcpy(before, g_color, sizeof before);

    surface_init(&surface);
    mask.alpha = g_mask;
    mask.width = (uint16_t)TW;
    mask.height = (uint16_t)TH;
    mask.stride = TW;
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    desc.mask = &mask;
    desc.intensity = 255u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    CHECK(memcmp(g_color, before, sizeof before) == 0);

    /* intensity 0 is also a no-op. */
    mark_block();
    memcpy(before, g_color, sizeof before);
    desc.intensity = 0u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    CHECK(memcmp(g_color, before, sizeof before) == 0);

    /* A partial mask value scales the contribution down. */
    {
        uint32_t x, y;
        unsigned int full, half;

        desc.intensity = 255u;
        desc.downsample = 2u;
        desc.radius = 1u;
        desc.passes = 1u;
        fill_background();
        mark_block();
        CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
              PIXELICA_OK);
        full = red_of(g_color[(size_t)3u * TW + 8u]);

        fill_background();
        mark_block();
        for (y = 0; y < TH; ++y) {
            for (x = 0; x < TW; ++x) {
                if (g_mask[(size_t)y * TW + x] != 0u) {
                    g_mask[(size_t)y * TW + x] = 128u;
                }
            }
        }
        CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
              PIXELICA_OK);
        half = red_of(g_color[(size_t)3u * TW + 8u]);
        CHECK(half < full);
        CHECK(half > red_of(k_background));
    }
}

static void test_emissive_image_and_threshold(void)
{
    PIXELICA_LightSurface surface;
    PIXELICA_FrameView emissive;
    PIXELICA_BloomDesc desc;
    PIXELICA_LightMask mask;
    size_t i;

    /* An emissive IMAGE selects on its own: black where nothing emits. */
    fill_background();
    for (i = 0; i < PIXELS; ++i) {
        g_emissive[i] = 0u;
    }
    g_emissive[(size_t)6u * TW + 8u] = k_lamp;

    surface_init(&surface);
    emissive.color = g_emissive;
    emissive.depth = NULL;
    emissive.width = (uint16_t)TW;
    emissive.height = (uint16_t)TH;
    emissive.stride = TW;
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    desc.emissive = &emissive;
    desc.downsample = 1u;
    desc.radius = 2u;
    desc.passes = 1u;
    desc.intensity = 255u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    CHECK(red_of(g_color[(size_t)6u * TW + 8u]) > red_of(k_background));
    CHECK(red_of(g_color[(size_t)6u * TW + 9u]) > red_of(k_background));
    CHECK_EQ_LONG(g_color[0], k_background); /* far away: untouched */

    /* threshold gates INSIDE the selection: a mid-bright emissive pixel
     * (luma 131) drops out above it and glows below it. */
    fill_background();
    for (i = 0; i < PIXELS; ++i) {
        g_emissive[i] = 0u;
    }
    g_emissive[(size_t)6u * TW + 8u] = 0x8410u;
    desc.threshold = 200u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    for (i = 0; i < PIXELS; ++i) {
        CHECK_EQ_LONG(g_color[i], k_background);
    }
    desc.threshold = 100u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    CHECK(g_color[(size_t)6u * TW + 8u] != k_background);
    desc.threshold = 0u;

    /* mask AND image together multiply. */
    fill_background();
    for (i = 0; i < PIXELS; ++i) {
        g_emissive[i] = k_lamp;
    }
    memset(g_mask, 0, sizeof g_mask);
    g_mask[(size_t)6u * TW + 8u] = 255u;
    mask.alpha = g_mask;
    mask.width = (uint16_t)TW;
    mask.height = (uint16_t)TH;
    mask.stride = TW;
    desc.mask = &mask;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    CHECK(g_color[(size_t)6u * TW + 8u] != k_background);
    CHECK_EQ_LONG(g_color[0], k_background);
}

/* The target itself may be the emissive source: that is the "bloom the frame
 * I just drew, through this mask" case, and it must stay correct even though
 * the composite writes the same memory it read. */
static void test_source_may_alias_target(void)
{
    PIXELICA_LightSurface surface;
    PIXELICA_FrameView view;
    PIXELICA_LightMask mask;
    PIXELICA_BloomDesc desc;
    uint16_t reference[PIXELS];

    fill_background();
    mark_block();
    surface_init(&surface);
    mask.alpha = g_mask;
    mask.width = (uint16_t)TW;
    mask.height = (uint16_t)TH;
    mask.stride = TW;
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    desc.mask = &mask;
    desc.downsample = 2u;
    desc.radius = 2u;
    desc.passes = 2u;
    desc.intensity = 200u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    memcpy(reference, g_color, sizeof reference);

    /* Same thing, but the emissive image explicitly points at the target. */
    fill_background();
    mark_block();
    view.color = g_color;
    view.depth = NULL;
    view.width = (uint16_t)TW;
    view.height = (uint16_t)TH;
    view.stride = TW;
    desc.emissive = &view;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    CHECK(memcmp(g_color, reference, sizeof reference) == 0);
}

/*
 * Regression: a descriptor living INSIDE the target pixel buffer would be
 * corrupted by the composite (and could redirect the writes out of bounds).
 */
static void test_descriptor_in_target_is_rejected(void)
{
    union {
        PIXELICA_LightSurface surface;
        PIXELICA_LightMask mask;
        PIXELICA_FrameView view;
        PIXELICA_BloomDesc desc;
        uint16_t pixels[256];
    } backing;
    PIXELICA_LightSurface surface;
    PIXELICA_LightMask mask;
    PIXELICA_FrameView view;
    PIXELICA_BloomDesc desc;

    memset(&backing, 0, sizeof backing);
    memset(g_mask, 255, sizeof g_mask);

    /* (a) the target descriptor is inside the target pixels */
    backing.surface.pixels = backing.pixels;
    backing.surface.width = 4u;
    backing.surface.height = 4u;
    backing.surface.stride = 4u;
    mask.alpha = g_mask;
    mask.width = 4u;
    mask.height = 4u;
    mask.stride = 4u;
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    desc.mask = &mask;
    desc.downsample = 1u;
    desc.radius = 1u;
    desc.passes = 1u;
    CHECK(pixelica_bloom_apply(&backing.surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&backing.surface, &desc), 0);

    /* (b) the mask descriptor is inside the target pixels */
    surface.pixels = backing.pixels;
    surface.width = 4u;
    surface.height = 4u;
    surface.stride = 4u;
    memset(&backing, 0, sizeof backing);
    backing.mask.alpha = g_mask;
    backing.mask.width = 4u;
    backing.mask.height = 4u;
    backing.mask.stride = 4u;
    desc.mask = &backing.mask;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_ERR_INVALID_ARG);

    /* (c) the bloom descriptor itself is inside the target pixels */
    memset(&backing, 0, sizeof backing);
    CHECK(pixelica_bloom_default(&backing.desc) == PIXELICA_OK);
    backing.desc.mask = &mask;
    backing.desc.downsample = 1u;
    backing.desc.radius = 1u;
    backing.desc.passes = 1u;
    CHECK(pixelica_bloom_apply(&surface, &backing.desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_ERR_INVALID_ARG);

    /* (d) the emissive view descriptor is inside the target pixels */
    memset(&backing, 0, sizeof backing);
    backing.view.color = g_emissive;
    backing.view.depth = NULL;
    backing.view.width = 4u;
    backing.view.height = 4u;
    backing.view.stride = 4u;
    desc.mask = NULL;
    desc.emissive = &backing.view;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_ERR_INVALID_ARG);

    /* The same surface with descriptors OUTSIDE it is accepted. */
    desc.emissive = &view;
    view.color = g_emissive;
    view.depth = NULL;
    view.width = 4u;
    view.height = 4u;
    view.stride = 4u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
}

static void test_scratch_rules(void)
{
    PIXELICA_LightSurface surface;
    PIXELICA_LightMask mask;
    PIXELICA_BloomDesc desc;
    uint16_t before[PIXELS];
    size_t needed;

    fill_background();
    mark_block();
    memcpy(before, g_color, sizeof before);
    surface_init(&surface);
    mask.alpha = g_mask;
    mask.width = (uint16_t)TW;
    mask.height = (uint16_t)TH;
    mask.stride = TW;
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    desc.mask = &mask;
    desc.downsample = 2u;
    desc.intensity = 255u;
    needed = pixelica_bloom_scratch_pixels(&surface, &desc);
    CHECK_EQ_LONG(needed, 6u * 8u * 6u);

    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, needed - 1u) ==
          PIXELICA_ERR_CAPACITY);
    CHECK(memcmp(g_color, before, sizeof before) == 0); /* nothing written */
    CHECK(pixelica_bloom_apply(&surface, &desc, NULL, needed) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(g_color, before, sizeof before) == 0);
    /* Scratch overlapping the target or a read-only source is refused. */
    CHECK(pixelica_bloom_apply(&surface, &desc, g_color, needed) ==
          PIXELICA_ERR_INVALID_ARG);
    {
        PIXELICA_FrameView view;
        PIXELICA_BloomDesc with_image = desc;

        view.color = g_emissive;
        view.depth = NULL;
        view.width = (uint16_t)TW;
        view.height = (uint16_t)TH;
        view.stride = TW;
        with_image.emissive = &view;
        CHECK(pixelica_bloom_apply(&surface, &with_image, g_emissive, needed) ==
              PIXELICA_ERR_INVALID_ARG);
        /* ... and so is scratch overlapping a descriptor. */
        CHECK(pixelica_bloom_apply(&surface, &with_image,
                                   (uint16_t *)(void *)&with_image, needed) ==
              PIXELICA_ERR_INVALID_ARG);
    }
    CHECK(memcmp(g_color, before, sizeof before) == 0);
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, needed) == PIXELICA_OK);

    /* Field ranges. */
    {
        unsigned int i;

        for (i = 0; i < 6u; ++i) {
            PIXELICA_BloomDesc bad = desc;

            switch (i) {
            case 0: bad.downsample = 0u; break;
            case 1: bad.downsample = (uint8_t)(PIXELICA_BLOOM_MAX_DOWNSAMPLE + 1u); break;
            case 2: bad.radius = 0u; break;
            case 3: bad.radius = (uint8_t)(PIXELICA_BLOOM_MAX_RADIUS + 1u); break;
            case 4: bad.passes = 0u; break;
            default: bad.passes = (uint8_t)(PIXELICA_BLOOM_MAX_PASSES + 1u); break;
            }
            CHECK(pixelica_bloom_apply(&surface, &bad, g_scratch, sizeof g_scratch / 2u) ==
                  PIXELICA_ERR_INVALID_ARG);
            CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &bad), 0);
        }
    }
    /* An invalid target. */
    {
        PIXELICA_LightSurface bad = surface;

        bad.pixels = NULL;
        CHECK(pixelica_bloom_apply(&bad, &desc, g_scratch, sizeof g_scratch / 2u) ==
              PIXELICA_ERR_INVALID_ARG);
        bad = surface;
        bad.width = 0u;
        CHECK(pixelica_bloom_apply(&bad, &desc, g_scratch, sizeof g_scratch / 2u) ==
              PIXELICA_ERR_INVALID_ARG);
        bad = surface;
        bad.stride = TW - 1u;
        CHECK(pixelica_bloom_apply(&bad, &desc, g_scratch, sizeof g_scratch / 2u) ==
              PIXELICA_ERR_INVALID_ARG);
    }
    CHECK(pixelica_bloom_apply(NULL, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_bloom_apply(&surface, NULL, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_ERR_INVALID_ARG);
}

/* Odd sizes, 1x1 frames, padded strides and saturation. */
static void test_edges(void)
{
    PIXELICA_LightSurface surface;
    PIXELICA_LightMask mask;
    PIXELICA_BloomDesc desc;
    uint16_t one = 0u;
    uint8_t one_mask = 255u;
    uint16_t padded[5u * 3u]; /* 3x3 used, stride 5 */
    uint8_t padded_mask[5u * 3u];
    size_t i;

    /* 1x1 frame with the maximum downsample. */
    surface.pixels = &one;
    surface.width = 1u;
    surface.height = 1u;
    surface.stride = 1u;
    mask.alpha = &one_mask;
    mask.width = 1u;
    mask.height = 1u;
    mask.stride = 1u;
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    desc.mask = &mask;
    desc.downsample = (uint8_t)PIXELICA_BLOOM_MAX_DOWNSAMPLE;
    desc.radius = (uint8_t)PIXELICA_BLOOM_MAX_RADIUS;
    desc.passes = (uint8_t)PIXELICA_BLOOM_MAX_PASSES;
    desc.intensity = 255u;
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 6);
    one = 0x4208u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, 6u) == PIXELICA_OK);
    CHECK(red_of(one) > 8u); /* it glowed onto itself */
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, 5u) == PIXELICA_ERR_CAPACITY);

    /* Saturation: a white frame fully selected stays white, never wraps. */
    for (i = 0; i < 15u; ++i) {
        padded[i] = 0xFFFFu;
        padded_mask[i] = 255u;
    }
    surface.pixels = padded;
    surface.width = 3u;
    surface.height = 3u;
    surface.stride = 5u;
    mask.alpha = padded_mask;
    mask.width = 3u;
    mask.height = 3u;
    mask.stride = 5u;
    desc.downsample = 2u; /* 3 is not divisible by 2 */
    desc.radius = 1u;
    desc.passes = 1u;
    CHECK_EQ_LONG(pixelica_bloom_scratch_pixels(&surface, &desc), 6u * 2u * 2u);
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, 6u * 2u * 2u) == PIXELICA_OK);
    for (i = 0; i < 3u; ++i) {
        CHECK_EQ_LONG(padded[i * 5u + 0u], 0xFFFF);
        CHECK_EQ_LONG(padded[i * 5u + 2u], 0xFFFF);
        /* The row padding is never touched. */
        CHECK_EQ_LONG(padded[i * 5u + 3u], 0xFFFF);
    }
    /* Padding really is skipped: poison it and check it survives. */
    padded[3] = 0x1234u;
    padded[4] = 0x5678u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, 6u * 2u * 2u) == PIXELICA_OK);
    CHECK_EQ_LONG(padded[3], 0x1234);
    CHECK_EQ_LONG(padded[4], 0x5678);
}

/*
 * Documented precision limit: a single dim texel can be averaged away by a
 * large downsample, and a small `downsample`/`radius` brings it back. These
 * are the numbers quoted in docs/pixel_light.md.
 */
static void test_precision_limits(void)
{
    PIXELICA_LightSurface surface;
    PIXELICA_LightMask mask;
    PIXELICA_BloomDesc desc;
    uint16_t neighbour;

    surface_init(&surface);
    mask.alpha = g_mask;
    mask.width = (uint16_t)TW;
    mask.height = (uint16_t)TH;
    mask.stride = TW;
    CHECK(pixelica_bloom_default(&desc) == PIXELICA_OK);
    desc.mask = &mask;
    desc.intensity = 255u;

    /* One mid-bright texel, downsample 8 + radius 8: spread over 64 pixels and
     * then 17 taps, it no longer reaches one native channel step anywhere. */
    fill_background();
    memset(g_mask, 0, sizeof g_mask);
    g_mask[(size_t)6u * TW + 8u] = 255u;
    g_color[(size_t)6u * TW + 8u] = 0x8410u; /* r=16 g=32 b=16 */
    desc.downsample = 8u;
    desc.radius = 8u;
    desc.passes = 1u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    neighbour = g_color[(size_t)6u * TW + 9u];
    CHECK_EQ_LONG(neighbour, k_background); /* measured: the glow vanishes */

    /* The same emitter with downsample 1 and radius 2 is clearly visible. */
    fill_background();
    g_color[(size_t)6u * TW + 8u] = 0x8410u;
    desc.downsample = 1u;
    desc.radius = 2u;
    CHECK(pixelica_bloom_apply(&surface, &desc, g_scratch, sizeof g_scratch / 2u) ==
          PIXELICA_OK);
    CHECK(g_color[(size_t)6u * TW + 9u] != k_background);
    CHECK(red_of(g_color[(size_t)6u * TW + 9u]) > red_of(k_background));
}

int main(void)
{
    test_defaults_and_scratch();
    test_selectivity();
    test_mask_gates_everything();
    test_emissive_image_and_threshold();
    test_source_may_alias_target();
    test_descriptor_in_target_is_rejected();
    test_scratch_rules();
    test_edges();
    test_precision_limits();
    return pixelica_t_finish("test_bloom");
}

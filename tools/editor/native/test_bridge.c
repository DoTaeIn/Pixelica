/* Unit test for the editor bridge's exported C ABI. No assert(): Release
 * builds define NDEBUG, so failures are counted explicitly. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"

static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define W 32
#define H 24
static unsigned char g_rgb[W * H * 3];

static const double PI = 3.14159265358979323846;

static int read_frame(pe_bridge *b)
{
    return pe_read_rgb(b, g_rgb, sizeof(g_rgb));
}

static int pixel_is(int x, int y, int r, int g, int b)
{
    const unsigned char *p = &g_rgb[((size_t)y * W + (size_t)x) * 3u];
    return p[0] == r && p[1] == g && p[2] == b;
}

static void test_create_limits(void)
{
    pe_bridge *b = NULL;
    CHECK(pe_abi_version() == PE_ABI_VERSION);
    CHECK(pe_create(0, 10, &b) == PE_ERR_INVALID_ARG && b == NULL);
    CHECK(pe_create(10, 0, &b) == PE_ERR_INVALID_ARG && b == NULL);
    CHECK(pe_create(-5, 10, &b) == PE_ERR_INVALID_ARG && b == NULL);
    CHECK(pe_create(PE_MAX_DIMENSION + 1, 1, &b) == PE_ERR_INVALID_ARG && b == NULL);
    CHECK(pe_create(PE_MAX_DIMENSION, 1024, &b) == PE_ERR_INVALID_ARG && b == NULL); /* > max pixels */
    CHECK(pe_create(10, 10, NULL) == PE_ERR_INVALID_ARG);
    CHECK(pe_create(1024, 1024, &b) == PE_OK && b != NULL);
    pe_destroy(b);
    pe_destroy(NULL);
    CHECK(pe_status_message(PE_ERR_STATE) != NULL);
    CHECK(pe_status_message(12345) != NULL);
}

static void test_begin_and_rgb_conversion(pe_bridge *b)
{
    unsigned char small[3] = {7, 7, 7};
    CHECK(read_frame(b) == PE_ERR_STATE);                       /* no frame yet */
    CHECK(pe_draw_rect(b, 0, 0, 1, 1, 0, 0xFFFFFF, 255) == PE_ERR_STATE);
    CHECK(pe_begin(b, 0x1000000u) == PE_ERR_INVALID_ARG);
    CHECK(pe_begin(NULL, 0) == PE_ERR_INVALID_ARG);

    CHECK(pe_begin(b, 0xFF0000u) == PE_OK);
    CHECK(read_frame(b) == PE_OK);
    CHECK(pixel_is(0, 0, 255, 0, 0));
    CHECK(pixel_is(W - 1, H - 1, 255, 0, 0));

    /* RGB565 quantization with bit replication on the way back. */
    CHECK(pe_begin(b, 0x808080u) == PE_OK);
    CHECK(read_frame(b) == PE_OK);
    CHECK(pixel_is(5, 5, 132, 130, 132));

    CHECK(pe_read_rgb(b, small, sizeof(small)) == PE_ERR_NO_MEMORY);
    CHECK(small[0] == 7 && small[1] == 7 && small[2] == 7);     /* untouched */
    CHECK(pe_read_rgb(b, NULL, 1000000) == PE_ERR_INVALID_ARG);
}

static void test_rect_alpha(pe_bridge *b)
{
    CHECK(pe_begin(b, 0x000000u) == PE_OK);
    CHECK(pe_draw_rect(b, 2, 2, 4, 3, 0, 0x00FF00u, 255) == PE_OK);
    CHECK(pe_draw_rect(b, 10, 2, 4, 4, 0, 0xFFFFFFu, 128) == PE_OK);
    CHECK(pe_draw_rect(b, 20, 2, 4, 4, 0, 0xFFFFFFu, 0) == PE_OK);
    CHECK(read_frame(b) == PE_OK);
    CHECK(pixel_is(2, 2, 0, 255, 0));
    CHECK(pixel_is(5, 4, 0, 255, 0));
    CHECK(pixel_is(6, 2, 0, 0, 0));       /* half-open extent */
    CHECK(pixel_is(2, 5, 0, 0, 0));
    CHECK(pixel_is(11, 3, 132, 130, 132)); /* (31*128+127)/255 etc. */
    CHECK(pixel_is(21, 3, 0, 0, 0));      /* opacity 0 writes nothing */
}

static void test_rotation(pe_bridge *b)
{
    /* 10x2 bar pivoting on its top-left (10,10); +90 degrees turns clockwise,
     * so it spans x in [8,10), y in [10,20). */
    CHECK(pe_begin(b, 0x000000u) == PE_OK);
    CHECK(pe_draw_rect(b, 10, 10, 10, 2, PI / 2.0, 0xFF0000u, 255) == PE_OK);
    CHECK(read_frame(b) == PE_OK);
    CHECK(pixel_is(9, 15, 255, 0, 0));
    CHECK(pixel_is(8, 18, 255, 0, 0));
    CHECK(pixel_is(15, 10, 0, 0, 0)); /* where the unrotated bar would be */
    CHECK(pixel_is(11, 15, 0, 0, 0));

    /* Negative extent mirrors around the pivot. */
    CHECK(pe_begin(b, 0x000000u) == PE_OK);
    CHECK(pe_draw_rect(b, 10, 10, -4, 2, 0, 0x0000FFu, 255) == PE_OK);
    CHECK(read_frame(b) == PE_OK);
    CHECK(pixel_is(7, 10, 0, 0, 255));
    CHECK(pixel_is(10, 10, 0, 0, 0));
}

static void test_invalid_draws(pe_bridge *b)
{
    /* Runtime-made specials: the NAN/INFINITY macros trip constant-overflow
     * warnings on some MSVC versions. */
    const double nan_value = strtod("nan", NULL);
    const double inf_value = strtod("inf", NULL);
    CHECK(isnan(nan_value) && isinf(inf_value));
    CHECK(pe_begin(b, 0x000000u) == PE_OK);
    CHECK(pe_draw_rect(b, nan_value, 0, 4, 4, 0, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, inf_value, 4, 4, 0, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, 0, 4, 4, inf_value, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, 0, 0, 4, 0, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, 0, 4, 0, 0, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, 0, 1e300, 4, 0, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 1e300, 0, 4, 4, 0, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, 0, 1e-300, 4, 0, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG); /* float underflow */
    CHECK(pe_draw_rect(b, 0, 0, 4, 4, 1e9, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, 0, 4, 4, 0, 0x1000000u, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, 0, 4, 4, 0, 0xFFFFFFu, 256) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(b, 0, 0, 4, 4, 0, 0xFFFFFFu, -1) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_rect(NULL, 0, 0, 4, 4, 0, 0xFFFFFFu, 255) == PE_ERR_INVALID_ARG);
    /* Far off-screen but finite is fine and draws nothing. */
    CHECK(pe_draw_rect(b, -5e6, 5e6, 100, 100, 1.0, 0xFFFFFFu, 255) == PE_OK);
    CHECK(read_frame(b) == PE_OK);
    CHECK(pixel_is(0, 0, 0, 0, 0));
    CHECK(pixel_is(W - 1, H - 1, 0, 0, 0));
}

static void test_textures(pe_bridge *b)
{
    /* 2x2: opaque red, transparent green, half-alpha white, opaque blue. */
    const unsigned char rgba[16] = {
        255, 0, 0, 255,   0, 255, 0, 0,
        255, 255, 255, 128, 0, 0, 255, 255,
    };
    uint32_t id = 0, id2 = 0, count = 99;
    size_t bytes = 99;
    CHECK(pe_texture_create(b, rgba, sizeof(rgba), 2, 2, &id) == PE_OK && id != 0);
    CHECK(pe_texture_stats(b, &count, &bytes) == PE_OK && count == 1 && bytes == 12);

    CHECK(pe_begin(b, 0x000000u) == PE_OK);
    /* Scaled 2x: each texel covers 2x2 pixels. */
    CHECK(pe_draw_texture(b, id, 0, 0, 4, 4, 0, 255) == PE_OK);
    CHECK(read_frame(b) == PE_OK);
    CHECK(pixel_is(0, 0, 255, 0, 0));
    CHECK(pixel_is(1, 1, 255, 0, 0));
    CHECK(pixel_is(2, 0, 0, 0, 0));       /* alpha 0 keeps the background */
    CHECK(pixel_is(3, 1, 0, 0, 0));
    CHECK(pixel_is(0, 2, 132, 130, 132)); /* alpha 128 over black */
    CHECK(pixel_is(2, 2, 0, 0, 255));

    /* Object opacity multiplies the image alpha. */
    CHECK(pe_begin(b, 0x000000u) == PE_OK);
    CHECK(pe_draw_texture(b, id, 0, 0, 2, 2, 0, 128) == PE_OK);
    CHECK(pe_draw_texture(b, id, 10, 0, 2, 2, 0, 0) == PE_OK);
    CHECK(read_frame(b) == PE_OK);
    CHECK(pixel_is(0, 0, 132, 0, 0));
    CHECK(pixel_is(1, 0, 0, 0, 0));
    CHECK(pixel_is(10, 0, 0, 0, 0));

    /* Invalid uploads and draws. */
    CHECK(pe_texture_create(b, rgba, sizeof(rgba) - 1, 2, 2, &id2) == PE_ERR_INVALID_ARG);
    CHECK(pe_texture_create(b, rgba, sizeof(rgba) + 4, 2, 2, &id2) == PE_ERR_INVALID_ARG);
    CHECK(pe_texture_create(b, rgba, sizeof(rgba), 0, 2, &id2) == PE_ERR_INVALID_ARG);
    CHECK(pe_texture_create(b, rgba, sizeof(rgba), PE_MAX_TEXTURE_DIMENSION + 1, 1, &id2) == PE_ERR_INVALID_ARG);
    CHECK(pe_texture_create(b, NULL, 16, 2, 2, &id2) == PE_ERR_INVALID_ARG);
    CHECK(pe_texture_create(b, rgba, sizeof(rgba), 2, 2, NULL) == PE_ERR_INVALID_ARG);
    CHECK(id2 == 0);
    CHECK(pe_draw_texture(b, id, 0, 0, 2, 2, 0, 300) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_texture(b, id, 0, 0, strtod("nan", NULL), 2, 0, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_texture(b, 0, 0, 0, 2, 2, 0, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_texture(b, id + 1u, 0, 0, 2, 2, 0, 255) == PE_ERR_INVALID_ARG);

    /* Destroy makes the id stale, even after the slot is reused. */
    CHECK(pe_texture_destroy(b, id) == PE_OK);
    CHECK(pe_texture_destroy(b, id) == PE_ERR_INVALID_ARG);
    CHECK(pe_draw_texture(b, id, 0, 0, 2, 2, 0, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_texture_create(b, rgba, sizeof(rgba), 2, 2, &id2) == PE_OK);
    CHECK(id2 != id);
    CHECK(pe_draw_texture(b, id, 0, 0, 2, 2, 0, 255) == PE_ERR_INVALID_ARG);
    CHECK(pe_texture_destroy(b, id2) == PE_OK);
    CHECK(pe_texture_stats(b, &count, &bytes) == PE_OK && count == 0 && bytes == 0);
}

static void test_texture_capacity(pe_bridge *b)
{
    const unsigned char px[4] = {1, 2, 3, 255};
    uint32_t ids[PE_MAX_TEXTURES];
    uint32_t extra = 0, count = 0;
    int i, ok = 1;
    for (i = 0; i < PE_MAX_TEXTURES; ++i) {
        if (pe_texture_create(b, px, sizeof(px), 1, 1, &ids[i]) != PE_OK) {
            ok = 0;
        }
    }
    CHECK(ok);
    CHECK(pe_texture_create(b, px, sizeof(px), 1, 1, &extra) == PE_ERR_CAPACITY && extra == 0);
    CHECK(pe_texture_stats(b, &count, NULL) == PE_OK && count == PE_MAX_TEXTURES);
    /* Leave half of them alive: pe_destroy must release them. */
    for (i = 0; i < PE_MAX_TEXTURES / 2; ++i) {
        CHECK(pe_texture_destroy(b, ids[i]) == PE_OK);
    }
}

static PIXELICA_AABB2D aabb(float x0, float y0, float x1, float y1)
{
    PIXELICA_AABB2D r;
    r.min.x = x0;
    r.min.y = y0;
    r.max.x = x1;
    r.max.y = y1;
    return r;
}

static void test_collision(void)
{
    const double nan_value = strtod("nan", NULL);
    const double inf_value = strtod("inf", NULL);
    /* Floor top at y=100, wall face at x=100. */
    const PIXELICA_AABB2D obs[2] = {aabb(-1000.0f, 100.0f, 1000.0f, 200.0f), aabb(100.0f, 0.0f, 110.0f, 100.0f)};
    PIXELICA_AABB2D a = aabb(0, 0, 10, 10), t = aabb(10, 0, 20, 10), o = aabb(5, 5, 15, 15);
    PIXELICA_Body2D body, saved;

    _Static_assert(sizeof(PIXELICA_Body2D) == 32u, "Body2D ctypes ABI size");
    CHECK(pe_aabb2d_overlaps(&a, &o) == 1);
    CHECK(pe_aabb2d_overlaps(&a, &t) == 0); /* touching */
    CHECK(pe_aabb2d_overlaps(&a, NULL) == 0);
    t = aabb(5, 5, 5, 15);
    CHECK(pe_aabb2d_overlaps(&a, &t) == 0); /* invalid */

    /* Floor: exact contact grounds on the first step. */
    memset(&body, 0, sizeof body);
    body.bounds = aabb(0, 68, 32, 100);
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 220.0, 600.0, 0, obs, 2) == PE_OK);
    CHECK(body.grounded == 1u && body.contacts == PIXELICA_CONTACT2D_BOTTOM && body.bounds.max.y == 100.0f);
    /* Jump. */
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 220.0, 600.0, 1, obs, 2) == PE_OK);
    CHECK(body.grounded == 0u && body.velocity.y == -220.0f && body.bounds.max.y == 72.5f);
    /* Wall: blocked side reported and horizontal speed zeroed. */
    memset(&body, 0, sizeof body);
    body.bounds = aabb(0, 68, 32, 100);
    body.velocity.x = 1000.0f;
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 220.0, 600.0, 0, obs, 2) == PE_OK);
    CHECK(body.bounds.max.x == 100.0f && body.velocity.x == 0.0f);
    CHECK(body.contacts == (PIXELICA_CONTACT2D_RIGHT | PIXELICA_CONTACT2D_BOTTOM));

    /* Invalid inputs leave the body untouched. */
    saved = body;
    CHECK(pe_body2d_step(NULL, 0.125, 600.0, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.0, 600.0, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.3, 600.0, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, nan_value, 600.0, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 1e-300, 600.0, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG); /* narrows to 0 */
    CHECK(pe_body2d_step(&body, 0.125, inf_value, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.125, 1e300, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.125, -1.0, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 10001.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 220.0, 0.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 220.0, 600.0, 2, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 220.0, 600.0, -1, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 220.0, 600.0, 0, NULL, 2) == PE_ERR_INVALID_ARG);
    CHECK(memcmp(&body, &saved, sizeof body) == 0);
    body.bounds = aabb(90, 68, 122, 100); /* starts inside the wall */
    saved = body;
    CHECK(pe_body2d_step(&body, 0.125, 600.0, 220.0, 600.0, 0, obs, 2) == PE_ERR_INVALID_ARG);
    CHECK(memcmp(&body, &saved, sizeof body) == 0);
}

int main(void)
{
    pe_bridge *b = NULL;
    test_create_limits();
    test_collision();
    if (pe_create(W, H, &b) != PE_OK || b == NULL) {
        fprintf(stderr, "pe_create(%d, %d) failed\n", W, H);
        return 1;
    }
    test_begin_and_rgb_conversion(b);
    test_rect_alpha(b);
    test_rotation(b);
    test_invalid_draws(b);
    test_textures(b);
    test_texture_capacity(b);
    pe_destroy(b);
    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_bridge: all checks passed\n");
    return 0;
}

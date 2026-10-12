/* Framebuffer snapshots and sampling (common library; includes no 3D header). */
#include <float.h>

#include "pixelica/framebuffer.h"
#include "test_util.h"

#define PAD 0xBEEFu
#define SENT 0x7777u

typedef union {
    PIXELICA_FrameView view;
    uint16_t px[64];
} ViewOverlay;

/* 3x2 view, stride 4, padding column = PAD. */
static uint16_t g_color[8];
static uint16_t g_depth[8];

static void init_planes(void)
{
    int x, y;

    for (y = 0; y < 2; ++y) {
        for (x = 0; x < 4; ++x) {
            g_color[y * 4 + x] = (x < 3) ? (uint16_t)(0x100 + y * 3 + x) : PAD;
            g_depth[y * 4 + x] = (x < 3) ? (uint16_t)(1000 + y * 3 + x) : PAD;
        }
    }
}

static PIXELICA_FrameView make_view(const uint16_t *c, const uint16_t *d,
                                    uint16_t w, uint16_t h, size_t stride)
{
    PIXELICA_FrameView v;

    v.color = c;
    v.depth = d;
    v.width = w;
    v.height = h;
    v.stride = stride;
    return v;
}

static void test_copy(void)
{
    PIXELICA_FrameView src = make_view(g_color, g_depth, 3, 2, 4);
    PIXELICA_FrameView out;
    uint16_t c[6], d[6];
    int i;

    init_planes();
    CHECK(pixelica_frame_copy(&src, c, 6, d, 6, &out) == PIXELICA_OK);
    CHECK(out.color == c && out.depth == d);
    CHECK(out.width == 3 && out.height == 2 && out.stride == 3);
    for (i = 0; i < 6; ++i) {
        CHECK(c[i] == (uint16_t)(0x100 + i));
        CHECK(d[i] == (uint16_t)(1000 + i));
    }
    /* Snapshot is independent of later source changes. */
    g_color[0] = 0;
    g_depth[5] = 0;
    CHECK(c[0] == 0x100 && d[5] == 1005);

    /* Color only, depth only; extra capacity is fine. */
    init_planes();
    {
        uint16_t big[10];
        CHECK(pixelica_frame_copy(&src, big, 10, NULL, 0, &out) == PIXELICA_OK);
        CHECK(out.color == big && out.depth == NULL && out.stride == 3);
        CHECK(pixelica_frame_copy(&src, NULL, 0, big, 6, &out) == PIXELICA_OK);
        CHECK(out.color == NULL && out.depth == big && big[5] == 1005);
    }
    {
        PIXELICA_FrameView conly = make_view(g_color, NULL, 3, 2, 4);
        PIXELICA_FrameView donly = make_view(NULL, g_depth, 3, 2, 4);

        CHECK(pixelica_frame_copy(&conly, c, 6, NULL, 0, &out) == PIXELICA_OK);
        CHECK(pixelica_frame_copy(&donly, NULL, 0, d, 6, &out) == PIXELICA_OK);
        CHECK(out.color == NULL && out.depth == d);
        /* Requesting a plane the source lacks. */
        CHECK(pixelica_frame_copy(&conly, c, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_frame_copy(&donly, c, 6, NULL, 0, &out) == PIXELICA_ERR_INVALID_ARG);
    }
}

static void test_copy_errors(void)
{
    PIXELICA_FrameView src = make_view(g_color, g_depth, 3, 2, 4);
    PIXELICA_FrameView out, before;
    uint16_t c[6], d[6];
    ViewOverlay ov;
    int i;

    init_planes();
    for (i = 0; i < 6; ++i) {
        c[i] = d[i] = SENT;
    }
    out = make_view(g_color, NULL, 9, 9, 99);
    before = out;
    memset(&ov, 0, sizeof ov);

#define EXPECT_UNCHANGED()                                                   \
    do {                                                                     \
        int k_;                                                              \
        CHECK(memcmp(&out, &before, sizeof out) == 0);                       \
        for (k_ = 0; k_ < 6; ++k_) {                                         \
            CHECK(c[k_] == SENT && d[k_] == SENT);                           \
        }                                                                    \
    } while (0)

    CHECK(pixelica_frame_copy(NULL, c, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, c, 6, d, 6, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, NULL, 0, NULL, 0, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, NULL, 3, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, c, 6, NULL, 1, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, c, 5, d, 6, &out) == PIXELICA_ERR_CAPACITY);
    CHECK(pixelica_frame_copy(&src, c, 6, d, 0, &out) == PIXELICA_ERR_CAPACITY);
    EXPECT_UNCHANGED();

    /* Invalid source descriptors. */
    {
        PIXELICA_FrameView bad;

        bad = make_view(NULL, NULL, 3, 2, 4);
        CHECK(pixelica_frame_copy(&bad, c, 6, NULL, 0, &out) == PIXELICA_ERR_INVALID_ARG);
        bad = make_view(g_color, g_depth, 0, 2, 4);
        CHECK(pixelica_frame_copy(&bad, c, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
        bad = make_view(g_color, g_depth, 3, 0, 4);
        CHECK(pixelica_frame_copy(&bad, c, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
        bad = make_view(g_color, g_depth, 4097, 1, 4097);
        CHECK(pixelica_frame_copy(&bad, c, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
        bad = make_view(g_color, g_depth, 3, 4097, 4);
        CHECK(pixelica_frame_copy(&bad, c, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
        bad = make_view(g_color, g_depth, 3, 2, 2);
        CHECK(pixelica_frame_copy(&bad, c, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
        bad = make_view(g_color, g_depth, 3, 2, SIZE_MAX);
        CHECK(pixelica_frame_copy(&bad, c, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
    }
    EXPECT_UNCHANGED();

    /* Overlaps: output/input and output/output. */
    CHECK(pixelica_frame_copy(&src, c, 6, c, 6, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, c, 6, c + 3, 6, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, g_color + 1, 6, d, 6, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, c, 6, g_depth + 2, 6, &out) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, c, 6, d, 6, &src) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_copy(&src, c, 6, d, 6, (PIXELICA_FrameView *)&g_color[2]) ==
          PIXELICA_ERR_INVALID_ARG);
    /* Output descriptor inside an output buffer. */
    CHECK(pixelica_frame_copy(&src, ov.px, 64, d, 6, &ov.view) == PIXELICA_ERR_INVALID_ARG);
    /* Output descriptor inside an input plane. */
    CHECK(pixelica_frame_copy(&src, c, 6, NULL, 0, (PIXELICA_FrameView *)(void *)g_depth) ==
          PIXELICA_ERR_INVALID_ARG);
    EXPECT_UNCHANGED();
}

static void test_sample_color(void)
{
    PIXELICA_FrameView v = make_view(g_color, g_depth, 3, 2, 4);
    uint16_t px = SENT;
    uint16_t two[2];
    PIXELICA_FrameView tv;
    const PIXELICA_WrapMode C = PIXELICA_WRAP_CLAMP, R = PIXELICA_WRAP_REPEAT;
    const PIXELICA_FilterMode N = PIXELICA_FILTER_NEAREST, B = PIXELICA_FILTER_BILINEAR;

    init_planes();
    /* Nearest, row 0 = top; v=0.5 is the second row. */
    CHECK(pixelica_frame_sample_color(&v, 0.0f, 0.0f, C, C, N, &px) == PIXELICA_OK);
    CHECK(px == 0x100);
    CHECK(pixelica_frame_sample_color(&v, 0.5f, 0.5f, C, C, N, &px) == PIXELICA_OK);
    CHECK(px == 0x104);
    CHECK(pixelica_frame_sample_color(&v, 0.99f, 0.99f, C, C, N, &px) == PIXELICA_OK);
    CHECK(px == 0x105);
    /* u = 1 clamps to the last texel and never reads the padding column. */
    CHECK(pixelica_frame_sample_color(&v, 1.0f, 1.0f, C, C, N, &px) == PIXELICA_OK);
    CHECK(px == 0x105);
    CHECK(pixelica_frame_sample_color(&v, 1.0f, 0.0f, R, R, N, &px) == PIXELICA_OK);
    CHECK(px == 0x100);
    CHECK(pixelica_frame_sample_color(&v, -0.1f, 0.0f, C, C, N, &px) == PIXELICA_OK);
    CHECK(px == 0x100);
    CHECK(pixelica_frame_sample_color(&v, -0.1f, 0.0f, R, R, N, &px) == PIXELICA_OK);
    CHECK(px == 0x102);
    CHECK(pixelica_frame_sample_color(&v, 0.0f, -0.25f, C, R, N, &px) == PIXELICA_OK);
    CHECK(px == 0x103); /* v wraps to row 1 */
    CHECK(pixelica_frame_sample_color(&v, FLT_MAX, -FLT_MAX, C, C, N, &px) == PIXELICA_OK);
    CHECK(px == 0x102);
    CHECK(pixelica_frame_sample_color(&v, FLT_MAX, -FLT_MAX, R, R, B, &px) == PIXELICA_OK);
    {
        uint16_t seam = 0;
        CHECK(pixelica_frame_sample_color(&v, 0.0f, 0.0f, R, R, B, &seam) == PIXELICA_OK);
        CHECK(px == seam); /* every huge float here is an exact integer UV */
    }
    CHECK(pixelica_frame_sample_color(&v, 3.5f, 2.5f, R, R, N, &px) == PIXELICA_OK);
    CHECK(px >= 0x100 && px <= 0x105);

    /* Bilinear on a 2x1 black/white strip. */
    two[0] = 0;
    two[1] = 0xFFFFu;
    tv = make_view(two, NULL, 2, 1, 2);
    CHECK(pixelica_frame_sample_color(&tv, 0.25f, 0.5f, C, C, B, &px) == PIXELICA_OK);
    CHECK(px == 0);
    CHECK(pixelica_frame_sample_color(&tv, 0.75f, 0.5f, C, C, B, &px) == PIXELICA_OK);
    CHECK(px == 0xFFFF);
    CHECK(pixelica_frame_sample_color(&tv, 0.5f, 0.5f, C, C, B, &px) == PIXELICA_OK);
    CHECK(px == (uint16_t)((16u << 11) | (32u << 5) | 16u));
    CHECK(pixelica_frame_sample_color(&tv, 0.0f, 0.5f, C, C, B, &px) == PIXELICA_OK);
    CHECK(px == 0); /* clamp: edge texel only */
    CHECK(pixelica_frame_sample_color(&tv, 0.0f, 0.5f, R, C, B, &px) == PIXELICA_OK);
    CHECK(px == (uint16_t)((16u << 11) | (32u << 5) | 16u)); /* seam blend */
    CHECK(pixelica_frame_sample_color(&tv, 1.0f, 0.5f, C, C, B, &px) == PIXELICA_OK);
    CHECK(px == 0xFFFF);
    /* Padded-stride bilinear reads only in-image texels. */
    CHECK(pixelica_frame_sample_color(&v, 1.0f, 0.0f, C, C, B, &px) == PIXELICA_OK);
    CHECK(px == 0x102); /* x1 clamps to the last texel, not the padding */

    /* Errors leave *out untouched. */
    px = SENT;
    CHECK(pixelica_frame_sample_color(NULL, 0.f, 0.f, C, C, N, &px) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_sample_color(&v, 0.f, 0.f, C, C, N, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_sample_color(&v, pixelica_t_nan(), 0.f, C, C, N, &px) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_sample_color(&v, 0.f, pixelica_t_inf(), C, C, N, &px) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_sample_color(&v, 0.f, 0.f, (PIXELICA_WrapMode)7, C, N, &px) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_sample_color(&v, 0.f, 0.f, C, C, (PIXELICA_FilterMode)9, &px) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_sample_color(&v, 0.f, 0.f, C, C, N, &g_color[1]) ==
          PIXELICA_ERR_INVALID_ARG);
    tv = make_view(NULL, g_depth, 3, 2, 4);
    CHECK(pixelica_frame_sample_color(&tv, 0.f, 0.f, C, C, N, &px) == PIXELICA_ERR_INVALID_ARG);
    CHECK(px == SENT);
}

static void test_sample_depth(void)
{
    PIXELICA_FrameView v = make_view(g_color, g_depth, 3, 2, 4);
    PIXELICA_FrameView conly = make_view(g_color, NULL, 3, 2, 4);
    uint16_t d = SENT;

    init_planes();
    CHECK(pixelica_frame_sample_depth(&v, 0.0f, 0.0f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, &d) == PIXELICA_OK);
    CHECK(d == 1000);
    /* Raw nearest value, never interpolated. */
    CHECK(pixelica_frame_sample_depth(&v, 0.4f, 0.9f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, &d) == PIXELICA_OK);
    CHECK(d == 1004);
    CHECK(pixelica_frame_sample_depth(&v, 1.0f, 0.0f, PIXELICA_WRAP_REPEAT,
                                      PIXELICA_WRAP_CLAMP, &d) == PIXELICA_OK);
    CHECK(d == 1000);
    CHECK(pixelica_frame_sample_depth(&v, 5.0f, 0.0f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, &d) == PIXELICA_OK);
    CHECK(d == 1002);

    d = SENT;
    CHECK(pixelica_frame_sample_depth(&conly, 0.f, 0.f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, &d) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_sample_depth(&v, pixelica_t_nan(), 0.f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, &d) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_sample_depth(&v, 0.f, 0.f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(d == SENT);
}

int main(void)
{
    test_copy();
    test_copy_errors();
    test_sample_color();
    test_sample_depth();
    return pixelica_t_finish("test_framebuffer");
}

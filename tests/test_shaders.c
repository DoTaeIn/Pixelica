/* Optional CPU shaders: exact effect math, stride/padding, validation before
 * writing, aliasing and the PAL presenter. Checks remain enabled in Release. */
#include <string.h>
#include "pixelica/shaders.h"
#include "test_util.h"

#define PAD 0x1234u

static PIXELICA_Shader make(PIXELICA_ShaderKind kind)
{
    PIXELICA_Shader s;
    memset(&s, 0, sizeof s);
    CHECK(pixelica_shader_default(kind, &s) == PIXELICA_OK);
    return s;
}

/* Applies `s` to a w x 1 row and compares with `expected`. */
static void check_row(const PIXELICA_Shader *s, const uint16_t *in, const uint16_t *expected,
                      uint16_t w)
{
    uint16_t px[8], scratch[8];
    PIXELICA_ShaderSurface surf;
    uint16_t i;
    memcpy(px, in, w * sizeof(uint16_t));
    surf.pixels = px; surf.width = w; surf.height = 1; surf.stride = w;
    CHECK(pixelica_shader_apply(&surf, s, scratch, 8) == PIXELICA_OK);
    for (i = 0; i < w; ++i) CHECK_EQ_LONG(px[i], expected[i]);
}

static void test_defaults(void)
{
    PIXELICA_Shader s = make(PIXELICA_SHADER_OUTLINE), keep;
    CHECK(s.amount == 0.2f && s.color == 0x0000u && s.levels == 4u && s.radius == 1u);
    s = make(PIXELICA_SHADER_TINT);
    CHECK(s.amount == 1.0f && s.color == 0xFFFFu && s.palette == NULL && s.palette_count == 0u &&
          s.callback == NULL && s.userdata == NULL);
    memcpy(&keep, &s, sizeof s);
    CHECK(pixelica_shader_default((PIXELICA_ShaderKind)99, &s) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&keep, &s, sizeof s) == 0);
    CHECK(pixelica_shader_default(PIXELICA_SHADER_TINT, NULL) == PIXELICA_ERR_INVALID_ARG);
}

static void test_point_effects(void)
{
    PIXELICA_Shader s = make(PIXELICA_SHADER_TINT);
    const uint16_t pal[3] = {0x0000u, 0xFFFFu, 0xF800u}, tie[2] = {0xF800u, 0x001Fu};
    const uint16_t tie_rev[2] = {0x001Fu, 0xF800u};

    s.color = 0xF800u;
    { const uint16_t i[2] = {0x07E0u, 0x0000u}, e[2] = {0xF800u, 0xF800u}; check_row(&s, i, e, 2); }
    s.amount = 0.0f;
    { const uint16_t i[1] = {0x1234u}; check_row(&s, i, i, 1); }
    s.amount = 0.5f; s.color = 0xFFFFu; /* 0 + 255 * 0.5 = 127.5 -> 128 */
    { const uint16_t i[1] = {0x0000u}, e[1] = {0x8410u}; check_row(&s, i, e, 1); }

    s = make(PIXELICA_SHADER_GRAYSCALE); /* red: (77*255+128)/256 = 77 */
    { const uint16_t i[2] = {0xF800u, 0xFFFFu}, e[2] = {0x4A69u, 0xFFFFu}; check_row(&s, i, e, 2); }
    s = make(PIXELICA_SHADER_INVERT);
    { const uint16_t i[2] = {0x0000u, 0xF800u}, e[2] = {0xFFFFu, 0x07FFu}; check_row(&s, i, e, 2); }

    s = make(PIXELICA_SHADER_POSTERIZE);
    s.amount = pixelica_t_nan(); /* unused by POSTERIZE */
    s.levels = 2;
    { const uint16_t i[2] = {0x8410u, 0x7BEFu}, e[2] = {0xFFFFu, 0x0000u}; check_row(&s, i, e, 2); }
    s.levels = 4; /* 123/125 -> step 1 -> 85 */
    { const uint16_t i[1] = {0x7BEFu}, e[1] = {0x52AAu}; check_row(&s, i, e, 1); }

    s = make(PIXELICA_SHADER_PALETTE);
    s.palette = pal; s.palette_count = 3;
    { const uint16_t i[2] = {0xE000u, 0xEF7Du}, e[2] = {0xF800u, 0xFFFFu}; check_row(&s, i, e, 2); }
    s.palette = tie; s.palette_count = 2; /* black is equidistant: first wins */
    { const uint16_t i[1] = {0x0000u}, e[1] = {0xF800u}; check_row(&s, i, e, 1); }
    s.palette = tie_rev;
    { const uint16_t i[1] = {0x0000u}, e[1] = {0x001Fu}; check_row(&s, i, e, 1); }
}

static void test_stride_scanlines_vignette(void)
{
    uint16_t px[6] = {0xFFFFu, 0xFFFFu, PAD, 0xFFFFu, 0xFFFFu, PAD}, v[25];
    PIXELICA_ShaderSurface surf = {px, 2, 2, 3};
    PIXELICA_Shader s = make(PIXELICA_SHADER_SCANLINES);
    unsigned i;

    CHECK(pixelica_shader_apply(&surf, &s, NULL, 0) == PIXELICA_OK);
    CHECK(px[0] == 0xFFFFu && px[1] == 0xFFFFu && px[3] == 0u && px[4] == 0u);
    CHECK(px[2] == PAD && px[5] == PAD);

    for (i = 0; i < 25; ++i) v[i] = 0xFFFFu;
    s = make(PIXELICA_SHADER_VIGNETTE);
    surf.pixels = v; surf.width = 5; surf.height = 5; surf.stride = 5;
    CHECK(pixelica_shader_apply(&surf, &s, NULL, 0) == PIXELICA_OK);
    CHECK_EQ_LONG(v[2 * 5 + 2], 0xFFFFu); /* center */
    CHECK_EQ_LONG(v[0], 0x0000u);         /* corner clamps to 1 */
    CHECK_EQ_LONG(v[2], 0x0000u);         /* top edge: ny = -1 */
    CHECK_EQ_LONG(v[2 * 5 + 1], 0xBDF7u); /* nx = -0.5: 255 * 0.75 -> 191 */
    for (i = 0; i < 3; ++i) v[i] = 0xFFFFu;
    surf.width = 1; surf.height = 3; surf.stride = 1; /* single-pixel axis uses 0 */
    CHECK(pixelica_shader_apply(&surf, &s, NULL, 0) == PIXELICA_OK);
    CHECK(v[0] == 0u && v[1] == 0xFFFFu && v[2] == 0u);
}

static void test_spatial(void)
{
    uint16_t px[12], scratch[9];
    PIXELICA_ShaderSurface surf = {px, 3, 3, 4};
    PIXELICA_Shader s = make(PIXELICA_SHADER_BLUR);
    unsigned i;

    for (i = 0; i < 12; ++i) px[i] = (uint16_t)((i % 4u == 3u) ? PAD : 0u);
    px[1 * 4 + 1] = 0xFFFFu;
    s.amount = pixelica_t_inf(); /* unused by BLUR */
    CHECK(pixelica_shader_scratch_pixels(&surf, &s) == 9u);
    CHECK(pixelica_shader_apply(&surf, &s, scratch, 9) == PIXELICA_OK);
    /* Every clamped 3x3 window sees the single white pixel exactly once:
     * (255 + 4) / 9 = 28 everywhere, proving reads come from the snapshot. */
    for (i = 0; i < 12; ++i) CHECK_EQ_LONG(px[i], (i % 4u == 3u) ? PAD : 0x18E3u);

    { const uint16_t in[3] = {0xFFFFu, 0, 0}, e[3] = {0xAD55u, 0x52AAu, 0}; check_row(&s, in, e, 3); }
    s.radius = 4; /* 1x1: every sample clamps to the pixel itself */
    { const uint16_t in[1] = {0x8410u}; check_row(&s, in, in, 1); }

    s = make(PIXELICA_SHADER_OUTLINE); /* threshold round(0.2 * 255) = 51 */
    s.color = 0xF800u;
    { const uint16_t in[3] = {0xFFFFu, 0xFFFFu, 0}, e[3] = {0xFFFFu, 0xF800u, 0xF800u};
      check_row(&s, in, e, 3); }
    s.amount = 1.0f; /* difference 255 is not > 255 */
    { const uint16_t in[3] = {0xFFFFu, 0xFFFFu, 0}; check_row(&s, in, in, 3); }
}

typedef struct CustomLog { unsigned calls, bad; } CustomLog;

static uint16_t shift_right(const PIXELICA_ShaderPixel *p, void *userdata)
{
    CustomLog *log = (CustomLog *)userdata;
    ++log->calls;
    if (p->source->stride != p->source->width ||
        p->color != pixelica_shader_sample(p->source, p->x, p->y)) ++log->bad;
    return pixelica_shader_sample(p->source, (int32_t)p->x - 1, p->y);
}

static void test_custom_and_sample(void)
{
    const uint16_t img[6] = {1, 2, PAD, 3, 4, PAD};
    PIXELICA_ShaderImage im = {img, 2, 2, 3};
    PIXELICA_Shader s = make(PIXELICA_SHADER_CUSTOM);
    CustomLog log = {0, 0};

    CHECK(pixelica_shader_sample(&im, -5, -5) == 1u && pixelica_shader_sample(&im, 9, 0) == 2u);
    CHECK(pixelica_shader_sample(&im, 0, 1) == 3u && pixelica_shader_sample(&im, 1, 70000) == 4u);
    CHECK(pixelica_shader_sample(NULL, 0, 0) == 0u);
    im.stride = 1;
    CHECK(pixelica_shader_sample(&im, 0, 0) == 0u);

    s.callback = shift_right; s.userdata = &log;
    { const uint16_t in[3] = {1, 2, 3}, e[3] = {1, 1, 2}; check_row(&s, in, e, 3); }
    CHECK(log.calls == 3u && log.bad == 0u);
}

static void test_invalid_no_mutation(void)
{
    uint16_t px[4] = {1, 2, 3, 4}, scratch[4] = {9, 9, 9, 9}, before[4];
    PIXELICA_ShaderSurface surf = {px, 2, 2, 2}, bad;
    PIXELICA_Shader s = make(PIXELICA_SHADER_TINT);
    const float amounts[4] = {pixelica_t_nan(), pixelica_t_inf(), -0.01f, 1.01f};
    union { PIXELICA_Shader sh; uint16_t px[64]; } ush;
    union { PIXELICA_ShaderSurface sf; uint16_t px[64]; } usf;
    unsigned i;

    memcpy(before, px, sizeof px);
#define REJECT(sf, sh, scr, n, code)                                          \
    do {                                                                      \
        CHECK(pixelica_shader_apply((sf), (sh), (scr), (n)) == (code));       \
        CHECK(memcmp(before, px, sizeof px) == 0);                            \
        CHECK(scratch[0] == 9u && scratch[3] == 9u);                          \
    } while (0)
    for (i = 0; i < 4; ++i) { s.amount = amounts[i]; REJECT(&surf, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG); }
    s = make(PIXELICA_SHADER_OUTLINE); s.amount = pixelica_t_nan();
    REJECT(&surf, &s, scratch, 4, PIXELICA_ERR_INVALID_ARG);
    s = make(PIXELICA_SHADER_POSTERIZE); s.levels = 1; REJECT(&surf, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);
    s.levels = 33; REJECT(&surf, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);
    s = make(PIXELICA_SHADER_PALETTE); REJECT(&surf, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);
    s.palette = scratch; s.palette_count = 257; REJECT(&surf, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);
    s.palette = px + 3; s.palette_count = 1; /* palette inside destination */
    REJECT(&surf, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);
    s = make(PIXELICA_SHADER_CUSTOM); REJECT(&surf, &s, scratch, 4, PIXELICA_ERR_INVALID_ARG);
    s.kind = (PIXELICA_ShaderKind)42; REJECT(&surf, &s, scratch, 4, PIXELICA_ERR_INVALID_ARG);
    REJECT(NULL, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);

    s = make(PIXELICA_SHADER_BLUR);
    s.radius = 0; REJECT(&surf, &s, scratch, 4, PIXELICA_ERR_INVALID_ARG);
    s.radius = 5; REJECT(&surf, &s, scratch, 4, PIXELICA_ERR_INVALID_ARG);
    s.radius = 1;
    REJECT(&surf, &s, scratch, 3, PIXELICA_ERR_CAPACITY);
    REJECT(&surf, &s, NULL, 0, PIXELICA_ERR_CAPACITY);
    REJECT(&surf, &s, NULL, 4, PIXELICA_ERR_INVALID_ARG);
    REJECT(&surf, &s, px, 4, PIXELICA_ERR_INVALID_ARG);     /* scratch == destination */
    REJECT(&surf, &s, px + 3, 4, PIXELICA_ERR_INVALID_ARG); /* partial overlap */
    CHECK(pixelica_shader_scratch_pixels(&surf, &s) == 4u);
    s.radius = 9;
    CHECK(pixelica_shader_scratch_pixels(&surf, &s) == 0u);
    s = make(PIXELICA_SHADER_TINT);
    CHECK(pixelica_shader_scratch_pixels(&surf, &s) == 0u);

    bad = surf; bad.width = 0; REJECT(&bad, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);
    bad = surf; bad.stride = 1; REJECT(&bad, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);
    bad = surf; bad.pixels = NULL; REJECT(&bad, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);
    bad = surf; bad.width = 4097; bad.stride = 4097;
    REJECT(&bad, &s, NULL, 0, PIXELICA_ERR_INVALID_ARG);

    /* Destination over the shader struct / surface struct; scratch over shader. */
    memset(&ush, 0, sizeof ush);
    ush.sh = make(PIXELICA_SHADER_TINT);
    bad.pixels = ush.px; bad.width = 8; bad.height = 1; bad.stride = 8;
    CHECK(pixelica_shader_apply(&bad, &ush.sh, NULL, 0) == PIXELICA_ERR_INVALID_ARG);
    usf.sf.pixels = usf.px; usf.sf.width = 8; usf.sf.height = 1; usf.sf.stride = 8;
    s = make(PIXELICA_SHADER_INVERT);
    CHECK(pixelica_shader_apply(&usf.sf, &s, NULL, 0) == PIXELICA_ERR_INVALID_ARG);
    CHECK(usf.sf.width == 8u && usf.sf.pixels == usf.px);
    ush.sh = make(PIXELICA_SHADER_BLUR);
    CHECK(pixelica_shader_apply(&surf, &ush.sh, ush.px, 64) == PIXELICA_ERR_INVALID_ARG);
    CHECK(ush.sh.kind == PIXELICA_SHADER_BLUR && ush.sh.radius == 1u);
    CHECK(memcmp(before, px, sizeof px) == 0);
#undef REJECT
}

typedef struct Mock {
    unsigned presents, logs;
    bool result;
    uint16_t last[16], w, h;
    uint32_t stride;
    const uint16_t *ptr;
} Mock;

static uint64_t m_time(void *ud) { return ud != NULL ? 1234u : 0u; }
static uint32_t m_buttons(void *ud) { return ud != NULL ? 5u : 0u; }
static void m_log(void *ud, const char *msg) { if (msg != NULL) ++((Mock *)ud)->logs; }
static bool m_present(void *ud, const uint16_t *px, uint16_t w, uint16_t h, uint32_t stride)
{
    Mock *m = (Mock *)ud;
    ++m->presents;
    m->ptr = px; m->w = w; m->h = h; m->stride = stride;
    memcpy(m->last, px, (size_t)w * h * sizeof(uint16_t));
    return m->result;
}

static void test_presenter(void)
{
    Mock m;
    PIXELICA_PlatformOps up = {NULL, m_time, m_buttons, m_present, m_log}, bad_up;
    PIXELICA_ShaderPresenter p, keep;
    PIXELICA_Shader s = make(PIXELICA_SHADER_INVERT);
    uint16_t out[4] = {7, 7, 7, 7}, scratch[4];
    const uint16_t frame[6] = {0x0000u, 0xF800u, PAD, 0xFFFFu, 0x07E0u, PAD};
    uint16_t big[9] = {0};
    union { PIXELICA_ShaderPresenter pr; uint16_t px[256]; } u;

    memset(&m, 0, sizeof m);
    m.result = true;
    up.userdata = &m;
    memset(&p, 0xAB, sizeof p);
    memcpy(&keep, &p, sizeof p);
    bad_up = up; bad_up.present_rgb565 = NULL;
    CHECK(pixelica_shader_presenter_init(&p, &bad_up, &s, out, 4, NULL, 0) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(&p, &up, &s, out, 0, NULL, 0) == PIXELICA_ERR_INVALID_ARG);
    s.amount = pixelica_t_nan();
    CHECK(pixelica_shader_presenter_init(&p, &up, &s, out, 4, NULL, 0) == PIXELICA_ERR_INVALID_ARG);
    s = make(PIXELICA_SHADER_BLUR);
    CHECK(pixelica_shader_presenter_init(&p, &up, &s, out, 4, NULL, 0) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(&p, &up, &s, out, 4, out + 2, 4) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&p, &keep, sizeof p) == 0);
    s = make(PIXELICA_SHADER_INVERT);
    /* Output inside the presenter struct itself. */
    CHECK(pixelica_shader_presenter_init(&u.pr, &up, &s, u.px + 4, 8, NULL, 0) ==
          PIXELICA_ERR_INVALID_ARG);

    CHECK(pixelica_shader_presenter_init(&p, &up, &s, out, 4, NULL, 0) == PIXELICA_OK);
    CHECK(p.ops.userdata == &p && p.ops.log != NULL);
    CHECK(p.ops.time_us(p.ops.userdata) == 1234u && p.ops.read_buttons(p.ops.userdata) == 5u);
    p.ops.log(p.ops.userdata, "hi");
    CHECK(m.logs == 1u);

    CHECK(p.ops.present_rgb565(p.ops.userdata, frame, 2, 2, 3));
    CHECK(m.presents == 1u && m.ptr == out && m.w == 2u && m.h == 2u && m.stride == 2u);
    CHECK(m.last[0] == 0xFFFFu && m.last[1] == 0x07FFu && m.last[2] == 0x0000u && m.last[3] == 0xF81Fu);
    CHECK(frame[0] == 0x0000u && frame[2] == PAD && frame[4] == 0x07E0u); /* input preserved */

    /* Failures: no forward, output unchanged. */
    CHECK(!p.ops.present_rgb565(p.ops.userdata, big, 3, 3, 3)); /* exceeds capacity */
    CHECK(!p.ops.present_rgb565(p.ops.userdata, out, 2, 2, 2)); /* frame == output */
    CHECK(!p.ops.present_rgb565(p.ops.userdata, NULL, 2, 2, 2));
    CHECK(!p.ops.present_rgb565(p.ops.userdata, frame, 2, 2, 1));
    p.shader.amount = 2.0f; /* invalid effect after init is revalidated */
    CHECK(!p.ops.present_rgb565(p.ops.userdata, frame, 2, 2, 3));
    p.shader = make(PIXELICA_SHADER_BLUR); /* needs scratch, none supplied */
    CHECK(!p.ops.present_rgb565(p.ops.userdata, frame, 2, 2, 3));
    p.scratch = out; p.scratch_capacity = 4; /* scratch aliases output */
    CHECK(!p.ops.present_rgb565(p.ops.userdata, frame, 2, 2, 3));
    CHECK(m.presents == 1u && out[0] == 0xFFFFu && out[3] == 0xF81Fu);

    p.scratch = scratch;
    CHECK(!p.ops.present_rgb565(p.ops.userdata, scratch, 2, 2, 2)); /* frame == scratch */
    CHECK(p.ops.present_rgb565(p.ops.userdata, big, 1, 1, 1));
    CHECK(m.presents == 2u && m.w == 1u && m.last[0] == 0u);
    m.result = false; /* upstream failure propagates */
    CHECK(!p.ops.present_rgb565(p.ops.userdata, big, 1, 1, 1));
    CHECK(m.presents == 3u);
    CHECK(!p.ops.present_rgb565(NULL, big, 1, 1, 1));

    up.log = NULL;
    CHECK(pixelica_shader_presenter_init(&p, &up, &s, out, 4, NULL, 0) == PIXELICA_OK);
    CHECK(p.ops.log == NULL);
}

int main(void)
{
    test_defaults();
    test_point_effects();
    test_stride_scanlines_vignette();
    test_spatial();
    test_custom_and_sample();
    test_invalid_no_mutation();
    test_presenter();
    return pixelica_t_finish("test_shaders");
}

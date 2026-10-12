/*
 * Tests for the optional shader module's PAL presenter, driven through a fake
 * PAL so no renderer, window or SDL is needed: forwarding of userdata, time,
 * buttons and log, exactly one present per frame, preserved source frame, tight
 * output stride, failures that neither mutate nor forward, an upstream failure
 * passing through, shader changes between frames and the immutable snapshot a
 * custom sampling callback reads.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/shaders.h"
#include "pixelica/status.h"

#include "test_util.h"

#define W 3u
#define H 2u
#define N (W * H)
#define SRC_STRIDE 5u
#define SRC_COUNT (SRC_STRIDE * H)
#define SENTINEL 0xABCDu
#define PADDING 0x1111u
#define TIME_VALUE 123456789012ull
#define BUTTONS 0x5u

typedef struct Fake {
    int time_calls, read_calls, log_calls, present_calls;
    bool result;
    void *present_userdata;
    const uint16_t *pixels;
    uint16_t width, height;
    uint32_t stride;
    uint16_t copy[N];
    char message[16];
} Fake;

static uint64_t fake_time(void *userdata)
{
    ++((Fake *)userdata)->time_calls;
    return TIME_VALUE;
}

static uint32_t fake_read(void *userdata)
{
    ++((Fake *)userdata)->read_calls;
    return BUTTONS;
}

static void fake_log(void *userdata, const char *message)
{
    Fake *fake = (Fake *)userdata;

    ++fake->log_calls;
    size_t length = strlen(message);
    if (length >= sizeof fake->message) length = sizeof fake->message - 1u;
    memcpy(fake->message, message, length);
    fake->message[length] = '\0';
}

static bool fake_present(void *userdata, const uint16_t *pixels, uint16_t width, uint16_t height,
                         uint32_t stride)
{
    Fake *fake = (Fake *)userdata;

    ++fake->present_calls;
    fake->present_userdata = userdata;
    fake->pixels = pixels;
    fake->width = width;
    fake->height = height;
    fake->stride = stride;
    if (stride == width && (size_t)width * height <= N) {
        memcpy(fake->copy, pixels, (size_t)width * height * sizeof pixels[0]);
    }
    return fake->result;
}

typedef struct Rig {
    Fake fake;
    PIXELICA_ShaderPresenter p;
    uint16_t output[N], scratch[N];
    uint16_t src[SRC_COUNT], src_before[SRC_COUNT];
} Rig;

static PIXELICA_PlatformOps fake_ops(Fake *fake)
{
    PIXELICA_PlatformOps ops;

    memset(&ops, 0, sizeof ops);
    ops.userdata = fake;
    ops.time_us = fake_time;
    ops.read_buttons = fake_read;
    ops.present_rgb565 = fake_present;
    ops.log = fake_log;
    return ops;
}

static uint16_t src_value(unsigned x, unsigned y)
{
    return (uint16_t)(0x2001u + 0x111u * (y * W + x));
}

static bool setup(Rig *r, PIXELICA_ShaderKind kind)
{
    PIXELICA_PlatformOps up;
    PIXELICA_Shader shader;

    memset(r, 0, sizeof *r);
    r->fake.result = true;
    up = fake_ops(&r->fake);
    for (unsigned i = 0u; i < SRC_COUNT; ++i) {
        r->src[i] = PADDING;
    }
    for (unsigned y = 0u; y < H; ++y) {
        for (unsigned x = 0u; x < W; ++x) {
            r->src[y * SRC_STRIDE + x] = src_value(x, y);
        }
    }
    memcpy(r->src_before, r->src, sizeof r->src);
    for (unsigned i = 0u; i < N; ++i) {
        r->output[i] = SENTINEL;
    }
    return pixelica_shader_default(kind, &shader) == PIXELICA_OK &&
           pixelica_shader_presenter_init(&r->p, &up, &shader, r->output, N, r->scratch, N) == PIXELICA_OK;
}

static bool present_src(Rig *r)
{
    return r->p.ops.present_rgb565(r->p.ops.userdata, r->src, (uint16_t)W, (uint16_t)H, SRC_STRIDE);
}

static bool output_untouched(const Rig *r)
{
    for (unsigned i = 0u; i < N; ++i) {
        if (r->output[i] != SENTINEL) {
            return false;
        }
    }
    return true;
}

/* A rejected present must forward nothing, leave the output alone and keep the source. */
static void check_rejected(Rig *r)
{
    CHECK(!present_src(r));
    CHECK_EQ_LONG(r->fake.present_calls, 0);
    CHECK(output_untouched(r));
    CHECK(memcmp(r->src, r->src_before, sizeof r->src) == 0);
}

static void test_init_and_forwarding(void)
{
    Rig r;

    CHECK(setup(&r, PIXELICA_SHADER_TINT));
    CHECK(r.p.ops.userdata == &r.p);
    CHECK(r.p.ops.time_us != NULL && r.p.ops.read_buttons != NULL && r.p.ops.present_rgb565 != NULL);
    CHECK(r.p.ops.log != NULL);
    CHECK(r.p.output == r.output && r.p.scratch == r.scratch);
    CHECK_EQ_LONG(r.p.output_capacity, N);
    CHECK_EQ_LONG(r.p.scratch_capacity, N);
    CHECK(r.p.upstream.userdata == &r.fake);

    CHECK(r.p.ops.time_us(r.p.ops.userdata) == TIME_VALUE);
    CHECK_EQ_LONG(r.p.ops.read_buttons(r.p.ops.userdata), BUTTONS);
    r.p.ops.log(r.p.ops.userdata, "hello");
    CHECK_EQ_LONG(r.fake.time_calls, 1);
    CHECK_EQ_LONG(r.fake.read_calls, 1);
    CHECK_EQ_LONG(r.fake.log_calls, 1);
    CHECK(strcmp(r.fake.message, "hello") == 0);
    CHECK_EQ_LONG(r.fake.present_calls, 0);
}

static void test_init_failures(void)
{
    Rig r;
    PIXELICA_PlatformOps up;
    PIXELICA_Shader shader;
    PIXELICA_ShaderPresenter out;
    PIXELICA_ShaderPresenter expected;
    uint16_t buf[N];

    CHECK(setup(&r, PIXELICA_SHADER_TINT));
    up = fake_ops(&r.fake);
    CHECK(pixelica_shader_default(PIXELICA_SHADER_TINT, &shader) == PIXELICA_OK);
    memset(&out, 0x5A, sizeof out);
    memcpy(&expected, &out, sizeof out);

    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, buf, 0u, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, NULL, N, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(&out, &up, NULL, buf, N, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(&out, NULL, &shader, buf, N, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(NULL, &up, &shader, buf, N, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    shader.kind = (PIXELICA_ShaderKind)99;
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, buf, N, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_default(PIXELICA_SHADER_TINT, &shader) == PIXELICA_OK);

    up.present_rgb565 = NULL;
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, buf, N, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    up = fake_ops(&r.fake);
    up.time_us = NULL;
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, buf, N, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    up = fake_ops(&r.fake);
    up.read_buttons = NULL;
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, buf, N, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    up = fake_ops(&r.fake);

    /* Output may not overlap the shader struct, the upstream table or the presenter itself. */
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, (uint16_t *)(void *)&shader,
                                         sizeof shader / sizeof(uint16_t), NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, (uint16_t *)(void *)&up,
                                         sizeof up / sizeof(uint16_t), NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, (uint16_t *)(void *)&out,
                                         sizeof out / sizeof(uint16_t), NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    /* Output and scratch may not overlap each other. */
    CHECK(pixelica_shader_presenter_init(&out, &up, &shader, buf, N, buf, N) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&out, &expected, sizeof out) == 0);
}

static void test_present_success(void)
{
    Rig r;

    CHECK(setup(&r, PIXELICA_SHADER_TINT));
    r.p.shader.color = 0xF800u; /* pure red expands to (255,0,0) */
    r.p.shader.amount = 1.0f;
    CHECK(present_src(&r));
    CHECK_EQ_LONG(r.fake.present_calls, 1);
    CHECK(r.fake.present_userdata == &r.fake);
    CHECK(r.fake.pixels == r.output);
    CHECK(r.fake.pixels != r.src);
    CHECK_EQ_LONG(r.fake.width, W);
    CHECK_EQ_LONG(r.fake.height, H);
    CHECK_EQ_LONG(r.fake.stride, W); /* tight, not the source's stride */
    for (unsigned i = 0u; i < N; ++i) {
        CHECK_EQ_LONG(r.output[i], 0xF800);
        CHECK_EQ_LONG(r.fake.copy[i], 0xF800);
    }
    CHECK(memcmp(r.src, r.src_before, sizeof r.src) == 0); /* source and its padding preserved */
    CHECK_EQ_LONG(r.fake.time_calls + r.fake.read_calls + r.fake.log_calls, 0);

    /* Amount 0 is an identity copy: the source values reach the PAL tightly packed. */
    r.p.shader.amount = 0.0f;
    CHECK(present_src(&r));
    CHECK_EQ_LONG(r.fake.present_calls, 2);
    for (unsigned y = 0u; y < H; ++y) {
        for (unsigned x = 0u; x < W; ++x) {
            CHECK_EQ_LONG(r.fake.copy[y * W + x], src_value(x, y));
        }
    }
}

static void test_present_rejections(void)
{
    Rig r;

    CHECK(setup(&r, PIXELICA_SHADER_TINT));

    /* Invalid frames. */
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, NULL, (uint16_t)W, (uint16_t)H, SRC_STRIDE));
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, r.src, 0u, (uint16_t)H, SRC_STRIDE));
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, r.src, (uint16_t)W, 0u, SRC_STRIDE));
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, r.src, (uint16_t)W, (uint16_t)H, W - 1u));
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, r.src, 5000u, (uint16_t)H, 5000u));
    CHECK_EQ_LONG(r.fake.present_calls, 0);
    CHECK(output_untouched(&r));

    /* Frame overlapping the owned output, scratch or the presenter. */
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, r.output, (uint16_t)W, (uint16_t)H, W));
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, r.scratch, (uint16_t)W, (uint16_t)H, W));
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, (const uint16_t *)(const void *)&r.p, (uint16_t)W,
                                  (uint16_t)H, W));
    CHECK_EQ_LONG(r.fake.present_calls, 0);
    CHECK(output_untouched(&r));

    /* Output too small for the frame (capacity changed after init is revalidated). */
    r.p.output_capacity = N - 1u;
    check_rejected(&r);
    r.p.output_capacity = N;

    /* Invalid effect values. */
    r.p.shader.kind = (PIXELICA_ShaderKind)99;
    check_rejected(&r);
    r.p.shader.kind = PIXELICA_SHADER_TINT;
    r.p.shader.amount = pixelica_t_nan();
    check_rejected(&r);
    r.p.shader.amount = 1.5f;
    check_rejected(&r);
    r.p.shader.amount = 1.0f;

    /* Effects needing scratch: missing, too small or aliased scratch. */
    r.p.shader.kind = PIXELICA_SHADER_BLUR;
    r.p.shader.radius = 1u;
    r.p.scratch_capacity = N - 1u;
    check_rejected(&r);
    r.p.scratch_capacity = N;
    r.p.scratch = NULL;
    check_rejected(&r);
    r.p.scratch = r.output;
    check_rejected(&r);
    r.p.scratch = r.scratch;
    r.p.shader.radius = 5u; /* out of range 1..4 */
    check_rejected(&r);
    r.p.shader.radius = 1u;
    CHECK(present_src(&r)); /* the same presenter recovers once everything is valid */
    CHECK_EQ_LONG(r.fake.present_calls, 1);
}

static void test_upstream_failure(void)
{
    Rig r;

    CHECK(setup(&r, PIXELICA_SHADER_INVERT));
    r.fake.result = false;
    CHECK(!present_src(&r));
    CHECK_EQ_LONG(r.fake.present_calls, 1); /* forwarded exactly once, not retried */
    r.fake.result = true;
    CHECK(present_src(&r));
    CHECK_EQ_LONG(r.fake.present_calls, 2);
}

static void test_owned_buffer_ranges(void)
{
    Rig r;
    PIXELICA_PlatformOps up;
    PIXELICA_Shader shader;
    uint16_t palette[1] = {0xf800u};

    CHECK(setup(&r, PIXELICA_SHADER_INVERT));
    /* Even a one-pixel frame cannot borrow the unused tail of an owned array. */
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, r.output + N - 1u, 1u, 1u, 1u));
    CHECK(!r.p.ops.present_rgb565(r.p.ops.userdata, r.scratch + N - 1u, 1u, 1u, 1u));
    CHECK(output_untouched(&r) && r.fake.present_calls == 0);
    r.p.output_capacity = SIZE_MAX;
    check_rejected(&r);
    r.p.output_capacity = N;
    r.p.scratch_capacity = SIZE_MAX;
    check_rejected(&r);
    r.p.scratch_capacity = N;
    r.p.output = (uint16_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    check_rejected(&r); /* range wraps before any dereference */
    r.p.output = r.output;

    up = fake_ops(&r.fake);
    CHECK(pixelica_shader_default(PIXELICA_SHADER_PALETTE, &shader) == PIXELICA_OK);
    shader.palette = palette; shader.palette_count = 1u;
    CHECK(pixelica_shader_presenter_init(&r.p, &up, &shader, r.output, N,
                                         palette, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_shader_presenter_init(&r.p, &up, &shader, r.output, N,
                                         r.scratch, N) == PIXELICA_OK);
    r.p.shader.palette = r.scratch;
    check_rejected(&r);
}

static void test_dynamic_shader(void)
{
    Rig r;
    uint16_t palette[1] = {0x07E0u};

    CHECK(setup(&r, PIXELICA_SHADER_INVERT));
    CHECK(present_src(&r));
    CHECK(r.fake.copy[0] != src_value(0u, 0u));

    /* Palette of one entry maps everything to it; a palette inside the output is rejected. */
    CHECK(pixelica_shader_default(PIXELICA_SHADER_PALETTE, &r.p.shader) == PIXELICA_OK);
    r.p.shader.palette = palette;
    r.p.shader.palette_count = 1u;
    CHECK(present_src(&r));
    for (unsigned i = 0u; i < N; ++i) {
        CHECK_EQ_LONG(r.fake.copy[i], 0x07E0);
    }
    CHECK_EQ_LONG(r.fake.present_calls, 2);
    for (unsigned i = 0u; i < N; ++i) {
        r.output[i] = SENTINEL;
    }
    r.p.shader.palette = r.output;
    CHECK(!present_src(&r));
    CHECK_EQ_LONG(r.fake.present_calls, 2);
    CHECK(output_untouched(&r));
    r.p.shader.palette = palette;
    r.p.shader.palette_count = 0u;
    CHECK(!present_src(&r));
    CHECK_EQ_LONG(r.fake.present_calls, 2);
}

typedef struct Spy {
    int calls;
    const uint16_t *source_pixels;
    uint16_t source_width, source_height;
} Spy;

static uint16_t shift_left(const PIXELICA_ShaderPixel *pixel, void *userdata)
{
    Spy *spy = (Spy *)userdata;

    ++spy->calls;
    spy->source_pixels = pixel->source->pixels;
    spy->source_width = pixel->source->width;
    spy->source_height = pixel->source->height;
    return pixelica_shader_sample(pixel->source, (int32_t)pixel->x + 1, (int32_t)pixel->y);
}

static void test_custom_snapshot(void)
{
    Rig r;
    Spy spy;

    memset(&spy, 0, sizeof spy);
    CHECK(setup(&r, PIXELICA_SHADER_INVERT));
    CHECK(pixelica_shader_default(PIXELICA_SHADER_CUSTOM, &r.p.shader) == PIXELICA_OK);
    r.p.shader.callback = shift_left;
    r.p.shader.userdata = &spy;
    CHECK(present_src(&r));
    CHECK_EQ_LONG(r.fake.present_calls, 1);
    CHECK_EQ_LONG(spy.calls, (int)N);
    CHECK_EQ_LONG(spy.source_width, W);
    CHECK_EQ_LONG(spy.source_height, H);
    CHECK(spy.source_pixels != r.output && spy.source_pixels != r.src);
    /* Each pixel sees its right neighbor's ORIGINAL value (clamped at the edge), so
     * the shift does not cascade through already written output pixels. */
    for (unsigned y = 0u; y < H; ++y) {
        for (unsigned x = 0u; x < W; ++x) {
            CHECK_EQ_LONG(r.output[y * W + x], src_value(x + 1u < W ? x + 1u : x, y));
        }
    }
    CHECK(memcmp(r.src, r.src_before, sizeof r.src) == 0);

    r.p.shader.callback = NULL; /* custom without callback is rejected, nothing forwarded */
    for (unsigned i = 0u; i < N; ++i) {
        r.output[i] = SENTINEL;
    }
    r.fake.present_calls = 0;
    check_rejected(&r);
}

int main(void)
{
    test_init_and_forwarding();
    test_init_failures();
    test_present_success();
    test_present_rejections();
    test_upstream_failure();
    test_owned_buffer_ranges();
    test_dynamic_shader();
    test_custom_snapshot();
    return pixelica_t_finish("test_shaders_presenter");
}

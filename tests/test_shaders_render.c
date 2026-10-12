/* Renderers use the common shader API without private framebuffer access. */
#include "pixelica/shaders.h"
#include "test_util.h"
#ifdef PIXELICA_TEST_SHADERS2D
#include "pixelica/renderer2d.h"
#endif

#ifdef PIXELICA_TEST_WITH_3D
typedef struct Capture { uint32_t presents; uint16_t pixels[16]; bool fail; } Capture;
static uint64_t clock_us(void *userdata) { (void)userdata; return 0u; }
static uint32_t buttons(void *userdata) { (void)userdata; return 0u; }
static bool present(void *userdata, const uint16_t *pixels,
                    uint16_t width, uint16_t height, uint32_t stride)
{
    Capture *capture = (Capture *)userdata;
    size_t y;
    ++capture->presents;
    CHECK(width == 4u && height == 4u && stride == 4u);
    for (y = 0u; y < 4u; ++y)
        memcpy(capture->pixels + y * 4u, pixels + y * stride, 4u * sizeof(uint16_t));
    return !capture->fail;
}
static void render3d(void)
{
    Capture capture = {0};
    PIXELICA_PlatformOps upstream = {&capture, clock_us, buttons, present, NULL};
    PIXELICA_ShaderPresenter presenter;
    PIXELICA_Shader shader;
    uint16_t output[16], scratch[16];
    PIXELICA_Config cfg = pixelica_t_config(4u, 4u);
    PIXELICA_Context *ctx = NULL;
    CHECK(pixelica_shader_default(PIXELICA_SHADER_INVERT, &shader) == PIXELICA_OK);
    CHECK(pixelica_shader_presenter_init(&presenter, &upstream, &shader,
        output, 16u, scratch, 16u) == PIXELICA_OK);
    CHECK(pixelica_create(&cfg, &presenter.ops, &ctx) == PIXELICA_OK);
    if (ctx == NULL) return;
    pixelica_begin_frame(ctx, 0xf800u);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK(capture.presents == 1u && capture.pixels[0] == 0x07ffu);
    CHECK(pixelica_color_buffer(ctx)[0] == 0xf800u);
    CHECK(pixelica_depth_buffer(ctx)[0] == PIXELICA_DEPTH_CLEAR);
    CHECK(pixelica_frame_stats(ctx).draw_calls == 0u);
    capture.fail = true;
    pixelica_begin_frame(ctx, 0xffffu);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_ERR_PLATFORM);
    CHECK(capture.presents == 2u && capture.pixels[0] == 0u);
    CHECK(pixelica_shader_default(PIXELICA_SHADER_BLUR, &presenter.shader) == PIXELICA_OK);
    pixelica_begin_frame(ctx, 0xf800u);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_ERR_PLATFORM);
    CHECK(capture.presents == 3u && capture.pixels[0] == 0xf800u);
    CHECK(pixelica_memory_guards_intact(ctx) == 1);
    pixelica_destroy(ctx);
}
#endif

#ifdef PIXELICA_TEST_SHADERS2D
static void render2d(void)
{
    uint16_t pixels[20], depth[20];
    PIXELICA2D_Surface canvas = {pixels, 4u, 4u, 5u, depth, 5u};
    PIXELICA2D_Context ctx;
    PIXELICA_ShaderSurface surface = {pixels, 4u, 4u, 5u};
    PIXELICA_Shader shader;
    PIXELICA2D_Stats stats;
    PIXELICA2D_Camera camera;
    PIXELICA2D_Rect clip;
    size_t i;
    for (i = 0u; i < 20u; ++i) { pixels[i] = 0x1234u; depth[i] = 0x5678u; }
    CHECK(pixelica2d_init(&ctx, &canvas) == PIXELICA_OK);
    CHECK(pixelica2d_clear(&ctx, 0xf800u) == PIXELICA_OK);
    stats = ctx.stats; camera = ctx.camera; clip = ctx.clip;
    CHECK(pixelica_shader_default(PIXELICA_SHADER_INVERT, &shader) == PIXELICA_OK);
    CHECK(pixelica_shader_apply(&surface, &shader, NULL, 0u) == PIXELICA_OK);
    for (i = 0u; i < 20u; ++i) {
        CHECK(pixels[i] == (i % 5u == 4u ? 0x1234u : 0x07ffu));
        CHECK(depth[i] == (i % 5u == 4u ? 0x5678u : PIXELICA2D_DEPTH_CLEAR));
    }
    CHECK(memcmp(&stats, &ctx.stats, sizeof(stats)) == 0);
    CHECK(memcmp(&camera, &ctx.camera, sizeof(camera)) == 0);
    CHECK(memcmp(&clip, &ctx.clip, sizeof(clip)) == 0);
}
#endif

int main(void)
{
#ifdef PIXELICA_TEST_SHADERS2D
    render2d();
#endif
#ifdef PIXELICA_TEST_WITH_3D
    render3d();
#endif
    return pixelica_t_finish("shaders_render");
}

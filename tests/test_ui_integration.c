#include "pixelica/ui.h"
#include "test_util.h"
#ifdef PIXELICA_TEST_UI2D
#include "pixelica/renderer2d.h"
#endif

static PIXELICA_UI_Widget panel(void)
{
    PIXELICA_UI_Widget result;
    const PIXELICA_UI_Rect rect = {1, 1, 4, 4};
    CHECK(pixelica_ui_widget_default(PIXELICA_UI_PANEL, 1u, rect, NULL, 0u, &result) == PIXELICA_OK);
    return result;
}

#ifdef PIXELICA_TEST_UI2D
static void test_2d(void)
{
    uint16_t color[72], depth[72];
    PIXELICA2D_Surface target = {color, 8u, 8u, 9u, depth, 9u};
    PIXELICA2D_Context canvas;
    PIXELICA_UI_Widget item = panel();
    PIXELICA_UI_Context ui;
    PIXELICA_UI_Surface surface = {color, 8u, 8u, 9u};
    PIXELICA2D_Stats stats;
    for (size_t i = 0u; i < 72u; ++i) { color[i] = 0x1234u; depth[i] = 0x5678u; }
    CHECK(pixelica2d_init(&canvas, &target) == PIXELICA_OK);
    CHECK(pixelica2d_clear(&canvas, 0xf800u) == PIXELICA_OK);
    stats = canvas.stats;
    CHECK(pixelica_ui_init(&ui, &item, 1u) == PIXELICA_OK);
    CHECK(pixelica_ui_render_rgb565(&ui, &surface) == PIXELICA_OK);
    CHECK(color[2u * 9u + 2u] == ui.theme.panel);
    CHECK(color[0] == 0xf800u);
    for (size_t i = 0u; i < 72u; ++i) {
        CHECK(depth[i] == (i % 9u == 8u ? 0x5678u : PIXELICA2D_DEPTH_CLEAR));
        if (i % 9u == 8u) CHECK(color[i] == 0x1234u);
    }
    CHECK(memcmp(&stats, &canvas.stats, sizeof(stats)) == 0);
}
#endif

#ifdef PIXELICA_TEST_WITH_3D
typedef struct Capture { uint16_t pixels[64]; unsigned count; bool ok; } Capture;
static uint64_t clock_us(void *ud) { (void)ud; return 0u; }
static uint32_t buttons(void *ud) { (void)ud; return 0u; }
static bool present(void *ud, const uint16_t *pixels, uint16_t w, uint16_t h, uint32_t stride)
{
    Capture *capture = (Capture *)ud;
    CHECK(w == 8u && h == 8u && stride == 8u);
    memcpy(capture->pixels, pixels, sizeof(capture->pixels));
    ++capture->count;
    return capture->ok;
}
static void test_3d(void)
{
    Capture capture = {{0}, 0u, true};
    PIXELICA_PlatformOps upstream = {&capture, clock_us, buttons, present, NULL};
    PIXELICA_UI_Widget item = panel();
    PIXELICA_UI_Context ui;
    PIXELICA_UI_Presenter presenter;
    uint16_t output[64];
    PIXELICA_Config config = pixelica_t_config(8u, 8u);
    PIXELICA_Context *ctx = NULL;
    CHECK(pixelica_ui_init(&ui, &item, 1u) == PIXELICA_OK);
    CHECK(pixelica_ui_presenter_init(&presenter, &upstream, &ui, output, 64u) == PIXELICA_OK);
    CHECK(pixelica_create(&config, &presenter.ops, &ctx) == PIXELICA_OK);
    if (ctx == NULL) return;
    pixelica_begin_frame(ctx, 0xf800u);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);
    CHECK(capture.count == 1u && capture.pixels[0] == 0xf800u);
    CHECK(capture.pixels[2u * 8u + 2u] == ui.theme.panel);
    CHECK(pixelica_color_buffer(ctx)[2u * 8u + 2u] == 0xf800u);
    CHECK(pixelica_depth_buffer(ctx)[0] == PIXELICA_DEPTH_CLEAR);
    CHECK(pixelica_frame_stats(ctx).draw_calls == 0u);
    capture.ok = false;
    pixelica_begin_frame(ctx, 0xf800u);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_ERR_PLATFORM);
    CHECK(capture.count == 2u);
    item.text = "\xc0\x80"; item.text_bytes = 2u; /* invalid UTF-8, fail before output copy */
    output[0] = 0x1234u;
    pixelica_begin_frame(ctx, 0x07e0u);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_ERR_PLATFORM);
    CHECK(output[0] == 0x1234u && capture.count == 2u);
    CHECK(pixelica_memory_guards_intact(ctx) == 1);
    pixelica_destroy(ctx);
}
#endif

int main(void)
{
#ifdef PIXELICA_TEST_UI2D
    test_2d();
#endif
#ifdef PIXELICA_TEST_WITH_3D
    test_3d();
#endif
#if !defined(PIXELICA_TEST_UI2D) && !defined(PIXELICA_TEST_WITH_3D)
    (void)panel();
#endif
    return pixelica_t_finish("ui_integration");
}

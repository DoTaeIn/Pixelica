/* Pixelica debug: frame timing windows, bounded formatting, UI HUD clipping. */
#include <string.h>

#include "pixelica/debug.h"
#include "pixelica/ui.h"
#include "test_util.h"

#define SENTINEL 0xABCDu

static void feed(PIXELICA_DebugTiming *t, uint32_t frame_us, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; ++i) {
        CHECK_EQ_LONG(pixelica_debug_timing_update(t, frame_us), PIXELICA_OK);
    }
}

static void test_init_and_errors(void)
{
    PIXELICA_DebugTiming t, zero, before;

    memset(&zero, 0, sizeof zero);
    CHECK_EQ_LONG(pixelica_debug_timing_init(NULL, 0), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 0), PIXELICA_OK);
    CHECK_EQ_LONG(t.window_us, PIXELICA_DEBUG_DEFAULT_WINDOW_US);
    CHECK(!t.has_report);

    /* Rejected init leaves the object alone. */
    before = t;
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, PIXELICA_DEBUG_MAX_WINDOW_US + 1u),
        PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&t, &before, sizeof t) == 0);
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 1u), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, PIXELICA_DEBUG_MAX_WINDOW_US), PIXELICA_OK);

    /* Uninitialized / NULL / oversized frame: rejected, state unchanged. */
    CHECK_EQ_LONG(pixelica_debug_timing_update(NULL, 1000), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_timing_update(&zero, 1000), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(zero.frames, 0);
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 0), PIXELICA_OK);
    feed(&t, 1000, 3);
    before = t;
    CHECK_EQ_LONG(pixelica_debug_timing_update(&t, PIXELICA_DEBUG_MAX_FRAME_US + 1u),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_timing_update(&t, UINT32_MAX), PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&t, &before, sizeof t) == 0);
}

static void test_window_report(void)
{
    PIXELICA_DebugTiming t;
    char text[PIXELICA_DEBUG_TEXT_BYTES];
    size_t len = 0;
    bool trunc = true;

    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 0), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, text, sizeof text, &len, &trunc), PIXELICA_OK);
    CHECK(strcmp(text, "FPS -- | -- ms") == 0);
    CHECK_EQ_LONG(len, 14);
    CHECK(!trunc);

    feed(&t, 16666, 30); /* 499980 us: window still open */
    CHECK(!t.has_report);
    CHECK_EQ_LONG(t.last_frame_us, 16666);
    CHECK_EQ_LONG(pixelica_debug_timing_update(&t, 20000), PIXELICA_OK);
    CHECK(t.has_report);
    CHECK_EQ_LONG(t.max_frame_us, 20000);
    CHECK_EQ_LONG(t.avg_frame_us, 16774); /* 519980 / 31 */
    CHECK(t.fps_milli >= 59600 && t.fps_milli <= 59700);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, text, sizeof text, &len, NULL), PIXELICA_OK);
    CHECK(strcmp(text, "FPS 59.6 | 16.8 ms | max 20.0 ms") == 0);
    CHECK_EQ_LONG(len, strlen(text));

    /* The next window starts clean; the old report stays until it completes. */
    feed(&t, 1000, 10);
    CHECK_EQ_LONG(t.avg_frame_us, 16774);
    feed(&t, 1000, 490);
    CHECK_EQ_LONG(t.avg_frame_us, 1000);
    CHECK_EQ_LONG(t.fps_milli, 1000000);
    CHECK_EQ_LONG(t.max_frame_us, 1000);
}

static void test_zero_and_overflow(void)
{
    PIXELICA_DebugTiming t;
    char text[PIXELICA_DEBUG_TEXT_BYTES];
    unsigned i;

    /* All-zero windows never invent an FPS and never overflow the count. */
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 1000), PIXELICA_OK);
    feed(&t, 0, PIXELICA_DEBUG_MAX_WINDOW_FRAMES * 2u + 5u);
    CHECK(!t.has_report);
    CHECK_EQ_LONG(t.last_frame_us, 0);
    CHECK_EQ_LONG(t.frames, 5);

    /* 1 us frames report one million FPS without saturation. */
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, PIXELICA_DEBUG_MAX_WINDOW_US), PIXELICA_OK);
    feed(&t, 1, PIXELICA_DEBUG_MAX_WINDOW_FRAMES);
    CHECK(t.has_report);
    CHECK_EQ_LONG(t.fps_milli, 1000000000u);
    CHECK_EQ_LONG(t.avg_frame_us, 1);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, text, sizeof text, NULL, NULL), PIXELICA_OK);
    CHECK(strcmp(text, "FPS 1000000.0 | 0.0 ms | max 0.0 ms") == 0);

    /* Coarse clocks can give many zero-duration frames, then one tick. */
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, PIXELICA_DEBUG_MAX_WINDOW_US), PIXELICA_OK);
    feed(&t, 0, PIXELICA_DEBUG_MAX_WINDOW_FRAMES - 1u);
    CHECK_EQ_LONG(pixelica_debug_timing_update(&t, 1), PIXELICA_OK);
    CHECK_EQ_LONG(t.fps_milli, UINT32_MAX);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, text, sizeof text, NULL, NULL), PIXELICA_OK);
    CHECK(strcmp(text, "FPS 4294967.3 | 0.0 ms | max 0.0 ms") == 0);

    /* Longest legal frames against the longest window: the sum cannot wrap. */
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, PIXELICA_DEBUG_MAX_WINDOW_US), PIXELICA_OK);
    for (i = 0; i < 5; ++i) {
        CHECK_EQ_LONG(pixelica_debug_timing_update(&t, PIXELICA_DEBUG_MAX_FRAME_US), PIXELICA_OK);
    }
    CHECK(!t.has_report);
    CHECK_EQ_LONG(t.accum_us, 50000000);
    CHECK_EQ_LONG(pixelica_debug_timing_update(&t, PIXELICA_DEBUG_MAX_FRAME_US), PIXELICA_OK);
    CHECK(t.has_report);
    CHECK_EQ_LONG(t.accum_us, 0);
    CHECK_EQ_LONG(t.fps_milli, 100);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, text, sizeof text, NULL, NULL), PIXELICA_OK);
    CHECK(strcmp(text, "FPS 0.1 | 10000.0 ms | max 10000.0 ms") == 0);
    CHECK(strlen(text) < PIXELICA_DEBUG_TEXT_BYTES);
}

static void test_format_truncation(void)
{
    PIXELICA_DebugTiming t;
    char buf[64];
    size_t len = 99;
    bool trunc = false;

    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 1000), PIXELICA_OK);
    feed(&t, 16667, 1); /* 60.0 fps, 16.7 ms */

    /* "FPS 60.0 | 16.7 ms | max 16.7 ms" cut at 8 chars. */
    memset(buf, 'x', sizeof buf);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, buf, 8, &len, &trunc), PIXELICA_OK);
    CHECK(strcmp(buf, "FPS 60.") == 0);
    CHECK_EQ_LONG(len, 7);
    CHECK(trunc);
    CHECK_EQ_LONG(buf[8], 'x'); /* nothing past the capacity */

    memset(buf, 'x', sizeof buf);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, buf, 1, &len, &trunc), PIXELICA_OK);
    CHECK_EQ_LONG(buf[0], 0);
    CHECK_EQ_LONG(len, 0);
    CHECK(trunc);
    CHECK_EQ_LONG(buf[1], 'x');

    /* Metric: exact fit is not truncation, one byte less is. */
    CHECK_EQ_LONG(pixelica_debug_format_metric("fps", 3, 5, buf, 7, &len, &trunc), PIXELICA_OK);
    CHECK(strcmp(buf, "fps: 5") == 0);
    CHECK_EQ_LONG(len, 6);
    CHECK(!trunc);
    CHECK_EQ_LONG(pixelica_debug_format_metric("fps", 3, 5, buf, 6, &len, &trunc), PIXELICA_OK);
    CHECK(strcmp(buf, "fps: ") == 0);
    CHECK(trunc);

    /* name is not NUL terminated: only name_bytes are used. */
    CHECK_EQ_LONG(pixelica_debug_format_metric("tris!!", 4, -2147483647 - 1, buf, sizeof buf,
        &len, &trunc), PIXELICA_OK);
    CHECK(strcmp(buf, "tris: -2147483648") == 0);
    CHECK_EQ_LONG(pixelica_debug_format_metric("0123456789012345678901234", 24, 0, buf,
        sizeof buf, NULL, NULL), PIXELICA_OK);
    CHECK(strcmp(buf, "012345678901234567890123: 0") == 0);
}

static void test_format_errors(void)
{
    PIXELICA_DebugTiming t, zero;
    char buf[16];
    size_t len_in_buf[4]; /* out_len aliasing the output buffer */
    char name[8] = "abcdefg";
    size_t len = 77;
    bool trunc = true;

    memset(&zero, 0, sizeof zero);
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 0), PIXELICA_OK);
    memset(buf, 'q', sizeof buf);

    CHECK_EQ_LONG(pixelica_debug_format_timing(NULL, buf, sizeof buf, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&zero, buf, sizeof buf, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, NULL, sizeof buf, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, buf, 0, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, (char *)&t, sizeof t, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG); /* output overlaps input */
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, (char *)len_in_buf, sizeof len_in_buf,
        &len_in_buf[1], &trunc), PIXELICA_ERR_INVALID_ARG); /* out_len inside the buffer */
    CHECK_EQ_LONG(pixelica_debug_format_timing(&t, buf, sizeof buf, &len,
        (bool *)((unsigned char *)&len + 1)), PIXELICA_ERR_INVALID_ARG);

    CHECK_EQ_LONG(pixelica_debug_format_metric(NULL, 3, 1, buf, sizeof buf, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_metric(name, 0, 1, buf, sizeof buf, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_metric(name, 25, 1, buf, sizeof buf, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_metric("a\nb", 3, 1, buf, sizeof buf, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_metric("a\x80", 2, 1, buf, sizeof buf, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_debug_format_metric(name, 3, 1, name, sizeof name, &len, &trunc),
        PIXELICA_ERR_INVALID_ARG);

    /* Every failure above left buffer and outputs untouched. */
    CHECK_EQ_LONG(len, 77);
    CHECK(trunc);
    CHECK_EQ_LONG(buf[0], 'q');
    CHECK_EQ_LONG(buf[15], 'q');
    CHECK(memcmp(name, "abcdefg", 8) == 0);
}

static void fill_surface(uint16_t *px, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        px[i] = SENTINEL;
    }
}

static void test_hud(void)
{
    enum { W = 320, H = 40, STRIDE = 330 };
    static uint16_t px[STRIDE * H];
    PIXELICA_UI_Surface s;
    PIXELICA_DebugTiming t;
    PIXELICA_UI_DebugHud hud, zero;
    PIXELICA_UI_Rect r;
    int32_t one_line_h;
    uint16_t panel, border;
    int x, y;

    memset(&zero, 0, sizeof zero);
    s.pixels = px;
    s.width = W;
    s.height = H;
    s.stride = STRIDE;
    fill_surface(px, sizeof px / sizeof px[0]);

    CHECK_EQ_LONG(pixelica_ui_debug_hud_init(NULL, 0, 0), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_init(&hud, 4, 3), PIXELICA_OK);
    panel = hud.ui.theme.panel;
    border = hud.ui.theme.border;
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 1000), PIXELICA_OK);
    feed(&t, 16667, 1);

    /* Empty HUD draws nothing. */
    CHECK_EQ_LONG(pixelica_ui_debug_hud_render_rgb565(&hud, &s), PIXELICA_OK);
    CHECK_EQ_LONG(px[3 * STRIDE + 4], SENTINEL);

    /* Errors keep the previous (empty) text. */
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, NULL, NULL, 0, 0), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, NULL, 3, 0), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, "bad\n", 4, 0), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&zero, &t, NULL, 0, 0), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(hud.panel.text_bytes, 0);
    CHECK_EQ_LONG(hud.panel.rect.width, 0);

    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, NULL, 0, 0), PIXELICA_OK);
    one_line_h = hud.panel.rect.height;
    CHECK(hud.panel.rect.width > 0 && one_line_h > 0);
    CHECK_EQ_LONG(hud.panel.text_bytes, strlen("FPS 60.0 | 16.7 ms | max 16.7 ms"));
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, "tris", 4, 1234), PIXELICA_OK);
    CHECK(hud.panel.rect.height > one_line_h);
    CHECK(memcmp(hud.text, "FPS 60.0 | 16.7 ms | max 16.7 ms\ntris: 1234", 43) == 0);
    CHECK_EQ_LONG(hud.panel.text_bytes, 43);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, "x", 1, 0), PIXELICA_OK);
    CHECK_EQ_LONG(hud.panel.text_bytes, 32 + 1 + 4);

    /* A failed update after a good one keeps the good text and size. */
    r = hud.panel.rect;
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, "", 0, 0), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(hud.panel.text_bytes, 37);
    CHECK(memcmp(&hud.panel.rect, &r, sizeof r) == 0);

    /* Render: panel + border inside, padding and outside untouched. */
    CHECK_EQ_LONG(pixelica_ui_debug_hud_render_rgb565(&hud, &s), PIXELICA_OK);
    CHECK_EQ_LONG(px[r.y * STRIDE + r.x], border);
    CHECK_EQ_LONG(px[(r.y + 1) * STRIDE + r.x + 1], panel);
    CHECK_EQ_LONG(px[(r.y + r.height - 1) * STRIDE + r.x + r.width - 1], border);
    CHECK_EQ_LONG(px[(r.y + r.height) * STRIDE + r.x], SENTINEL);
    CHECK_EQ_LONG(px[r.y * STRIDE + r.x + r.width], SENTINEL);
    CHECK_EQ_LONG(px[0], SENTINEL);
    for (y = 0; y < H; ++y) {
        for (x = W; x < STRIDE; ++x) {
            CHECK_EQ_LONG(px[y * STRIDE + x], SENTINEL);
        }
    }

    /* Partly off the top-left: clipped, right/bottom border still visible. */
    fill_surface(px, sizeof px / sizeof px[0]);
    hud.x = -10;
    hud.y = -6;
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, "x", 1, 0), PIXELICA_OK);
    r = hud.panel.rect;
    CHECK_EQ_LONG(r.x, -10);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_render_rgb565(&hud, &s), PIXELICA_OK);
    CHECK_EQ_LONG(px[1 * STRIDE + r.x + r.width - 1], border);
    CHECK_EQ_LONG(px[(r.y + r.height - 1) * STRIDE + 1], border);

    /* Partly off the bottom-right: clipped to the surface, padding intact. */
    fill_surface(px, sizeof px / sizeof px[0]);
    hud.x = W - 10;
    hud.y = H - 4;
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, NULL, 0, 0), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_render_rgb565(&hud, &s), PIXELICA_OK);
    CHECK_EQ_LONG(px[(H - 4) * STRIDE + W - 10], border);
    CHECK_EQ_LONG(px[(H - 3) * STRIDE + W - 9], panel);
    for (y = 0; y < H; ++y) {
        for (x = W; x < STRIDE; ++x) {
            CHECK_EQ_LONG(px[y * STRIDE + x], SENTINEL);
        }
    }

    /* Entirely outside, or a bad surface: nothing changes. */
    fill_surface(px, sizeof px / sizeof px[0]);
    hud.x = 5000;
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&hud, &t, NULL, 0, 0), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_render_rgb565(&hud, &s), PIXELICA_OK);
    for (x = 0; x < STRIDE * H; ++x) {
        CHECK_EQ_LONG(px[x], SENTINEL);
    }
    s.pixels = NULL;
    CHECK_EQ_LONG(pixelica_ui_debug_hud_render_rgb565(&hud, &s), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_render_rgb565(&zero, &s), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_render_rgb565(NULL, &s), PIXELICA_ERR_INVALID_ARG);
}

static void test_hud_copy_rejected(void)
{
    PIXELICA_UI_DebugHud a, b;
    PIXELICA_DebugTiming t;

    CHECK_EQ_LONG(pixelica_ui_debug_hud_init(&a, 0, 0), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_debug_timing_init(&t, 0), PIXELICA_OK);
    b = a; /* self pointers now refer to a */
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&b, &t, NULL, 0, 0), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_ui_debug_hud_update(&a, &t, NULL, 0, 0), PIXELICA_OK);
}

int main(void)
{
    test_init_and_errors();
    test_window_report();
    test_zero_and_overflow();
    test_format_truncation();
    test_format_errors();
    test_hud();
    test_hud_copy_rejected();
    return pixelica_t_finish("test_debug");
}

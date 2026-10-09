/*
 * Host test for platforms/esp32p4/lcd_transport.c with a MOCKED port.
 *
 * The mock models an asynchronous DMA engine: draw() only queues a transfer;
 * the pixels are read from the staging buffer later, when the test (or
 * wait_done) "completes" it, and the completion event is delivered through
 * pixelica_lcd_transport_isr_done() like the board's done callback would. A staging
 * buffer that is rewritten while a transfer is pending, a second draw while
 * one is pending, or a free while one is pending are all counted as
 * violations and must stay at zero.
 *
 * This proves the ownership/lifetime logic only. It uses no ESP-IDF, no LCD
 * and no DMA, and says nothing about real hardware behaviour.
 */
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lcd_transport.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        ++g_checks;                                                          \
        if (a_ != e_) {                                                      \
            ++g_failures;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s == %s (%lld vs %lld)\n", \
                    __FILE__, __LINE__, #actual, #expected, a_, e_);         \
        }                                                                    \
    } while (0)

#define FRAME_W 8
#define FRAME_H 20
#define STRIDE 10
#define PANEL_W 32
#define PANEL_H 40
#define SENTINEL 0xAAAAu
#define BACKING_BYTES 4096

typedef enum WaitMode {
    WAIT_COMPLETE,              /* complete the pending transfer, as hardware eventually would */
    WAIT_TIMEOUT,               /* nothing ever completes */
    WAIT_SPURIOUS_THEN_COMPLETE,/* first wake-up carries no completion */
    WAIT_COMPLETE_TWICE         /* duplicate completion event */
} WaitMode;

typedef struct Mock {
    WaitMode wait_mode;
    bool spurious_done;
    int draw_rc;
    bool draw_rc_queues; /* a failing draw still left a transfer queued */
    bool quiesce_result;
    int quiesce_calls;
    int wait_calls;

    bool pending;
    int x0, y0, x1, y1;
    const uint16_t *pending_src;
    uint64_t pending_checksum;
    int draws;
    int overlapping_draws;
    int corrupted_transfers; /* staging changed between draw and completion */
    int free_while_pending;

    int alloc_calls;
    int free_calls;
    bool fail_alloc;
    size_t alloc_alignment;
    size_t alloc_bytes;

    uint16_t panel[PANEL_H][PANEL_W];
} Mock;

static Mock g_m;
static PIXELICA_LcdTransport g_t;
static alignas(128) uint8_t g_backing[BACKING_BYTES];
static uint16_t g_frame[STRIDE * FRAME_H];

static uint64_t checksum(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0u; i < bytes; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

static size_t pending_bytes(void)
{
    return (size_t)(g_m.x1 - g_m.x0) * (size_t)(g_m.y1 - g_m.y0) * 2u;
}

/* The "hardware" finishing the queued transfer and raising the done event. */
static void hardware_completes(void)
{
    if (!g_m.pending) {
        return;
    }
    if (checksum(g_m.pending_src, pending_bytes()) != g_m.pending_checksum) {
        g_m.corrupted_transfers += 1;
    }
    const uint16_t *src = g_m.pending_src;
    for (int y = g_m.y0; y < g_m.y1; ++y) {
        for (int x = g_m.x0; x < g_m.x1; ++x) {
            g_m.panel[y][x] = *src++;
        }
    }
    g_m.pending = false;
    pixelica_lcd_transport_isr_done(&g_t);
}

static int mock_draw(void *ctx, int x0, int y0, int x1, int y1, const void *pixels)
{
    (void)ctx;
    g_m.draws += 1;
    if (g_m.pending) {
        g_m.overlapping_draws += 1;
    }
    if (g_m.draw_rc != 0 && !g_m.draw_rc_queues) {
        return g_m.draw_rc;
    }
    g_m.pending = true;
    g_m.x0 = x0;
    g_m.y0 = y0;
    g_m.x1 = x1;
    g_m.y1 = y1;
    g_m.pending_src = pixels;
    g_m.pending_checksum = checksum(pixels, pending_bytes());
    return g_m.draw_rc;
}

static bool mock_wait(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    g_m.wait_calls += 1;
    switch (g_m.wait_mode) {
    case WAIT_COMPLETE:
        if (!g_m.pending) {
            return false; /* nothing queued, so no event will ever come */
        }
        hardware_completes();
        return true;
    case WAIT_TIMEOUT:
        return false;
    case WAIT_SPURIOUS_THEN_COMPLETE:
        if (!g_m.spurious_done) {
            g_m.spurious_done = true;
            return true;
        }
        if (!g_m.pending) {
            return false;
        }
        hardware_completes();
        return true;
    case WAIT_COMPLETE_TWICE:
        if (!g_m.pending) {
            return false;
        }
        hardware_completes();
        pixelica_lcd_transport_isr_done(&g_t);
        return true;
    }
    return false;
}

static void *mock_alloc(void *ctx, size_t alignment, size_t bytes)
{
    (void)ctx;
    g_m.alloc_calls += 1;
    g_m.alloc_alignment = alignment;
    g_m.alloc_bytes = bytes;
    if (g_m.fail_alloc || bytes > sizeof(g_backing) || alignment > 128u) {
        return NULL;
    }
    memset(g_backing, 0xEE, sizeof(g_backing));
    return g_backing;
}

static void mock_free(void *ctx, void *staging)
{
    (void)ctx;
    g_m.free_calls += 1;
    if (g_m.pending) {
        g_m.free_while_pending += 1;
    }
    memset(staging, 0xDD, sizeof(g_backing)); /* a late DMA read would now see poison */
}

static bool mock_quiesce(void *ctx)
{
    (void)ctx;
    g_m.quiesce_calls += 1;
    if (g_m.quiesce_result) {
        g_m.pending = false; /* hardware idle: the abandoned transfer is gone */
    }
    return g_m.quiesce_result;
}

static void reset(void)
{
    memset(&g_m, 0, sizeof(g_m));
    memset(&g_t, 0, sizeof(g_t));
    for (int y = 0; y < PANEL_H; ++y) {
        for (int x = 0; x < PANEL_W; ++x) {
            g_m.panel[y][x] = (uint16_t)SENTINEL;
        }
    }
    g_m.wait_mode = WAIT_COMPLETE;
}

static PIXELICA_LcdTransportConfig config(bool with_quiesce)
{
    PIXELICA_LcdTransportConfig c;
    memset(&c, 0, sizeof(c));
    c.port.draw = mock_draw;
    c.port.wait_done = mock_wait;
    c.port.alloc_staging = mock_alloc;
    c.port.free_staging = mock_free;
    c.port.quiesce = with_quiesce ? mock_quiesce : NULL;
    c.width = FRAME_W;
    c.height = FRAME_H;
    c.timeout_ms = 10u;
    return c;
}

static void fill_frame(uint32_t seed)
{
    for (int y = 0; y < FRAME_H; ++y) {
        for (int x = 0; x < STRIDE; ++x) {
            g_frame[y * STRIDE + x] = x < FRAME_W
                ? (uint16_t)(seed * 4099u + (uint32_t)y * 131u + (uint32_t)x * 7u + 1u)
                : (uint16_t)0xBEEFu; /* row padding must never reach the panel */
        }
    }
}

static uint16_t swap16(uint16_t v)
{
    return (uint16_t)((uint16_t)(v << 8) | (uint16_t)(v >> 8));
}

static bool panel_matches(int ox, int oy, bool swap)
{
    for (int y = 0; y < FRAME_H; ++y) {
        for (int x = 0; x < FRAME_W; ++x) {
            uint16_t expected = g_frame[y * STRIDE + x];
            if (swap) {
                expected = swap16(expected);
            }
            if (g_m.panel[oy + y][ox + x] != expected) {
                return false;
            }
        }
    }
    return true;
}

static bool pixel_outside_region_untouched(int ox, int oy)
{
    for (int y = 0; y < PANEL_H; ++y) {
        for (int x = 0; x < PANEL_W; ++x) {
            bool inside = x >= ox && x < ox + FRAME_W && y >= oy && y < oy + FRAME_H;
            if (!inside && g_m.panel[y][x] != SENTINEL) {
                return false;
            }
        }
    }
    return true;
}

static PIXELICA_LcdStatus present_frame(void)
{
    return pixelica_lcd_transport_present(&g_t, g_frame, FRAME_W, FRAME_H, STRIDE);
}

static void test_init_validation(void)
{
    reset();
    PIXELICA_LcdTransportConfig c = config(false);

    CHECK_EQ(pixelica_lcd_transport_init(NULL, &c), PIXELICA_LCD_ERR_INVALID_ARG);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, NULL), PIXELICA_LCD_ERR_INVALID_ARG);

    PIXELICA_LcdTransportConfig bad = c;
    bad.width = 0;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &bad), PIXELICA_LCD_ERR_INVALID_ARG);
    bad = c;
    bad.height = 0;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &bad), PIXELICA_LCD_ERR_INVALID_ARG);
    bad = c;
    bad.alignment = 3;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &bad), PIXELICA_LCD_ERR_INVALID_ARG);
    bad = c;
    bad.alignment = 1;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &bad), PIXELICA_LCD_ERR_INVALID_ARG);
    bad = c;
    bad.port.draw = NULL;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &bad), PIXELICA_LCD_ERR_INVALID_ARG);
    bad = c;
    bad.port.wait_done = NULL;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &bad), PIXELICA_LCD_ERR_INVALID_ARG);
    bad = c;
    bad.port.alloc_staging = NULL;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &bad), PIXELICA_LCD_ERR_INVALID_ARG);
    bad = c;
    bad.port.free_staging = NULL;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &bad), PIXELICA_LCD_ERR_INVALID_ARG);
    CHECK_EQ(g_m.alloc_calls, 0); /* nothing was allocated for invalid input */
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_UNINIT);

    /* Uninitialized transport refuses to present/deinit. */
    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_STATE);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_ERR_STATE);
    CHECK_EQ(pixelica_lcd_transport_recover(&g_t), PIXELICA_LCD_ERR_STATE);

    /* Allocation failure leaves the transport unusable and frees nothing. */
    g_m.fail_alloc = true;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_ERR_NO_MEMORY);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_UNINIT);
    CHECK_EQ(g_m.free_calls, 0);
    g_m.fail_alloc = false;

    /* Defaults: whole-frame staging, 128-byte alignment, size rounded up to it. */
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.alloc_alignment, PIXELICA_LCD_DEFAULT_ALIGNMENT);
    CHECK_EQ(g_m.alloc_bytes, 384); /* 8*20*2 = 320 -> 384 */
    CHECK(((uintptr_t)g_t.staging % PIXELICA_LCD_DEFAULT_ALIGNMENT) == 0u);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.free_calls, 1);
}

static void test_full_frame_with_offset_and_stride(void)
{
    reset();
    PIXELICA_LcdTransportConfig c = config(false);
    c.offset_x = 5;
    c.offset_y = 3;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);

    fill_frame(1);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.draws, 1);
    CHECK_EQ(g_m.x0, 5);
    CHECK_EQ(g_m.y0, 3);
    CHECK_EQ(g_m.x1, 5 + FRAME_W);
    CHECK_EQ(g_m.y1, 3 + FRAME_H);
    CHECK(panel_matches(5, 3, false));
    CHECK(pixel_outside_region_untouched(5, 3));
    CHECK(!g_m.pending);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_IDLE);

    /* The engine may overwrite its buffer as soon as present returns. */
    memset(g_frame, 0, sizeof(g_frame));
    CHECK(g_m.panel[3][5] != 0u || g_m.panel[3][6] != 0u);

    CHECK_EQ(g_m.overlapping_draws, 0);
    CHECK_EQ(g_m.corrupted_transfers, 0);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
}

static void test_banded_byte_swap_reuses_one_staging_buffer(void)
{
    reset();
    PIXELICA_LcdTransportConfig c = config(false);
    c.staging_rows = 7;
    c.alignment = 64;
    c.swap_bytes = true;
    c.offset_x = 2;
    c.offset_y = 1;
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.alloc_bytes, 128); /* 7*8*2 = 112 -> 128 */

    fill_frame(2);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.draws, 3); /* 7 + 7 + 6 rows */
    CHECK_EQ(g_m.y0, 1 + 14);
    CHECK_EQ(g_m.y1, 1 + 20);
    CHECK(panel_matches(2, 1, true));
    CHECK(pixel_outside_region_untouched(2, 1));

    fill_frame(3);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.draws, 6);
    CHECK(panel_matches(2, 1, true));
    CHECK_EQ(g_m.alloc_calls, 1); /* permanent staging: never allocated per frame */
    CHECK_EQ(g_m.free_calls, 0);
    CHECK_EQ(g_m.overlapping_draws, 0);
    CHECK_EQ(g_m.corrupted_transfers, 0);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
}

static void test_argument_validation_keeps_transport_usable(void)
{
    reset();
    PIXELICA_LcdTransportConfig c = config(false);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    fill_frame(4);

    CHECK_EQ(pixelica_lcd_transport_present(&g_t, NULL, FRAME_W, FRAME_H, STRIDE), PIXELICA_LCD_ERR_INVALID_ARG);
    CHECK_EQ(pixelica_lcd_transport_present(&g_t, g_frame, FRAME_W + 1, FRAME_H, STRIDE), PIXELICA_LCD_ERR_INVALID_ARG);
    CHECK_EQ(pixelica_lcd_transport_present(&g_t, g_frame, FRAME_W, FRAME_H - 1, STRIDE), PIXELICA_LCD_ERR_INVALID_ARG);
    CHECK_EQ(pixelica_lcd_transport_present(&g_t, g_frame, FRAME_W, FRAME_H, FRAME_W - 1), PIXELICA_LCD_ERR_INVALID_ARG);
    CHECK_EQ(pixelica_lcd_transport_present(NULL, g_frame, FRAME_W, FRAME_H, STRIDE), PIXELICA_LCD_ERR_INVALID_ARG);
    CHECK_EQ(g_m.draws, 0);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_IDLE);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
}

static void test_spurious_wakeup_is_not_completion(void)
{
    reset();
    g_m.wait_mode = WAIT_SPURIOUS_THEN_COMPLETE;
    PIXELICA_LcdTransportConfig c = config(false);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    fill_frame(5);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.wait_calls, 2); /* the wake-up without a counted completion was ignored */
    CHECK(panel_matches(0, 0, false));
    CHECK_EQ(g_m.corrupted_transfers, 0);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
}

static void test_timeout_poisons_until_completion_is_counted(void)
{
    reset();
    g_m.wait_mode = WAIT_TIMEOUT;
    PIXELICA_LcdTransportConfig c = config(false);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    fill_frame(6);

    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_TIMEOUT);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_POISONED);
    CHECK(g_m.pending); /* the DMA is still reading staging */

    /* While poisoned: no new draw, staging not rewritten, nothing freed. */
    uint64_t staging_before = checksum(g_backing, sizeof(g_backing));
    int draws_before = g_m.draws;
    int waits_before = g_m.wait_calls;
    fill_frame(7);
    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_POISONED);
    CHECK_EQ(g_m.draws, draws_before);
    CHECK_EQ(g_m.wait_calls, waits_before);
    CHECK(checksum(g_backing, sizeof(g_backing)) == staging_before);

    /* Shutdown cannot prove the DMA idle: ownership is kept, memory not freed. */
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_ERR_BUSY);
    CHECK_EQ(g_m.free_calls, 0);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_POISONED);
    CHECK_EQ(pixelica_lcd_transport_recover(&g_t), PIXELICA_LCD_ERR_BUSY);

    /* The late completion finally arrives and is counted. */
    hardware_completes();
    g_m.wait_mode = WAIT_COMPLETE;
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK); /* poison cleared by the counted completion */
    CHECK(panel_matches(0, 0, false));
    CHECK_EQ(g_m.overlapping_draws, 0);
    CHECK_EQ(g_m.corrupted_transfers, 0);

    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.free_calls, 1);
    CHECK_EQ(g_m.free_while_pending, 0);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_CLOSED);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_ERR_STATE); /* no double free */
    CHECK_EQ(g_m.free_calls, 1);
}

static void test_recover_waits_for_late_completion(void)
{
    reset();
    g_m.wait_mode = WAIT_TIMEOUT;
    PIXELICA_LcdTransportConfig c = config(false);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    fill_frame(8);
    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_TIMEOUT);

    CHECK_EQ(pixelica_lcd_transport_recover(&g_t), PIXELICA_LCD_ERR_BUSY);
    g_m.wait_mode = WAIT_COMPLETE; /* the board's completion path starts working again */
    CHECK_EQ(pixelica_lcd_transport_recover(&g_t), PIXELICA_LCD_OK);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_IDLE);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.free_while_pending, 0);
}

static void test_draw_failure_never_counts_as_completion(void)
{
    /* A failing draw that still left a transfer queued. */
    reset();
    g_m.draw_rc = -1;
    g_m.draw_rc_queues = true;
    PIXELICA_LcdTransportConfig c = config(false);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    fill_frame(9);
    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_DRAW);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_POISONED);
    CHECK(g_m.pending);
    g_m.wait_mode = WAIT_TIMEOUT;
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_ERR_BUSY);
    CHECK_EQ(g_m.free_calls, 0);
    g_m.wait_mode = WAIT_COMPLETE;
    CHECK_EQ(pixelica_lcd_transport_recover(&g_t), PIXELICA_LCD_OK); /* completion observed */
    g_m.draw_rc = 0;
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.overlapping_draws, 0);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.free_while_pending, 0);

    /* A failing draw that queued nothing: no event will ever arrive, so only a
     * board-supplied quiesce can clear the poison. */
    reset();
    g_m.draw_rc = -1;
    g_m.draw_rc_queues = false;
    PIXELICA_LcdTransportConfig with_quiesce = config(true);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &with_quiesce), PIXELICA_LCD_OK);
    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_DRAW);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_POISONED);
    g_m.quiesce_result = false;
    CHECK_EQ(pixelica_lcd_transport_recover(&g_t), PIXELICA_LCD_ERR_BUSY);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_ERR_BUSY);
    CHECK_EQ(g_m.free_calls, 0);
    g_m.quiesce_result = true;
    CHECK_EQ(pixelica_lcd_transport_recover(&g_t), PIXELICA_LCD_OK);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_IDLE);
    g_m.draw_rc = 0;
    fill_frame(10);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK); /* counters were resynchronized */
    CHECK(panel_matches(0, 0, false));
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.free_while_pending, 0);
}

static void test_event_count_mismatch_is_a_protocol_error(void)
{
    /* Duplicate completion for one draw. */
    reset();
    g_m.wait_mode = WAIT_COMPLETE_TWICE;
    PIXELICA_LcdTransportConfig c = config(true);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    fill_frame(11);
    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_PROTOCOL);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_POISONED);
    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_POISONED);
    g_m.quiesce_result = false;
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_ERR_BUSY);
    CHECK_EQ(g_m.free_calls, 0);
    g_m.quiesce_result = true;
    g_m.wait_mode = WAIT_COMPLETE;
    CHECK_EQ(pixelica_lcd_transport_recover(&g_t), PIXELICA_LCD_OK);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);

    /* A stray event while IDLE: refuse to draw rather than trust the next wait. */
    reset();
    PIXELICA_LcdTransportConfig c2 = config(false);
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c2), PIXELICA_LCD_OK);
    pixelica_lcd_transport_isr_done(&g_t);
    fill_frame(12);
    CHECK_EQ(present_frame(), PIXELICA_LCD_ERR_PROTOCOL);
    CHECK_EQ(g_m.draws, 0);
    CHECK_EQ(pixelica_lcd_transport_state(&g_t), PIXELICA_LCD_STATE_POISONED);
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_ERR_BUSY);
    CHECK_EQ(g_m.free_calls, 0);
}

static void test_counter_wraparound(void)
{
    reset();
    PIXELICA_LcdTransportConfig c = config(false);
    c.staging_rows = 1; /* 20 draws per frame */
    CHECK_EQ(pixelica_lcd_transport_init(&g_t, &c), PIXELICA_LCD_OK);
    g_t.submitted = UINT32_MAX - 5u;
    g_t.completed = UINT32_MAX - 5u;
    fill_frame(13);
    CHECK_EQ(present_frame(), PIXELICA_LCD_OK);
    CHECK_EQ(g_m.draws, FRAME_H);
    CHECK_EQ(g_t.submitted, g_t.completed);
    CHECK(panel_matches(0, 0, false));
    CHECK_EQ(pixelica_lcd_transport_deinit(&g_t), PIXELICA_LCD_OK);
}

int main(void)
{
    test_init_validation();
    test_full_frame_with_offset_and_stride();
    test_banded_byte_swap_reuses_one_staging_buffer();
    test_argument_validation_keeps_transport_usable();
    test_spurious_wakeup_is_not_completion();
    test_timeout_poisons_until_completion_is_counted();
    test_recover_waits_for_late_completion();
    test_draw_failure_never_counts_as_completion();
    test_event_count_mismatch_is_a_protocol_error();
    test_counter_wraparound();

    if (g_failures != 0) {
        fprintf(stderr, "test_lcd_transport: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_lcd_transport: all %d checks passed\n", g_checks);
    return 0;
}

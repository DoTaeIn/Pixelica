/*
 * Tests for the UI RGB565 renderer, built-in/custom font and PAL presenter.
 * No renderer, SDL or window: widgets are drawn into a plain pixel buffer and
 * pixels are checked at computed positions (default 5x7 font, scale 1, padding 2,
 * so a glyph cell is 5x7 at advance 6 and line height 8).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/status.h"
#include "pixelica/ui.h"

#include "test_util.h"

#define SENT 0xA5A5u
#define C_PANEL 0x1001u
#define C_TEXT 0x2002u
#define C_DIS 0x3003u
#define C_CTRL 0x4004u
#define C_HOVER 0x5005u
#define C_ACT 0x6006u
#define C_ACC 0x7007u
#define C_BORDER 0x8008u

static uint16_t g_buf[64 * 48];
static PIXELICA_UI_Surface g_surf;
static PIXELICA_UI_Widget g_w[4];
static PIXELICA_UI_Context g_ui;

static PIXELICA_UI_Widget make(PIXELICA_UI_Kind kind, uint32_t id, int32_t x, int32_t y,
    int32_t w, int32_t h, const char *text)
{
    PIXELICA_UI_Widget wd;

    memset(&wd, 0, sizeof wd);
    wd.kind = kind;
    wd.id = id;
    wd.rect.x = x;
    wd.rect.y = y;
    wd.rect.width = w;
    wd.rect.height = h;
    wd.text = text;
    wd.text_bytes = text != NULL ? strlen(text) : 0u;
    wd.visible = wd.enabled = true;
    wd.min = 0.0f;
    wd.max = 10.0f;
    wd.step = 1.0f;
    return wd;
}

static void reset(uint16_t w, uint16_t h, uint32_t stride)
{
    size_t i;

    for (i = 0; i < sizeof g_buf / sizeof g_buf[0]; ++i) g_buf[i] = (uint16_t)SENT;
    g_surf.pixels = g_buf;
    g_surf.width = w;
    g_surf.height = h;
    g_surf.stride = stride;
}

static void begin(size_t count)
{
    CHECK(pixelica_ui_init(&g_ui, g_w, count) == PIXELICA_OK);
    g_ui.theme.panel = C_PANEL;
    g_ui.theme.text = C_TEXT;
    g_ui.theme.disabled_text = C_DIS;
    g_ui.theme.control = C_CTRL;
    g_ui.theme.hover = C_HOVER;
    g_ui.theme.active = C_ACT;
    g_ui.theme.accent = C_ACC;
    g_ui.theme.border = C_BORDER;
    g_ui.theme.padding = 2u;
    g_ui.theme.font = pixelica_ui_font_default();
}

static uint16_t at(int x, int y) { return g_buf[(size_t)y * g_surf.stride + (size_t)x]; }
static PIXELICA_Status draw(void) { return pixelica_ui_render_rgb565(&g_ui, &g_surf); }

static size_t count(uint16_t color)
{
    size_t i, n = 0;

    for (i = 0; i < sizeof g_buf / sizeof g_buf[0]; ++i) n += g_buf[i] == color;
    return n;
}

static void test_panel_label_clip(void)
{
    size_t y;

    reset(40, 20, 44);
    g_w[0] = make(PIXELICA_UI_PANEL, 1, 2, 3, 20, 10, NULL);
    begin(1);
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(2, 3) == C_BORDER && at(21, 12) == C_BORDER && at(3, 4) == C_PANEL);
    CHECK(at(1, 3) == SENT && at(22, 3) == SENT && at(2, 13) == SENT);
    for (y = 0; y < 20; ++y) CHECK(at(40, (int)y) == SENT && at(43, (int)y) == SENT); /* row padding */

    reset(40, 20, 40); /* label: padding 2, 'A' row 0 = .XXX., row 3 = XXXXX */
    g_w[0] = make(PIXELICA_UI_LABEL, 1, 0, 0, 30, 12, "A");
    begin(1);
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(3, 2) == C_TEXT && at(2, 2) == SENT && at(2, 5) == C_TEXT && at(6, 5) == C_TEXT);
    CHECK(at(7, 5) == SENT && at(3, 3) == SENT); /* no background */
    reset(40, 20, 40);
    g_ui.theme.font.scale = 2u; /* every font pixel becomes 2x2 */
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(4, 2) == C_TEXT && at(5, 3) == C_TEXT && at(3, 2) == SENT && at(2, 2) == SENT);
    CHECK(at(11, 9) == C_TEXT && at(12, 9) == SENT);

    reset(40, 20, 40); /* text is clipped to the widget rect */
    g_w[0] = make(PIXELICA_UI_LABEL, 1, 0, 0, 4, 12, "AAAA");
    g_ui.theme.font.scale = 1u;
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(2, 5) == C_TEXT && at(3, 5) == C_TEXT && at(4, 5) == SENT && at(8, 5) == SENT);

    reset(40, 20, 40); /* partly off-surface, then extreme int32 rects */
    g_w[0] = make(PIXELICA_UI_PANEL, 1, -5, -5, 20, 20, NULL);
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(0, 0) == C_PANEL && at(14, 0) == C_BORDER && at(15, 0) == SENT);
    CHECK(at(0, 14) == C_BORDER && at(0, 15) == SENT);
    reset(40, 20, 40);
    g_w[0] = make(PIXELICA_UI_PANEL, 1, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, "A");
    g_w[1] = make(PIXELICA_UI_PANEL, 2, INT32_MAX - 3, 0, INT32_MAX, 5, "A");
    begin(2);
    CHECK(draw() == PIXELICA_OK);
    CHECK(count(SENT) == sizeof g_buf / sizeof g_buf[0]);
}

static void test_invalid_no_mutation(void)
{
    uint16_t saved[sizeof g_buf / sizeof g_buf[0]];
    PIXELICA_UI_Widget wsaved[4];
    PIXELICA_UI_Surface bad;

    reset(20, 10, 20);
    g_w[0] = make(PIXELICA_UI_PANEL, 1, 0, 0, 10, 10, NULL);
    begin(1);
    g_buf[100] = 0x4141u; /* reads as "AA" */
    memcpy(saved, g_buf, sizeof saved);
    CHECK(pixelica_ui_render_rgb565(NULL, &g_surf) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_ui_render_rgb565(&g_ui, NULL) == PIXELICA_ERR_INVALID_ARG);
    bad = g_surf; bad.width = 0;
    CHECK(pixelica_ui_render_rgb565(&g_ui, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = g_surf; bad.width = 4097;
    CHECK(pixelica_ui_render_rgb565(&g_ui, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = g_surf; bad.height = 0;
    CHECK(pixelica_ui_render_rgb565(&g_ui, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = g_surf; bad.stride = 19;
    CHECK(pixelica_ui_render_rgb565(&g_ui, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = g_surf; bad.pixels = NULL;
    CHECK(pixelica_ui_render_rgb565(&g_ui, &bad) == PIXELICA_ERR_INVALID_ARG);

    g_w[0].id = 0; /* invalid widget */
    CHECK(draw() == PIXELICA_ERR_INVALID_ARG);
    g_w[0].id = 1;
    g_w[0].text = "\xC0\xAF"; /* overlong UTF-8 */
    g_w[0].text_bytes = 2;
    CHECK(draw() == PIXELICA_ERR_INVALID_ARG);
    g_w[0].text = NULL;
    g_w[0].text_bytes = 0;
    g_ui.theme.padding = 17u;
    CHECK(draw() == PIXELICA_ERR_INVALID_ARG);
    g_ui.theme.padding = 2u;
    g_ui.theme.font.glyph = NULL;
    CHECK(draw() == PIXELICA_ERR_INVALID_ARG);
    g_ui.theme.font = pixelica_ui_font_default();
    g_ui.magic = 0;
    CHECK(draw() == PIXELICA_ERR_INVALID_ARG);
    begin(1);

    g_w[0].text = (const char *)(g_buf + 100); /* borrowed text inside the destination */
    g_w[0].text_bytes = 1;
    CHECK(draw() == PIXELICA_ERR_INVALID_ARG);
    g_w[0].text = "A"; /* same widget with disjoint text is fine */
    CHECK(draw() == PIXELICA_OK);
    g_w[0].text = NULL;
    g_w[0].text_bytes = 0;
    CHECK(memcmp(saved, g_buf, 100 * sizeof g_buf[0]) != 0); /* only the valid draw wrote */

    memcpy(wsaved, g_w, sizeof wsaved); /* destination inside the widget array */
    bad.pixels = (uint16_t *)g_w;
    bad.width = 2; bad.height = 1; bad.stride = 2;
    CHECK(pixelica_ui_render_rgb565(&g_ui, &bad) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(wsaved, g_w, sizeof wsaved) == 0);
    CHECK(draw() == PIXELICA_OK);
}

typedef struct Cb {
    uint32_t seen[8];
    int n;
    bool dirty;
} Cb;

static bool cb_glyph(void *userdata, uint32_t cp, uint32_t rows[32])
{
    Cb *cb = (Cb *)userdata;
    int i;

    for (i = 0; i < 32; ++i) cb->dirty = cb->dirty || rows[i] != 0u;
    if (cb->n < 8) cb->seen[cb->n] = cp;
    ++cb->n;
    if (cp == (uint32_t)'A') return false; /* missing glyph */
    rows[0] = 0x80000000u; /* left-most column */
    rows[15] = 0x00010000u; /* column 15 */
    return true;
}

static void test_custom_korean_and_fallback(void)
{
    static const char hangul[] = "\xED\x95\x9C" "A\n" "\xEA\xB8\x80"; /* U+D55C 'A' LF U+AE00 */
    Cb cb;

    memset(&cb, 0, sizeof cb);
    reset(64, 48, 64);
    g_w[0] = make(PIXELICA_UI_LABEL, 1, 0, 0, 60, 40, hangul);
    begin(1);
    g_ui.theme.font.width = 16u;
    g_ui.theme.font.height = 16u;
    g_ui.theme.font.glyph = cb_glyph;
    g_ui.theme.font.userdata = &cb;
    CHECK(draw() == PIXELICA_OK);
    CHECK(cb.n == 3 && cb.seen[0] == 0xD55Cu && cb.seen[1] == (uint32_t)'A' && cb.seen[2] == 0xAE00u);
    CHECK(!cb.dirty); /* rows arrive zeroed */
    CHECK(at(2, 2) == C_TEXT && at(17, 17) == C_TEXT && at(3, 2) == SENT && at(16, 17) == SENT);
    /* 'A' returned false: 16x16 outline at x = 2 + 17 */
    CHECK(at(19, 2) == C_TEXT && at(34, 2) == C_TEXT && at(25, 2) == C_TEXT);
    CHECK(at(19, 17) == C_TEXT && at(34, 17) == C_TEXT && at(19, 9) == C_TEXT && at(34, 9) == C_TEXT);
    CHECK(at(25, 9) == SENT);
    CHECK(at(2, 19) == C_TEXT && at(17, 34) == C_TEXT); /* second line: y = 2 + 17 */

    {
        static const char *const same[] = { "a", "A", "\xC3\xA9", "[", "~", "\xED\x95\x9C", "?" };
        uint16_t shot[sizeof same / sizeof same[0]][30 * 12];
        size_t i, row;

        for (i = 0; i < sizeof same / sizeof same[0]; ++i) {
            reset(30, 12, 30);
            g_w[0] = make(PIXELICA_UI_LABEL, 1, 0, 0, 30, 12, same[i]);
            begin(1);
            CHECK(draw() == PIXELICA_OK);
            for (row = 0; row < 30 * 12; ++row) shot[i][row] = g_buf[row];
        }
        CHECK(memcmp(shot[0], shot[1], sizeof shot[0]) == 0); /* lower case maps to upper */
        CHECK(memcmp(shot[0], shot[6], sizeof shot[0]) != 0);
        for (i = 2; i < 6; ++i) CHECK(memcmp(shot[i], shot[6], sizeof shot[0]) == 0); /* '?' */
    }
}

static void test_widget_states(void)
{
    reset(40, 20, 40);
    g_w[0] = make(PIXELICA_UI_BUTTON, 1, 0, 0, 30, 14, "OK");
    begin(1);
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(1, 1) == C_CTRL && at(0, 0) == C_BORDER && at(10, 3) == C_TEXT);
    reset(40, 20, 40);
    g_ui.events[0].hovered = true;
    CHECK(draw() == PIXELICA_OK && at(1, 1) == C_HOVER);
    reset(40, 20, 40);
    g_ui.events[0].pressed = g_ui.events[0].focused = true;
    CHECK(draw() == PIXELICA_OK && at(1, 1) == C_ACT && at(0, 0) == C_ACC);
    reset(40, 20, 40);
    g_w[0].enabled = false; /* stale events of an ineligible widget are not drawn */
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(1, 1) == C_CTRL && at(0, 0) == C_BORDER && count(C_TEXT) == 0 && count(C_DIS) > 0);

    reset(40, 20, 40); /* checkbox: 8x8 box at (2,2), label after it */
    g_w[0] = make(PIXELICA_UI_CHECKBOX, 1, 0, 0, 40, 12, "X");
    begin(1);
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(2, 2) == C_BORDER && at(3, 3) == C_CTRL && at(5, 5) == C_CTRL && at(12, 3) == C_TEXT);
    reset(40, 20, 40);
    g_w[0].checked = true;
    CHECK(draw() == PIXELICA_OK && at(5, 5) == C_ACC && at(4, 4) == C_ACC && at(3, 3) == C_CTRL);

    reset(40, 20, 40); /* slider: track x 2..37, thumb 4 wide, room 32 */
    g_w[0] = make(PIXELICA_UI_SLIDER, 1, 0, 0, 40, 20, NULL);
    g_w[0].value = 10.0f;
    begin(1);
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(35, 15) == C_ACC && at(10, 17) == C_CTRL && at(10, 15) == SENT && at(3, 15) == SENT);
    reset(40, 20, 40);
    g_w[0].value = 0.0f;
    CHECK(draw() == PIXELICA_OK && at(3, 15) == C_ACC && at(35, 15) == SENT);
    reset(40, 20, 40);
    g_w[0].value = 5.0f;
    CHECK(draw() == PIXELICA_OK && at(19, 15) == C_ACC && at(3, 15) == SENT);

    reset(40, 20, 40); /* progress: bar fills half of the 20 inner pixels */
    g_w[0] = make(PIXELICA_UI_PROGRESS, 1, 0, 0, 22, 8, NULL);
    g_w[0].value = 0.5f;
    begin(1);
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(0, 0) == C_BORDER && at(1, 1) == C_ACT && at(10, 1) == C_ACT && at(11, 1) == C_CTRL);

    reset(40, 20, 40); /* list order wins, invisible widgets draw nothing */
    g_w[0] = make(PIXELICA_UI_PANEL, 1, 0, 0, 10, 10, NULL);
    g_w[1] = make(PIXELICA_UI_BUTTON, 2, 5, 5, 10, 10, NULL);
    g_w[2] = make(PIXELICA_UI_PANEL, 3, 20, 0, 10, 10, NULL);
    g_w[2].visible = false;
    begin(3);
    CHECK(draw() == PIXELICA_OK);
    CHECK(at(6, 6) == C_CTRL && at(1, 1) == C_PANEL && at(25, 5) == SENT);
}

#define PW 8u
#define PH 4u
#define SS 10u
#define TIME_VALUE 123456789012ull

typedef struct Fake {
    int time_calls, read_calls, log_calls, present_calls;
    bool result;
    const uint16_t *pixels;
    uint32_t stride;
    uint16_t copy[PW * PH];
} Fake;

static Fake g_fake;

static uint64_t fake_time(void *ud) { ++((Fake *)ud)->time_calls; return TIME_VALUE; }
static uint32_t fake_read(void *ud) { ++((Fake *)ud)->read_calls; return 5u; }
static void fake_log(void *ud, const char *m) { (void)m; ++((Fake *)ud)->log_calls; }

static bool fake_present(void *ud, const uint16_t *px, uint16_t w, uint16_t h, uint32_t stride)
{
    Fake *f = (Fake *)ud;

    ++f->present_calls;
    f->pixels = px;
    f->stride = stride;
    if (stride == w && (size_t)w * h <= PW * PH) memcpy(f->copy, px, (size_t)w * h * sizeof px[0]);
    return f->result;
}

static void test_presenter(void)
{
    uint16_t src[SS * PH], src0[SS * PH], out[PW * PH];
    PIXELICA_PlatformOps up, bad;
    PIXELICA_UI_Presenter pr, untouched, small;
    size_t i;

    memset(&g_fake, 0, sizeof g_fake);
    g_fake.result = true;
    for (i = 0; i < SS * PH; ++i) src[i] = (uint16_t)(0x100u + i);
    memcpy(src0, src, sizeof src);
    for (i = 0; i < PW * PH; ++i) out[i] = (uint16_t)SENT;
    up.userdata = &g_fake;
    up.time_us = fake_time;
    up.read_buttons = fake_read;
    up.present_rgb565 = fake_present;
    up.log = fake_log;
    g_w[0] = make(PIXELICA_UI_PANEL, 1, 1, 1, 4, 4, NULL);
    begin(1);

    memset(&pr, 0x5A, sizeof pr);
    memcpy(&untouched, &pr, sizeof pr);
    bad = up; bad.time_us = NULL;
    CHECK(pixelica_ui_presenter_init(&pr, &bad, &g_ui, out, PW * PH) == PIXELICA_ERR_INVALID_ARG);
    bad = up; bad.read_buttons = NULL;
    CHECK(pixelica_ui_presenter_init(&pr, &bad, &g_ui, out, PW * PH) == PIXELICA_ERR_INVALID_ARG);
    bad = up; bad.present_rgb565 = NULL;
    CHECK(pixelica_ui_presenter_init(&pr, &bad, &g_ui, out, PW * PH) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_ui_presenter_init(NULL, &up, &g_ui, out, PW * PH) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_ui_presenter_init(&pr, NULL, &g_ui, out, PW * PH) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_ui_presenter_init(&pr, &up, NULL, out, PW * PH) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_ui_presenter_init(&pr, &up, &g_ui, NULL, PW * PH) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_ui_presenter_init(&pr, &up, &g_ui, out, 0) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_ui_presenter_init(&pr, &up, &g_ui, (uint16_t *)g_w, 4) == PIXELICA_ERR_INVALID_ARG);
    g_w[0].id = 0;
    CHECK(pixelica_ui_presenter_init(&pr, &up, &g_ui, out, PW * PH) == PIXELICA_ERR_INVALID_ARG);
    g_w[0].id = 1;
    CHECK(memcmp(&pr, &untouched, sizeof pr) == 0);

    CHECK(pixelica_ui_presenter_init(&pr, &up, &g_ui, out, PW * PH) == PIXELICA_OK);
    CHECK(pr.ops.time_us(pr.ops.userdata) == TIME_VALUE && g_fake.time_calls == 1);
    CHECK(pr.ops.read_buttons(pr.ops.userdata) == 5u && g_fake.read_calls == 1);
    CHECK(pr.ops.log != NULL);
    pr.ops.log(pr.ops.userdata, "hi");
    CHECK(g_fake.log_calls == 1);

    /* source frame has row padding: output is tight and drawn on, source is kept */
    CHECK(pr.ops.present_rgb565(pr.ops.userdata, src, PW, PH, SS));
    CHECK(g_fake.present_calls == 1 && g_fake.pixels == out && g_fake.stride == PW);
    CHECK(g_fake.copy[0] == src0[0] && g_fake.copy[PW + 7] == src0[SS + 7]);
    CHECK(g_fake.copy[PW + 1] == C_BORDER && g_fake.copy[2 * PW + 2] == C_PANEL);
    CHECK(memcmp(src, src0, sizeof src) == 0);

    /* rejected frames: false, no upstream call, output untouched */
    for (i = 0; i < PW * PH; ++i) out[i] = (uint16_t)SENT;
    CHECK(pixelica_ui_presenter_init(&small, &up, &g_ui, out, PW * PH - 1u) == PIXELICA_OK);
    CHECK(!small.ops.present_rgb565(small.ops.userdata, src, PW, PH, SS)); /* capacity */
    g_w[0].id = 0; /* invalid UI */
    CHECK(!pr.ops.present_rgb565(pr.ops.userdata, src, PW, PH, SS));
    g_w[0].id = 1;
    CHECK(!pr.ops.present_rgb565(pr.ops.userdata, src, PW, PH, PW - 1u)); /* stride < width */
    CHECK(!pr.ops.present_rgb565(pr.ops.userdata, src, 0, PH, SS));
    CHECK(!pr.ops.present_rgb565(pr.ops.userdata, out, PW, PH, PW)); /* frame is the output */
    CHECK(!pr.ops.present_rgb565(pr.ops.userdata, NULL, PW, PH, SS));
    CHECK(g_fake.present_calls == 1);
    for (i = 0; i < PW * PH; ++i) CHECK(out[i] == SENT);

    g_fake.result = false; /* upstream failure propagates, called exactly once */
    CHECK(!pr.ops.present_rgb565(pr.ops.userdata, src, PW, PH, SS));
    CHECK(g_fake.present_calls == 2);
}

static void test_builtin_korean(void)
{
    uint32_t rows[32];
    uint32_t x, y;
    reset(64u, 48u, 64u);
    g_w[0] = make(PIXELICA_UI_LABEL, 1u, 0, 0, 64, 48, "\xED\x95\x9C");
    begin(1u);
    g_ui.theme.font = pixelica_font_korean(1u);
    CHECK(draw() == PIXELICA_OK);
    CHECK(g_ui.theme.font.glyph(NULL, 0xD55Cu, rows));
    for (y = 0u; y < 16u; ++y) for (x = 0u; x < 16u; ++x) {
        bool ink = (rows[y] & (UINT32_C(0x80000000) >> x)) != 0u;
        CHECK(at((int)x+2, (int)y+2) == (ink ? C_TEXT : SENT));
    }
}

int main(void)
{
    test_builtin_korean();
    test_panel_label_clip();
    test_invalid_no_mutation();
    test_custom_korean_and_fallback();
    test_widget_states();
    test_presenter();
    return pixelica_t_finish("test_ui_render");
}

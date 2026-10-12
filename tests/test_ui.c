/* Pixelica UI core: validation, UTF-8, aliasing, focus/capture state machine. */
#include <float.h>
#include <stdbool.h>
#include <string.h>

#include "pixelica/ui.h"
#include "test_util.h"

#define K_PREV PIXELICA_UI_PREVIOUS
#define K_NEXT PIXELICA_UI_NEXT
#define K_ACC PIXELICA_UI_ACCEPT

static PIXELICA_UI_Rect rc(int32_t x, int32_t y, int32_t w, int32_t h)
{
    PIXELICA_UI_Rect r;
    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    return r;
}

static PIXELICA_UI_Widget mk(PIXELICA_UI_Kind kind, uint32_t id, PIXELICA_UI_Rect r)
{
    PIXELICA_UI_Widget w;
    memset(&w, 0, sizeof w);
    CHECK(pixelica_ui_widget_default(kind, id, r, "W", 1u, &w) == PIXELICA_OK);
    return w;
}

static PIXELICA_UI_Input ptr(int32_t x, int32_t y, bool down)
{
    PIXELICA_UI_Input in = pixelica_ui_input_buttons(0u);
    in.pointer_x = x;
    in.pointer_y = y;
    in.pointer_valid = true;
    in.pointer_down = down;
    return in;
}

static PIXELICA_UI_Input keys(uint32_t buttons)
{
    PIXELICA_UI_Input in = pixelica_ui_input_buttons(0u);
    in.buttons = buttons;
    return in;
}

static bool upd(PIXELICA_UI_Context *ui, PIXELICA_UI_Input in)
{
    return pixelica_ui_update(ui, &in) == PIXELICA_OK;
}

static void test_defaults_and_utf8(void)
{
    static const char *const bad[] = { "\x80", "\xC0\x80", "\xC1\xBF", "\xE0\x80\x80",
        "\xED\xA0\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
        "\xE2\x82", "\xC3" };
    static char big[PIXELICA_UI_MAX_TEXT_BYTES + 1u];
    const char *good = "A\xC3\xA9\xED\x95\x9C\xF0\x9F\x98\x80"; /* A, e-acute, Hangul, emoji */
    PIXELICA_UI_Theme t = pixelica_ui_theme_default();
    PIXELICA_UI_Widget w;
    PIXELICA_UI_Input in;
    size_t i;

    CHECK(t.padding == 2u && t.font.glyph != NULL);
    CHECK(t.font.width == 5u && t.font.height == 7u && t.font.scale == 1u);
    CHECK(pixelica_ui_widget_default(PIXELICA_UI_SLIDER, 1u, rc(0, 0, 4, 4), good, strlen(good), &w) ==
        PIXELICA_OK);
    CHECK(w.visible && w.enabled && w.min == 0.0f && w.max == 1.0f && w.step > 0.0f && !w.checked);
    CHECK(pixelica_ui_widget_default(PIXELICA_UI_LABEL, 2u, rc(0, 0, 0, 0), NULL, 0u, &w) == PIXELICA_OK);
    for (i = 0u; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(pixelica_ui_widget_default(PIXELICA_UI_LABEL, 3u, rc(0, 0, 1, 1), bad[i], strlen(bad[i]),
            &w) == PIXELICA_ERR_INVALID_ARG);
    }
    CHECK(pixelica_ui_widget_default(PIXELICA_UI_LABEL, 3u, rc(0, 0, 1, 1), NULL, 1u, &w) != PIXELICA_OK);
    CHECK(pixelica_ui_widget_default(PIXELICA_UI_LABEL, 0u, rc(0, 0, 1, 1), NULL, 0u, &w) != PIXELICA_OK);
    CHECK(pixelica_ui_widget_default(PIXELICA_UI_LABEL, 3u, rc(0, 0, -1, 1), NULL, 0u, &w) != PIXELICA_OK);
    CHECK(pixelica_ui_widget_default((PIXELICA_UI_Kind)99, 3u, rc(0, 0, 1, 1), NULL, 0u, &w) != PIXELICA_OK);
    memset(big, 'a', sizeof big);
    CHECK(pixelica_ui_widget_default(PIXELICA_UI_LABEL, 3u, rc(0, 0, 1, 1), big, sizeof big, &w) !=
        PIXELICA_OK);
    CHECK(w.id == 2u && w.kind == PIXELICA_UI_LABEL); /* failures leave out untouched */
    CHECK(pixelica_ui_widget_default(PIXELICA_UI_LABEL, 3u, rc(0, 0, 1, 1), big, sizeof big - 1u, &w) ==
        PIXELICA_OK);

    in = pixelica_ui_input_buttons(PIXELICA_BTN_UP | PIXELICA_BTN_DOWN | PIXELICA_BTN_LEFT |
        PIXELICA_BTN_RIGHT | PIXELICA_BTN_START | PIXELICA_BTN_QUIT);
    CHECK(in.buttons == (K_PREV | K_NEXT | PIXELICA_UI_LEFT | PIXELICA_UI_RIGHT | K_ACC));
    CHECK(!in.pointer_valid && !in.pointer_down);
    CHECK(pixelica_ui_input_buttons(PIXELICA_BTN_A).buttons == K_ACC);
}

static void test_validation_and_aliases(void)
{
    PIXELICA_UI_Widget w[3], keep[3];
    PIXELICA_UI_Context ui, snap;
    PIXELICA_UI_Input in = keys(0u);

    w[0] = mk(PIXELICA_UI_BUTTON, 1u, rc(0, 0, 10, 10));
    w[1] = mk(PIXELICA_UI_LABEL, 2u, rc(0, 0, 10, 10));
    w[2] = mk(PIXELICA_UI_SLIDER, 3u, rc(0, 0, 10, 10));
    w[1].value = pixelica_t_nan(); /* irrelevant for labels */
    memset(&ui, 0xAB, sizeof ui);
    CHECK(pixelica_ui_init(&ui, w, 0u) != PIXELICA_OK);    /* non-NULL needs count */
    CHECK(pixelica_ui_init(&ui, NULL, 1u) != PIXELICA_OK);
    CHECK(pixelica_ui_init(&ui, w, PIXELICA_UI_MAX_WIDGETS + 1u) != PIXELICA_OK);
    w[2].id = 1u;
    CHECK(pixelica_ui_init(&ui, w, 3u) != PIXELICA_OK); /* duplicate id */
    w[2].id = 3u;
    w[2].min = 1.0f;
    CHECK(pixelica_ui_init(&ui, w, 3u) != PIXELICA_OK); /* min == max */
    w[2].min = 0.0f;
    w[2].step = 0.0f;
    CHECK(pixelica_ui_init(&ui, w, 3u) != PIXELICA_OK);
    w[2].step = 0.1f;
    w[2].value = pixelica_t_nan();
    CHECK(pixelica_ui_init(&ui, w, 3u) != PIXELICA_OK);
    w[2].value = 0.0f;
    w[1].kind = PIXELICA_UI_PROGRESS;
    CHECK(pixelica_ui_init(&ui, w, 3u) != PIXELICA_OK); /* progress NaN */
    w[1].kind = PIXELICA_UI_LABEL;
    CHECK(pixelica_ui_init(&ui, (PIXELICA_UI_Widget *)(void *)&ui, 1u) != PIXELICA_OK);
    memset(&ui, 'A', sizeof ui); /* valid UTF-8 bytes, but text aliases ctx */
    memcpy(&snap, &ui, sizeof ui);
    w[1].text = (const char *)&ui;
    w[1].text_bytes = 8u;
    CHECK(pixelica_ui_init(&ui, w, 3u) != PIXELICA_OK);
    w[0].rect = rc(65, 65, 65, 65); /* text aliasing the widget array */
    w[1].text = (const char *)&w[0].rect;
    w[1].text_bytes = sizeof w[0].rect;
    CHECK(pixelica_ui_init(&ui, w, 3u) != PIXELICA_OK);
    CHECK(memcmp(&ui, &snap, sizeof ui) == 0);
    w[0].rect = rc(0, 0, 10, 10);
    w[1].text = "ok";
    w[1].text_bytes = 2u;
    CHECK(pixelica_ui_init(&ui, w, 3u) == PIXELICA_OK);
    CHECK(ui.focus_id == 1u && ui.active_id == 0u && !ui.previous.pointer_valid);

    /* set_widgets / update failures preserve everything. */
    CHECK(upd(&ui, keys(K_NEXT)));
    memcpy(&snap, &ui, sizeof ui);
    memcpy(keep, w, sizeof w);
    w[2].value = 2.0f;
    CHECK(pixelica_ui_set_widgets(&ui, w, 3u) != PIXELICA_OK);
    CHECK(!upd(&ui, keys(0u)));
    w[2].value = 0.0f;
    CHECK(!upd(&ui, keys(0x40u)));             /* outside PIXELICA_UI_BUTTONS */
    CHECK(pixelica_ui_update(&ui, &ui.previous) != PIXELICA_OK); /* input aliases ctx */
    CHECK(pixelica_ui_update(&ui, NULL) != PIXELICA_OK);
    ui.theme.padding = 17u;
    CHECK(!upd(&ui, in));
    ui.theme.padding = 2u;
    CHECK(memcmp(&ui, &snap, sizeof ui) == 0 && memcmp(w, keep, sizeof w) == 0);
    ui.theme.font.glyph = NULL;
    CHECK(pixelica_ui_set_widgets(&ui, w, 1u) != PIXELICA_OK && ui.count == 3u);
}

static void test_pointer(void)
{
    PIXELICA_UI_Widget w[3];
    PIXELICA_UI_Context ui;
    PIXELICA_UI_Input in;

    w[0] = mk(PIXELICA_UI_BUTTON, 1u, rc(0, 0, 10, 10));
    w[1] = mk(PIXELICA_UI_PANEL, 2u, rc(0, 0, 40, 40));   /* input transparent */
    w[2] = mk(PIXELICA_UI_CHECKBOX, 3u, rc(5, 0, 10, 10)); /* overlaps, topmost */
    CHECK(pixelica_ui_init(&ui, w, 3u) == PIXELICA_OK);
    CHECK(upd(&ui, ptr(12, 5, false)));
    CHECK(pixelica_ui_event(&ui, 3u).hovered && ui.focus_id == 1u && ui.wants_pointer); /* no steal */
    CHECK(upd(&ui, ptr(20, 20, false)) && !ui.wants_pointer); /* over panel only */
    CHECK(upd(&ui, ptr(7, 5, true)));
    CHECK(ui.active_id == 3u && ui.focus_id == 3u && pixelica_ui_event(&ui, 3u).pressed);
    CHECK(!pixelica_ui_event(&ui, 1u).hovered);
    CHECK(upd(&ui, ptr(30, 30, true)) && ui.active_id == 3u && ui.wants_pointer);
    CHECK(!pixelica_ui_event(&ui, 3u).pressed);
    CHECK(upd(&ui, ptr(30, 30, false)) && ui.active_id == 0u && !w[2].checked); /* release outside */
    CHECK(!pixelica_ui_event(&ui, 3u).clicked);
    CHECK(upd(&ui, ptr(2, 2, true)) && ui.active_id == 1u);
    CHECK(upd(&ui, ptr(3, 3, false)) && pixelica_ui_event(&ui, 1u).clicked);
    CHECK(upd(&ui, ptr(3, 3, false)) && !pixelica_ui_event(&ui, 1u).clicked); /* once */
    CHECK(upd(&ui, ptr(20, 20, true)) && ui.active_id == 0u); /* press on nothing */
    CHECK(upd(&ui, ptr(2, 2, true)) && ui.active_id == 0u);   /* drag in: no press */
    in = ptr(2, 2, true);
    in.pointer_valid = false;
    CHECK(upd(&ui, in) && !ui.previous.pointer_down); /* invalid pointer neutralized */
}

static void test_keyboard(void)
{
    PIXELICA_UI_Widget w[4];
    PIXELICA_UI_Context ui;
    PIXELICA_UI_Input in;

    w[0] = mk(PIXELICA_UI_BUTTON, 1u, rc(0, 0, 10, 10));
    w[1] = mk(PIXELICA_UI_LABEL, 2u, rc(0, 10, 10, 10));
    w[2] = mk(PIXELICA_UI_CHECKBOX, 3u, rc(0, 20, 10, 10));
    w[3] = mk(PIXELICA_UI_SLIDER, 4u, rc(0, 30, 10, 10));
    w[3].step = 0.25f;
    CHECK(pixelica_ui_init(&ui, w, 4u) == PIXELICA_OK && ui.focus_id == 1u);
    CHECK(upd(&ui, keys(K_NEXT)) && ui.focus_id == 3u && ui.wants_buttons); /* skips label */
    CHECK(upd(&ui, keys(K_NEXT)) && ui.focus_id == 3u);                     /* held: no repeat */
    CHECK(upd(&ui, keys(0u)) && !ui.wants_buttons);
    CHECK(upd(&ui, keys(K_ACC)) && w[2].checked && pixelica_ui_event(&ui, 3u).changed);
    CHECK(pixelica_ui_event(&ui, 3u).pressed && pixelica_ui_event(&ui, 3u).clicked);
    CHECK(upd(&ui, keys(K_ACC)) && w[2].checked && pixelica_ui_event(&ui, 3u).pressed);
    CHECK(!pixelica_ui_event(&ui, 3u).clicked);
    CHECK(upd(&ui, keys(0u)) && upd(&ui, keys(K_NEXT)) && ui.focus_id == 4u);
    CHECK(upd(&ui, keys(PIXELICA_UI_RIGHT)) && w[3].value == 0.25f && pixelica_ui_event(&ui, 4u).changed);
    CHECK(upd(&ui, keys(PIXELICA_UI_RIGHT)) && w[3].value == 0.25f); /* no auto-repeat */
    CHECK(upd(&ui, keys(K_ACC)) && w[3].value == 0.25f && !pixelica_ui_event(&ui, 4u).clicked);
    CHECK(!pixelica_ui_event(&ui, 4u).pressed);
    CHECK(upd(&ui, keys(PIXELICA_UI_LEFT)) && w[3].value == 0.0f);
    CHECK(upd(&ui, keys(0u)) && upd(&ui, keys(PIXELICA_UI_LEFT)) && w[3].value == 0.0f);
    CHECK(!pixelica_ui_event(&ui, 4u).changed); /* clamped, unchanged */
    CHECK(upd(&ui, keys(K_NEXT)) && ui.focus_id == 1u); /* wrap */
    CHECK(upd(&ui, keys(K_PREV)) && ui.focus_id == 4u);
    /* Navigation ignored during pointer capture. */
    CHECK(upd(&ui, ptr(5, 5, true)) && ui.active_id == 1u);
    in = ptr(5, 5, true);
    in.buttons = K_NEXT;
    CHECK(upd(&ui, in) && ui.focus_id == 1u && ui.active_id == 1u);
    CHECK(upd(&ui, ptr(5, 5, false)) && pixelica_ui_event(&ui, 1u).clicked);
}

static void test_slider_math(void)
{
    PIXELICA_UI_Widget w[1];
    PIXELICA_UI_Context ui;

    w[0] = mk(PIXELICA_UI_SLIDER, 1u, rc(10, 0, 101, 4));
    w[0].min = -FLT_MAX;
    w[0].max = FLT_MAX;
    w[0].step = FLT_MAX;
    CHECK(pixelica_ui_init(&ui, w, 1u) == PIXELICA_OK);
    CHECK(upd(&ui, ptr(85, 2, true)) && w[0].value == FLT_MAX * 0.5f && pixelica_ui_event(&ui, 1u).changed);
    CHECK(upd(&ui, ptr(500, 90, true)) && w[0].value == FLT_MAX); /* drag anywhere, clamped */
    CHECK(upd(&ui, ptr(-500, 90, false)) && w[0].value == -FLT_MAX && ui.active_id == 0u);
    CHECK(upd(&ui, keys(PIXELICA_UI_RIGHT)) && w[0].value == 0.0f);
    CHECK(upd(&ui, keys(0u)) && upd(&ui, keys(PIXELICA_UI_RIGHT)) && w[0].value == FLT_MAX);
    w[0].min = 0.0f;
    w[0].max = 1.0f;
    w[0].value = 0.5f;
    CHECK(upd(&ui, ptr(35, 2, true)) && fabsf(w[0].value - 0.25f) < 1e-6f); /* continuous */
    CHECK(upd(&ui, ptr(35, 2, false)));
    w[0].rect.width = 1; /* width 1 maps to min */
    CHECK(upd(&ui, ptr(10, 2, true)) && w[0].value == 0.0f && upd(&ui, ptr(10, 2, false)));
}

static void test_hidden_cancel_and_menus(void)
{
    PIXELICA_UI_Widget w[2], menu[1];
    PIXELICA_UI_Context ui;
    PIXELICA_UI_Input in;

    w[0] = mk(PIXELICA_UI_CHECKBOX, 1u, rc(0, 0, 10, 10));
    w[1] = mk(PIXELICA_UI_BUTTON, 2u, rc(20, 0, 10, 10));
    CHECK(pixelica_ui_init(&ui, w, 2u) == PIXELICA_OK);
    /* Hidden capture cancels without click; focus repairs; stale events neutral. */
    CHECK(upd(&ui, ptr(5, 5, true)) && ui.active_id == 1u);
    w[0].visible = false;
    CHECK(!pixelica_ui_event(&ui, 1u).pressed);
    CHECK(upd(&ui, ptr(5, 5, false)) && ui.active_id == 0u && !w[0].checked && ui.focus_id == 2u);
    w[0].visible = true;
    w[0].enabled = false;
    CHECK(upd(&ui, ptr(5, 5, true)) && ui.active_id == 0u && !ui.wants_pointer);
    CHECK(upd(&ui, keys(K_NEXT)) && ui.focus_id == 2u); /* disabled skipped */
    w[0].enabled = true;
    /* Pointer loss and CANCEL drop the capture. */
    CHECK(upd(&ui, ptr(5, 5, true)) && ui.active_id == 1u);
    in = ptr(5, 5, false);
    in.pointer_valid = false;
    CHECK(upd(&ui, in) && ui.active_id == 0u && !w[0].checked);
    CHECK(upd(&ui, ptr(5, 5, true)) && ui.active_id == 1u);
    in = ptr(5, 5, true);
    in.buttons = PIXELICA_UI_CANCEL;
    CHECK(upd(&ui, in) && ui.active_id == 0u);
    in.pointer_down = false;
    CHECK(upd(&ui, in) && !w[0].checked && !pixelica_ui_event(&ui, 1u).clicked);
    CHECK(ui.focus_id == 1u && upd(&ui, keys(0u)));
    CHECK(upd(&ui, keys(K_ACC | PIXELICA_UI_CANCEL)) && !w[0].checked); /* suppressed */
    CHECK(!pixelica_ui_event(&ui, 1u).pressed);
    CHECK(upd(&ui, keys(K_ACC)) && !pixelica_ui_event(&ui, 1u).pressed);
    CHECK(upd(&ui, keys(0u)));

    /* Cancel also releases a keyboard press already held across frames. */
    CHECK(upd(&ui, keys(K_ACC)) && pixelica_ui_event(&ui, 1u).pressed);
    CHECK(upd(&ui, keys(K_ACC | PIXELICA_UI_CANCEL)) && w[0].checked);
    CHECK(!pixelica_ui_event(&ui, 1u).pressed && !pixelica_ui_event(&ui, 1u).clicked);
    CHECK(upd(&ui, keys(K_ACC)) && !pixelica_ui_event(&ui, 1u).pressed);
    CHECK(upd(&ui, keys(0u)));
    w[0].checked = false;

    /* Pointer release + keyboard accept in one update toggles once. */
    CHECK(upd(&ui, ptr(5, 5, true)));
    in = ptr(5, 5, false);
    in.buttons = K_ACC;
    CHECK(upd(&ui, in) && w[0].checked && pixelica_ui_event(&ui, 1u).clicked);
    in.pointer_down = true; /* press while ACCEPT held: still pointer only */
    CHECK(upd(&ui, in) && w[0].checked);
    in.pointer_down = false;
    CHECK(upd(&ui, in) && !w[0].checked);

    /* A held ACCEPT never activates a freshly opened menu. */
    menu[0] = mk(PIXELICA_UI_BUTTON, 9u, rc(0, 0, 10, 10));
    CHECK(upd(&ui, keys(K_ACC)));
    CHECK(pixelica_ui_set_widgets(&ui, menu, 1u) == PIXELICA_OK && ui.focus_id == 9u);
    CHECK(ui.previous.buttons == K_ACC && !pixelica_ui_event(&ui, 9u).focused);
    CHECK(upd(&ui, keys(K_ACC)) && !pixelica_ui_event(&ui, 9u).clicked);
    CHECK(!pixelica_ui_event(&ui, 9u).pressed && ui.wants_buttons);
    CHECK(upd(&ui, keys(0u)) && upd(&ui, keys(K_ACC)) && pixelica_ui_event(&ui, 9u).clicked);
    CHECK(pixelica_ui_set_widgets(&ui, NULL, 0u) == PIXELICA_OK && ui.focus_id == 0u);
    CHECK(upd(&ui, keys(K_NEXT)) && !ui.wants_buttons && !ui.wants_pointer);
}

static void test_layout(void)
{
    PIXELICA_UI_Layout l, keep;
    PIXELICA_UI_Rect r;

    CHECK(pixelica_ui_layout_init(&l, rc(5, 7, 20, 30), -1, false) != PIXELICA_OK);
    CHECK(pixelica_ui_layout_init(&l, rc(5, 7, 20, 30), 2, false) == PIXELICA_OK && l.cursor == 0);
    CHECK(pixelica_ui_layout_next(&l, 10, &r) == PIXELICA_OK);
    CHECK(r.x == 5 && r.y == 7 && r.width == 20 && r.height == 10 && l.cursor == 12);
    CHECK(pixelica_ui_layout_next(&l, 0, &r) != PIXELICA_OK);
    memcpy(&keep, &l, sizeof l);
    CHECK(pixelica_ui_layout_next(&l, 19, &r) == PIXELICA_ERR_CAPACITY);
    CHECK(memcmp(&l, &keep, sizeof l) == 0 && r.y == 7);
    CHECK(pixelica_ui_layout_next(&l, 18, &r) == PIXELICA_OK && r.y == 19 && l.cursor == 30);
    CHECK(pixelica_ui_layout_next(&l, 1, &r) == PIXELICA_ERR_CAPACITY);
    CHECK(pixelica_ui_layout_next(&l, 1, (PIXELICA_UI_Rect *)(void *)&l) != PIXELICA_OK);
    CHECK(pixelica_ui_layout_init(&l, rc(INT32_MAX - 5, 0, 100, 4), INT32_MAX, true) == PIXELICA_OK);
    CHECK(pixelica_ui_layout_next(&l, 5, &r) == PIXELICA_OK && r.x == INT32_MAX - 5 && l.cursor == 100);
    l.cursor = 10;
    CHECK(pixelica_ui_layout_next(&l, 5, &r) == PIXELICA_ERR_INVALID_ARG); /* x overflows int32 */
}

int main(void)
{
    test_defaults_and_utf8();
    test_validation_and_aliases();
    test_pointer();
    test_keyboard();
    test_slider_math();
    test_hidden_cancel_and_menus();
    test_layout();
    return pixelica_t_finish("test_ui");
}

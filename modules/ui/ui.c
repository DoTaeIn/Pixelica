/*
 * Pixelica - built-in UI core: defaults, strict UTF-8, transactional
 * validation, layout and the pointer/keyboard focus-capture state machine.
 * See pixelica/ui.h for the contract. No heap, no global mutable state, no
 * renderer: drawing lives in ui_rgb565.c / ui_font.c / ui_presenter.c, which
 * share the private interface in ui_internal.h.
 */
#include "pixelica/ui.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/image.h"
#include "pixelica/input.h"
#include "ui_internal.h"

/* ---- address ranges ---------------------------------------------------- */

static bool ui_range_ok(const void *p, size_t bytes)
{
    return bytes <= UINTPTR_MAX - (uintptr_t)p;
}

/* Both ranges must already pass ui_range_ok. Empty ranges never overlap. */
static bool ui_overlap(const void *a, size_t a_bytes, const void *b, size_t b_bytes)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;

    if (a_bytes == 0u || b_bytes == 0u) {
        return false;
    }
    return x < y + b_bytes && y < x + a_bytes;
}

/* ---- strict UTF-8 ------------------------------------------------------ */

bool pixelica_ui_utf8_next(const char *text, size_t length, size_t *offset,
    uint32_t *codepoint)
{
    return pixelica_utf8_next(text, length, offset, codepoint);
}

/* ---- validation -------------------------------------------------------- */

static bool ui_text_valid(const char *text, size_t bytes)
{
    size_t offset = 0u;
    uint32_t cp;

    if (bytes > PIXELICA_UI_MAX_TEXT_BYTES || (text == NULL && bytes != 0u) ||
        !ui_range_ok(text, bytes)) {
        return false;
    }
    while (offset < bytes) {
        if (!pixelica_ui_utf8_next(text, bytes, &offset, &cp)) {
            return false;
        }
    }
    return true;
}

static bool ui_widget_valid(const PIXELICA_UI_Widget *w)
{
    if ((int)w->kind < (int)PIXELICA_UI_PANEL || (int)w->kind > (int)PIXELICA_UI_PROGRESS ||
        w->id == 0u || w->rect.width < 0 || w->rect.height < 0 ||
        !ui_text_valid(w->text, w->text_bytes)) {
        return false;
    }
    if (w->kind == PIXELICA_UI_SLIDER) {
        return isfinite(w->min) && isfinite(w->max) && w->min < w->max &&
            isfinite(w->step) && w->step > 0.0f && isfinite(w->value) &&
            w->value >= w->min && w->value <= w->max;
    }
    if (w->kind == PIXELICA_UI_PROGRESS) {
        return isfinite(w->value) && w->value >= 0.0f && w->value <= 1.0f;
    }
    return true; /* irrelevant fields (even NaN) are ignored */
}

static bool ui_theme_valid(const PIXELICA_UI_Theme *t)
{
    const PIXELICA_UI_Font *f = &t->font;

    return t->padding <= 16u && f->width >= 1u && f->width <= 16u && f->height >= 1u &&
        f->height <= 32u && f->scale >= 1u && f->scale <= 4u && f->glyph != NULL;
}

/* Everything a context at address `ctx` would hold, without touching it. */
static PIXELICA_Status ui_check(const void *ctx, const PIXELICA_UI_Widget *widgets,
    size_t count, const PIXELICA_UI_Theme *theme)
{
    const size_t ctx_bytes = sizeof(PIXELICA_UI_Context);
    size_t array_bytes, i, j;

    if (ctx == NULL || count > PIXELICA_UI_MAX_WIDGETS || (widgets == NULL) != (count == 0u) ||
        !ui_range_ok(ctx, ctx_bytes) || !ui_theme_valid(theme)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    array_bytes = count * sizeof(PIXELICA_UI_Widget);
    if (!ui_range_ok(widgets, array_bytes) ||
        ui_overlap(ctx, ctx_bytes, widgets, array_bytes)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0u; i < count; ++i) {
        const PIXELICA_UI_Widget *w = &widgets[i];
        if (!ui_widget_valid(w) || ui_overlap(w->text, w->text_bytes, ctx, ctx_bytes) ||
            ui_overlap(w->text, w->text_bytes, widgets, array_bytes)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        for (j = 0u; j < i; ++j) {
            if (widgets[j].id == w->id) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_ui_validate(const PIXELICA_UI_Context *ui)
{
    if (ui == NULL || ui->magic != PIXELICA_UI_MAGIC) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return ui_check(ui, ui->widgets, ui->count, &ui->theme);
}

/* ---- defaults ---------------------------------------------------------- */

PIXELICA_UI_Theme pixelica_ui_theme_default(void)
{
    PIXELICA_UI_Theme t;

    memset(&t, 0, sizeof t);
    t.panel = PIXELICA_RGB565(24, 28, 44);
    t.text = PIXELICA_RGB565(232, 236, 240);
    t.disabled_text = PIXELICA_RGB565(112, 116, 128);
    t.control = PIXELICA_RGB565(52, 60, 88);
    t.hover = PIXELICA_RGB565(76, 88, 128);
    t.active = PIXELICA_RGB565(32, 120, 96);
    t.accent = PIXELICA_RGB565(248, 200, 64);
    t.border = PIXELICA_RGB565(136, 148, 184);
    t.padding = 2u;
    t.font = pixelica_ui_font_default();
    return t;
}

PIXELICA_Status pixelica_ui_widget_default(PIXELICA_UI_Kind kind, uint32_t id,
    PIXELICA_UI_Rect rect, const char *text, size_t text_bytes, PIXELICA_UI_Widget *out)
{
    PIXELICA_UI_Widget w;

    memset(&w, 0, sizeof w);
    w.kind = kind;
    w.id = id;
    w.rect = rect;
    w.text = text;
    w.text_bytes = text_bytes;
    w.visible = true;
    w.enabled = true;
    w.max = 1.0f;
    w.step = 0.1f;
    if (out == NULL || !ui_range_ok(out, sizeof *out) || !ui_widget_valid(&w) ||
        ui_overlap(text, text_bytes, out, sizeof *out)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = w;
    return PIXELICA_OK;
}

PIXELICA_UI_Input pixelica_ui_input_buttons(uint32_t logical_buttons)
{
    PIXELICA_UI_Input in;
    uint32_t b = 0u;

    memset(&in, 0, sizeof in);
    if ((logical_buttons & PIXELICA_BTN_UP) != 0u) b |= PIXELICA_UI_PREVIOUS;
    if ((logical_buttons & PIXELICA_BTN_DOWN) != 0u) b |= PIXELICA_UI_NEXT;
    if ((logical_buttons & PIXELICA_BTN_LEFT) != 0u) b |= PIXELICA_UI_LEFT;
    if ((logical_buttons & PIXELICA_BTN_RIGHT) != 0u) b |= PIXELICA_UI_RIGHT;
    if ((logical_buttons & (PIXELICA_BTN_A | PIXELICA_BTN_START)) != 0u) b |= PIXELICA_UI_ACCEPT;
    in.buttons = b;
    return in;
}

/* ---- widget queries ---------------------------------------------------- */

static bool ui_interactive(const PIXELICA_UI_Widget *w)
{
    return (w->kind == PIXELICA_UI_BUTTON || w->kind == PIXELICA_UI_CHECKBOX ||
               w->kind == PIXELICA_UI_SLIDER) &&
        w->visible && w->enabled && w->rect.width > 0 && w->rect.height > 0;
}

/* Index of `id`, or count when absent (ids are nonzero, so 0 never matches). */
static size_t ui_find(const PIXELICA_UI_Context *ui, uint32_t id)
{
    size_t i;

    for (i = 0u; i < ui->count; ++i) {
        if (ui->widgets[i].id == id) {
            break;
        }
    }
    return i;
}

/* Next (dir > 0) or previous eligible control after index `from`, wrapping and
 * ending on `from` itself; from >= count starts at the list edge. 0 if none. */
static uint32_t ui_focus_step(const PIXELICA_UI_Context *ui, size_t from, int dir)
{
    size_t n = ui->count, k, i;

    for (k = 1u; k <= n; ++k) {
        if (from >= n) {
            i = dir > 0 ? k - 1u : n - k;
        } else {
            i = dir > 0 ? (from + k) % n : (from + n - k) % n;
        }
        if (ui_interactive(&ui->widgets[i])) {
            return ui->widgets[i].id;
        }
    }
    return 0u;
}

static bool ui_hit(const PIXELICA_UI_Rect *r, int32_t x, int32_t y)
{
    return (int64_t)x >= r->x && (int64_t)x < (int64_t)r->x + r->width &&
        (int64_t)y >= r->y && (int64_t)y < (int64_t)r->y + r->height;
}

/* Topmost (last in list) eligible control under the pointer, or count. */
static size_t ui_topmost(const PIXELICA_UI_Context *ui, const PIXELICA_UI_Input *in)
{
    size_t i = ui->count;

    if (!in->pointer_valid) {
        return ui->count;
    }
    while (i-- > 0u) {
        const PIXELICA_UI_Widget *w = &ui->widgets[i];
        if (ui_interactive(w) && ui_hit(&w->rect, in->pointer_x, in->pointer_y)) {
            return i;
        }
    }
    return ui->count;
}

/* Double math keeps max-min and value+step finite for any finite floats. */
static float ui_slider_clamp(const PIXELICA_UI_Widget *w, double v)
{
    if (!(v > (double)w->min)) {
        return w->min;
    }
    if (v >= (double)w->max) {
        return w->max;
    }
    return (float)v;
}

/* Continuous (not snapped) mapping of x over the full rect width. */
static float ui_slider_at(const PIXELICA_UI_Widget *w, int32_t x)
{
    double t;

    if (w->rect.width <= 1) {
        return w->min;
    }
    t = ((double)x - (double)w->rect.x) / ((double)w->rect.width - 1.0);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return ui_slider_clamp(w, (double)w->min + ((double)w->max - (double)w->min) * t);
}

/* ---- context ----------------------------------------------------------- */

static void ui_reset(PIXELICA_UI_Context *ui)
{
    memset(ui->events, 0, sizeof ui->events);
    ui->active_id = 0u;
    ui->wants_pointer = false;
    ui->wants_buttons = false;
    ui->focus_id = ui_focus_step(ui, ui->count, 1);
}

PIXELICA_Status pixelica_ui_init(PIXELICA_UI_Context *out,
    PIXELICA_UI_Widget *widgets, size_t count)
{
    PIXELICA_UI_Theme theme;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    theme = pixelica_ui_theme_default();
    if (ui_check(out, widgets, count, &theme) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    out->widgets = widgets;
    out->count = count;
    out->theme = theme;
    out->magic = PIXELICA_UI_MAGIC;
    ui_reset(out);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_ui_set_widgets(PIXELICA_UI_Context *ui,
    PIXELICA_UI_Widget *widgets, size_t count)
{
    if (ui == NULL || ui->magic != PIXELICA_UI_MAGIC ||
        ui_check(ui, widgets, count, &ui->theme) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    ui->widgets = widgets;
    ui->count = count;
    ui_reset(ui); /* previous input kept: a held ACCEPT cannot hit the new menu */
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_ui_update(PIXELICA_UI_Context *ui, const PIXELICA_UI_Input *input)
{
    PIXELICA_UI_Input in;
    PIXELICA_UI_Widget *w;
    uint32_t rising, focus, active;
    size_t n, hit, fi, ai, i, target;
    bool cancel, pointer_used = false, clicked = false, changed = false;
    bool held_before, key_pressed;

    if (pixelica_ui_validate(ui) != PIXELICA_OK || input == NULL ||
        (input->buttons & ~PIXELICA_UI_BUTTONS) != 0u || !ui_range_ok(input, sizeof *input) ||
        ui_overlap(input, sizeof *input, ui, sizeof *ui) ||
        ui_overlap(input, sizeof *input, ui->widgets, ui->count * sizeof *ui->widgets)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    in = *input;
    if (!in.pointer_valid) {
        in.pointer_down = false;
    }
    n = ui->count;
    target = n;
    rising = in.buttons & ~ui->previous.buttons;
    cancel = (rising & PIXELICA_UI_CANCEL) != 0u;
    hit = ui_topmost(ui, &in);

    /* Repair focus after hide/disable/delete; keep the held-ACCEPT look only
     * if this widget already showed it (so a new menu never looks pressed). */
    focus = ui->focus_id;
    fi = ui_find(ui, focus);
    if (fi >= n || !ui_interactive(&ui->widgets[fi])) {
        focus = ui_focus_step(ui, fi, 1);
        fi = ui_find(ui, focus);
    }
    held_before = focus == ui->focus_id && fi < n && ui->events[fi].pressed &&
        ui->events[fi].focused;

    /* Pointer loss, CANCEL or an ineligible/deleted target drops the capture. */
    active = ui->active_id;
    ai = ui_find(ui, active);
    if (active != 0u && (ai >= n || !ui_interactive(&ui->widgets[ai]) || !in.pointer_valid || cancel)) {
        active = 0u;
    }

    if (active != 0u) {
        w = &ui->widgets[ai];
        pointer_used = true;
        if (w->kind == PIXELICA_UI_SLIDER) {
            float v = ui_slider_at(w, in.pointer_x);
            if (v != w->value) {
                w->value = v;
                target = ai;
                changed = true;
            }
        }
        if (!in.pointer_down) {
            if (w->kind != PIXELICA_UI_SLIDER && hit == ai) {
                target = ai;
                clicked = true;
                if (w->kind == PIXELICA_UI_CHECKBOX) {
                    w->checked = !w->checked;
                    changed = true;
                }
            }
            active = 0u;
        }
    } else if (!cancel && in.pointer_down && !ui->previous.pointer_down && hit < n) {
        w = &ui->widgets[hit];
        pointer_used = true;
        active = w->id;
        focus = w->id;
        fi = hit;
        if (w->kind == PIXELICA_UI_SLIDER) {
            float v = ui_slider_at(w, in.pointer_x);
            if (v != w->value) {
                w->value = v;
                target = hit;
                changed = true;
            }
        }
    }

    /* Keyboard only when the pointer did nothing: no same-update double toggle. */
    if (!pointer_used) {
        if ((rising & PIXELICA_UI_NEXT) != 0u) {
            focus = ui_focus_step(ui, fi, 1);
            fi = ui_find(ui, focus);
        }
        if ((rising & PIXELICA_UI_PREVIOUS) != 0u) {
            focus = ui_focus_step(ui, fi, -1);
            fi = ui_find(ui, focus);
        }
        if (!cancel && fi < n) {
            w = &ui->widgets[fi];
            if (w->kind != PIXELICA_UI_SLIDER && (rising & PIXELICA_UI_ACCEPT) != 0u) {
                target = fi;
                clicked = true;
                if (w->kind == PIXELICA_UI_CHECKBOX) {
                    w->checked = !w->checked;
                    changed = true;
                }
            } else if (w->kind == PIXELICA_UI_SLIDER) {
                int dir = ((rising & PIXELICA_UI_RIGHT) != 0u ? 1 : 0) -
                    ((rising & PIXELICA_UI_LEFT) != 0u ? 1 : 0);
                if (dir != 0) {
                    float v = ui_slider_clamp(w, (double)w->value + (double)dir * (double)w->step);
                    if (v != w->value) {
                        w->value = v;
                        target = fi;
                        changed = true;
                    }
                }
            }
        }
    }
    key_pressed = !cancel && !pointer_used && fi < n && ui->widgets[fi].kind != PIXELICA_UI_SLIDER &&
        (in.buttons & PIXELICA_UI_ACCEPT) != 0u &&
        (((rising & PIXELICA_UI_ACCEPT) != 0u) ||
            ((rising & PIXELICA_UI_ACCEPT) == 0u && held_before && focus == ui->focus_id));

    memset(ui->events, 0, sizeof ui->events);
    for (i = 0u; i < n; ++i) {
        PIXELICA_UI_Event *e = &ui->events[i];
        w = &ui->widgets[i];
        if (!ui_interactive(w)) {
            continue; /* decorative or ineligible: neutral events */
        }
        e->hovered = i == hit;
        e->focused = w->id == focus;
        e->pressed = (w->id == active && (i == hit || w->kind == PIXELICA_UI_SLIDER)) ||
            (e->focused && key_pressed);
        e->clicked = i == target && clicked;
        e->changed = i == target && changed;
    }
    ui->focus_id = focus;
    ui->active_id = active;
    ui->wants_pointer = hit < n || active != 0u || pointer_used;
    ui->wants_buttons = focus != 0u && (in.buttons & PIXELICA_UI_BUTTONS) != 0u;
    ui->previous = in;
    return PIXELICA_OK;
}

PIXELICA_UI_Event pixelica_ui_event(const PIXELICA_UI_Context *ui, uint32_t id)
{
    PIXELICA_UI_Event none;
    size_t i;

    memset(&none, 0, sizeof none);
    /* Structural check only (cheap per call); stale events of widgets made
     * ineligible since the last update read as neutral. */
    if (ui == NULL || ui->magic != PIXELICA_UI_MAGIC || ui->count > PIXELICA_UI_MAX_WIDGETS ||
        (ui->widgets == NULL) != (ui->count == 0u) || id == 0u) {
        return none;
    }
    i = ui_find(ui, id);
    if (i >= ui->count || !ui_interactive(&ui->widgets[i])) {
        return none;
    }
    return ui->events[i];
}

/* ---- layout ------------------------------------------------------------ */

PIXELICA_Status pixelica_ui_layout_init(PIXELICA_UI_Layout *out,
    PIXELICA_UI_Rect bounds, int32_t gap, bool horizontal)
{
    if (out == NULL || bounds.width < 0 || bounds.height < 0 || gap < 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    out->bounds = bounds;
    out->gap = gap;
    out->cursor = 0;
    out->horizontal = horizontal;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_ui_layout_next(PIXELICA_UI_Layout *layout,
    int32_t extent, PIXELICA_UI_Rect *out)
{
    PIXELICA_UI_Rect r;
    int64_t limit, end, pos;

    if (layout == NULL || out == NULL || extent <= 0 || !ui_range_ok(layout, sizeof *layout) ||
        !ui_range_ok(out, sizeof *out) || ui_overlap(layout, sizeof *layout, out, sizeof *out)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    r = layout->bounds;
    limit = layout->horizontal ? r.width : r.height;
    if (r.width < 0 || r.height < 0 || layout->gap < 0 || layout->cursor < 0 ||
        layout->cursor > limit) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    pos = (int64_t)(layout->horizontal ? r.x : r.y) + layout->cursor;
    if (pos > INT32_MAX) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    end = (int64_t)layout->cursor + extent;
    if (end > limit) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (layout->horizontal) {
        r.x = (int32_t)pos;
        r.width = extent;
    } else {
        r.y = (int32_t)pos;
        r.height = extent;
    }
    end += layout->gap;
    layout->cursor = (int32_t)(end > limit ? limit : end);
    *out = r;
    return PIXELICA_OK;
}

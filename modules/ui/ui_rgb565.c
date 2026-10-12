/*
 * Pixelica UI - standalone RGB565 widget renderer.
 * See modules/ui/include/pixelica/ui.h.
 *
 * The whole UI and the surface are validated before the first pixel write.
 * All geometry is int64 so INT32 rects and positions cannot overflow, every
 * primitive clips to the widget rect and the surface, and nothing allocates.
 */
#include <stdint.h>
#include <string.h>

#include "pixelica/ui.h"
#include "ui_internal.h"

/* Half-open byte range [lo, hi) of one caller object or array. */
typedef struct UiRange {
    uintptr_t lo, hi;
} UiRange;

static bool range_of(const void *p, size_t count, size_t elem, UiRange *out)
{
    uintptr_t lo = (uintptr_t)p;
    size_t bytes;

    if (count != 0u && elem > SIZE_MAX / count) {
        return false;
    }
    bytes = count * elem;
    if ((uintmax_t)bytes > (uintmax_t)(UINTPTR_MAX - lo)) {
        return false;
    }
    out->lo = lo;
    out->hi = lo + (uintptr_t)bytes;
    return true;
}

static bool overlaps(UiRange a, UiRange b)
{
    return a.lo < a.hi && b.lo < b.hi && a.lo < b.hi && b.lo < a.hi;
}

PIXELICA_Status pixelica_ui_validate_surface(const PIXELICA_UI_Context *ui,
    const PIXELICA_UI_Surface *surface)
{
    UiRange pix, desc, ctx, arr, txt;
    PIXELICA_UI_Surface s;
    PIXELICA_Status status;
    size_t rows, span, i;

    if (ui == NULL || surface == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    status = pixelica_ui_validate(ui);
    if (status != PIXELICA_OK) {
        return status;
    }
    s = *surface;
    if (s.pixels == NULL || s.width == 0u || s.height == 0u ||
        (uint32_t)s.width > PIXELICA_UI_MAX_DIMENSION ||
        (uint32_t)s.height > PIXELICA_UI_MAX_DIMENSION || s.stride < (uint32_t)s.width) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    rows = (size_t)s.height - 1u;
    if (rows != 0u && (size_t)s.stride > (SIZE_MAX - (size_t)s.width) / rows) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    span = rows * (size_t)s.stride + (size_t)s.width;
    if (!range_of(s.pixels, span, sizeof(uint16_t), &pix) ||
        !range_of(surface, 1u, sizeof(*surface), &desc) ||
        !range_of(ui, 1u, sizeof(*ui), &ctx) ||
        !range_of(ui->widgets, ui->count, sizeof(ui->widgets[0]), &arr) ||
        overlaps(pix, desc) || overlaps(pix, ctx) || overlaps(pix, arr)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < ui->count; ++i) {
        if (ui->widgets[i].text_bytes != 0u &&
            (!range_of(ui->widgets[i].text, ui->widgets[i].text_bytes, 1u, &txt) ||
             overlaps(pix, txt))) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

/* ---- primitives: every one clips to the canvas clip rectangle ------------- */

typedef struct Canvas {
    uint16_t *pixels;
    size_t stride;
    int64_t cx0, cy0, cx1, cy1; /* current clip: widget rect inside the surface */
    PIXELICA_UI_Font font;
} Canvas;

static void fill(const Canvas *c, int64_t x, int64_t y, int64_t w, int64_t h, uint16_t color)
{
    int64_t x1 = x + w, y1 = y + h, xx, yy;

    x = x < c->cx0 ? c->cx0 : x;
    y = y < c->cy0 ? c->cy0 : y;
    x1 = x1 > c->cx1 ? c->cx1 : x1;
    y1 = y1 > c->cy1 ? c->cy1 : y1;
    if (x >= x1 || y >= y1) {
        return;
    }
    for (yy = y; yy < y1; ++yy) {
        uint16_t *row = c->pixels + (size_t)yy * c->stride;

        for (xx = x; xx < x1; ++xx) {
            row[xx] = color;
        }
    }
}

static void border(const Canvas *c, int64_t x, int64_t y, int64_t w, int64_t h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    fill(c, x, y, w, 1, color);
    fill(c, x, y + h - 1, w, 1, color);
    fill(c, x, y + 1, 1, h - 2, color);
    fill(c, x + w - 1, y + 1, 1, h - 2, color);
}

static void glyph(const Canvas *c, uint32_t cp, int64_t gx, int64_t gy, uint16_t color)
{
    const PIXELICA_UI_Font *f = &c->font;
    int64_t sc = f->scale, rx, ry;
    uint32_t rows[32], mask = 0xFFFFFFFFu << (32u - f->width);

    if (gx + (int64_t)f->width * sc <= c->cx0 || gx >= c->cx1 ||
        gy + (int64_t)f->height * sc <= c->cy0 || gy >= c->cy1) {
        return; /* fully clipped: the callback is not even asked */
    }
    memset(rows, 0, sizeof rows);
    if (!f->glyph(f->userdata, cp, rows)) { /* missing glyph: cell outline */
        for (ry = 0; ry < f->height; ++ry) {
            rows[ry] = (ry == 0 || ry + 1 == f->height) ? mask
                : (0x80000000u | (0x80000000u >> (f->width - 1u)));
        }
    }
    for (ry = 0; ry < f->height; ++ry) {
        uint32_t bits = rows[ry] & mask;

        for (rx = 0; bits != 0u && rx < f->width; ++rx) {
            if ((bits & (0x80000000u >> (unsigned)rx)) != 0u) {
                fill(c, gx + rx * sc, gy + ry * sc, sc, sc, color);
            }
        }
    }
}

static int64_t text_lines(const char *text, size_t len)
{
    size_t off = 0;
    uint32_t cp;
    int64_t lines = 1;

    while (pixelica_ui_utf8_next(text, len, &off, &cp)) {
        lines += cp == 10u;
    }
    return lines;
}

static int64_t line_glyphs(const char *text, size_t len, size_t off)
{
    uint32_t cp;
    int64_t n = 0;

    while (pixelica_ui_utf8_next(text, len, &off, &cp) && cp != 10u) {
        ++n;
    }
    return n;
}

/* Left aligned at x, or every line centered in [x, x + rw). Newline starts a
 * new line; the cell advance is (width + 1) * scale, the line height
 * (height + 1) * scale. */
static void draw_text(const Canvas *c, const char *text, size_t len, int64_t x, int64_t y,
    int64_t rw, bool center, uint16_t color)
{
    int64_t sc = c->font.scale, adv = ((int64_t)c->font.width + 1) * sc;
    int64_t lh = ((int64_t)c->font.height + 1) * sc;
    size_t off = 0;
    uint32_t cp;

    if (len == 0u) {
        return;
    }
    for (;;) {
        int64_t cx = x;

        if (center) {
            int64_t n = line_glyphs(text, len, off);

            cx = x + (rw - (n != 0 ? n * adv - sc : 0)) / 2;
        }
        for (;;) {
            if (!pixelica_ui_utf8_next(text, len, &off, &cp)) {
                return;
            }
            if (cp == 10u) {
                break;
            }
            glyph(c, cp, cx, y, color);
            cx += adv;
        }
        y += lh;
        if (y >= c->cy1) {
            return;
        }
    }
}

static int64_t text_height(const Canvas *c, const char *text, size_t len)
{
    int64_t sc = c->font.scale;

    return text_lines(text, len) * (((int64_t)c->font.height + 1) * sc) - sc;
}

/* Text centered in the rect on both axes. */
static void centered(const Canvas *c, const PIXELICA_UI_Widget *w, uint16_t color)
{
    if (w->text_bytes != 0u) {
        draw_text(c, w->text, w->text_bytes, w->rect.x,
            (int64_t)w->rect.y + ((int64_t)w->rect.height - text_height(c, w->text, w->text_bytes)) / 2,
            w->rect.width, true, color);
    }
}

static double clamp01(double v)
{
    return !(v > 0.0) ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* ---- widgets ------------------------------------------------------------- */

static void draw_widget(Canvas *c, const PIXELICA_UI_Theme *t, const PIXELICA_UI_Widget *w,
    const PIXELICA_UI_Event *ev, int64_t surf_w, int64_t surf_h)
{
    int64_t x = w->rect.x, y = w->rect.y, rw = w->rect.width, rh = w->rect.height;
    int64_t pad = t->padding, sc = t->font.scale, lh = ((int64_t)t->font.height + 1) * sc;
    bool interactive = w->kind == PIXELICA_UI_BUTTON || w->kind == PIXELICA_UI_CHECKBOX ||
        w->kind == PIXELICA_UI_SLIDER;
    bool eligible = interactive && w->enabled; /* area > 0 is checked below */
    bool hov = eligible && ev->hovered, prs = eligible && ev->pressed, foc = eligible && ev->focused;
    uint16_t txt = (interactive && !w->enabled) ? t->disabled_text : t->text;
    uint16_t body = prs ? t->active : (hov ? t->hover : t->control);
    uint16_t edge = foc ? t->accent : t->border;

    if (rw <= 0 || rh <= 0) {
        return;
    }
    c->cx0 = x > 0 ? x : 0;
    c->cy0 = y > 0 ? y : 0;
    c->cx1 = x + rw < surf_w ? x + rw : surf_w;
    c->cy1 = y + rh < surf_h ? y + rh : surf_h;
    if (c->cx0 >= c->cx1 || c->cy0 >= c->cy1) {
        return;
    }
    switch (w->kind) {
    case PIXELICA_UI_PANEL:
        fill(c, x, y, rw, rh, t->panel);
        border(c, x, y, rw, rh, t->border);
        draw_text(c, w->text, w->text_bytes, x + pad, y + pad, rw, false, txt);
        break;
    case PIXELICA_UI_LABEL:
        draw_text(c, w->text, w->text_bytes, x + pad, y + pad, rw, false, txt);
        break;
    case PIXELICA_UI_BUTTON:
        fill(c, x, y, rw, rh, body);
        border(c, x, y, rw, rh, edge);
        centered(c, w, txt);
        break;
    case PIXELICA_UI_CHECKBOX: {
        int64_t side = lh, bx = x + pad, by = y + (rh - side) / 2, in = side >= 6 ? 2 : 1;

        fill(c, bx, by, side, side, body);
        border(c, bx, by, side, side, edge);
        if (w->checked) {
            fill(c, bx + in, by + in, side - 2 * in, side - 2 * in, eligible ? t->accent : txt);
        }
        if (w->text_bytes != 0u) {
            draw_text(c, w->text, w->text_bytes, bx + side + pad,
                y + (rh - text_height(c, w->text, w->text_bytes)) / 2, rw, false, txt);
        }
        break;
    }
    case PIXELICA_UI_SLIDER: {
        double span = (double)w->max - (double)w->min;
        double frac = span > 0.0 ? clamp01(((double)w->value - (double)w->min) / span) : 0.0;
        int64_t x0 = x + pad, x1 = x + rw - pad, track = 2 * sc, thumb_w = 4 * sc;
        int64_t ty = y + rh - pad - track, room, tx;
        uint16_t thumb = !w->enabled ? t->disabled_text : (prs ? t->active : (hov ? t->hover : t->accent));

        if (x1 <= x0) {
            x0 = x;
            x1 = x + rw;
        }
        room = (x1 - x0) - thumb_w;
        room = room < 0 ? 0 : room;
        tx = x0 + (int64_t)(frac * (double)room + 0.5);
        draw_text(c, w->text, w->text_bytes, x + pad, y + pad, rw, false, txt);
        fill(c, x0, ty, x1 - x0, track, t->control);
        fill(c, tx, ty - 2 * sc, thumb_w, track + 4 * sc, thumb);
        if (foc) {
            border(c, x, y, rw, rh, t->accent);
        }
        break;
    }
    case PIXELICA_UI_PROGRESS: {
        double frac = clamp01((double)w->value);

        fill(c, x, y, rw, rh, t->control);
        border(c, x, y, rw, rh, t->border);
        fill(c, x + 1, y + 1, (int64_t)(frac * (double)(rw - 2) + 0.5), rh - 2, t->active);
        centered(c, w, t->text);
        break;
    }
    default:
        break;
    }
}

PIXELICA_Status pixelica_ui_render_rgb565(const PIXELICA_UI_Context *ui,
    PIXELICA_UI_Surface *surface)
{
    PIXELICA_UI_Theme theme;
    Canvas canvas;
    size_t i;

    if (pixelica_ui_validate_surface(ui, surface) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    theme = ui->theme; /* the glyph callback cannot retarget the font mid-frame */
    canvas.pixels = surface->pixels;
    canvas.stride = surface->stride;
    canvas.cx0 = canvas.cy0 = canvas.cx1 = canvas.cy1 = 0;
    canvas.font = theme.font;
    for (i = 0; i < ui->count; ++i) {
        if (ui->widgets[i].visible) {
            draw_widget(&canvas, &theme, &ui->widgets[i], &ui->events[i],
                surface->width, surface->height);
        }
    }
    return PIXELICA_OK;
}

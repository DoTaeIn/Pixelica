/*
 * Pixelica UI - PAL presenter for the 3D/hybrid path.
 * See modules/ui/include/pixelica/ui.h.
 *
 * present_rgb565 copies the read-only engine frame into the caller output with
 * a tight stride, draws the UI over it, then calls the upstream present exactly
 * once. Every check runs before the first byte is copied, so a rejected frame
 * leaves the output untouched and never reaches the upstream.
 */
#include <stdint.h>
#include <string.h>

#include "pixelica/ui.h"
#include "ui_internal.h"

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

/* r touches neither the context, the widget array nor any borrowed text.
 * Only call after pixelica_ui_validate(ui) succeeded. */
static bool clear_of_ui(const PIXELICA_UI_Context *ui, UiRange r)
{
    UiRange ctx, arr, txt;
    size_t i;

    if (!range_of(ui, 1u, sizeof(*ui), &ctx) ||
        !range_of(ui->widgets, ui->count, sizeof(ui->widgets[0]), &arr) ||
        overlaps(r, ctx) || overlaps(r, arr)) {
        return false;
    }
    for (i = 0; i < ui->count; ++i) {
        if (ui->widgets[i].text_bytes != 0u &&
            (!range_of(ui->widgets[i].text, ui->widgets[i].text_bytes, 1u, &txt) ||
             overlaps(r, txt))) {
            return false;
        }
    }
    return true;
}

static uint64_t ui_time_us(void *userdata)
{
    const PIXELICA_UI_Presenter *p = (const PIXELICA_UI_Presenter *)userdata;

    if (p == NULL || p->upstream.time_us == NULL) {
        return 0u;
    }
    return p->upstream.time_us(p->upstream.userdata);
}

static uint32_t ui_read_buttons(void *userdata)
{
    const PIXELICA_UI_Presenter *p = (const PIXELICA_UI_Presenter *)userdata;

    if (p == NULL || p->upstream.read_buttons == NULL) {
        return 0u;
    }
    return p->upstream.read_buttons(p->upstream.userdata);
}

static void ui_log(void *userdata, const char *message)
{
    const PIXELICA_UI_Presenter *p = (const PIXELICA_UI_Presenter *)userdata;

    if (p != NULL && p->upstream.log != NULL) {
        p->upstream.log(p->upstream.userdata, message);
    }
}

static bool ui_present(void *userdata, const uint16_t *pixels, uint16_t width, uint16_t height,
    uint32_t stride)
{
    const PIXELICA_UI_Presenter *pp = (const PIXELICA_UI_Presenter *)userdata;
    PIXELICA_UI_Presenter p;
    PIXELICA_UI_Surface surface;
    UiRange r_frame, r_self, r_output;
    size_t n, rows, y;

    if (pp == NULL) {
        return false;
    }
    p = *pp; /* a glyph callback cannot retarget this frame */
    if (p.upstream.present_rgb565 == NULL || p.ui == NULL || p.output == NULL ||
        pixels == NULL || width == 0u || height == 0u ||
        (uint32_t)width > PIXELICA_UI_MAX_DIMENSION ||
        (uint32_t)height > PIXELICA_UI_MAX_DIMENSION || stride < (uint32_t)width) {
        return false;
    }
    rows = (size_t)height - 1u;
    if (rows != 0u && (size_t)stride > (SIZE_MAX - (size_t)width) / rows) {
        return false;
    }
    n = (size_t)width * (size_t)height;
    if (p.output_capacity < n ||
        !range_of(pixels, rows * (size_t)stride + (size_t)width, sizeof(uint16_t), &r_frame) ||
        !range_of(pp, 1u, sizeof(*pp), &r_self) ||
        !range_of(p.output, p.output_capacity, sizeof(uint16_t), &r_output) ||
        overlaps(r_frame, r_output) || overlaps(r_frame, r_self) || overlaps(r_output, r_self)) {
        return false;
    }
    surface.pixels = p.output;
    surface.width = width;
    surface.height = height;
    surface.stride = width;
    if (pixelica_ui_validate_surface(p.ui, &surface) != PIXELICA_OK ||
        !clear_of_ui(p.ui, r_output) || !clear_of_ui(p.ui, r_self)) {
        return false;
    }
    for (y = 0; y <= rows; ++y) {
        memcpy(p.output + y * (size_t)width, pixels + y * (size_t)stride,
            (size_t)width * sizeof(uint16_t));
    }
    if (pixelica_ui_render_rgb565(p.ui, &surface) != PIXELICA_OK) {
        return false;
    }
    return p.upstream.present_rgb565(p.upstream.userdata, p.output, width, height, width);
}

PIXELICA_Status pixelica_ui_presenter_init(PIXELICA_UI_Presenter *out,
    const PIXELICA_PlatformOps *upstream, const PIXELICA_UI_Context *ui,
    uint16_t *output, size_t output_capacity)
{
    UiRange r_out, r_up, r_output;

    if (out == NULL || upstream == NULL || ui == NULL || output == NULL ||
        output_capacity == 0u || upstream->time_us == NULL ||
        upstream->read_buttons == NULL || upstream->present_rgb565 == NULL ||
        pixelica_ui_validate(ui) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!range_of(out, 1u, sizeof(*out), &r_out) ||
        !range_of(upstream, 1u, sizeof(*upstream), &r_up) ||
        !range_of(output, output_capacity, sizeof(uint16_t), &r_output) ||
        overlaps(r_out, r_up) || overlaps(r_out, r_output) || overlaps(r_output, r_up) ||
        !clear_of_ui(ui, r_out) || !clear_of_ui(ui, r_output)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    out->upstream = *upstream;
    out->ops.userdata = out;
    out->ops.time_us = ui_time_us;
    out->ops.read_buttons = ui_read_buttons;
    out->ops.present_rgb565 = ui_present;
    out->ops.log = upstream->log != NULL ? ui_log : NULL;
    out->ui = ui;
    out->output = output;
    out->output_capacity = output_capacity;
    return PIXELICA_OK;
}

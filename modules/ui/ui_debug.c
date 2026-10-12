/*
 * Pixelica UI - debug HUD: pixelica/debug.h text shown as one UI panel.
 * Reuses the UI widget renderer and font; no heap, no global state, no
 * renderer. See modules/ui/include/pixelica/ui.h.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/debug.h"
#include "pixelica/ui.h"
#include "ui_internal.h"

#define HUD_MAGIC 0x50554844u /* "PUHD" */

static bool hud_valid(const PIXELICA_UI_DebugHud *hud)
{
    return hud != NULL && hud->magic == HUD_MAGIC && hud->ui.widgets == &hud->panel &&
        hud->ui.count == 1u && hud->panel.text == hud->text &&
        hud->panel.kind == PIXELICA_UI_PANEL && hud->panel.text_bytes < sizeof hud->text;
}

PIXELICA_Status pixelica_ui_debug_hud_init(PIXELICA_UI_DebugHud *out, int32_t x, int32_t y)
{
    PIXELICA_UI_Rect rect;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    rect.x = x;
    rect.y = y;
    rect.width = 0;
    rect.height = 0;
    if (pixelica_ui_widget_default(PIXELICA_UI_PANEL, 1u, rect, out->text, 0u,
            &out->panel) != PIXELICA_OK ||
        pixelica_ui_init(&out->ui, &out->panel, 1u) != PIXELICA_OK) {
        memset(out, 0, sizeof *out); /* magic 0: every later call rejects it */
        return PIXELICA_ERR_INVALID_ARG;
    }
    out->x = x;
    out->y = y;
    out->magic = HUD_MAGIC;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_ui_debug_hud_update(PIXELICA_UI_DebugHud *hud,
    const PIXELICA_DebugTiming *timing, const char *metric_name,
    size_t metric_name_bytes, int32_t metric_value)
{
    char line1[PIXELICA_DEBUG_TEXT_BYTES], line2[PIXELICA_DEBUG_TEXT_BYTES];
    char text[sizeof hud->text];
    size_t len1, len2 = 0u, total, width_glyphs;
    int64_t adv, lh, sc, pad, w, h;
    bool has_metric = metric_name != NULL;

    if (!hud_valid(hud) || timing == NULL || pixelica_ui_validate(&hud->ui) != PIXELICA_OK ||
        (!has_metric && metric_name_bytes != 0u)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (pixelica_debug_format_timing(timing, line1, sizeof line1, &len1, NULL) != PIXELICA_OK ||
        (has_metric && pixelica_debug_format_metric(metric_name, metric_name_bytes,
                           metric_value, line2, sizeof line2, &len2, NULL) != PIXELICA_OK)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memcpy(text, line1, len1);
    total = len1;
    width_glyphs = len1; /* ASCII only: one glyph per byte */
    if (has_metric) {
        text[total++] = '\n';
        memcpy(text + total, line2, len2);
        total += len2;
        if (len2 > width_glyphs) {
            width_glyphs = len2;
        }
    }

    sc = hud->ui.theme.font.scale;
    adv = ((int64_t)hud->ui.theme.font.width + 1) * sc;
    lh = ((int64_t)hud->ui.theme.font.height + 1) * sc;
    pad = hud->ui.theme.padding;
    w = (int64_t)width_glyphs * adv - sc + 2 * pad + 2; /* text + padding + border */
    h = (has_metric ? 2 : 1) * lh - sc + 2 * pad + 2;

    memcpy(hud->text, text, total);
    hud->panel.text_bytes = total;
    hud->panel.rect.x = hud->x;
    hud->panel.rect.y = hud->y;
    hud->panel.rect.width = (int32_t)w;
    hud->panel.rect.height = (int32_t)h;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_ui_debug_hud_render_rgb565(const PIXELICA_UI_DebugHud *hud,
    PIXELICA_UI_Surface *surface)
{
    if (!hud_valid(hud)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return pixelica_ui_render_rgb565(&hud->ui, surface);
}

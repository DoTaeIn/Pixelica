/* Private UI interface shared by core (ui.c) and renderer (ui_rgb565.c,
 * ui_font.c, ui_presenter.c). Not installed, not public API. */
#ifndef PIXELICA_UI_INTERNAL_H
#define PIXELICA_UI_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pixelica/ui.h"

#define PIXELICA_UI_MAGIC 0x50554931u

/* Whole-context validation: magic, widget array/count, every widget, theme,
 * strict UTF-8 text and address-range aliasing (ctx/widgets/text). */
PIXELICA_Status pixelica_ui_validate(const PIXELICA_UI_Context *ui);
/* Renderer: validates UI plus surface descriptor/pixel range and overlaps. */
PIXELICA_Status pixelica_ui_validate_surface(const PIXELICA_UI_Context *ui,
    const PIXELICA_UI_Surface *surface);
/* Strict UTF-8 decode at *offset; advances *offset on success. Returns false
 * at end of text or on malformed input (offset/codepoint untouched). */
bool pixelica_ui_utf8_next(const char *text, size_t length, size_t *offset,
    uint32_t *codepoint);

#endif

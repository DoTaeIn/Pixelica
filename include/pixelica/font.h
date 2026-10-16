/* Shared fixed-cell Unicode bitmap fonts. No allocation or renderer dependency. */
#ifndef PIXELICA_FONT_H
#define PIXELICA_FONT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pixelica/status.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Pure callback: write rows[0..height), bit31 = left pixel; false means missing.
 * Must not modify rendering state, font, text, target or userdata; do not retain rows.
 * Backing data and userdata remain caller-owned and must not overlap draw targets. */
typedef bool (*PIXELICA_GlyphFn)(void *userdata, uint32_t codepoint, uint32_t rows[32]);
/* Preserve the existing UI struct tag as well as its typedef via ui.h. */
typedef struct PIXELICA_UI_Font {
    uint8_t width, height, scale; /* width 1..16, height 1..32, scale 1..4 */
    PIXELICA_GlyphFn glyph;
    void *userdata;
} PIXELICA_Font;
/* Strict decoder: false on end/invalid; outputs unchanged on false. */
bool pixelica_utf8_next(const char *text, size_t length, size_t *offset, uint32_t *codepoint);
bool pixelica_font_valid(const PIXELICA_Font *font);
/* Validate all UTF-8 before invoking callbacks. Newlines are not glyphs.
 * OK if covered, NOT_FOUND if missing, INVALID_ARG on invalid font/text/UTF-8.
 * missing counts occurrences; first_missing is 0 when none. Optional outputs
 * remain unchanged on INVALID_ARG and must not overlap inputs or each other.
 * Fonts/text are borrowed; callbacks must be pure and deterministic. */
PIXELICA_Status pixelica_font_check_text(const PIXELICA_Font *font, const char *text,
    size_t length, size_t *missing, uint32_t *first_missing);
/* Built-in GalmuriMono11: 16x16, Hangul syllables, ASCII and additional scripts.
 * Does not cover all Unicode. OFL-1.1 notice must accompany redistribution.
 * Invalid scale returns an invalid font (scale=0). Static data, no heap.
 * Separate object in common archive: linked only when this function is used. */
PIXELICA_Font pixelica_font_korean(uint8_t scale);
#ifdef __cplusplus
}
#endif
#endif

#include "pixelica/font.h"

bool pixelica_utf8_next(const char *text, size_t length, size_t *offset,
    uint32_t *codepoint)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t i, n, k;
    uint32_t c, min_value;

    if (text == NULL || offset == NULL || codepoint == NULL || *offset >= length) {
        return false;
    }
    i = *offset;
    c = s[i];
    if (c < 0x80u) {
        n = 1u;
        min_value = 0u;
    } else if (c >= 0xC2u && c <= 0xDFu) {
        n = 2u;
        c &= 0x1Fu;
        min_value = 0x80u;
    } else if ((c & 0xF0u) == 0xE0u) {
        n = 3u;
        c &= 0x0Fu;
        min_value = 0x800u;
    } else if (c >= 0xF0u && c <= 0xF4u) {
        n = 4u;
        c &= 0x07u;
        min_value = 0x10000u;
    } else {
        return false; /* stray continuation, C0/C1 overlong lead, F5..FF */
    }
    if (n > length - i) {
        return false; /* truncated */
    }
    for (k = 1u; k < n; ++k) {
        uint32_t b = s[i + k];
        if ((b & 0xC0u) != 0x80u) {
            return false;
        }
        c = (c << 6) | (b & 0x3Fu);
    }
    if (c < min_value || c > 0x10FFFFu || (c >= 0xD800u && c <= 0xDFFFu)) {
        return false;
    }
    *offset = i + n;
    *codepoint = c;
    return true;
}

bool pixelica_font_valid(const PIXELICA_Font *font)
{
    return font != NULL && font->glyph != NULL && font->width >= 1u && font->width <= 16u &&
        font->height >= 1u && font->height <= 32u && font->scale >= 1u && font->scale <= 4u;
}

PIXELICA_Status pixelica_font_check_text(const PIXELICA_Font *font, const char *text,
    size_t length, size_t *missing, uint32_t *first_missing)
{
    size_t off = 0u, count = 0u;
    uint32_t cp, first = 0u, rows[32];
    if (!pixelica_font_valid(font) || (text == NULL && length != 0u)) return PIXELICA_ERR_INVALID_ARG;
    while (off < length) {
        if (!pixelica_utf8_next(text, length, &off, &cp)) return PIXELICA_ERR_INVALID_ARG;
    }
    off = 0u;
    while (pixelica_utf8_next(text, length, &off, &cp)) {
        if (cp != 10u && !font->glyph(font->userdata, cp, rows)) {
            if (count == 0u) first = cp;
            ++count;
        }
    }
    if (missing != NULL) *missing = count;
    if (first_missing != NULL) *first_missing = first;
    return count == 0u ? PIXELICA_OK : PIXELICA_ERR_NOT_FOUND;
}

#include "pixelica/font.h"
#include "font_korean_data.inc"

static bool korean_glyph(void *userdata, uint32_t cp, uint32_t rows[32])
{
    size_t lo = 0u, hi = sizeof korean_codes / sizeof korean_codes[0], i;
    (void)userdata;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (korean_codes[mid] < cp) lo = mid + 1u;
        else hi = mid;
    }
    if (lo == sizeof korean_codes / sizeof korean_codes[0] || korean_codes[lo] != cp) return false;
    for (i = 0u; i < 16u; ++i) rows[i] = (uint32_t)korean_rows[lo][i] << 16;
    return true;
}

PIXELICA_Font pixelica_font_korean(uint8_t scale)
{
    PIXELICA_Font font = {16u, 16u, 0u, korean_glyph, NULL};
    if (scale >= 1u && scale <= 4u) font.scale = scale;
    return font;
}

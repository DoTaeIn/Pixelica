#include <stdio.h>
#include <string.h>
#include "pixelica/font.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
    PIXELICA_Font f = pixelica_font_korean(1u), bad = pixelica_font_korean(0u);
    size_t missing = 9u, off = 0u;
    uint32_t first = 9u, cp = 99u, rows[32];
    uint32_t c;
    const char sample[] = "A\xED\x95\x9C\xEA\xB8\x80\n\xE3\x81\x82\xE4\xB8\xAD"; /* A한글 あ中 */
    CHECK(pixelica_font_valid(&f));
    CHECK(!pixelica_font_valid(&bad));
    CHECK(pixelica_font_check_text(&f, sample, sizeof sample-1u, &missing, &first) == PIXELICA_OK);
    CHECK(missing == 0u && first == 0u);
    for (c = 0xAC00u; c <= 0xD7A3u; ++c) CHECK(f.glyph(f.userdata, c, rows));
    CHECK(pixelica_font_check_text(&f, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80", 8u, &missing, &first) == PIXELICA_ERR_NOT_FOUND);
    CHECK(missing == 2u && first == 0x1F600u);
    CHECK(pixelica_font_check_text(&f, "A\xED\xA0\x80", 4u, &missing, &first) == PIXELICA_ERR_INVALID_ARG);
    CHECK(missing == 2u && first == 0x1F600u);
    CHECK(pixelica_font_check_text(&f, NULL, 0u, &missing, &first) == PIXELICA_OK);
    CHECK(pixelica_font_check_text(&f, NULL, 1u, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_font_check_text(&bad, "A", 1u, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(!pixelica_utf8_next("\xF0\x80\x80\x80", 4u, &off, &cp));
    CHECK(off == 0u && cp == 99u);
    CHECK(!pixelica_utf8_next("\xF4\x90\x80\x80", 4u, &off, &cp));
    CHECK(!pixelica_utf8_next("\xE3\x81", 2u, &off, &cp));
    CHECK(pixelica_utf8_next("\xF0\x9F\x98\x80", 4u, &off, &cp));
    CHECK(off == 4u && cp == 0x1F600u);
    puts("font: Unicode coverage, missing and invalid UTF-8 passed");
    return 0;
}

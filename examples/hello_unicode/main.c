/* HEADLESS UTF-8/UI parity demo. Optional argv[1]: PPM capture path. */
#include <stdio.h>
#include <string.h>
#include "pixelica/renderer2d.h"
#include "pixelica/ui.h"
#define W 384u
#define H 128u
static uint16_t pixels[W*H], ui_pixels[W*H];
int main(int argc, char **argv)
{
    const char text[] = "Pixelica Unicode\n\xED\x95\x9C\xEA\xB8\x80: \xEC\x8B\x9C\xEC\x9E\x91 \xEC\xA0\x80\xEC\x9E\xA5 \xEC\x9D\xB4\xEC\x96\xB4\xED\x95\x98\xEA\xB8\xB0\n\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E: \xE3\x81\xAF\xE3\x81\x98\xE3\x82\x81\xE3\x82\x8B \xE4\xBF\x9D\xE5\xAD\x98\n\xE4\xB8\xAD\xE6\x96\x87: \xE9\x96\x8B\xE5\xA7\x8B \xE4\xBF\x9D\xE5\xAD\x98\nEnglish: Play Save\n\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC \xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9";
    PIXELICA_Font font = pixelica_font_korean(1u);
    PIXELICA2D_Context ctx;
    PIXELICA2D_Surface surface = {pixels, W, H, W, NULL, 0u};
    PIXELICA_UI_Surface ui_surface = {ui_pixels, W, H, W};
    PIXELICA_UI_Context ui;
    PIXELICA_UI_Widget label;
    size_t missing, i;
    uint32_t first;
    FILE *fp;
    if (pixelica_font_check_text(&font, text, sizeof text-1u, &missing, &first) != PIXELICA_OK) {
        fprintf(stderr, "Missing glyphs: %zu, first U+%04X\n", missing, (unsigned)first);
        return 1;
    }
    if (pixelica2d_init(&ctx, &surface) != PIXELICA_OK ||
        pixelica2d_clear(&ctx, 0x1084u) != PIXELICA_OK) return 2;
    memcpy(ui_pixels, pixels, sizeof pixels);
    if (pixelica2d_draw_text_utf8(&ctx, &font, text, sizeof text-1u, 8, 8, 0xFFFFu, 255u) != PIXELICA_OK) return 3;
    memset(&label, 0, sizeof label);
    label.kind = PIXELICA_UI_LABEL; label.id = 1u;
    label.rect.x = 8; label.rect.y = 8; label.rect.width = (int32_t)W-8; label.rect.height = (int32_t)H-8;
    label.text = text; label.text_bytes = sizeof text-1u; label.visible = label.enabled = true;
    if (pixelica_ui_init(&ui, &label, 1u) != PIXELICA_OK) return 4;
    ui.theme.font = font; ui.theme.padding = 0u; ui.theme.text = 0xFFFFu;
    if (pixelica_ui_render_rgb565(&ui, &ui_surface) != PIXELICA_OK ||
        memcmp(pixels, ui_pixels, sizeof pixels) != 0) return 5;
    if (argc > 1) {
        fp = fopen(argv[1], "wb");
        if (fp == NULL) return 6;
        if (fprintf(fp, "P6\n%u %u\n255\n", W, H) < 0) { fclose(fp); return 7; }
        for (i = 0u; i < W*H; ++i) {
            unsigned p = pixels[i];
            unsigned char rgb[3] = {(unsigned char)(((p>>11)&31u)*255u/31u),
                (unsigned char)(((p>>5)&63u)*255u/63u), (unsigned char)((p&31u)*255u/31u)};
            if (fwrite(rgb, 1u, 3u, fp) != 3u) { fclose(fp); return 7; }
        }
        if (fclose(fp) != 0) return 7;
    }
    puts("hello_unicode: missing=0 ui_2d_pixels_match=1");
    return 0;
}

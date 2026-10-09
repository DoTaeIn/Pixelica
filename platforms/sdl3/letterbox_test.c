/* Headless check of the SDL3 platform's integer-scale / letterbox placement. */
#include <stdio.h>

#include "letterbox.h"

static int g_failures;

static void expect_rect(const char *name, int out_w, int out_h, int src_w, int src_h,
                        int x, int y, int w, int h)
{
    PIXELICA_Sdl3Rect r = {-1, -1, -1, -1};
    if (!pixelica_sdl3_compute_letterbox(out_w, out_h, src_w, src_h, &r) ||
        r.x != x || r.y != y || r.w != w || r.h != h) {
        fprintf(stderr, "FAIL %s: got %d,%d %dx%d expected %d,%d %dx%d\n", name,
                r.x, r.y, r.w, r.h, x, y, w, h);
        ++g_failures;
    }
}

static void expect_rejected(const char *name, int out_w, int out_h, int src_w, int src_h)
{
    PIXELICA_Sdl3Rect r;
    if (pixelica_sdl3_compute_letterbox(out_w, out_h, src_w, src_h, &r)) {
        fprintf(stderr, "FAIL %s: expected rejection\n", name);
        ++g_failures;
    }
}

int main(void)
{
    expect_rect("exact 3x", 768, 576, 256, 192, 0, 0, 768, 576);
    expect_rect("1080p picks 5x, pillar+letter", 1920, 1080, 256, 192, 320, 60, 1280, 960);
    expect_rect("wide window", 1000, 400, 256, 192, 244, 8, 256 * 2, 192 * 2);
    expect_rect("320x240 does not stretch", 320, 240, 256, 192, 32, 24, 256, 192);
    expect_rect("tall window", 300, 1000, 256, 192, 22, 404, 256, 192);
    expect_rect("below 1x keeps aspect", 128, 128, 256, 192, 0, 16, 128, 96);
    expect_rejected("zero output", 0, 100, 256, 192);
    expect_rejected("negative source", 100, 100, -1, 192);
    if (pixelica_sdl3_compute_letterbox(100, 100, 256, 192, NULL)) {
        fprintf(stderr, "FAIL NULL rect accepted\n");
        ++g_failures;
    }

    if (g_failures != 0) {
        return 1;
    }
    puts("letterbox: ok");
    return 0;
}

#include "letterbox.h"

#include <stddef.h>
#include <stdint.h>

bool pixelica_sdl3_compute_letterbox(int out_w, int out_h, int src_w, int src_h,
                                PIXELICA_Sdl3Rect *out_rect)
{
    if (out_rect == NULL || out_w <= 0 || out_h <= 0 || src_w <= 0 || src_h <= 0) {
        return false;
    }

    int scale_x = out_w / src_w;
    int scale_y = out_h / src_h;
    int scale = scale_x < scale_y ? scale_x : scale_y;
    int w, h;

    if (scale >= 1) {
        /* scale <= out/src, so the products cannot exceed the output size. */
        w = src_w * scale;
        h = src_h * scale;
    } else {
        /* Smaller than 1x: fit by the tighter axis, keep the aspect ratio. */
        int64_t lhs = (int64_t)out_w * src_h;
        int64_t rhs = (int64_t)out_h * src_w;
        if (lhs <= rhs) {
            w = out_w;
            h = (int)(((int64_t)src_h * out_w) / src_w);
        } else {
            h = out_h;
            w = (int)(((int64_t)src_w * out_h) / src_h);
        }
        if (w < 1) {
            w = 1;
        }
        if (h < 1) {
            h = 1;
        }
    }

    out_rect->x = (out_w - w) / 2;
    out_rect->y = (out_h - h) / 2;
    out_rect->w = w;
    out_rect->h = h;
    return true;
}

/*
 * Display placement for the SDL3 platform. Pure integer math, no SDL types,
 * so it can be unit-tested without a window.
 */
#ifndef PIXELICA_SDL3_LETTERBOX_H
#define PIXELICA_SDL3_LETTERBOX_H

#include <stdbool.h>

typedef struct PIXELICA_Sdl3Rect {
    int x, y, w, h;
} PIXELICA_Sdl3Rect;

/*
 * Places a src_w x src_h image inside an out_w x out_h output (pixels).
 * Uses the largest integer scale that fits and centers the result (bars on
 * the unused sides). If even 1x does not fit, falls back to an aspect-correct
 * non-integer shrink. Returns false for non-positive sizes or overflow.
 */
bool pixelica_sdl3_compute_letterbox(int out_w, int out_h, int src_w, int src_h,
                                PIXELICA_Sdl3Rect *out_rect);

#endif /* PIXELICA_SDL3_LETTERBOX_H */

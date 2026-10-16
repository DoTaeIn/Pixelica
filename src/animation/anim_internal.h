/*
 * Pixelica - private helpers shared by the animation sources
 * (src/animation/animation.c and src/animation/anim_clip.c).
 *
 * These are implementation details of the common library: they are not
 * declared by any public header and must not be used outside src/animation.
 * They exist so clips reuse the exact easing and interpolation of
 * pixelica_anim_sample instead of a second, divergent copy.
 */
#ifndef PIXELICA_ANIM_INTERNAL_H
#define PIXELICA_ANIM_INTERNAL_H

#include "pixelica/animation.h"

/* 1 if `ease` is one of the PIXELICA_Ease values, else 0. */
int pixelica_anim_ease_valid(PIXELICA_Ease ease);

/* ease(clamp(t, 0, 1)) in double. `ease` must be valid and t non-NaN.
 * The result is in [0, 1]; 0 maps to exactly 0 and 1 to exactly 1. */
double pixelica_anim_ease_eval(PIXELICA_Ease ease, double t);

/* from + (to - from) * e for finite endpoints and e in [0, 1]. Always
 * within [min(from, to), max(from, to)] and never overflows. */
float pixelica_anim_lerp(float from, float to, double e);

#endif /* PIXELICA_ANIM_INTERNAL_H */

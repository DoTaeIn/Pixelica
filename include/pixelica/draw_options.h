/* Shared opt-in sampling and transparency state. RGB565 assets stay unchanged. */
#ifndef PIXELICA_DRAW_OPTIONS_H
#define PIXELICA_DRAW_OPTIONS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum PIXELICA_WrapMode {
    PIXELICA_WRAP_CLAMP = 0,
    PIXELICA_WRAP_REPEAT = 1
} PIXELICA_WrapMode;
/* Texture filter for pixelica_draw_mesh_effects (renderer.h). Existing draw
 * APIs and zeroed descriptors always sample NEAREST. */
typedef enum PIXELICA_FilterMode {
    PIXELICA_FILTER_NEAREST = 0,
    PIXELICA_FILTER_BILINEAR = 1
} PIXELICA_FilterMode;
/*
 * How a shaded fragment reaches the FRAMEBUFFER. This is the final
 * destination blend and is unrelated to PIXELICA_TextureCombine
 * (renderer.h), whose PIXELICA_TEXTURE_ADD combines one texture LAYER into
 * the fragment color before any of this runs.
 */
typedef enum PIXELICA_AlphaMode {
    PIXELICA_ALPHA_OPAQUE = 0,
    PIXELICA_ALPHA_TEST = 1,
    PIXELICA_ALPHA_BLEND = 2,
    PIXELICA_ALPHA_ADD = 3
} PIXELICA_AlphaMode;
typedef struct PIXELICA_DrawOptions {
    PIXELICA_WrapMode wrap_u, wrap_v;
    PIXELICA_AlphaMode alpha_mode;
    uint8_t opacity;       /* 0..255; multiplied with the sampled alpha in TEST/BLEND */
    uint8_t alpha_cutoff;  /* TEST discards effective alpha < cutoff */
    const uint8_t *texture_alpha; /* optional tightly packed width*height alpha plane */
    size_t texture_alpha_bytes;   /* >= width*height when the plane is supplied */
} PIXELICA_DrawOptions;
/*
 * OPAQUE ignores opacity and the alpha plane. TEST keeps surviving RGB565
 * pixels opaque and follows normal depth writes.
 *
 * Both framebuffer blend modes use the same EFFECTIVE alpha
 *   a = round(sampled_texture_alpha * opacity / 255)
 * (255 when there is no alpha plane; see each renderer for the extra
 * per-vertex / per-sprite factors), skip a == 0 entirely, are depth-TESTED
 * like every other fragment and never WRITE depth, so overlapping
 * transparent surfaces do not occlude each other:
 *   BLEND  per 5/6/5 channel  dst = (src*a + dst*(255-a) + 127) / 255
 *   ADD    per 5/6/5 channel  dst = min(dst + (src*a + 127) / 255, max)
 *          saturating at the channel maximum (31 / 63 / 31)
 * ADD is order-independent (saturation aside), so additive geometry needs no
 * sorting; BLEND does. Draw opaque and TEST geometry first, then BLEND
 * back-to-front, then ADD in any order. Because neither writes depth,
 * geometry drawn afterwards is not occluded by them.
 *
 * Borrow all input storage for the call, disjoint from the destination
 * buffers. NULL options selects these defaults. Existing draw APIs keep
 * their behavior.
 */
static inline PIXELICA_DrawOptions pixelica_draw_options_default(void)
{
    PIXELICA_DrawOptions o;
    o.wrap_u = PIXELICA_WRAP_CLAMP;
    o.wrap_v = PIXELICA_WRAP_CLAMP;
    o.alpha_mode = PIXELICA_ALPHA_OPAQUE;
    o.opacity = 255u;
    o.alpha_cutoff = 128u;
    o.texture_alpha = NULL;
    o.texture_alpha_bytes = 0u;
    return o;
}
#ifdef __cplusplus
}
#endif
#endif

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
typedef enum PIXELICA_AlphaMode {
    PIXELICA_ALPHA_OPAQUE = 0,
    PIXELICA_ALPHA_TEST = 1,
    PIXELICA_ALPHA_BLEND = 2
} PIXELICA_AlphaMode;
typedef struct PIXELICA_DrawOptions {
    PIXELICA_WrapMode wrap_u, wrap_v;
    PIXELICA_AlphaMode alpha_mode;
    uint8_t opacity;       /* 0..255; multiplied with the sampled alpha in TEST/BLEND */
    uint8_t alpha_cutoff;  /* TEST discards effective alpha < cutoff */
    const uint8_t *texture_alpha; /* optional tightly packed width*height alpha plane */
    size_t texture_alpha_bytes;   /* >= width*height when the plane is supplied */
} PIXELICA_DrawOptions;
/* OPAQUE ignores opacity and the alpha plane. BLEND skips alpha=0 and never
 * writes depth; draw opaque/TEST first, then BLEND back-to-front. TEST keeps
 * surviving RGB565 pixels opaque and follows normal depth writes. Borrow all
 * input storage for the call, disjoint from the destination buffers. NULL
 * options selects these defaults. Existing draw APIs keep their behavior. */
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

/* Pixelica - private triangle rasterizer interface. */
#ifndef PIXELICA_RASTER_H
#define PIXELICA_RASTER_H

#include <stdint.h>

#include "pixelica/draw_options.h"
#include "pixelica/renderer.h" /* PIXELICA_MAX_TEXTURE_LAYERS, PIXELICA_TextureCombine */

/* Extra perspective attributes (premultiplied by q like uq/vq): layer k
 * uses xq[2k] (u) and xq[2k+1] (v); vertex color r,g,b,a (0..255, not
 * normalized) use xq[PIXELICA_RASTER_COLOR_ATTR + 0..3]. Unused slots are 0. */
#define PIXELICA_RASTER_COLOR_ATTR (2u * PIXELICA_MAX_TEXTURE_LAYERS)
#define PIXELICA_RASTER_EXTRA_ATTRS (PIXELICA_RASTER_COLOR_ATTR + 4u)

typedef struct PIXELICA_RasterTarget {
    uint16_t *color;
    uint16_t *depth;
    unsigned char *coverage; /* width*height bits, bit (y*width+x) */
    int32_t width;  /* 1..PIXELICA_MAX_DIMENSION */
    int32_t height; /* 1..PIXELICA_MAX_DIMENSION */
} PIXELICA_RasterTarget;

/* Snapped screen vertex. x,y are fixed point with PIXELICA_SUBPIXEL_BITS
 * fractional bits, y down, and MUST lie in [0, width << bits] x
 * [0, height << bits]. z is the depth value in [0, PIXELICA_DEPTH_MAX_DRAWABLE].
 * Textured triangles also carry the perspective weight q = 1/w (scaled by
 * any common positive factor) and the premultiplied texture coordinates
 * uq = u*q, vq = v*q; flat triangles ignore them. Layered/vertex-colored
 * triangles also carry xq (see PIXELICA_RASTER_EXTRA_ATTRS) and always q. */
typedef struct PIXELICA_ScreenVertex {
    int32_t x, y;
    float z;
    float q, uq, vq;
    float xq[PIXELICA_RASTER_EXTRA_ATTRS];
} PIXELICA_ScreenVertex;

/* One additional texture layer (validated: texels non-NULL, 1..max dims). */
typedef struct PIXELICA_RasterLayer {
    const uint16_t *texels;
    int32_t tex_width, tex_height;
    int repeat_u, repeat_v;
    PIXELICA_TextureCombine combine;
    uint8_t opacity;
} PIXELICA_RasterLayer;

/* How covered fragments that pass the depth test are colored. */
typedef struct PIXELICA_RasterShade {
    uint16_t color;          /* used when texels == NULL */
    const uint16_t *texels;  /* row-major, row 0 = top; NULL = flat color */
    int32_t tex_width;       /* 1..PIXELICA_MAX_TEXTURE_DIMENSION when textured */
    int32_t tex_height;
    const uint8_t *alpha;    /* NULL = 255; indexed exactly like texels */
    int repeat_u, repeat_v;  /* nonzero: wrap t - floor(t), else clamp */
    PIXELICA_AlphaMode alpha_mode;
    uint8_t opacity, alpha_cutoff; /* unused for PIXELICA_ALPHA_OPAQUE */
    /* Layered material (all zero for legacy draws = original path). After
     * the base color: layers[0..layer_count) in order, then (if
     * vertex_colors) the interpolated RGBA vertex color modulation; its
     * alpha multiplies the base texture alpha before opacity (non-OPAQUE). */
    PIXELICA_RasterLayer layers[PIXELICA_MAX_TEXTURE_LAYERS];
    uint32_t layer_count;    /* 0..PIXELICA_MAX_TEXTURE_LAYERS */
    int vertex_colors;       /* nonzero: modulate by xq color attrs */
    /* Mesh effects (zero = legacy path). bilinear filters the base texture,
     * its alpha plane and every layer. pixel runs per depth-passing fragment
     * after opacity and before the alpha test/blend/write; false discards. */
    int bilinear;
    PIXELICA_PixelFn pixel;
    void *pixel_userdata;
} PIXELICA_RasterShade;

/* Per-triangle fragment counts (each <= width * height). */
typedef struct PIXELICA_RasterCounts {
    uint32_t written;       /* color written (passed depth and alpha) */
    uint32_t covered;       /* covered pixel centers, before the depth test */
    uint32_t newly_covered; /* covered pixels not yet in the coverage bitmap */
} PIXELICA_RasterCounts;

/*
 * Fills one triangle using the top-left rule and a strict less-than uint16
 * depth test. `expected_sign` is the sign (+1/-1) the screen-space signed
 * area (x1-x0)(y2-y0)-(y1-y0)(x2-x0) must have; a zero area or the opposite
 * sign (snapping sliver) draws nothing. `counts` is overwritten.
 * Every covered pixel sets its coverage bit (even if depth/alpha reject it).
 * Depth is tested before any texel is sampled. OPAQUE writes depth+color;
 * TEST discards effective alpha < cutoff before any write, else as OPAQUE;
 * BLEND skips alpha 0 and blends color without writing depth.
 * With shade->pixel the callback sees every depth-passing fragment and may
 * replace its color/alpha or discard it (no color, no depth).
 */
void pixelica_raster_triangle(const PIXELICA_RasterTarget *target,
                         const PIXELICA_ScreenVertex *a,
                         const PIXELICA_ScreenVertex *b,
                         const PIXELICA_ScreenVertex *c,
                         int expected_sign, const PIXELICA_RasterShade *shade,
                         PIXELICA_RasterCounts *counts);

#endif /* PIXELICA_RASTER_H */

/* Pixelica - private triangle rasterizer interface. */
#ifndef PIXELICA_RASTER_H
#define PIXELICA_RASTER_H

#include <stdint.h>

typedef struct PIXELICA_RasterTarget {
    uint16_t *color;
    uint16_t *depth;
    int32_t width;  /* 1..PIXELICA_MAX_DIMENSION */
    int32_t height; /* 1..PIXELICA_MAX_DIMENSION */
} PIXELICA_RasterTarget;

/* Snapped screen vertex. x,y are fixed point with PIXELICA_SUBPIXEL_BITS
 * fractional bits, y down, and MUST lie in [0, width << bits] x
 * [0, height << bits]. z is the depth value in [0, PIXELICA_DEPTH_MAX_DRAWABLE].
 * Textured triangles also carry the perspective weight q = 1/w (scaled by
 * any common positive factor) and the premultiplied texture coordinates
 * uq = u*q, vq = v*q; flat triangles ignore them. */
typedef struct PIXELICA_ScreenVertex {
    int32_t x, y;
    float z;
    float q, uq, vq;
} PIXELICA_ScreenVertex;

/* How covered fragments that pass the depth test are colored. */
typedef struct PIXELICA_RasterShade {
    uint16_t color;          /* used when texels == NULL */
    const uint16_t *texels;  /* row-major, row 0 = top; NULL = flat color */
    int32_t tex_width;       /* 1..PIXELICA_MAX_TEXTURE_DIMENSION when textured */
    int32_t tex_height;
} PIXELICA_RasterShade;

/* Per-triangle fragment counts (each <= width * height). */
typedef struct PIXELICA_RasterCounts {
    uint32_t written;       /* passed the depth test */
    uint32_t covered;       /* covered pixel centers, before the depth test */
    uint32_t newly_covered; /* covered pixels whose depth was still clear */
} PIXELICA_RasterCounts;

/*
 * Fills one triangle using the top-left rule and a strict less-than uint16
 * depth test. `expected_sign` is the sign (+1/-1) the screen-space signed
 * area (x1-x0)(y2-y0)-(y1-y0)(x2-x0) must have; a zero area or the opposite
 * sign (snapping sliver) draws nothing. `counts` is overwritten.
 */
void pixelica_raster_triangle(const PIXELICA_RasterTarget *target,
                         const PIXELICA_ScreenVertex *a,
                         const PIXELICA_ScreenVertex *b,
                         const PIXELICA_ScreenVertex *c,
                         int expected_sign, const PIXELICA_RasterShade *shade,
                         PIXELICA_RasterCounts *counts);

#endif /* PIXELICA_RASTER_H */

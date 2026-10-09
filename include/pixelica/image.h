/*
 * Pixelica - shared image types (RGB565 pixels and textures).
 *
 * Part of the common library (Pixelica::common): used by the 3D renderer
 * (renderer.h), the 2D renderer (renderer2d.h) and the texture asset loader
 * (assets_common.h). renderer.h includes this header, so every symbol that used
 * to live there is still visible to code that includes renderer.h or pixelica.h.
 */
#ifndef PIXELICA_IMAGE_H
#define PIXELICA_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest accepted texture width/height (texels). */
#define PIXELICA_MAX_TEXTURE_DIMENSION 4096u

/* Pack 8-bit channels into RGB565. */
#define PIXELICA_RGB565(r, g, b)                                  \
    ((uint16_t)((((uint32_t)(r) & 0xF8u) << 8) |             \
                (((uint32_t)(g) & 0xFCu) << 3) |             \
                (((uint32_t)(b) & 0xFFu) >> 3)))

/*
 * RGB565 texture, row-major, row 0 = TOP row, stride = width. Valid when
 * pixels != NULL and 1 <= width, height <= PIXELICA_MAX_TEXTURE_DIMENSION.
 * Read only during a draw call.
 */
typedef struct PIXELICA_Texture {
    const uint16_t *pixels;
    uint16_t width, height;
} PIXELICA_Texture;

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_IMAGE_H */

/*
 * Pixelica - asset format v1: shared container constants, CRC-32 and the
 * texture (.p3tex) loader. Part of the common library (Pixelica::common); it
 * has no dependency on the 3D renderer. The mesh (.p3mesh) and cart (.p3cart)
 * loaders are declared in assets.h, which includes this header.
 *
 * The container layout, error codes and storage guarantees are documented at
 * the top of assets.h and in docs/asset_format.md; they apply unchanged to the
 * functions declared here:
 *   - the whole input is validated before caller storage or *out is changed;
 *     on failure *out is zeroed and storage is untouched;
 *   - `data` and `storage` must not overlap (PIXELICA_ERR_INVALID_ARG);
 *   - no heap use and no file I/O.
 *
 * .p3tex ("P3TX"): INFO = width:u16, height:u16, reserved:u32 (= 0),
 *   DATA = width * height RGB565 u16, row 0 = top.
 */
#ifndef PIXELICA_ASSETS_COMMON_H
#define PIXELICA_ASSETS_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/image.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configurable limits (define before including, identically for every
 * translation unit, to change them). Defaults match tools/pixelica_pack. */
#ifndef PIXELICA_ASSET_MAX_FILE_BYTES
#define PIXELICA_ASSET_MAX_FILE_BYTES (16u * 1024u * 1024u)
#endif
#ifndef PIXELICA_ASSET_MAX_VERTICES
#define PIXELICA_ASSET_MAX_VERTICES 65535u
#endif
#ifndef PIXELICA_ASSET_MAX_INDICES
#define PIXELICA_ASSET_MAX_INDICES 196608u
#endif
#ifndef PIXELICA_ASSET_MAX_TEXTURE_DIMENSION
#define PIXELICA_ASSET_MAX_TEXTURE_DIMENSION 4096u
#endif
/* Bounds the O(n^2) duplicate-name check of pixelica_cart_open(). */
#ifndef PIXELICA_CART_MAX_ENTRIES
#define PIXELICA_CART_MAX_ENTRIES 1024u
#endif

#define PIXELICA_ASSET_VERSION 1u
#define PIXELICA_ASSET_MAX_CHUNKS 16u
#define PIXELICA_CART_NAME_BYTES 32u

/* Four-character code as stored in the file: c0 is the first byte. */
#define PIXELICA_FOURCC(c0, c1, c2, c3)                                   \
    ((uint32_t)(uint8_t)(c0) | ((uint32_t)(uint8_t)(c1) << 8) |      \
     ((uint32_t)(uint8_t)(c2) << 16) | ((uint32_t)(uint8_t)(c3) << 24))

#define PIXELICA_ASSET_KIND_MESH PIXELICA_FOURCC('P', '3', 'M', 'S')
#define PIXELICA_ASSET_KIND_TEXTURE PIXELICA_FOURCC('P', '3', 'T', 'X')
#define PIXELICA_ASSET_KIND_CART PIXELICA_FOURCC('P', '3', 'C', 'T')

/* IEEE 802.3 / zlib CRC-32 of `size` bytes (0 for NULL). */
uint32_t pixelica_crc32(const void *data, size_t size);

/* Validates the whole file; *out_bytes = storage needed by
 * pixelica_texture_asset_load() (0 on failure). */
PIXELICA_Status pixelica_texture_asset_memory_required(const void *data, size_t size,
                                             size_t *out_bytes);

/* out->pixels points into storage (which must outlive the texture). */
PIXELICA_Status pixelica_texture_asset_load(PIXELICA_Texture *out, const void *data,
                                  size_t size, void *storage,
                                  size_t storage_size);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_ASSETS_COMMON_H */

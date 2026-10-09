/*
 * Pixelica - asset format v1 loaders (.p3mesh, .p3tex, .p3cart).
 *
 * Container (all three formats), little-endian, no padding:
 *   header (16 bytes)
 *     magic[4]          "P3MS" / "P3TX" / "P3CT"
 *     version:u16       1
 *     header_size:u16   16 + 16 * chunk_count
 *     file_size:u32     exact size of the whole file
 *     chunk_count:u32   1..16
 *   chunk table, chunk_count entries of 16 bytes
 *     type[4], offset:u32 (absolute), length:u32, crc32:u32 (IEEE/zlib of
 *     the payload)
 *   payloads, tightly packed in table order: the first starts at
 *   header_size, each next one at the previous end, the last ends at
 *   file_size (no gaps, overlap or trailing bytes).
 * Duplicate chunk types and chunk types not listed for the format are
 * rejected (v1 has no optional-chunk mechanism). Floats are IEEE-754
 * binary32. See docs/asset_format.md for the full specification.
 *
 * .p3mesh ("P3MS"): VTX0 = vertex_count * (x,y,z f32), IDX0 = u16 triangle
 *   list, COL0 = one RGB565 u16 per triangle, optional UV00 = vertex_count *
 *   (u,v f32). Counts derive from lengths. Rules: vertices and indices
 *   non-empty, index_count % 3 == 0, every index < vertex_count, positions
 *   finite, UVs finite and inside [0, 1]. No texture is referenced by the
 *   file; the caller binds one.
 * .p3tex ("P3TX"): INFO = width:u16, height:u16, reserved:u32 (= 0),
 *   DATA = width * height RGB565 u16, row 0 = top.
 * .p3cart ("P3CT"): ENTR = count:u32 + count records of 48 bytes:
 *   resource_id:u32 (non-zero), kind[4] ("P3MS"/"P3TX"), name[32]
 *   (1..31 chars of [A-Za-z0-9_.-], then NUL, then zero padding),
 *   offset:u32 (relative to DATA), length:u32. DATA = the complete nested
 *   resource files, tightly packed in ENTR order. Records are sorted by
 *   strictly ascending id; names are unique; no empty resources.
 *
 * Error codes:
 *   PIXELICA_ERR_INVALID_ARG  NULL pointers, or `data` and `storage` overlap.
 *   PIXELICA_ERR_BAD_ASSET    any format violation (including CRC mismatch).
 *   PIXELICA_ERR_CAPACITY     a well-formed file exceeding a configured limit
 *                        (PIXELICA_ASSET_MAX_* / PIXELICA_CART_MAX_ENTRIES).
 *   PIXELICA_ERR_NO_MEMORY    storage_size smaller than *_memory_required().
 *   PIXELICA_ERR_NOT_FOUND    pixelica_cart_find(): no resource with that id.
 *
 * Guarantees:
 *   - The whole input is validated (structure, CRCs, geometry) before
 *     caller storage or output is changed. On failure *out is zeroed and
 *     storage is left untouched.
 *   - Input bytes may have any alignment. Mesh/texture loaders copy decoded
 *     values into caller storage of any alignment (the required size
 *     includes alignment slack). No heap use and no file I/O.
 *   - `data` and `storage` must not overlap (rejected with
 *     PIXELICA_ERR_INVALID_ARG). After loading, the asset does not reference
 *     `data`; it references `storage`, which must outlive it.
 *   - A cart is a zero-copy view: `data` must outlive the cart and every
 *     pointer obtained from it, and must not be modified.
 *
 * Portability: decoding requires IEEE-754 binary32 floats with the same
 * byte order as uint32_t (true for every supported target); other hosts fail
 * to compile.
 */
#ifndef PIXELICA_ASSETS_H
#define PIXELICA_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/assets_common.h" /* limits, FOURCC, kinds, pixelica_crc32, .p3tex loader */
#include "pixelica/renderer.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A loaded mesh. mesh.vertices/indices and material.triangle_colors/uvs
 * point into the caller's storage; mesh.color is 0xFFFF; material.uvs is
 * NULL when the file has no UV00 chunk; material.texture is NULL (bind a
 * texture by setting it). Draw with pixelica_draw_mesh_ex(ctx, &a.mesh, model,
 * &a.material).
 */
typedef struct PIXELICA_MeshAsset {
    PIXELICA_Mesh mesh;
    PIXELICA_Material material;
} PIXELICA_MeshAsset;

/* Validates the whole file; *out_bytes = storage needed by
 * pixelica_mesh_asset_load() (0 on failure). */
PIXELICA_Status pixelica_mesh_asset_memory_required(const void *data, size_t size,
                                          size_t *out_bytes);
PIXELICA_Status pixelica_mesh_asset_load(PIXELICA_MeshAsset *out, const void *data,
                               size_t size, void *storage,
                               size_t storage_size);

/* Textures (pixelica_texture_asset_memory_required / pixelica_texture_asset_load) follow
 * the same contract and are declared in assets_common.h, included above. */

/* Zero-copy cart view (zeroed when invalid). */
typedef struct PIXELICA_Cart {
    const uint8_t *data;
    size_t size;
    uint32_t count;
} PIXELICA_Cart;

/* Directory record of one cart resource. */
typedef struct PIXELICA_CartEntry {
    uint32_t id;
    uint32_t kind; /* PIXELICA_ASSET_KIND_MESH or PIXELICA_ASSET_KIND_TEXTURE */
    char name[PIXELICA_CART_NAME_BYTES]; /* NUL-terminated */
    const void *data;               /* complete nested file */
    size_t size;
} PIXELICA_CartEntry;

/* Validates the cart and every nested resource completely. */
PIXELICA_Status pixelica_cart_open(PIXELICA_Cart *out, const void *data, size_t size);

/* Complete nested resource file for `id` (pass it to the matching loader).
 * Outputs are zeroed/NULL on failure; PIXELICA_ERR_NOT_FOUND if absent. */
PIXELICA_Status pixelica_cart_find(const PIXELICA_Cart *cart, uint32_t id,
                         const void **out_data, size_t *out_size);

/* Record `index` (0..count-1, ascending id order). Zeroed on failure;
 * PIXELICA_ERR_NOT_FOUND when index >= count. */
PIXELICA_Status pixelica_cart_entry(const PIXELICA_Cart *cart, uint32_t index,
                          PIXELICA_CartEntry *out);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_ASSETS_H */

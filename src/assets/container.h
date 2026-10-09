/* Pixelica - private v1 asset container parsing (see include/pixelica/assets.h and
 * assets_common.h). Shared by the common texture loader and the 3D mesh/cart
 * loaders; it includes only the common asset header. */
#ifndef PIXELICA_ASSET_CONTAINER_H
#define PIXELICA_ASSET_CONTAINER_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/assets_common.h"

#define PIXELICA_ASSET_HEADER_BYTES 16u
#define PIXELICA_ASSET_CHUNK_ENTRY_BYTES 16u

#define PIXELICA_CHUNK_VTX0 PIXELICA_FOURCC('V', 'T', 'X', '0')
#define PIXELICA_CHUNK_IDX0 PIXELICA_FOURCC('I', 'D', 'X', '0')
#define PIXELICA_CHUNK_COL0 PIXELICA_FOURCC('C', 'O', 'L', '0')
#define PIXELICA_CHUNK_UV00 PIXELICA_FOURCC('U', 'V', '0', '0')
#define PIXELICA_CHUNK_INFO PIXELICA_FOURCC('I', 'N', 'F', 'O')
#define PIXELICA_CHUNK_DATA PIXELICA_FOURCC('D', 'A', 'T', 'A')
#define PIXELICA_CHUNK_ENTR PIXELICA_FOURCC('E', 'N', 'T', 'R')

/* Alignment the loaders give decoded arrays inside caller storage. */
#define PIXELICA_ASSET_STORAGE_ALIGN 16u

typedef struct PIXELICA_ChunkView {
    uint32_t type;
    const uint8_t *data; /* inside the file; any alignment */
    uint32_t length;
} PIXELICA_ChunkView;

typedef struct PIXELICA_ContainerView {
    uint32_t chunk_count;
    PIXELICA_ChunkView chunks[PIXELICA_ASSET_MAX_CHUNKS];
} PIXELICA_ContainerView;

uint16_t pixelica_rd_u16(const uint8_t *p);
uint32_t pixelica_rd_u32(const uint8_t *p);
float pixelica_rd_f32(const uint8_t *p);

/*
 * Parses and validates the container: size limits, header fields, chunk
 * table (allowed and unique types, exact tight packing, no trailing bytes)
 * and, when verify_crc != 0, every chunk CRC. Does not check that required
 * chunks are present. PIXELICA_OK, PIXELICA_ERR_BAD_ASSET or PIXELICA_ERR_CAPACITY (file
 * larger than PIXELICA_ASSET_MAX_FILE_BYTES). `out` is filled only on success.
 */
PIXELICA_Status pixelica_container_parse(const uint8_t *data, size_t size,
                               uint32_t magic, const uint32_t *allowed,
                               size_t allowed_count, int verify_crc,
                               PIXELICA_ContainerView *out);

/* Chunk of `type` or NULL. */
const PIXELICA_ChunkView *pixelica_container_find(const PIXELICA_ContainerView *view,
                                        uint32_t type);

/* 1 if the byte ranges [a, a+a_size) and [b, b+b_size) share a byte. */
int pixelica_ranges_overlap(const void *a, size_t a_size, const void *b,
                       size_t b_size);

/* Bytes from `storage` to the next PIXELICA_ASSET_STORAGE_ALIGN boundary. */
size_t pixelica_align_pad(const void *storage);

/* Full validation of a nested/standalone file, no output. */
PIXELICA_Status pixelica_mesh_asset_validate(const uint8_t *data, size_t size);
PIXELICA_Status pixelica_texture_asset_validate(const uint8_t *data, size_t size);

#endif /* PIXELICA_ASSET_CONTAINER_H */

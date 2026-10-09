/*
 * Pixelica - v1 asset container: little-endian readers, CRC-32 and the
 * header/chunk-table validator shared by every asset format.
 *
 * Every offset/length is bounds-checked before it is used in arithmetic.
 * The file size is limited to PIXELICA_ASSET_MAX_FILE_BYTES (< 2^32), so after
 * the first check all sums below fit in size_t and uint32_t.
 */
#include <float.h>
#include <string.h>

#include "container.h"

/* The decoder reinterprets the 32 bits of a little-endian u32 as a float. */
typedef char pixelica_ieee_float_check
    [(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128 &&
      sizeof(float) == sizeof(uint32_t)) ? 1 : -1];

#if PIXELICA_ASSET_MAX_FILE_BYTES > 0xFFFFFFFFu || PIXELICA_ASSET_MAX_FILE_BYTES < 16u
#error "PIXELICA_ASSET_MAX_FILE_BYTES must be in 16..0xFFFFFFFF"
#endif

/* CRC-32 (reflected polynomial 0xEDB88320), one nibble at a time. */
static const uint32_t k_crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t pixelica_crc32(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;

    if (p == NULL) {
        return 0;
    }
    for (i = 0; i < size; ++i) {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ k_crc_nibble[crc & 0x0Fu];
        crc = (crc >> 4) ^ k_crc_nibble[crc & 0x0Fu];
    }
    return crc ^ 0xFFFFFFFFu;
}

uint16_t pixelica_rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

uint32_t pixelica_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

float pixelica_rd_f32(const uint8_t *p)
{
    uint32_t bits = pixelica_rd_u32(p);
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

PIXELICA_Status pixelica_container_parse(const uint8_t *data, size_t size,
                               uint32_t magic, const uint32_t *allowed,
                               size_t allowed_count, int verify_crc,
                               PIXELICA_ContainerView *out)
{
    PIXELICA_ContainerView view;
    uint32_t chunk_count;
    size_t header_size;
    size_t expected;
    uint32_t i;

    if (data == NULL || out == NULL) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (size > (size_t)PIXELICA_ASSET_MAX_FILE_BYTES) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (size < PIXELICA_ASSET_HEADER_BYTES) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (pixelica_rd_u32(data) != magic ||
        (uint32_t)pixelica_rd_u16(data + 4) != PIXELICA_ASSET_VERSION ||
        (size_t)pixelica_rd_u32(data + 8) != size) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    chunk_count = pixelica_rd_u32(data + 12);
    if (chunk_count == 0 || chunk_count > PIXELICA_ASSET_MAX_CHUNKS) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    header_size = PIXELICA_ASSET_HEADER_BYTES +
                  (size_t)chunk_count * PIXELICA_ASSET_CHUNK_ENTRY_BYTES;
    if ((size_t)pixelica_rd_u16(data + 6) != header_size || header_size > size) {
        return PIXELICA_ERR_BAD_ASSET;
    }

    memset(&view, 0, sizeof view);
    view.chunk_count = chunk_count;
    expected = header_size;
    for (i = 0; i < chunk_count; ++i) {
        const uint8_t *entry = data + PIXELICA_ASSET_HEADER_BYTES +
                               (size_t)i * PIXELICA_ASSET_CHUNK_ENTRY_BYTES;
        const uint32_t type = pixelica_rd_u32(entry);
        const size_t offset = (size_t)pixelica_rd_u32(entry + 4);
        const size_t length = (size_t)pixelica_rd_u32(entry + 8);
        const uint32_t crc = pixelica_rd_u32(entry + 12);
        size_t k;
        int known = 0;

        for (k = 0; k < allowed_count; ++k) {
            if (allowed[k] == type) {
                known = 1;
            }
        }
        if (!known) {
            return PIXELICA_ERR_BAD_ASSET; /* unknown chunk type */
        }
        for (k = 0; k < i; ++k) {
            if (view.chunks[k].type == type) {
                return PIXELICA_ERR_BAD_ASSET; /* duplicate chunk */
            }
        }
        /* expected <= size holds here; check before adding. */
        if (offset != expected || length > size - offset) {
            return PIXELICA_ERR_BAD_ASSET; /* gap, overlap or out of bounds */
        }
        if (verify_crc && pixelica_crc32(data + offset, length) != crc) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        view.chunks[i].type = type;
        view.chunks[i].data = data + offset;
        view.chunks[i].length = (uint32_t)length;
        expected = offset + length;
    }
    if (expected != size) {
        return PIXELICA_ERR_BAD_ASSET; /* trailing bytes */
    }
    *out = view;
    return PIXELICA_OK;
}

const PIXELICA_ChunkView *pixelica_container_find(const PIXELICA_ContainerView *view,
                                        uint32_t type)
{
    uint32_t i;

    for (i = 0; i < view->chunk_count; ++i) {
        if (view->chunks[i].type == type) {
            return &view->chunks[i];
        }
    }
    return NULL;
}

int pixelica_ranges_overlap(const void *a, size_t a_size, const void *b,
                       size_t b_size)
{
    const uintptr_t a0 = (uintptr_t)a;
    const uintptr_t b0 = (uintptr_t)b;
    const uintptr_t a1 = (a_size > UINTPTR_MAX - a0) ? UINTPTR_MAX
                                                      : a0 + a_size;
    const uintptr_t b1 = (b_size > UINTPTR_MAX - b0) ? UINTPTR_MAX
                                                      : b0 + b_size;

    if (a_size == 0 || b_size == 0) {
        return 0;
    }
    return a0 < b1 && b0 < a1;
}

size_t pixelica_align_pad(const void *storage)
{
    const uintptr_t addr = (uintptr_t)storage;
    return (size_t)((PIXELICA_ASSET_STORAGE_ALIGN -
                     (addr % PIXELICA_ASSET_STORAGE_ALIGN)) %
                    PIXELICA_ASSET_STORAGE_ALIGN);
}

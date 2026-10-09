/*
 * Pixelica - .p3tex v1 loader: validate everything, then decode the RGB565
 * texels into caller storage (first 16-byte aligned address).
 */
#include <string.h>

#include "container.h"

#if PIXELICA_ASSET_MAX_TEXTURE_DIMENSION > PIXELICA_MAX_TEXTURE_DIMENSION || \
    PIXELICA_ASSET_MAX_TEXTURE_DIMENSION < 1u
#error "PIXELICA_ASSET_MAX_TEXTURE_DIMENSION must be in 1..PIXELICA_MAX_TEXTURE_DIMENSION"
#endif

#define PIXELICA_TEX_INFO_BYTES 8u

typedef struct PIXELICA_TextureLayout {
    const uint8_t *texels; /* inside the input file, not the local view */
    uint16_t width, height;
    size_t texel_count;
} PIXELICA_TextureLayout;

static PIXELICA_Status pixelica_texture_inspect(const uint8_t *data, size_t size,
                                      PIXELICA_TextureLayout *l)
{
    static const uint32_t allowed[2] = {PIXELICA_CHUNK_INFO, PIXELICA_CHUNK_DATA};
    PIXELICA_ContainerView view;
    const PIXELICA_ChunkView *info;
    const PIXELICA_ChunkView *pixels;
    PIXELICA_Status st;

    st = pixelica_container_parse(data, size, PIXELICA_ASSET_KIND_TEXTURE, allowed, 2u,
                             1, &view);
    if (st != PIXELICA_OK) {
        return st;
    }
    info = pixelica_container_find(&view, PIXELICA_CHUNK_INFO);
    pixels = pixelica_container_find(&view, PIXELICA_CHUNK_DATA);
    if (info == NULL || pixels == NULL) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    /* `view` is local: keep a pointer into the input, not into the view. */
    l->texels = pixels->data;
    if (info->length != PIXELICA_TEX_INFO_BYTES ||
        pixelica_rd_u32(info->data + 4) != 0u) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    l->width = pixelica_rd_u16(info->data);
    l->height = pixelica_rd_u16(info->data + 2);
    if (l->width == 0 || l->height == 0) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    l->texel_count = (size_t)l->width * (size_t)l->height; /* < 2^32 */
    /* DATA length is < 2^32 bytes, so compare via division (no overflow). */
    if (pixels->length % 2u != 0 ||
        (size_t)(pixels->length / 2u) != l->texel_count) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if ((uint32_t)l->width > PIXELICA_ASSET_MAX_TEXTURE_DIMENSION ||
        (uint32_t)l->height > PIXELICA_ASSET_MAX_TEXTURE_DIMENSION) {
        return PIXELICA_ERR_CAPACITY;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_texture_asset_validate(const uint8_t *data, size_t size)
{
    PIXELICA_TextureLayout l;

    return pixelica_texture_inspect(data, size, &l);
}

PIXELICA_Status pixelica_texture_asset_memory_required(const void *data, size_t size,
                                             size_t *out_bytes)
{
    PIXELICA_TextureLayout l;
    PIXELICA_Status st;

    if (out_bytes == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_bytes = 0;
    if (data == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_texture_inspect((const uint8_t *)data, size, &l);
    if (st != PIXELICA_OK) {
        return st;
    }
    *out_bytes = l.texel_count * sizeof(uint16_t) +
                 (PIXELICA_ASSET_STORAGE_ALIGN - 1u);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_texture_asset_load(PIXELICA_Texture *out, const void *data,
                                  size_t size, void *storage,
                                  size_t storage_size)
{
    PIXELICA_TextureLayout l;
    PIXELICA_Status st;
    uint16_t *texels;
    size_t i;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (data == NULL || storage == NULL ||
        pixelica_ranges_overlap(data, size, storage, storage_size)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_texture_inspect((const uint8_t *)data, size, &l);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (storage_size < l.texel_count * sizeof(uint16_t) +
                           (PIXELICA_ASSET_STORAGE_ALIGN - 1u)) {
        return PIXELICA_ERR_NO_MEMORY;
    }

    texels = (uint16_t *)(void *)((unsigned char *)storage +
                                  pixelica_align_pad(storage));
    for (i = 0; i < l.texel_count; ++i) {
        texels[i] = pixelica_rd_u16(l.texels + i * 2u);
    }
    out->pixels = texels;
    out->width = l.width;
    out->height = l.height;
    return PIXELICA_OK;
}

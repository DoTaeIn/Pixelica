/*
 * Pixelica - .p3mesh v1 loader. Two phases: pixelica_mesh_inspect() validates
 * the whole file reading only the input bytes; decoding into caller
 * storage happens afterwards and cannot fail.
 *
 * Storage layout (from the first 16-byte aligned address in storage):
 *   PIXELICA_Vec3 vertices[vertex_count]
 *   PIXELICA_Vec2 uvs[vertex_count]          (only with UV00)
 *   uint16_t indices[index_count]
 *   uint16_t colors[index_count / 3]
 */
#include <math.h>
#include <string.h>

#include "pixelica/assets.h"

#include "container.h"

#if PIXELICA_ASSET_MAX_VERTICES > 65535u || PIXELICA_ASSET_MAX_VERTICES < 1u
#error "PIXELICA_ASSET_MAX_VERTICES must be in 1..65535 (uint16 indices)"
#endif
#if PIXELICA_ASSET_MAX_INDICES > 0x7FFFFFFFu || PIXELICA_ASSET_MAX_INDICES < 3u
#error "PIXELICA_ASSET_MAX_INDICES must be in 3..0x7FFFFFFF"
#endif

#define PIXELICA_MESH_VERTEX_BYTES 12u
#define PIXELICA_MESH_UV_BYTES 8u

typedef struct PIXELICA_MeshLayout {
    const PIXELICA_ChunkView *vtx, *idx, *col, *uv;
    uint32_t vertex_count;
    uint32_t index_count;
    size_t off_uvs, off_indices, off_colors, payload_bytes;
} PIXELICA_MeshLayout;

static PIXELICA_Status pixelica_mesh_inspect(const uint8_t *data, size_t size,
                                   PIXELICA_ContainerView *view, PIXELICA_MeshLayout *l)
{
    static const uint32_t allowed[4] = {PIXELICA_CHUNK_VTX0, PIXELICA_CHUNK_IDX0,
                                        PIXELICA_CHUNK_COL0, PIXELICA_CHUNK_UV00};
    PIXELICA_Status st;
    uint32_t i;

    st = pixelica_container_parse(data, size, PIXELICA_ASSET_KIND_MESH, allowed, 4u, 1,
                             view);
    if (st != PIXELICA_OK) {
        return st;
    }
    memset(l, 0, sizeof *l);
    l->vtx = pixelica_container_find(view, PIXELICA_CHUNK_VTX0);
    l->idx = pixelica_container_find(view, PIXELICA_CHUNK_IDX0);
    l->col = pixelica_container_find(view, PIXELICA_CHUNK_COL0);
    l->uv = pixelica_container_find(view, PIXELICA_CHUNK_UV00);
    if (l->vtx == NULL || l->idx == NULL || l->col == NULL) {
        return PIXELICA_ERR_BAD_ASSET; /* missing required chunk */
    }

    if (l->vtx->length == 0 || l->vtx->length % PIXELICA_MESH_VERTEX_BYTES != 0) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (l->idx->length == 0 || l->idx->length % 2u != 0) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    l->vertex_count = l->vtx->length / PIXELICA_MESH_VERTEX_BYTES;
    l->index_count = l->idx->length / 2u;
    if (l->index_count % 3u != 0) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (l->col->length / 2u != l->index_count / 3u ||
        l->col->length % 2u != 0) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (l->uv != NULL &&
        (l->uv->length % PIXELICA_MESH_UV_BYTES != 0 ||
         l->uv->length / PIXELICA_MESH_UV_BYTES != l->vertex_count)) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (l->vertex_count > PIXELICA_ASSET_MAX_VERTICES ||
        l->index_count > PIXELICA_ASSET_MAX_INDICES) {
        return PIXELICA_ERR_CAPACITY;
    }

    for (i = 0; i < l->vertex_count * 3u; ++i) {
        if (!isfinite(pixelica_rd_f32(l->vtx->data + (size_t)i * 4u))) {
            return PIXELICA_ERR_BAD_ASSET;
        }
    }
    for (i = 0; i < l->index_count; ++i) {
        if ((uint32_t)pixelica_rd_u16(l->idx->data + (size_t)i * 2u) >=
            l->vertex_count) {
            return PIXELICA_ERR_BAD_ASSET;
        }
    }
    if (l->uv != NULL) {
        for (i = 0; i < l->vertex_count * 2u; ++i) {
            const float t = pixelica_rd_f32(l->uv->data + (size_t)i * 4u);
            if (!(t >= 0.0f && t <= 1.0f)) { /* also rejects NaN */
                return PIXELICA_ERR_BAD_ASSET;
            }
        }
    }

    /* Bounded by the limits above: no overflow (< 2 MiB). */
    l->off_uvs = (size_t)l->vertex_count * sizeof(PIXELICA_Vec3);
    l->off_indices = l->off_uvs +
        (l->uv != NULL ? (size_t)l->vertex_count * sizeof(PIXELICA_Vec2) : 0u);
    l->off_colors = l->off_indices + (size_t)l->index_count * sizeof(uint16_t);
    l->payload_bytes = l->off_colors +
                       (size_t)(l->index_count / 3u) * sizeof(uint16_t);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_mesh_asset_validate(const uint8_t *data, size_t size)
{
    PIXELICA_ContainerView view;
    PIXELICA_MeshLayout l;

    return pixelica_mesh_inspect(data, size, &view, &l);
}

PIXELICA_Status pixelica_mesh_asset_memory_required(const void *data, size_t size,
                                          size_t *out_bytes)
{
    PIXELICA_ContainerView view;
    PIXELICA_MeshLayout l;
    PIXELICA_Status st;

    if (out_bytes == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_bytes = 0;
    if (data == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_mesh_inspect((const uint8_t *)data, size, &view, &l);
    if (st != PIXELICA_OK) {
        return st;
    }
    *out_bytes = l.payload_bytes + (PIXELICA_ASSET_STORAGE_ALIGN - 1u);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_mesh_asset_load(PIXELICA_MeshAsset *out, const void *data,
                               size_t size, void *storage,
                               size_t storage_size)
{
    PIXELICA_ContainerView view;
    PIXELICA_MeshLayout l;
    PIXELICA_Status st;
    unsigned char *base;
    PIXELICA_Vec3 *vertices;
    PIXELICA_Vec2 *uvs = NULL;
    uint16_t *indices;
    uint16_t *colors;
    uint32_t i;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (data == NULL || storage == NULL ||
        pixelica_ranges_overlap(data, size, storage, storage_size)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_mesh_inspect((const uint8_t *)data, size, &view, &l);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (storage_size < l.payload_bytes + (PIXELICA_ASSET_STORAGE_ALIGN - 1u)) {
        return PIXELICA_ERR_NO_MEMORY;
    }

    /* Validated: decode (cannot fail from here on). */
    base = (unsigned char *)storage + pixelica_align_pad(storage);
    vertices = (PIXELICA_Vec3 *)(void *)base;
    indices = (uint16_t *)(void *)(base + l.off_indices);
    colors = (uint16_t *)(void *)(base + l.off_colors);
    for (i = 0; i < l.vertex_count; ++i) {
        const uint8_t *p = l.vtx->data + (size_t)i * PIXELICA_MESH_VERTEX_BYTES;
        vertices[i].x = pixelica_rd_f32(p);
        vertices[i].y = pixelica_rd_f32(p + 4);
        vertices[i].z = pixelica_rd_f32(p + 8);
    }
    if (l.uv != NULL) {
        uvs = (PIXELICA_Vec2 *)(void *)(base + l.off_uvs);
        for (i = 0; i < l.vertex_count; ++i) {
            const uint8_t *p = l.uv->data + (size_t)i * PIXELICA_MESH_UV_BYTES;
            uvs[i].x = pixelica_rd_f32(p);
            uvs[i].y = pixelica_rd_f32(p + 4);
        }
    }
    for (i = 0; i < l.index_count; ++i) {
        indices[i] = pixelica_rd_u16(l.idx->data + (size_t)i * 2u);
    }
    for (i = 0; i < l.index_count / 3u; ++i) {
        colors[i] = pixelica_rd_u16(l.col->data + (size_t)i * 2u);
    }

    out->mesh.vertices = vertices;
    out->mesh.vertex_count = l.vertex_count;
    out->mesh.indices = indices;
    out->mesh.index_count = l.index_count;
    out->mesh.color = 0xFFFFu;
    out->material.uvs = uvs;
    out->material.triangle_colors = colors;
    out->material.texture = NULL;
    return PIXELICA_OK;
}

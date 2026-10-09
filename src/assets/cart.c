/*
 * Pixelica - .p3cart v1: zero-copy resource package view.
 *
 * pixelica_cart_open() validates the container, the directory and every nested
 * resource completely. pixelica_cart_find()/pixelica_cart_entry() re-parse the
 * header and directory without CRCs (cheap) and bounds-check everything
 * again, so they stay memory-safe even for a cart struct that was not
 * produced by pixelica_cart_open().
 */
#include <string.h>

#include "pixelica/assets.h"

#include "container.h"

#define PIXELICA_CART_RECORD_BYTES 48u
#define PIXELICA_CART_NAME_OFFSET 8u
#define PIXELICA_CART_OFFSET_OFFSET 40u
#define PIXELICA_CART_LENGTH_OFFSET 44u

#if PIXELICA_CART_MAX_ENTRIES < 1u || PIXELICA_CART_MAX_ENTRIES > 0x00FFFFFFu
#error "PIXELICA_CART_MAX_ENTRIES must be in 1..0xFFFFFF"
#endif

typedef struct PIXELICA_CartDir {
    const uint8_t *records; /* count * 48 bytes */
    uint32_t count;
    const uint8_t *payload; /* DATA chunk */
    uint32_t payload_length;
} PIXELICA_CartDir;

static int pixelica_name_char_ok(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
}

/* 1..31 allowed characters, a NUL, then only zero bytes. */
static int pixelica_name_ok(const uint8_t *name)
{
    uint32_t len = 0;
    uint32_t i;

    while (len < PIXELICA_CART_NAME_BYTES && name[len] != 0) {
        if (!pixelica_name_char_ok(name[len])) {
            return 0;
        }
        ++len;
    }
    if (len == 0 || len >= PIXELICA_CART_NAME_BYTES) {
        return 0;
    }
    for (i = len; i < PIXELICA_CART_NAME_BYTES; ++i) {
        if (name[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* Container + directory shape (no per-record checks). */
static PIXELICA_Status pixelica_cart_directory(const uint8_t *data, size_t size,
                                     int verify_crc, PIXELICA_CartDir *dir)
{
    static const uint32_t allowed[2] = {PIXELICA_CHUNK_ENTR, PIXELICA_CHUNK_DATA};
    PIXELICA_ContainerView view;
    const PIXELICA_ChunkView *entr;
    const PIXELICA_ChunkView *payload;
    uint32_t count;
    PIXELICA_Status st;

    st = pixelica_container_parse(data, size, PIXELICA_ASSET_KIND_CART, allowed, 2u,
                             verify_crc, &view);
    if (st != PIXELICA_OK) {
        return st;
    }
    entr = pixelica_container_find(&view, PIXELICA_CHUNK_ENTR);
    payload = pixelica_container_find(&view, PIXELICA_CHUNK_DATA);
    if (entr == NULL || payload == NULL || entr->length < 4u) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    count = pixelica_rd_u32(entr->data);
    if (count == 0 ||
        count > (entr->length - 4u) / PIXELICA_CART_RECORD_BYTES ||
        4u + count * PIXELICA_CART_RECORD_BYTES != entr->length) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (count > PIXELICA_CART_MAX_ENTRIES) {
        return PIXELICA_ERR_CAPACITY;
    }
    dir->records = entr->data + 4;
    dir->count = count;
    dir->payload = payload->data;
    dir->payload_length = payload->length;
    return PIXELICA_OK;
}

static const uint8_t *pixelica_record(const PIXELICA_CartDir *dir, uint32_t index)
{
    return dir->records + (size_t)index * PIXELICA_CART_RECORD_BYTES;
}

PIXELICA_Status pixelica_cart_open(PIXELICA_Cart *out, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    PIXELICA_CartDir dir;
    PIXELICA_Status st;
    uint32_t expected = 0;
    uint32_t prev_id = 0;
    uint32_t i, k;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    if (bytes == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_cart_directory(bytes, size, 1, &dir);
    if (st != PIXELICA_OK) {
        return st;
    }

    for (i = 0; i < dir.count; ++i) {
        const uint8_t *rec = pixelica_record(&dir, i);
        const uint32_t id = pixelica_rd_u32(rec);
        const uint32_t kind = pixelica_rd_u32(rec + 4);
        const uint32_t offset = pixelica_rd_u32(rec + PIXELICA_CART_OFFSET_OFFSET);
        const uint32_t length = pixelica_rd_u32(rec + PIXELICA_CART_LENGTH_OFFSET);
        const uint8_t *res;

        if (id == 0 || (i > 0 && id <= prev_id)) {
            return PIXELICA_ERR_BAD_ASSET; /* zero, unsorted or duplicate id */
        }
        prev_id = id;
        if (kind != PIXELICA_ASSET_KIND_MESH && kind != PIXELICA_ASSET_KIND_TEXTURE) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        if (!pixelica_name_ok(rec + PIXELICA_CART_NAME_OFFSET)) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        for (k = 0; k < i; ++k) {
            if (memcmp(pixelica_record(&dir, k) + PIXELICA_CART_NAME_OFFSET,
                       rec + PIXELICA_CART_NAME_OFFSET, PIXELICA_CART_NAME_BYTES) == 0) {
                return PIXELICA_ERR_BAD_ASSET; /* duplicate name */
            }
        }
        /* expected <= payload_length holds here. */
        if (offset != expected || length == 0 ||
            length > dir.payload_length - offset) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        res = dir.payload + offset;
        if (length < 4u || pixelica_rd_u32(res) != kind) {
            return PIXELICA_ERR_BAD_ASSET; /* kind does not match nested magic */
        }
        st = kind == PIXELICA_ASSET_KIND_MESH
                 ? pixelica_mesh_asset_validate(res, length)
                 : pixelica_texture_asset_validate(res, length);
        if (st != PIXELICA_OK) {
            return st == PIXELICA_ERR_CAPACITY ? PIXELICA_ERR_CAPACITY
                                          : PIXELICA_ERR_BAD_ASSET;
        }
        expected = offset + length;
    }
    if (expected != dir.payload_length) {
        return PIXELICA_ERR_BAD_ASSET; /* unreferenced trailing DATA bytes */
    }

    out->data = bytes;
    out->size = size;
    out->count = dir.count;
    return PIXELICA_OK;
}

/* Bounds-checked resource location of record `index`. */
static PIXELICA_Status pixelica_cart_locate(const PIXELICA_CartDir *dir, uint32_t index,
                                  const void **out_data, size_t *out_size)
{
    const uint8_t *rec = pixelica_record(dir, index);
    const uint32_t offset = pixelica_rd_u32(rec + PIXELICA_CART_OFFSET_OFFSET);
    const uint32_t length = pixelica_rd_u32(rec + PIXELICA_CART_LENGTH_OFFSET);

    if (offset > dir->payload_length ||
        length > dir->payload_length - offset || length == 0) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    *out_data = dir->payload + offset;
    *out_size = (size_t)length;
    return PIXELICA_OK;
}

static PIXELICA_Status pixelica_cart_reopen(const PIXELICA_Cart *cart, PIXELICA_CartDir *dir)
{
    PIXELICA_Status st;

    if (cart == NULL || cart->data == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_cart_directory(cart->data, cart->size, 0, dir);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (dir->count != cart->count) {
        return PIXELICA_ERR_INVALID_ARG; /* not the view pixelica_cart_open made */
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_cart_find(const PIXELICA_Cart *cart, uint32_t id,
                         const void **out_data, size_t *out_size)
{
    PIXELICA_CartDir dir;
    PIXELICA_Status st;
    uint32_t lo, hi;

    if (out_data != NULL) {
        *out_data = NULL;
    }
    if (out_size != NULL) {
        *out_size = 0;
    }
    if (out_data == NULL || out_size == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_cart_reopen(cart, &dir);
    if (st != PIXELICA_OK) {
        return st;
    }
    /* Records are sorted by strictly ascending id (checked on open). */
    lo = 0;
    hi = dir.count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint32_t mid_id = pixelica_rd_u32(pixelica_record(&dir, mid));

        if (mid_id == id) {
            return pixelica_cart_locate(&dir, mid, out_data, out_size);
        }
        if (mid_id < id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return PIXELICA_ERR_NOT_FOUND;
}

PIXELICA_Status pixelica_cart_entry(const PIXELICA_Cart *cart, uint32_t index,
                          PIXELICA_CartEntry *out)
{
    PIXELICA_CartDir dir;
    PIXELICA_Status st;
    const uint8_t *rec;
    const void *res_data = NULL;
    size_t res_size = 0;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    st = pixelica_cart_reopen(cart, &dir);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (index >= dir.count) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    st = pixelica_cart_locate(&dir, index, &res_data, &res_size);
    if (st != PIXELICA_OK) {
        return st;
    }
    rec = pixelica_record(&dir, index);
    out->id = pixelica_rd_u32(rec);
    out->kind = pixelica_rd_u32(rec + 4);
    memcpy(out->name, rec + PIXELICA_CART_NAME_OFFSET, PIXELICA_CART_NAME_BYTES);
    out->name[PIXELICA_CART_NAME_BYTES - 1u] = '\0';
    out->data = res_data;
    out->size = res_size;
    return PIXELICA_OK;
}

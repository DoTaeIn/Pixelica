/*
 * Pixelica - versioned save/settings container and PC file helpers.
 *
 * encode/decode/inspect are allocation-free and only touch caller memory.
 * Every length comes from a u32 field and is compared in 64-bit arithmetic
 * before it is used, so a hostile header cannot overflow size_t.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* plain fopen; every other warning stays on */
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "pixelica/save.h"

#include "save_internal.h"

#if PIXELICA_SAVE_FILE_MAX_BYTES > 0xFFFFFFFFu || PIXELICA_SAVE_FILE_MAX_BYTES < 20u
#error "PIXELICA_SAVE_FILE_MAX_BYTES must be in 20..0xFFFFFFFF"
#endif

#define SAVE_TMP_SUFFIX ".tmp"
#define SAVE_CRC_INIT 0xFFFFFFFFu

static const uint8_t k_save_magic[4] = { 'P', 'X', 'S', 'V' };

/* CRC-32 (reflected polynomial 0xEDB88320), one nibble at a time. */
static const uint32_t k_save_crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

/* Running (pre-inverted) CRC state; finish with ^ SAVE_CRC_INIT. */
static uint32_t save_crc_update(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < size; ++i) {
        crc ^= (uint32_t)p[i];
        crc = (crc >> 4) ^ k_save_crc_nibble[crc & 0x0Fu];
        crc = (crc >> 4) ^ k_save_crc_nibble[crc & 0x0Fu];
    }
    return crc;
}

static uint16_t save_rd16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t save_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void save_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void save_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static void save_put_header(uint8_t h[PIXELICA_SAVE_HEADER_BYTES],
                            uint32_t schema_version, uint32_t payload_size)
{
    memcpy(h, k_save_magic, sizeof k_save_magic);
    save_wr16(h + 4, (uint16_t)PIXELICA_SAVE_FORMAT_VERSION);
    save_wr16(h + 6, 0);
    save_wr32(h + 8, schema_version);
    save_wr32(h + 12, payload_size);
}

/* Magic, container format and reserved field (not length or CRC). */
static int save_header_ok(const uint8_t *h)
{
    return memcmp(h, k_save_magic, sizeof k_save_magic) == 0 &&
           save_rd16(h + 4) == PIXELICA_SAVE_FORMAT_VERSION &&
           save_rd16(h + 6) == 0;
}

/* Read-only full validation of an encoded block. */
static PIXELICA_Status save_validate(const uint8_t *p, size_t size,
                                     uint32_t *schema_version, uint32_t *payload_size)
{
    uint32_t len;
    uint32_t crc;

    if (size < PIXELICA_SAVE_OVERHEAD_BYTES || !save_header_ok(p)) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    len = save_rd32(p + 12);
    if ((uint64_t)size != (uint64_t)len + PIXELICA_SAVE_OVERHEAD_BYTES) {
        return PIXELICA_ERR_BAD_ASSET; /* truncated or trailing bytes */
    }
    crc = save_crc_update(SAVE_CRC_INIT, p, size - 4u) ^ SAVE_CRC_INIT;
    if (crc != save_rd32(p + size - 4u)) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    *schema_version = save_rd32(p + 8);
    *payload_size = len;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_save_encoded_size(size_t payload_size, size_t *out_size)
{
    if (out_size == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (payload_size > PIXELICA_SAVE_MAX_PAYLOAD_BYTES) {
        return PIXELICA_ERR_CAPACITY;
    }
    *out_size = payload_size + PIXELICA_SAVE_OVERHEAD_BYTES;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_save_encode(uint32_t schema_version, const void *payload,
                                     size_t payload_size, void *out,
                                     size_t out_capacity, size_t *out_size)
{
    uint8_t header[PIXELICA_SAVE_HEADER_BYTES];
    uint8_t *dst = (uint8_t *)out;
    size_t need;
    uint32_t crc;
    PIXELICA_Status st;

    if (out_size == NULL || (payload == NULL && payload_size != 0) ||
        (out == NULL && out_capacity != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_save_encoded_size(payload_size, &need);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (out_capacity < need) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    /* Move the payload first: it may overlap the header bytes of `out`. */
    if (payload_size != 0) {
        memmove(dst + PIXELICA_SAVE_HEADER_BYTES, payload, payload_size);
    }
    save_put_header(header, schema_version, (uint32_t)payload_size);
    memcpy(dst, header, sizeof header);
    crc = save_crc_update(SAVE_CRC_INIT, header, sizeof header);
    crc = save_crc_update(crc, dst + PIXELICA_SAVE_HEADER_BYTES, payload_size);
    save_wr32(dst + PIXELICA_SAVE_HEADER_BYTES + payload_size, crc ^ SAVE_CRC_INIT);
    *out_size = need;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_save_inspect(const void *data, size_t data_size,
                                      uint32_t *out_schema_version,
                                      size_t *out_payload_size)
{
    uint32_t schema;
    uint32_t len;
    PIXELICA_Status st;

    if (data == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = save_validate((const uint8_t *)data, data_size, &schema, &len);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (out_schema_version != NULL) {
        *out_schema_version = schema;
    }
    if (out_payload_size != NULL) {
        *out_payload_size = (size_t)len;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_save_decode(const void *data, size_t data_size,
                                     uint32_t *out_schema_version,
                                     void *out_payload, size_t payload_capacity,
                                     size_t *out_payload_size)
{
    uint32_t schema;
    uint32_t len;
    PIXELICA_Status st;

    if (data == NULL || out_payload_size == NULL ||
        (out_payload == NULL && payload_capacity != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = save_validate((const uint8_t *)data, data_size, &schema, &len);
    if (st != PIXELICA_OK) {
        return st;
    }
    if ((size_t)len > payload_capacity) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    if (len != 0) {
        memmove(out_payload, (const uint8_t *)data + PIXELICA_SAVE_HEADER_BYTES,
                (size_t)len);
    }
    if (out_schema_version != NULL) {
        *out_schema_version = schema;
    }
    *out_payload_size = (size_t)len;
    return PIXELICA_OK;
}

/* ---- PC file helpers --------------------------------------------------- */

/* Replaces `path` with `tmp` without deleting `path` first. */
static int save_replace(const char *tmp, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

/* One piece of the file to write. A zero-size piece may have a NULL pointer. */
typedef struct SaveChunk {
    const void *data;
    size_t size;
} SaveChunk;

/*
 * Streams `chunk_count` chunks into "<path>.tmp" (never truncating an existing
 * temp file) and replaces `path` with it. Used by pixelica_save_file_write
 * (header / payload / CRC) and by pixelica_save_raw_write (one blob), so both
 * have exactly the same atomicity and failure behavior. `path` is assumed
 * non-empty and the total size already checked against the file limit.
 */
static PIXELICA_Status save_write_atomic(const char *path, const SaveChunk *chunks,
                                         size_t chunk_count)
{
    size_t path_len;
    size_t i;
    char *tmp;
    FILE *file;
    int ok = 1;

    path_len = strlen(path);
    if (path_len > SIZE_MAX - sizeof SAVE_TMP_SUFFIX) {
        return PIXELICA_ERR_CAPACITY;
    }
    tmp = (char *)malloc(path_len + sizeof SAVE_TMP_SUFFIX);
    if (tmp == NULL) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    memcpy(tmp, path, path_len);
    memcpy(tmp + path_len, SAVE_TMP_SUFFIX, sizeof SAVE_TMP_SUFFIX);

    file = fopen(tmp, "wbx"); /* never truncate an existing temp file or link */
    if (file == NULL) {
        free(tmp);
        return PIXELICA_ERR_PLATFORM;
    }
    for (i = 0; ok && i < chunk_count; ++i) {
        if (chunks[i].size != 0) {
            ok = fwrite(chunks[i].data, 1, chunks[i].size, file) == chunks[i].size;
        }
    }
    if (ok) {
        ok = fflush(file) == 0;
    }
    if (fclose(file) != 0) {
        ok = 0;
    }
    if (ok) {
        ok = save_replace(tmp, path);
    }
    if (!ok) {
        (void)remove(tmp);
    }
    free(tmp);
    return ok ? PIXELICA_OK : PIXELICA_ERR_PLATFORM;
}

PIXELICA_Status pixelica_save_file_write(const char *path, uint32_t schema_version,
                                         const void *payload, size_t payload_size)
{
    uint8_t header[PIXELICA_SAVE_HEADER_BYTES];
    uint8_t trailer[4];
    SaveChunk chunks[3];
    size_t total;
    uint32_t crc;
    PIXELICA_Status st;

    if (path == NULL || path[0] == '\0' || (payload == NULL && payload_size != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_save_encoded_size(payload_size, &total);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (total > PIXELICA_SAVE_FILE_MAX_BYTES) {
        return PIXELICA_ERR_CAPACITY; /* pixelica_save_file_read would reject it */
    }

    save_put_header(header, schema_version, (uint32_t)payload_size);
    crc = save_crc_update(SAVE_CRC_INIT, header, sizeof header);
    crc = save_crc_update(crc, payload, payload_size);
    save_wr32(trailer, crc ^ SAVE_CRC_INIT);

    chunks[0].data = header;
    chunks[0].size = sizeof header;
    chunks[1].data = payload;
    chunks[1].size = payload_size;
    chunks[2].data = trailer;
    chunks[2].size = sizeof trailer;
    return save_write_atomic(path, chunks, 3u);
}

PIXELICA_Status pixelica_save_raw_write(const char *path, const void *data, size_t size)
{
    SaveChunk chunk;

    if (path == NULL || path[0] == '\0' || (data == NULL && size != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (size > PIXELICA_SAVE_FILE_MAX_BYTES) {
        return PIXELICA_ERR_CAPACITY;
    }
    chunk.data = data;
    chunk.size = size;
    return save_write_atomic(path, &chunk, 1u);
}

PIXELICA_Status pixelica_save_raw_read(const char *path, void *out, size_t capacity,
                                       size_t *out_size)
{
    FILE *file;
    long end;
    size_t total;
    PIXELICA_Status st = PIXELICA_OK;

    if (path == NULL || path[0] == '\0' || out_size == NULL ||
        (out == NULL && capacity != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    errno = 0;
    file = fopen(path, "rb");
    if (file == NULL) {
#ifdef ENOENT
        if (errno == ENOENT) {
            return PIXELICA_ERR_NOT_FOUND;
        }
#endif
        return PIXELICA_ERR_PLATFORM;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        st = PIXELICA_ERR_PLATFORM;
        end = 0;
    } else {
        end = ftell(file);
        if (end < 0) {
            st = PIXELICA_ERR_PLATFORM;
            end = 0;
        }
    }
    total = (size_t)end;
    if (st == PIXELICA_OK && (uint64_t)total > (uint64_t)PIXELICA_SAVE_FILE_MAX_BYTES) {
        st = PIXELICA_ERR_CAPACITY;
    }
    if (st == PIXELICA_OK) {
        /* Report the size before the capacity check so a caller can grow. */
        *out_size = total;
        if (total > capacity) {
            st = PIXELICA_ERR_NO_MEMORY;
        } else if (fseek(file, 0, SEEK_SET) != 0) {
            st = PIXELICA_ERR_PLATFORM;
        } else if (total != 0 && fread(out, 1, total, file) != total) {
            st = PIXELICA_ERR_PLATFORM;
        }
    }
    if (fclose(file) != 0 && st == PIXELICA_OK) {
        st = PIXELICA_ERR_PLATFORM;
    }
    return st;
}

PIXELICA_Status pixelica_save_file_read(const char *path, uint32_t *out_schema_version,
                                        void *out_payload, size_t payload_capacity,
                                        size_t *out_payload_size)
{
    uint8_t header[PIXELICA_SAVE_HEADER_BYTES];
    uint8_t *buf = NULL;
    uint64_t total = 0;
    size_t rest;
    FILE *file;
    PIXELICA_Status st = PIXELICA_OK;

    if (path == NULL || path[0] == '\0' || out_payload_size == NULL ||
        (out_payload == NULL && payload_capacity != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    errno = 0;
    file = fopen(path, "rb");
    if (file == NULL) {
#ifdef ENOENT
        if (errno == ENOENT) {
            return PIXELICA_ERR_NOT_FOUND;
        }
#endif
        return PIXELICA_ERR_PLATFORM;
    }
    if (fread(header, 1, sizeof header, file) != sizeof header) {
        st = ferror(file) ? PIXELICA_ERR_PLATFORM : PIXELICA_ERR_BAD_ASSET;
    } else if (!save_header_ok(header)) {
        st = PIXELICA_ERR_BAD_ASSET;
    } else {
        total = (uint64_t)save_rd32(header + 12) + PIXELICA_SAVE_OVERHEAD_BYTES;
        if (total <= PIXELICA_SAVE_FILE_MAX_BYTES) {
            buf = (uint8_t *)malloc((size_t)total);
        }
        if (total > PIXELICA_SAVE_FILE_MAX_BYTES) {
            st = PIXELICA_ERR_CAPACITY;
        } else if (buf == NULL) {
            st = PIXELICA_ERR_NO_MEMORY;
        } else {
            memcpy(buf, header, sizeof header);
            rest = (size_t)total - sizeof header;
            if (fread(buf + sizeof header, 1, rest, file) != rest) {
                st = ferror(file) ? PIXELICA_ERR_PLATFORM : PIXELICA_ERR_BAD_ASSET;
            } else if (fgetc(file) != EOF) {
                st = PIXELICA_ERR_BAD_ASSET; /* trailing bytes */
            } else if (ferror(file)) {
                st = PIXELICA_ERR_PLATFORM;
            }
        }
    }
    if (fclose(file) != 0 && st == PIXELICA_OK) {
        st = PIXELICA_ERR_PLATFORM;
    }
    if (st == PIXELICA_OK) {
        /* Validates CRC/length again and copies only on success. */
        st = pixelica_save_decode(buf, (size_t)total, out_schema_version, out_payload,
                                  payload_capacity, out_payload_size);
    }
    free(buf);
    return st;
}

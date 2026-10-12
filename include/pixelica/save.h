/*
 * Pixelica - versioned save/settings container.
 *
 * Wraps a caller-defined payload in a small, portable binary envelope
 * (explicit little-endian, independent of struct padding or host endian):
 *
 *   offset size  field
 *   0      4     magic "PXSV"
 *   4      2     container format version (PIXELICA_SAVE_FORMAT_VERSION)
 *   6      2     reserved, must be 0
 *   8      4     schema version (chosen by the game, returned on decode)
 *   12     4     payload length in bytes (N)
 *   16     N     payload
 *   16+N   4     CRC-32 (IEEE, reflected 0xEDB88320) of bytes 0..16+N-1
 *
 * The payload is opaque bytes: serialize fields one by one in a fixed byte
 * order (see docs/save.md); do NOT persist raw structs.
 *
 * encode/decode/inspect never allocate and keep no global state. On any
 * failure every output (including the destination buffer) is left
 * byte-for-byte unchanged. Errors:
 *   PIXELICA_ERR_INVALID_ARG  NULL required pointer (a buffer may be NULL
 *                             only when its size/capacity is 0)
 *   PIXELICA_ERR_NO_MEMORY    caller buffer too small
 *   PIXELICA_ERR_CAPACITY     payload larger than the format/file limit
 *   PIXELICA_ERR_BAD_ASSET    bad magic/format/reserved field, truncated or
 *                             oversized data, length mismatch, CRC mismatch
 *   PIXELICA_ERR_PLATFORM     file open/read/write/close/replace failed
 *   PIXELICA_ERR_NOT_FOUND    file to read does not exist
 */
#ifndef PIXELICA_SAVE_H
#define PIXELICA_SAVE_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_SAVE_FORMAT_VERSION 1u
#define PIXELICA_SAVE_HEADER_BYTES 16u
/* Header plus CRC trailer: encoded size = payload size + this. */
#define PIXELICA_SAVE_OVERHEAD_BYTES 20u
/* Largest payload whose encoded size still fits in 32 bits. */
#define PIXELICA_SAVE_MAX_PAYLOAD_BYTES (0xFFFFFFFFu - PIXELICA_SAVE_OVERHEAD_BYTES)

/* Upper bound on a whole save FILE (header + payload + CRC) for the file
 * helpers below, which allocate one buffer of the file's size while
 * reading. Override when compiling src/game/save.c (20..0xFFFFFFFF). */
#ifndef PIXELICA_SAVE_FILE_MAX_BYTES
#define PIXELICA_SAVE_FILE_MAX_BYTES (16u * 1024u * 1024u)
#endif

/* *out_size = payload_size + PIXELICA_SAVE_OVERHEAD_BYTES.
 * CAPACITY if payload_size > PIXELICA_SAVE_MAX_PAYLOAD_BYTES. */
PIXELICA_Status pixelica_save_encoded_size(size_t payload_size, size_t *out_size);

/*
 * Writes the envelope for `payload` into out[0..*out_size). Fails with
 * NO_MEMORY if out_capacity is smaller than the encoded size. `payload` may
 * overlap `out` (it is moved first). `out_size` must not overlap `out`.
 */
PIXELICA_Status pixelica_save_encode(uint32_t schema_version, const void *payload,
                                     size_t payload_size, void *out,
                                     size_t out_capacity, size_t *out_size);

/*
 * Fully validates data[0..data_size) (exact length, header, CRC) and
 * reports the schema version and payload size without copying. Either
 * output pointer may be NULL.
 */
PIXELICA_Status pixelica_save_inspect(const void *data, size_t data_size,
                                      uint32_t *out_schema_version,
                                      size_t *out_payload_size);

/*
 * Fully validates `data` like inspect, then copies the payload into
 * out_payload (NO_MEMORY if payload_capacity is too small, nothing written).
 * `out_payload` may overlap `data`. out_schema_version (may be NULL) and
 * out_payload_size (required) must not overlap out_payload. The schema
 * version is NOT checked: compare it yourself and migrate if needed.
 */
PIXELICA_Status pixelica_save_decode(const void *data, size_t data_size,
                                     uint32_t *out_schema_version,
                                     void *out_payload, size_t payload_capacity,
                                     size_t *out_payload_size);

/*
 * PC file helpers (standard C stdio; Windows uses MoveFileExA).
 *
 * write: CAPACITY if the encoded size exceeds PIXELICA_SAVE_FILE_MAX_BYTES.
 * Streams the envelope into a new "<path>.tmp" in the same directory
 * (an existing temp file makes the call fail), checks every write, flush and
 * close, then replaces `path` (MoveFileExA REPLACE_EXISTING on Windows,
 * rename() elsewhere). The old file is never removed first; on failure the
 * newly created temp file is removed and `path` is untouched. A pre-existing
 * temp file is never removed. The only heap use is the
 * temp path string (strlen(path) + 5 bytes); the payload is streamed.
 *
 * read: reads the whole file (at most PIXELICA_SAVE_FILE_MAX_BYTES, else
 * CAPACITY) into one temporary heap buffer, validates it fully, and only
 * then copies into out_payload, so on any failure the destination is
 * unchanged. NOT_FOUND if the file does not exist; NO_MEMORY also if the
 * temporary buffer cannot be allocated.
 *
 * Paths are narrow (char) strings passed straight to fopen/MoveFileExA, so
 * on Windows they use the ANSI code page. Single writer per path; not
 * thread-safe for the same path; no power-loss durability guarantee.
 */
PIXELICA_Status pixelica_save_file_write(const char *path, uint32_t schema_version,
                                         const void *payload, size_t payload_size);

PIXELICA_Status pixelica_save_file_read(const char *path, uint32_t *out_schema_version,
                                        void *out_payload, size_t payload_capacity,
                                        size_t *out_payload_size);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_SAVE_H */

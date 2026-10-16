/*
 * Pixelica - file helpers shared inside pixelica_common (src/game/).
 *
 * NOT a public header: it is not installed and nothing outside src/game/ may
 * include it. save.c owns the implementation; storage_fs.c reuses it so the
 * PC storage adapter writes through exactly the same atomic
 * temp-file-and-replace path as pixelica_save_file_write().
 *
 * Both functions treat their bytes as opaque: the save envelope (magic,
 * schema version, CRC-32) is added and checked by the public save/storage
 * API, not here.
 */
#ifndef PIXELICA_SAVE_INTERNAL_H
#define PIXELICA_SAVE_INTERNAL_H

#include <stddef.h>

#include "pixelica/status.h"

/*
 * Writes data[0..size) to "<path>.tmp" in the same directory (an existing
 * temp file makes the call fail without touching it), checks every write,
 * flush and close, then replaces `path`. On failure the newly created temp
 * file is removed and `path` is left exactly as it was.
 *
 * INVALID_ARG for a NULL/empty path or NULL data with size != 0, CAPACITY for
 * size > PIXELICA_SAVE_FILE_MAX_BYTES or a path too long to extend,
 * NO_MEMORY when the temp path string cannot be allocated, PLATFORM for any
 * open/write/flush/close/replace failure.
 */
PIXELICA_Status pixelica_save_raw_write(const char *path, const void *data, size_t size);

/*
 * Reads the whole file into out[0..capacity). *out_size is set to the file's
 * size as soon as the file could be measured, so NO_MEMORY tells the caller
 * how much it needs without touching `out`; `out` may be NULL when capacity
 * is 0. An I/O failure during the read itself may leave `out` partially
 * written, so callers read into scratch memory, not into game state.
 *
 * NOT_FOUND when the file does not exist, CAPACITY when it is larger than
 * PIXELICA_SAVE_FILE_MAX_BYTES, NO_MEMORY when capacity is too small,
 * PLATFORM for any other I/O failure.
 */
PIXELICA_Status pixelica_save_raw_read(const char *path, void *out, size_t capacity,
                                       size_t *out_size);

#endif /* PIXELICA_SAVE_INTERNAL_H */

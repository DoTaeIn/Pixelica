/*
 * Pixelica - portable save storage: game id, numbered slots, platform
 * callbacks and a PC filesystem adapter.
 *
 * The module is the portable half of persistence. It owns
 *   - the GAME ID (one namespace per game, see the rules below),
 *   - the SLOT numbers (0 .. PIXELICA_STORAGE_SLOT_MAX-1),
 *   - the save ENVELOPE: every slot is stored as the versioned, CRC-32
 *     protected container of pixelica/save.h, so schema versioning and
 *     corruption detection are identical on every platform.
 * A platform supplies only a dumb blob store (PIXELICA_StorageOps): put these
 * bytes under (game id, slot), give them back, remove them, report the size.
 * It never has to know the format. pixelica/storage.h adds no heap use, no
 * global state, no renderer and no clock; every buffer is caller-owned and one
 * PIXELICA_Storage is used by one thread.
 *
 * What it does NOT do: migration between schema versions (the game branches on
 * the version it gets back), encryption, cloud sync, compression, enumerating
 * unknown game ids, directory listing, or locking between processes.
 *
 * Game id rules (pixelica_storage_game_id_valid). 1..PIXELICA_STORAGE_GAME_ID_MAX
 * bytes of 'a'-'z', '0'-'9', '-' or '_', not starting or ending with '-' / '_',
 * and not a Windows reserved device name (con, prn, aux, nul, com0..com9,
 * lpt0..lpt9). Upper case, '.', '/', '\', ':' and the empty string are refused,
 * so "..", "../../etc/passwd", "C:\\x" and ".ssh" can never become a path
 * component. An adapter re-validates the id in every callback, so a hand-built
 * PIXELICA_StorageOps call cannot escape its directory either.
 *
 * Scratch buffer: pixelica_storage_init() borrows one caller buffer that holds
 * the encoded envelope of the largest slot the game uses, i.e.
 * max payload + PIXELICA_SAVE_OVERHEAD_BYTES bytes
 * (pixelica_storage_max_payload() reports what a given buffer allows). It must
 * stay valid and untouched until shutdown; its contents after a call are
 * unspecified.
 *
 * Errors (a failed call changes nothing in the caller's buffers; a failed save
 * leaves the stored slot as it was, as far as the adapter is atomic):
 *   PIXELICA_ERR_INVALID_ARG  NULL, uninitialised/shut-down storage, bad game
 *                             id, slot >= PIXELICA_STORAGE_SLOT_MAX, required
 *                             callback missing from PIXELICA_StorageOps
 *   PIXELICA_ERR_NO_MEMORY    scratch buffer or out_payload too small
 *   PIXELICA_ERR_CAPACITY     payload over the envelope/platform limit
 *   PIXELICA_ERR_BAD_ASSET    stored bytes are corrupt (magic/length/CRC)
 *   PIXELICA_ERR_PLATFORM     a PIXELICA_StorageOps callback reported failure
 *   PIXELICA_ERR_NOT_FOUND    the slot holds nothing
 */
#ifndef PIXELICA_STORAGE_H
#define PIXELICA_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/save.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Game id length without the terminating NUL. */
#define PIXELICA_STORAGE_GAME_ID_MAX 32u
/* Valid slots are 0 .. PIXELICA_STORAGE_SLOT_MAX - 1. */
#define PIXELICA_STORAGE_SLOT_MAX 16u
/* Buffer size of the PC adapter's path strings (including the NUL). */
#define PIXELICA_STORAGE_PATH_MAX 512u

/*
 * Platform blob store. `data` is a complete Pixelica save envelope; the
 * platform stores and returns it byte-for-byte and interprets nothing.
 *
 *   write   REQUIRED. Store data[0..size) under (game_id, slot), replacing
 *           whatever was there. Should be atomic: on failure the previous
 *           content must survive. size is 20..PIXELICA_SAVE_FILE_MAX_BYTES.
 *   read    REQUIRED. NOT_FOUND when the slot is empty. Otherwise set
 *           *out_size to the stored size; copy into out[0..capacity) and
 *           return OK when capacity is big enough, else return NO_MEMORY
 *           WITHOUT writing to out (so a caller can size its buffer from
 *           *out_size). `out` may be NULL when capacity is 0.
 *   erase   optional. Remove the slot; NOT_FOUND when it was already empty.
 *           NULL makes pixelica_storage_erase() return INVALID_ARG.
 *   size    optional. Report the stored size without copying (NOT_FOUND when
 *           empty). NULL makes pixelica_storage_exists() fall back to
 *           read(..., NULL, 0, &n).
 *
 * `userdata` is borrowed and must stay valid while the storage is in use.
 * Callbacks must not call back into the PIXELICA_Storage that invoked them.
 */
typedef struct PIXELICA_StorageOps {
    void *userdata;
    PIXELICA_Status (*write)(void *userdata, const char *game_id, uint32_t slot,
                             const void *data, size_t size);
    PIXELICA_Status (*read)(void *userdata, const char *game_id, uint32_t slot,
                            void *out, size_t capacity, size_t *out_size);
    PIXELICA_Status (*erase)(void *userdata, const char *game_id, uint32_t slot);
    PIXELICA_Status (*size)(void *userdata, const char *game_id, uint32_t slot,
                            size_t *out_size);
} PIXELICA_StorageOps;

/* Caller-owned; treat every field as internal. Zero-initialise before the
 * FIRST pixelica_storage_init (PIXELICA_Storage s = {0}). */
typedef struct PIXELICA_Storage {
    uint32_t state;
    char game_id[PIXELICA_STORAGE_GAME_ID_MAX + 1u];
    PIXELICA_StorageOps ops;
    uint8_t *scratch;
    size_t scratch_size;
} PIXELICA_Storage;

/* true when `game_id` satisfies the rules in the file comment. NULL is false. */
bool pixelica_storage_game_id_valid(const char *game_id);

/*
 * Binds a game id, a platform table (copied by value; `userdata` is borrowed)
 * and a scratch buffer. `scratch` may be NULL only when scratch_size is 0,
 * which allows erase/exists but makes save/load/inspect return NO_MEMORY.
 * Do not call on a live storage; shut it down first.
 */
PIXELICA_Status pixelica_storage_init(PIXELICA_Storage *storage, const char *game_id,
                                      const PIXELICA_StorageOps *ops,
                                      void *scratch, size_t scratch_size);

/* Clears the storage (it may be init-ed again). Nothing is freed or flushed;
 * a second shutdown returns INVALID_ARG. */
PIXELICA_Status pixelica_storage_shutdown(PIXELICA_Storage *storage);

/* The game id in use, or NULL for an uninitialised storage. */
const char *pixelica_storage_game_id(const PIXELICA_Storage *storage);

/* Largest payload the bound scratch buffer can carry
 * (scratch_size - PIXELICA_SAVE_OVERHEAD_BYTES, or 0 when it is smaller). */
PIXELICA_Status pixelica_storage_max_payload(const PIXELICA_Storage *storage,
                                             size_t *out_payload_size);

/* Scratch size a payload of `payload_size` bytes needs
 * (= pixelica_save_encoded_size). Works without a storage. */
PIXELICA_Status pixelica_storage_required_scratch(size_t payload_size, size_t *out_size);

/*
 * Wraps payload[0..payload_size) in the envelope (in the scratch buffer) and
 * hands it to ops.write. `schema_version` is the game's own number, returned
 * unchanged by load/inspect. payload may be NULL when payload_size is 0.
 */
PIXELICA_Status pixelica_storage_save(PIXELICA_Storage *storage, uint32_t slot,
                                      uint32_t schema_version, const void *payload,
                                      size_t payload_size);

/*
 * Reads the slot into the scratch buffer, validates magic/length/CRC, then
 * copies the payload into out_payload. Nothing is written to out_payload
 * unless the whole call succeeds. out_schema_version may be NULL;
 * out_payload_size is required. out_payload may be NULL when
 * payload_capacity is 0 (NO_MEMORY unless the payload is empty).
 */
PIXELICA_Status pixelica_storage_load(PIXELICA_Storage *storage, uint32_t slot,
                                      uint32_t *out_schema_version, void *out_payload,
                                      size_t payload_capacity, size_t *out_payload_size);

/* Like load but copies nothing: full validation plus the schema version and
 * payload size (both outputs optional), for sizing a buffer or showing a slot
 * list. */
PIXELICA_Status pixelica_storage_inspect(PIXELICA_Storage *storage, uint32_t slot,
                                         uint32_t *out_schema_version,
                                         size_t *out_payload_size);

/* ops.erase. INVALID_ARG when the platform table has no erase callback. */
PIXELICA_Status pixelica_storage_erase(PIXELICA_Storage *storage, uint32_t slot);

/* *out_exists tells whether the slot holds anything. It says nothing about
 * whether the content is valid (use inspect for that). Uses ops.size when
 * present, otherwise a zero-capacity ops.read. */
PIXELICA_Status pixelica_storage_exists(PIXELICA_Storage *storage, uint32_t slot,
                                        bool *out_exists);

/* ---- PC filesystem adapter --------------------------------------------- */
/*
 * Stores every slot as "<root>/<game id>/save<NN>.pxsv" (NN = two decimal
 * digits) through the same atomic temp-file-and-replace path as
 * pixelica_save_file_write(): a failed write removes the temp file and leaves
 * the previous save untouched, and a read validates the envelope before it
 * copies anything.
 *
 * It creates "<root>" and "<root>/<game id>" on the first write (one level
 * each; the parents of `root` must already exist) and never deletes a
 * directory. Paths are narrow (char) strings handed to the C library, so on
 * Windows they use the ANSI code page. One writer per slot; not thread-safe
 * for the same slot; no power-loss durability guarantee (docs/save.md).
 */
typedef struct PIXELICA_StorageFs {
    char root[PIXELICA_STORAGE_PATH_MAX];
    size_t root_len;
    char path[PIXELICA_STORAGE_PATH_MAX]; /* scratch for the path being used */
} PIXELICA_StorageFs;

/*
 * Fills *out_ops with the adapter's callbacks and out_ops->userdata = fs.
 * `fs` is caller-owned, must stay at a stable address and must outlive every
 * storage that uses the table. `root` is an absolute or relative directory
 * path, copied into `fs`; empty, or too long for a slot path to fit in
 * PIXELICA_STORAGE_PATH_MAX, is INVALID_ARG. Nothing touches the filesystem
 * yet.
 */
PIXELICA_Status pixelica_storage_fs_init(PIXELICA_StorageFs *fs, const char *root,
                                         PIXELICA_StorageOps *out_ops);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_STORAGE_H */

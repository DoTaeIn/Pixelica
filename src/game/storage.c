/*
 * Pixelica - portable save storage (game id + slots + envelope) on top of a
 * platform blob store.
 *
 * Everything here is allocation-free and only touches caller memory: the
 * PIXELICA_Storage itself, the borrowed scratch buffer and the caller's
 * payload buffers. The save envelope (pixelica/save.h) is applied here, once,
 * so every platform adapter inherits the magic, schema version and CRC-32
 * check without implementing them.
 */
#include <string.h>

#include "pixelica/storage.h"

#define STORAGE_MAGIC 0x53544F52u /* "STOR": set by init, cleared by shutdown */

static bool storage_live(const PIXELICA_Storage *storage)
{
    return storage != NULL && storage->state == STORAGE_MAGIC;
}

/* Live storage and a slot inside the fixed range. */
static PIXELICA_Status storage_check(const PIXELICA_Storage *storage, uint32_t slot)
{
    if (!storage_live(storage) || slot >= PIXELICA_STORAGE_SLOT_MAX) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

static bool storage_id_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/* Windows refuses these names (with or without an extension) for a file, so
 * they can never be a game directory either. Checked lower-case only: the id
 * rules already reject upper case. */
static bool storage_id_reserved(const char *game_id, size_t len)
{
    static const char *const k_names[] = { "con", "prn", "aux", "nul" };
    size_t i;

    for (i = 0; i < sizeof k_names / sizeof k_names[0]; ++i) {
        if (len == 3u && memcmp(game_id, k_names[i], 3u) == 0) {
            return true;
        }
    }
    if (len == 4u && game_id[3] >= '0' && game_id[3] <= '9' &&
        (memcmp(game_id, "com", 3u) == 0 || memcmp(game_id, "lpt", 3u) == 0)) {
        return true;
    }
    return false;
}

bool pixelica_storage_game_id_valid(const char *game_id)
{
    size_t len;
    size_t i;

    if (game_id == NULL) {
        return false;
    }
    /* Bounded scan: never reads past the limit even for an unterminated id. */
    for (len = 0; len <= PIXELICA_STORAGE_GAME_ID_MAX; ++len) {
        if (game_id[len] == '\0') {
            break;
        }
    }
    if (len == 0u || len > PIXELICA_STORAGE_GAME_ID_MAX) {
        return false; /* empty, or longer than the limit */
    }
    for (i = 0; i < len; ++i) {
        if (!storage_id_char_ok(game_id[i])) {
            return false; /* rejects '.', '/', '\\', ':', upper case, spaces */
        }
    }
    if (game_id[0] == '-' || game_id[0] == '_' || game_id[len - 1u] == '-' ||
        game_id[len - 1u] == '_') {
        return false;
    }
    return !storage_id_reserved(game_id, len);
}

PIXELICA_Status pixelica_storage_init(PIXELICA_Storage *storage, const char *game_id,
                                      const PIXELICA_StorageOps *ops, void *scratch,
                                      size_t scratch_size)
{
    PIXELICA_Storage fresh;
    size_t len;

    if (storage == NULL || ops == NULL || ops->write == NULL || ops->read == NULL ||
        (scratch == NULL && scratch_size != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (storage->state == STORAGE_MAGIC || !pixelica_storage_game_id_valid(game_id)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(&fresh, 0, sizeof fresh);
    len = strlen(game_id);
    memcpy(fresh.game_id, game_id, len); /* len <= GAME_ID_MAX, rest stays NUL */
    fresh.ops = *ops;
    fresh.scratch = (uint8_t *)scratch;
    fresh.scratch_size = scratch_size;
    fresh.state = STORAGE_MAGIC;
    *storage = fresh;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_storage_shutdown(PIXELICA_Storage *storage)
{
    if (!storage_live(storage)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(storage, 0, sizeof *storage);
    return PIXELICA_OK;
}

const char *pixelica_storage_game_id(const PIXELICA_Storage *storage)
{
    return storage_live(storage) ? storage->game_id : NULL;
}

PIXELICA_Status pixelica_storage_max_payload(const PIXELICA_Storage *storage,
                                             size_t *out_payload_size)
{
    if (!storage_live(storage) || out_payload_size == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_payload_size = storage->scratch_size < PIXELICA_SAVE_OVERHEAD_BYTES
                            ? 0u
                            : storage->scratch_size - PIXELICA_SAVE_OVERHEAD_BYTES;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_storage_required_scratch(size_t payload_size, size_t *out_size)
{
    return pixelica_save_encoded_size(payload_size, out_size);
}

PIXELICA_Status pixelica_storage_save(PIXELICA_Storage *storage, uint32_t slot,
                                      uint32_t schema_version, const void *payload,
                                      size_t payload_size)
{
    size_t encoded = 0;
    PIXELICA_Status st;

    st = storage_check(storage, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (payload == NULL && payload_size != 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_save_encode(schema_version, payload, payload_size, storage->scratch,
                              storage->scratch_size, &encoded);
    if (st != PIXELICA_OK) {
        return st; /* NO_MEMORY: scratch too small, CAPACITY: over the format limit */
    }
    return storage->ops.write(storage->ops.userdata, storage->game_id, slot,
                              storage->scratch, encoded);
}

/* Reads the slot into the scratch buffer and validates it. */
static PIXELICA_Status storage_read_scratch(PIXELICA_Storage *storage, uint32_t slot,
                                            uint32_t *out_schema_version,
                                            size_t *out_payload_size, size_t *out_encoded)
{
    size_t encoded = 0;
    PIXELICA_Status st;

    st = storage->ops.read(storage->ops.userdata, storage->game_id, slot,
                           storage->scratch, storage->scratch_size, &encoded);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (encoded > storage->scratch_size) {
        return PIXELICA_ERR_PLATFORM; /* the adapter lied about what it copied */
    }
    *out_encoded = encoded;
    return pixelica_save_inspect(storage->scratch, encoded, out_schema_version,
                                 out_payload_size);
}

PIXELICA_Status pixelica_storage_load(PIXELICA_Storage *storage, uint32_t slot,
                                      uint32_t *out_schema_version, void *out_payload,
                                      size_t payload_capacity, size_t *out_payload_size)
{
    size_t encoded = 0;
    size_t payload_size = 0;
    uint32_t schema = 0;
    PIXELICA_Status st;

    st = storage_check(storage, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (out_payload_size == NULL || (out_payload == NULL && payload_capacity != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = storage_read_scratch(storage, slot, &schema, &payload_size, &encoded);
    if (st != PIXELICA_OK) {
        return st;
    }
    /* Validated above, so this only copies (or reports NO_MEMORY). */
    return pixelica_save_decode(storage->scratch, encoded, out_schema_version,
                                out_payload, payload_capacity, out_payload_size);
}

PIXELICA_Status pixelica_storage_inspect(PIXELICA_Storage *storage, uint32_t slot,
                                         uint32_t *out_schema_version,
                                         size_t *out_payload_size)
{
    size_t encoded = 0;
    size_t payload_size = 0;
    uint32_t schema = 0;
    PIXELICA_Status st;

    st = storage_check(storage, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    st = storage_read_scratch(storage, slot, &schema, &payload_size, &encoded);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (out_schema_version != NULL) {
        *out_schema_version = schema;
    }
    if (out_payload_size != NULL) {
        *out_payload_size = payload_size;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_storage_erase(PIXELICA_Storage *storage, uint32_t slot)
{
    PIXELICA_Status st;

    st = storage_check(storage, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (storage->ops.erase == NULL) {
        return PIXELICA_ERR_INVALID_ARG; /* the platform table does not offer it */
    }
    return storage->ops.erase(storage->ops.userdata, storage->game_id, slot);
}

PIXELICA_Status pixelica_storage_exists(PIXELICA_Storage *storage, uint32_t slot,
                                        bool *out_exists)
{
    size_t size = 0;
    PIXELICA_Status st;

    st = storage_check(storage, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (out_exists == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (storage->ops.size != NULL) {
        st = storage->ops.size(storage->ops.userdata, storage->game_id, slot, &size);
    } else {
        /* No size callback: a zero-capacity read answers the same question.
         * A non-empty slot is at least PIXELICA_SAVE_OVERHEAD_BYTES long, so
         * it always reports NO_MEMORY rather than OK. */
        st = storage->ops.read(storage->ops.userdata, storage->game_id, slot, NULL, 0u,
                               &size);
        if (st == PIXELICA_ERR_NO_MEMORY) {
            st = PIXELICA_OK;
        }
    }
    if (st == PIXELICA_ERR_NOT_FOUND) {
        *out_exists = false;
        return PIXELICA_OK;
    }
    if (st != PIXELICA_OK) {
        return st;
    }
    *out_exists = true;
    return PIXELICA_OK;
}

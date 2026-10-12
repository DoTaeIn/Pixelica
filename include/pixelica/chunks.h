/*
 * Pixelica - small caller-owned chunk manager (common library, 2D/2.5D/3D).
 *
 * Keeps a fixed set of integer chunk coordinates "loaded" so that it matches
 * a desired inclusive range, calling the game's load/unload callbacks to do
 * the real work. No heap, no filesystem, no renderer, no threads, no global
 * state. 2D games simply use z = 0 everywhere.
 *
 * What the module does NOT do: it does not store chunk data, read files,
 * draw, cull, or run anything asynchronously. The game owns chunk sizes,
 * storage, I/O, the per-frame budget and every resource. Callbacks are
 * SYNCHRONOUS: they run inside pixelica_chunks_update()/clear() and must have
 * finished their work when they return. Background loading requires a game-owned
 * queue with readiness and cancellation handling outside this module.
 *
 * Typical flow:
 *   pixelica_chunks_init(&chunks, slots, N, load, unload, game);
 *   per frame:
 *     pixelica_chunk_coord(player_pos, chunk_size, &center);   (or camera bounds)
 *     pixelica_chunk_range(center, radius, &range);
 *     pixelica_chunks_set_range(&chunks, range);               (no callbacks)
 *     pixelica_chunks_update(&chunks, budget, &ops);           (callbacks)
 *   on exit: pixelica_chunks_clear(&chunks);                   (unloads all)
 *
 * update() reconciliation order (deterministic):
 *   1. every loaded slot outside the desired range is unloaded, in slot
 *      index order;
 *   2. only when step 1 has finished, every missing coordinate of the range
 *      is loaded in z, then y, then x order (x varies fastest).
 * Each successful callback consumes one operation; max_operations == 0 means
 * unlimited. When the budget runs out update() returns OK and continues on
 * the next call. A load failure stops update() and returns the callback's
 * status: earlier operations of that call stay valid, the failed coordinate
 * stays unloaded and is retried by the next update(). Unload cannot fail.
 * Before the first set_range() update() does nothing.
 *
 * Cost: work per call is bounded by the capacity (never by the coordinate
 * space). Lookups are linear, so update() is O(capacity^2) in the worst case;
 * keep capacity modest (up to a few hundred slots).
 *
 * Errors: invalid input (NULL, uninitialised manager, min > max, int32
 * overflow, call from inside a callback) returns PIXELICA_ERR_INVALID_ARG and
 * a range larger than the capacity returns PIXELICA_ERR_CAPACITY; in both
 * cases nothing is changed or written and no callback runs. Only a load
 * failure inside update() reports partial progress (see above).
 *
 * Ownership (all borrowed, nothing is freed by the module):
 *   - PIXELICA_Chunks: caller-owned, at a stable address from init until the
 *     caller stops using it, used by one thread. Its fields are readable
 *     (`count` is the number of loaded chunks) but must not be modified.
 *   - slots: borrowed array of `capacity` entries, exclusively owned by the
 *     manager from init on; do not touch it while the manager is in use.
 *     Reading it is fine (loaded == true entries are the loaded chunks).
 *   - load/unload/userdata: borrowed, must stay valid while chunks are loaded.
 *     Both callbacks are required (pass a no-op if there is nothing to do).
 * Zero-initialize the manager before its FIRST init (PIXELICA_Chunks c = {0}).
 * Re-entrancy: from inside a callback only pixelica_chunks_contains() and
 * reading fields are allowed; init/set_range/update/clear on the same manager
 * return PIXELICA_ERR_INVALID_ARG.
 */
#ifndef PIXELICA_CHUNKS_H
#define PIXELICA_CHUNKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PIXELICA_ChunkCoord {
    int32_t x, y, z;
} PIXELICA_ChunkCoord;

/* Inclusive on both ends: min.x <= x <= max.x (same for y and z). */
typedef struct PIXELICA_ChunkRange {
    PIXELICA_ChunkCoord min;
    PIXELICA_ChunkCoord max;
} PIXELICA_ChunkRange;

typedef struct PIXELICA_ChunkSlot {
    PIXELICA_ChunkCoord coord;
    bool loaded;
} PIXELICA_ChunkSlot;

/* Return PIXELICA_OK once the chunk is ready; any other status is a failure
 * that update() returns unchanged (the chunk counts as not loaded). */
typedef PIXELICA_Status (*PIXELICA_ChunkLoadFn)(void *userdata, PIXELICA_ChunkCoord coord);
/* Release whatever load() acquired. Cannot fail. */
typedef void (*PIXELICA_ChunkUnloadFn)(void *userdata, PIXELICA_ChunkCoord coord);

typedef struct PIXELICA_Chunks {
    uint32_t state;
    bool busy;       /* inside a callback */
    bool has_target; /* set_range() was called since init/clear */
    PIXELICA_ChunkRange target;
    PIXELICA_ChunkSlot *slots;
    size_t capacity;
    size_t count; /* loaded chunks */
    PIXELICA_ChunkLoadFn load;
    PIXELICA_ChunkUnloadFn unload;
    void *userdata;
} PIXELICA_Chunks;

/* Validates everything first, then zeroes the slot array and the state. Does
 * not run callbacks. A live manager that still has loaded chunks (or is
 * inside a callback) is rejected; clear it first. capacity must be > 0. */
PIXELICA_Status pixelica_chunks_init(PIXELICA_Chunks *out, PIXELICA_ChunkSlot *slots,
                                     size_t capacity, PIXELICA_ChunkLoadFn load,
                                     PIXELICA_ChunkUnloadFn unload, void *userdata);

/* Chunk containing `position`: floor(position / chunk_size) per axis, so
 * negative positions map to negative chunks (-0.5 / 1 -> -1). Sizes may
 * differ per axis and must be finite and > 0; position must be finite; the
 * result must fit in int32. For 2D use z = 0 and any positive size.z. */
PIXELICA_Status pixelica_chunk_coord(PIXELICA_Vec3 position, PIXELICA_Vec3 chunk_size,
                                     PIXELICA_ChunkCoord *out);

/* out = [center - radius, center + radius] per axis. radius >= 0 per axis
 * (0 = only the center); the extents must fit in int32. */
PIXELICA_Status pixelica_chunk_range(PIXELICA_ChunkCoord center, PIXELICA_ChunkCoord radius,
                                     PIXELICA_ChunkRange *out);

/* Replaces the desired range (also a pending one). No callback runs; the
 * next update() applies it. min <= max per axis (INVALID_ARG) and the volume
 * must be <= capacity (CAPACITY). */
PIXELICA_Status pixelica_chunks_set_range(PIXELICA_Chunks *chunks, PIXELICA_ChunkRange range);

/* Reconciles loaded chunks with the desired range (see header comment).
 * out_operations (may be NULL, must not overlap manager/slots) receives the number of successful callbacks,
 * also when a load failure is returned. */
PIXELICA_Status pixelica_chunks_update(PIXELICA_Chunks *chunks, size_t max_operations,
                                       size_t *out_operations);

/* True if `coord` is loaded. False for NULL/uninitialised managers. */
bool pixelica_chunks_contains(const PIXELICA_Chunks *chunks, PIXELICA_ChunkCoord coord);

/* Unloads every loaded chunk (slot index order) and forgets the desired
 * range. The manager stays usable (set_range again) or may be re-inited. */
PIXELICA_Status pixelica_chunks_clear(PIXELICA_Chunks *chunks);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_CHUNKS_H */

#include "pixelica/chunks.h"

#include <math.h>
#include <string.h>

enum { STATE_LIVE = 0x43484b31u };

static bool address_range(const void *p, size_t bytes)
{
    return p != NULL && bytes <= UINTPTR_MAX - (uintptr_t)p;
}

static bool overlaps(const void *a, size_t a_bytes, const void *b, size_t b_bytes)
{
    return (uintptr_t)a < (uintptr_t)b + b_bytes &&
           (uintptr_t)b < (uintptr_t)a + a_bytes;
}

static bool is_live(const PIXELICA_Chunks *c)
{
    return c != NULL && c->state == STATE_LIVE;
}

static bool coord_equal(PIXELICA_ChunkCoord a, PIXELICA_ChunkCoord b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static bool range_has(const PIXELICA_ChunkRange *r, PIXELICA_ChunkCoord c)
{
    return c.x >= r->min.x && c.x <= r->max.x && c.y >= r->min.y && c.y <= r->max.y &&
           c.z >= r->min.z && c.z <= r->max.z;
}

static bool find_loaded(const PIXELICA_Chunks *c, PIXELICA_ChunkCoord coord)
{
    size_t i;

    for (i = 0; i < c->capacity; ++i) {
        if (c->slots[i].loaded && coord_equal(c->slots[i].coord, coord)) {
            return true;
        }
    }
    return false;
}

/* floor(p / s) for finite p and finite s > 0. fmod is exact, so the sign of r
 * decides the floor correction even when the quotient rounds to an integer. */
static bool floor_div(float p, float s, int32_t *out)
{
    double dp = (double)p;
    double ds = (double)s;
    double r = fmod(dp, ds);
    double k = round((dp - r) / ds);

    if (r < 0.0) {
        k -= 1.0;
    }
    if (!(k >= (double)INT32_MIN && k <= (double)INT32_MAX)) {
        return false;
    }
    *out = (int32_t)k;
    return true;
}

static bool size_valid(float s)
{
    return isfinite(s) && s > 0.0f;
}

PIXELICA_Status pixelica_chunks_init(PIXELICA_Chunks *out, PIXELICA_ChunkSlot *slots,
                                     size_t capacity, PIXELICA_ChunkLoadFn load,
                                     PIXELICA_ChunkUnloadFn unload, void *userdata)
{
    if (out == NULL || slots == NULL || capacity == 0u ||
        capacity > SIZE_MAX / sizeof(PIXELICA_ChunkSlot) || load == NULL || unload == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!address_range(out, sizeof *out) ||
        !address_range(slots, capacity * sizeof *slots) ||
        overlaps(out, sizeof *out, slots, capacity * sizeof *slots)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (out->state == STATE_LIVE && (out->busy || out->count != 0u)) {
        return PIXELICA_ERR_INVALID_ARG; /* would leak loaded chunks */
    }
    memset(slots, 0, capacity * sizeof *slots);
    memset(out, 0, sizeof *out);
    out->state = STATE_LIVE;
    out->slots = slots;
    out->capacity = capacity;
    out->load = load;
    out->unload = unload;
    out->userdata = userdata;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_chunk_coord(PIXELICA_Vec3 position, PIXELICA_Vec3 chunk_size,
                                     PIXELICA_ChunkCoord *out)
{
    PIXELICA_ChunkCoord c;

    if (out == NULL || !isfinite(position.x) || !isfinite(position.y) ||
        !isfinite(position.z) || !size_valid(chunk_size.x) || !size_valid(chunk_size.y) ||
        !size_valid(chunk_size.z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!floor_div(position.x, chunk_size.x, &c.x) ||
        !floor_div(position.y, chunk_size.y, &c.y) ||
        !floor_div(position.z, chunk_size.z, &c.z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = c;
    return PIXELICA_OK;
}

static bool extent(int32_t center, int32_t radius, int32_t *lo, int32_t *hi)
{
    int64_t a = (int64_t)center - (int64_t)radius;
    int64_t b = (int64_t)center + (int64_t)radius;

    if (radius < 0 || a < INT32_MIN || b > INT32_MAX) {
        return false;
    }
    *lo = (int32_t)a;
    *hi = (int32_t)b;
    return true;
}

PIXELICA_Status pixelica_chunk_range(PIXELICA_ChunkCoord center, PIXELICA_ChunkCoord radius,
                                     PIXELICA_ChunkRange *out)
{
    PIXELICA_ChunkRange r;

    if (out == NULL || !extent(center.x, radius.x, &r.min.x, &r.max.x) ||
        !extent(center.y, radius.y, &r.min.y, &r.max.y) ||
        !extent(center.z, radius.z, &r.min.z, &r.max.z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = r;
    return PIXELICA_OK;
}

static uint64_t span(int32_t lo, int32_t hi)
{
    return (uint64_t)((int64_t)hi - (int64_t)lo) + 1u; /* 1 .. 2^32 for lo <= hi */
}

/* Only for ranges already accepted by set_range (volume <= capacity). */
static size_t range_volume(const PIXELICA_ChunkRange *r)
{
    return (size_t)(span(r->min.x, r->max.x) * span(r->min.y, r->max.y) *
                    span(r->min.z, r->max.z));
}

PIXELICA_Status pixelica_chunks_set_range(PIXELICA_Chunks *chunks, PIXELICA_ChunkRange range)
{
    uint64_t ex, ey, ez;
    uint64_t cap;

    if (!is_live(chunks) || chunks->busy || range.min.x > range.max.x ||
        range.min.y > range.max.y || range.min.z > range.max.z) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* Divisions instead of products: a*b <= cap <=> a <= cap / b, no overflow. */
    ex = span(range.min.x, range.max.x);
    ey = span(range.min.y, range.max.y);
    ez = span(range.min.z, range.max.z);
    cap = (uint64_t)chunks->capacity;
    if (ex > cap / ey || ex * ey > cap / ez) {
        return PIXELICA_ERR_CAPACITY;
    }
    chunks->target = range;
    chunks->has_target = true;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_chunks_update(PIXELICA_Chunks *chunks, size_t max_operations,
                                       size_t *out_operations)
{
    PIXELICA_ChunkRange r;
    PIXELICA_Status status = PIXELICA_OK;
    size_t ops = 0;
    size_t free_slot = 0;
    size_t volume;
    size_t i;
    int64_t x, y, z;

    if (!is_live(chunks) || chunks->busy) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (out_operations != NULL &&
        (!address_range(out_operations, sizeof *out_operations) ||
         overlaps(out_operations, sizeof *out_operations, chunks, sizeof *chunks) ||
         overlaps(out_operations, sizeof *out_operations, chunks->slots,
             chunks->capacity * sizeof *chunks->slots))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!chunks->has_target) {
        if (out_operations != NULL) {
            *out_operations = 0;
        }
        return PIXELICA_OK;
    }
    r = chunks->target;
    volume = range_volume(&r); /* <= capacity, checked by set_range */
    chunks->busy = true;

    /* 1. Unload everything outside the range, slot index order. */
    for (i = 0; i < chunks->capacity; ++i) {
        PIXELICA_ChunkSlot *s = &chunks->slots[i];

        if (!s->loaded || range_has(&r, s->coord)) {
            continue;
        }
        if (max_operations != 0u && ops >= max_operations) {
            goto done;
        }
        chunks->unload(chunks->userdata, s->coord);
        s->loaded = false;
        --chunks->count;
        ++ops;
    }

    /* 2. Load missing coordinates, z/y/x order. Every loaded slot is now in
     * range and volume <= capacity, so a free slot exists for each one. */
    for (z = r.min.z; z <= r.max.z; ++z) {
        for (y = r.min.y; y <= r.max.y; ++y) {
            for (x = r.min.x; x <= r.max.x; ++x) {
                PIXELICA_ChunkCoord c;

                if (chunks->count == volume) {
                    goto done; /* range fully loaded, skip the scan */
                }
                c.x = (int32_t)x;
                c.y = (int32_t)y;
                c.z = (int32_t)z;
                if (find_loaded(chunks, c)) {
                    continue;
                }
                if (max_operations != 0u && ops >= max_operations) {
                    goto done;
                }
                while (chunks->slots[free_slot].loaded) {
                    ++free_slot;
                }
                chunks->slots[free_slot].coord = c;
                status = chunks->load(chunks->userdata, c);
                if (status != PIXELICA_OK) {
                    goto done; /* slot stays empty; retried next update */
                }
                chunks->slots[free_slot].loaded = true;
                ++chunks->count;
                ++ops;
            }
        }
    }

done:
    chunks->busy = false;
    if (out_operations != NULL) {
        *out_operations = ops;
    }
    return status;
}

bool pixelica_chunks_contains(const PIXELICA_Chunks *chunks, PIXELICA_ChunkCoord coord)
{
    return is_live(chunks) && find_loaded(chunks, coord);
}

PIXELICA_Status pixelica_chunks_clear(PIXELICA_Chunks *chunks)
{
    size_t i;

    if (!is_live(chunks) || chunks->busy) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    chunks->busy = true;
    for (i = 0; i < chunks->capacity; ++i) {
        PIXELICA_ChunkSlot *s = &chunks->slots[i];

        if (s->loaded) {
            chunks->unload(chunks->userdata, s->coord);
            s->loaded = false;
            --chunks->count;
        }
    }
    chunks->has_target = false;
    chunks->busy = false;
    return PIXELICA_OK;
}

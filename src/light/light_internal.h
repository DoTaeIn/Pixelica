/*
 * Private helpers shared by the sources in src/light (not installed).
 *
 * Address-range overlap checks with the same rules as the rest of the common
 * library (src/game/framebuffer.c): half-open byte ranges, a NULL pointer or
 * a range that would wrap the address space is never usable.
 */
#ifndef PIXELICA_LIGHT_INTERNAL_H
#define PIXELICA_LIGHT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct LtRange {
    uintptr_t lo, hi; /* half-open byte range [lo, hi) */
} LtRange;

static inline int lt_range(const void *p, size_t bytes, LtRange *r)
{
    uintptr_t lo = (uintptr_t)p;

    if (p == NULL || bytes == 0u || bytes > (size_t)UINTPTR_MAX - lo) {
        return 0;
    }
    r->lo = lo;
    r->hi = lo + (uintptr_t)bytes;
    return 1;
}

static inline int lt_overlap(const LtRange *a, const LtRange *b)
{
    return a->lo < b->hi && b->lo < a->hi;
}

#endif /* PIXELICA_LIGHT_INTERNAL_H */

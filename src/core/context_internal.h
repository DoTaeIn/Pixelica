/* Pixelica - private context layout shared by core and renderer sources. */
#ifndef PIXELICA_CONTEXT_INTERNAL_H
#define PIXELICA_CONTEXT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/pixelica.h"

#define PIXELICA_CONTEXT_MAGIC 0x50334443u /* "P3DC" */
#define PIXELICA_GUARD_BYTES 16u
#define PIXELICA_GUARD_BYTE_VALUE 0xA5u

/* Frame-arena element: one transformed vertex plus its clip outcode. */
typedef struct PIXELICA_ClipVertex {
    float x, y, z, w;
    uint32_t outcode;
} PIXELICA_ClipVertex;

struct PIXELICA_Context {
    uint32_t magic;
    PIXELICA_Config cfg;
    PIXELICA_PlatformOps platform;
    int has_platform;

    void *heap_block; /* non-NULL only when created by pixelica_create() */

    uint16_t *color;           /* width*height, row 0 = top */
    uint16_t *depth;           /* width*height */
    unsigned char *arena;      /* frame_arena_bytes, 16-byte aligned */
    unsigned char *guards[4];  /* before color, after color/depth/arena */

    float view_proj[16];
    int view_proj_valid;
    PIXELICA_CullMode cull_mode;

    int in_frame;
    uint32_t draw_count;
    PIXELICA_FrameStats stats;

    /* Fixed-timestep accumulator in units of 1/PIXELICA_UPDATE_HZ microsecond. */
    uint64_t accumulator;
};

/* 1 if ctx is non-NULL and alive. */
int pixelica_context_valid(const PIXELICA_Context *ctx);

/* Saturating counter add used for stats. */
static inline uint32_t pixelica_sat_add_u32(uint32_t a, uint32_t b)
{
    return (a > UINT32_MAX - b) ? UINT32_MAX : a + b;
}

#endif /* PIXELICA_CONTEXT_INTERNAL_H */

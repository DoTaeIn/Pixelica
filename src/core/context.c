/*
 * Pixelica - context lifetime, frame protocol and fixed timestep.
 *
 * Memory layout of the single block (offsets relative to a 16-byte aligned
 * base inside the block; every region start is 16-byte aligned except where
 * noted):
 *
 *   [PIXELICA_Context][guard0][color W*H*2][guard1][depth W*H*2][guard2]
 *   [arena frame_arena_bytes][guard3][coverage ceil(W*H/8)][guard4]
 *
 * The coverage bitmap (one bit per pixel, cleared by begin_frame) records
 * which pixels any fragment covered this frame, so pixels_covered stays
 * exact even when alpha-tested or blended fragments write no depth.
 *
 * Guards are PIXELICA_GUARD_BYTES bytes of PIXELICA_GUARD_BYTE_VALUE and are checked
 * by pixelica_memory_guards_intact().
 */
#include "context_internal.h"

#include <stdlib.h>
#include <string.h>

#define PIXELICA_ALIGN 16u

typedef struct PIXELICA_Layout {
    size_t guard0, color, guard1, depth, guard2, arena, guard3, coverage,
        guard4, total;
} PIXELICA_Layout;

/* Bitmap bytes; pixels <= PIXELICA_MAX_DIMENSION^2, so no overflow. */
static size_t pixelica_coverage_bytes(const PIXELICA_Config *cfg)
{
    return ((size_t)cfg->width * (size_t)cfg->height + 7u) / 8u;
}

static int pixelica_add_size(size_t *acc, size_t add)
{
    if (*acc > SIZE_MAX - add) {
        return 0;
    }
    *acc += add;
    return 1;
}

static int pixelica_align_size(size_t *acc)
{
    size_t rem = *acc % PIXELICA_ALIGN;
    return rem == 0 ? 1 : pixelica_add_size(acc, PIXELICA_ALIGN - rem);
}

static PIXELICA_Status pixelica_validate_config(const PIXELICA_Config *cfg)
{
    if (cfg == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (cfg->width == 0 || cfg->height == 0 ||
        (uint32_t)cfg->width > PIXELICA_MAX_DIMENSION ||
        (uint32_t)cfg->height > PIXELICA_MAX_DIMENSION) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (cfg->max_draw_commands == 0 || cfg->max_vertices_per_mesh == 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* 65535 * 20 fits comfortably in uint32_t. */
    if (cfg->frame_arena_bytes <
        (uint32_t)cfg->max_vertices_per_mesh * PIXELICA_ARENA_BYTES_PER_VERTEX) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

static PIXELICA_Status pixelica_validate_platform(const PIXELICA_PlatformOps *platform)
{
    if (platform == NULL) {
        return PIXELICA_OK;
    }
    if (platform->time_us == NULL || platform->read_buttons == NULL ||
        platform->present_rgb565 == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

static int pixelica_compute_layout(const PIXELICA_Config *cfg, PIXELICA_Layout *l)
{
    size_t pixels = (size_t)cfg->width * (size_t)cfg->height;
    size_t plane_bytes;
    size_t at = 0;

    if (pixels > SIZE_MAX / sizeof(uint16_t)) {
        return 0;
    }
    plane_bytes = pixels * sizeof(uint16_t);

    if (!pixelica_add_size(&at, sizeof(PIXELICA_Context)) || !pixelica_align_size(&at)) {
        return 0;
    }
    l->guard0 = at;
    if (!pixelica_add_size(&at, PIXELICA_GUARD_BYTES)) {
        return 0;
    }
    l->color = at;
    if (!pixelica_add_size(&at, plane_bytes)) {
        return 0;
    }
    l->guard1 = at;
    if (!pixelica_add_size(&at, PIXELICA_GUARD_BYTES)) {
        return 0;
    }
    l->depth = at; /* 2-byte aligned: plane_bytes is even, guards are 16 */
    if (!pixelica_add_size(&at, plane_bytes)) {
        return 0;
    }
    l->guard2 = at;
    if (!pixelica_add_size(&at, PIXELICA_GUARD_BYTES) || !pixelica_align_size(&at)) {
        return 0;
    }
    l->arena = at;
    if (!pixelica_add_size(&at, (size_t)cfg->frame_arena_bytes)) {
        return 0;
    }
    l->guard3 = at;
    if (!pixelica_add_size(&at, PIXELICA_GUARD_BYTES) || !pixelica_align_size(&at)) {
        return 0;
    }
    l->coverage = at;
    if (!pixelica_add_size(&at, pixelica_coverage_bytes(cfg))) {
        return 0;
    }
    l->guard4 = at;
    if (!pixelica_add_size(&at, PIXELICA_GUARD_BYTES)) {
        return 0;
    }
    l->total = at;
    return 1;
}

static void pixelica_log_platform(const PIXELICA_PlatformOps *platform, const char *msg)
{
    if (platform != NULL && platform->log != NULL) {
        platform->log(platform->userdata, msg);
    }
}

int pixelica_context_valid(const PIXELICA_Context *ctx)
{
    return ctx != NULL && ctx->magic == PIXELICA_CONTEXT_MAGIC;
}

PIXELICA_Status pixelica_memory_requirement(const PIXELICA_Config *cfg, size_t *out_bytes)
{
    PIXELICA_Layout l;
    size_t total;

    if (out_bytes == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_bytes = 0;
    if (pixelica_validate_config(cfg) != PIXELICA_OK || !pixelica_compute_layout(cfg, &l)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    total = l.total;
    if (!pixelica_add_size(&total, PIXELICA_ALIGN - 1u)) { /* slack for any alignment */
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_bytes = total;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_create_in_buffer(const PIXELICA_Config *cfg,
                                const PIXELICA_PlatformOps *platform,
                                void *memory, size_t memory_bytes,
                                PIXELICA_Context **out_ctx)
{
    PIXELICA_Layout l;
    size_t required;
    uintptr_t addr;
    size_t pad;
    unsigned char *base;
    PIXELICA_Context *ctx;
    size_t pixels;
    size_t i;

    if (out_ctx == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_ctx = NULL;
    if (memory == NULL || pixelica_validate_config(cfg) != PIXELICA_OK ||
        pixelica_validate_platform(platform) != PIXELICA_OK) {
        pixelica_log_platform(platform, "pixelica: invalid create arguments");
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (pixelica_memory_requirement(cfg, &required) != PIXELICA_OK ||
        !pixelica_compute_layout(cfg, &l)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (memory_bytes < required) {
        pixelica_log_platform(platform, "pixelica: memory block too small");
        return PIXELICA_ERR_NO_MEMORY;
    }

    addr = (uintptr_t)memory;
    pad = (size_t)((PIXELICA_ALIGN - (addr % PIXELICA_ALIGN)) % PIXELICA_ALIGN);
    base = (unsigned char *)memory + pad;

    ctx = (PIXELICA_Context *)(void *)base;
    memset(ctx, 0, sizeof *ctx);
    ctx->cfg = *cfg;
    if (platform != NULL) {
        ctx->platform = *platform;
        ctx->has_platform = 1;
    }
    ctx->color = (uint16_t *)(void *)(base + l.color);
    ctx->depth = (uint16_t *)(void *)(base + l.depth);
    ctx->arena = base + l.arena;
    ctx->coverage = base + l.coverage;
    ctx->guards[0] = base + l.guard0;
    ctx->guards[1] = base + l.guard1;
    ctx->guards[2] = base + l.guard2;
    ctx->guards[3] = base + l.guard3;
    ctx->guards[4] = base + l.guard4;
    for (i = 0; i < PIXELICA_GUARD_COUNT; ++i) {
        memset(ctx->guards[i], (int)PIXELICA_GUARD_BYTE_VALUE, PIXELICA_GUARD_BYTES);
    }

    /* Deterministic initial contents: black color, cleared depth. */
    pixels = (size_t)cfg->width * (size_t)cfg->height;
    for (i = 0; i < pixels; ++i) {
        ctx->color[i] = 0;
        ctx->depth[i] = PIXELICA_DEPTH_CLEAR;
    }
    memset(ctx->arena, 0, cfg->frame_arena_bytes);
    memset(ctx->coverage, 0, pixelica_coverage_bytes(cfg));

    pixelica_mat4_identity(ctx->view_proj);
    ctx->view_proj_valid = 1;
    ctx->cull_mode = PIXELICA_CULL_BACK;
    ctx->magic = PIXELICA_CONTEXT_MAGIC;
    *out_ctx = ctx;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_create(const PIXELICA_Config *cfg,
                      const PIXELICA_PlatformOps *platform,
                      PIXELICA_Context **out_ctx)
{
    size_t required;
    void *block;
    PIXELICA_Status st;

    if (out_ctx == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_ctx = NULL;
    if (pixelica_validate_config(cfg) != PIXELICA_OK ||
        pixelica_validate_platform(platform) != PIXELICA_OK ||
        pixelica_memory_requirement(cfg, &required) != PIXELICA_OK) {
        pixelica_log_platform(platform, "pixelica: invalid create arguments");
        return PIXELICA_ERR_INVALID_ARG;
    }
    block = malloc(required);
    if (block == NULL) {
        pixelica_log_platform(platform, "pixelica: out of memory");
        return PIXELICA_ERR_NO_MEMORY;
    }
    st = pixelica_create_in_buffer(cfg, platform, block, required, out_ctx);
    if (st != PIXELICA_OK) {
        free(block);
        *out_ctx = NULL;
        return st;
    }
    (*out_ctx)->heap_block = block;
    return PIXELICA_OK;
}

void pixelica_destroy(PIXELICA_Context *ctx)
{
    void *block;

    if (!pixelica_context_valid(ctx) || ctx->pixel_callback_active) {
        return;
    }
    block = ctx->heap_block;
    ctx->magic = 0;
    free(block); /* free(NULL) is a no-op for caller-owned memory */
}

void pixelica_set_view_projection(PIXELICA_Context *ctx, const float matrix[16])
{
    if (!pixelica_context_valid(ctx) || ctx->pixel_callback_active) {
        return;
    }
    if (!pixelica_mat4_is_finite(matrix)) {
        ctx->view_proj_valid = 0;
        return;
    }
    memcpy(ctx->view_proj, matrix, sizeof ctx->view_proj);
    ctx->view_proj_valid = 1;
}

void pixelica_set_cull_mode(PIXELICA_Context *ctx, PIXELICA_CullMode mode)
{
    if (!pixelica_context_valid(ctx) || ctx->pixel_callback_active) {
        return;
    }
    if (mode == PIXELICA_CULL_BACK || mode == PIXELICA_CULL_NONE) {
        ctx->cull_mode = mode;
    }
}

void pixelica_begin_frame(PIXELICA_Context *ctx, uint16_t clear_color)
{
    size_t pixels;
    size_t i;

    if (!pixelica_context_valid(ctx) || ctx->pixel_callback_active) {
        return;
    }
    pixels = (size_t)ctx->cfg.width * (size_t)ctx->cfg.height;
    for (i = 0; i < pixels; ++i) {
        ctx->color[i] = clear_color;
        ctx->depth[i] = PIXELICA_DEPTH_CLEAR;
    }
    memset(ctx->coverage, 0, pixelica_coverage_bytes(&ctx->cfg));
    ctx->draw_count = 0;
    memset(&ctx->stats, 0, sizeof ctx->stats);
    ctx->stats.last_error = PIXELICA_OK;
    ctx->in_frame = 1;
}

PIXELICA_Status pixelica_end_frame(PIXELICA_Context *ctx)
{
    PIXELICA_Status st = PIXELICA_OK;

    if (!pixelica_context_valid(ctx) || ctx->pixel_callback_active) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!ctx->in_frame) {
        ctx->stats.last_error = PIXELICA_ERR_INVALID_ARG;
        return PIXELICA_ERR_INVALID_ARG;
    }
    ctx->in_frame = 0;
    if (ctx->has_platform) {
        if (!ctx->platform.present_rgb565(ctx->platform.userdata, ctx->color,
                                          ctx->cfg.width, ctx->cfg.height,
                                          (uint32_t)ctx->cfg.width)) {
            st = PIXELICA_ERR_PLATFORM;
            ctx->stats.last_error = st;
        }
    }
    return st;
}

const uint16_t *pixelica_color_buffer(const PIXELICA_Context *ctx)
{
    return pixelica_context_valid(ctx) ? ctx->color : NULL;
}

const uint16_t *pixelica_depth_buffer(const PIXELICA_Context *ctx)
{
    return pixelica_context_valid(ctx) ? ctx->depth : NULL;
}

PIXELICA_FrameStats pixelica_frame_stats(const PIXELICA_Context *ctx)
{
    PIXELICA_FrameStats zero;

    if (pixelica_context_valid(ctx)) {
        return ctx->stats;
    }
    memset(&zero, 0, sizeof zero);
    return zero;
}

void pixelica_update(PIXELICA_Context *ctx, uint64_t elapsed_us,
                PIXELICA_UpdateFn update, void *userdata)
{
    const uint64_t step_units = 1000000u; /* one step in 1/60 us units */
    const float dt = 1.0f / (float)PIXELICA_UPDATE_HZ;
    uint32_t buttons = 0;
    uint64_t steps;
    uint64_t i;

    if (!pixelica_context_valid(ctx) || ctx->pixel_callback_active || update == NULL) {
        return;
    }
    if (ctx->has_platform) {
        buttons = ctx->platform.read_buttons(ctx->platform.userdata);
    }
    if (elapsed_us > PIXELICA_MAX_ELAPSED_US) {
        elapsed_us = PIXELICA_MAX_ELAPSED_US;
    }
    /* accumulator < step_units on entry, so this is < 6.2e7: no overflow. */
    ctx->accumulator += elapsed_us * (uint64_t)PIXELICA_UPDATE_HZ;
    steps = ctx->accumulator / step_units;
    if (steps > PIXELICA_MAX_UPDATES_PER_CALL) {
        steps = PIXELICA_MAX_UPDATES_PER_CALL;
        ctx->accumulator %= step_units; /* drop excess whole steps */
    } else {
        ctx->accumulator -= steps * step_units;
    }
    for (i = 0; i < steps; ++i) {
        update(userdata, dt, buttons);
    }
}

int pixelica_memory_guards_intact(const PIXELICA_Context *ctx)
{
    size_t g;
    size_t i;

    if (!pixelica_context_valid(ctx)) {
        return 0;
    }
    for (g = 0; g < PIXELICA_GUARD_COUNT; ++g) {
        for (i = 0; i < PIXELICA_GUARD_BYTES; ++i) {
            if (ctx->guards[g][i] != PIXELICA_GUARD_BYTE_VALUE) {
                return 0;
            }
        }
    }
    return 1;
}

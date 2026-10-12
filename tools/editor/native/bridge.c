/* Pixelica editor bridge implementation. See bridge.h for the contract. */
#ifndef PE_BRIDGE_BUILD
#define PE_BRIDGE_BUILD 1
#endif
#include "bridge.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pixelica/renderer2d.h"

#define PE_MAGIC 0x50454231u /* "PEB1" */

typedef struct pe_texture_slot {
    uint16_t *texels;
    uint8_t *alpha;
    size_t bytes;
    PIXELICA_Texture texture;
    uint16_t generation;
    bool used;
} pe_texture_slot;

struct pe_bridge {
    uint32_t magic;
    bool frame_started;
    uint16_t *pixels; /* separate heap block: never overlaps ctx or textures */
    size_t pixel_count;
    PIXELICA2D_Context ctx;
    uint16_t solid_texel; /* 1x1 texture stretched for solid rectangles */
    PIXELICA_Texture solid;
    uint32_t texture_count;
    size_t texture_bytes;
    pe_texture_slot slots[PE_MAX_TEXTURES];
};

static bool bridge_valid(const pe_bridge *bridge)
{
    return bridge != NULL && bridge->magic == PE_MAGIC && bridge->pixels != NULL;
}

static uint16_t rgb_to_565(uint32_t rgb)
{
    return PIXELICA_RGB565((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
}

static int status_from_engine(PIXELICA_Status status)
{
    switch (status) {
    case PIXELICA_OK: return PE_OK;
    case PIXELICA_ERR_INVALID_ARG: return PE_ERR_INVALID_ARG;
    case PIXELICA_ERR_NO_MEMORY: return PE_ERR_NO_MEMORY;
    case PIXELICA_ERR_CAPACITY: return PE_ERR_CAPACITY;
    default: return PE_ERR_ENGINE;
    }
}

static bool geometry_valid(double x, double y, double width, double height, double rotation)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) || !isfinite(rotation)) {
        return false;
    }
    if (fabs(x) > PE_MAX_COORD || fabs(y) > PE_MAX_COORD) {
        return false;
    }
    if (width == 0.0 || height == 0.0 || fabs(width) > PE_MAX_EXTENT || fabs(height) > PE_MAX_EXTENT) {
        return false;
    }
    return fabs(rotation) <= PE_MAX_ROTATION;
}

/* A float scale must stay finite and non-zero after narrowing. */
static bool scale_valid(float value)
{
    return isfinite(value) && value != 0.0f;
}

static pe_texture_slot *slot_from_id(pe_bridge *bridge, uint32_t id)
{
    uint32_t index_plus_one = id & 0xFFFFu;
    uint32_t generation = id >> 16;
    pe_texture_slot *slot;
    if (index_plus_one == 0u || index_plus_one > PE_MAX_TEXTURES || generation == 0u) {
        return NULL;
    }
    slot = &bridge->slots[index_plus_one - 1u];
    if (!slot->used || slot->generation != generation) {
        return NULL;
    }
    return slot;
}

static int draw_sprite(pe_bridge *bridge, const PIXELICA_Texture *texture, double x, double y,
                       double scale_x, double scale_y, double rotation, int opacity,
                       const PIXELICA_DrawOptions *options)
{
    PIXELICA2D_Sprite sprite;
    PIXELICA_Status status;
    float sx = (float)scale_x;
    float sy = (float)scale_y;
    if (!scale_valid(sx) || !scale_valid(sy)) {
        return PE_ERR_INVALID_ARG;
    }
    status = pixelica2d_sprite_init(&sprite, texture);
    if (status != PIXELICA_OK) {
        return status_from_engine(status);
    }
    sprite.position.x = (float)x;
    sprite.position.y = (float)y;
    sprite.scale.x = sx;
    sprite.scale.y = sy;
    sprite.pivot.x = 0.0f; /* top-left pivot */
    sprite.pivot.y = 0.0f;
    sprite.rotation = (float)rotation;
    sprite.opacity = (uint8_t)opacity;
    status = options != NULL ? pixelica2d_draw_sprite_ex(&bridge->ctx, &sprite, options)
                             : pixelica2d_draw_sprite(&bridge->ctx, &sprite);
    return status_from_engine(status);
}

int pe_abi_version(void)
{
    return PE_ABI_VERSION;
}

const char *pe_status_message(int status)
{
    switch (status) {
    case PE_OK: return "ok";
    case PE_ERR_INVALID_ARG: return "invalid argument (null, non-finite, out of range or stale id)";
    case PE_ERR_NO_MEMORY: return "out of memory or output buffer too small";
    case PE_ERR_CAPACITY: return "texture capacity exhausted";
    case PE_ERR_STATE: return "no frame started (call pe_begin first)";
    case PE_ERR_ENGINE: return "Pixelica2D rejected the call";
    default: return "unknown status";
    }
}

int pe_create(int width, int height, pe_bridge **out)
{
    pe_bridge *bridge;
    PIXELICA2D_Surface surface;
    PIXELICA_Status status;
    size_t count;
    if (out == NULL || width < 1 || height < 1 || width > PE_MAX_DIMENSION || height > PE_MAX_DIMENSION) {
        return PE_ERR_INVALID_ARG;
    }
    count = (size_t)width * (size_t)height;
    if (count > (size_t)PE_MAX_PIXELS) {
        return PE_ERR_INVALID_ARG;
    }
    bridge = (pe_bridge *)calloc(1u, sizeof(*bridge));
    if (bridge == NULL) {
        return PE_ERR_NO_MEMORY;
    }
    bridge->pixels = (uint16_t *)calloc(count, sizeof(uint16_t));
    if (bridge->pixels == NULL) {
        free(bridge);
        return PE_ERR_NO_MEMORY;
    }
    bridge->pixel_count = count;
    memset(&surface, 0, sizeof(surface));
    surface.pixels = bridge->pixels;
    surface.width = (uint16_t)width;
    surface.height = (uint16_t)height;
    surface.stride = (uint32_t)width;
    surface.depth = NULL;
    surface.depth_stride = 0u;
    status = pixelica2d_init(&bridge->ctx, &surface);
    if (status != PIXELICA_OK) {
        free(bridge->pixels);
        free(bridge);
        return status_from_engine(status);
    }
    bridge->solid.pixels = &bridge->solid_texel;
    bridge->solid.width = 1u;
    bridge->solid.height = 1u;
    bridge->magic = PE_MAGIC;
    *out = bridge;
    return PE_OK;
}

void pe_destroy(pe_bridge *bridge)
{
    uint32_t i;
    if (!bridge_valid(bridge)) {
        return;
    }
    for (i = 0u; i < PE_MAX_TEXTURES; ++i) {
        free(bridge->slots[i].texels);
        free(bridge->slots[i].alpha);
    }
    free(bridge->pixels);
    bridge->pixels = NULL;
    bridge->magic = 0u;
    free(bridge);
}

int pe_begin(pe_bridge *bridge, uint32_t background_rgb)
{
    PIXELICA_Status status;
    if (!bridge_valid(bridge) || background_rgb > 0xFFFFFFu) {
        return PE_ERR_INVALID_ARG;
    }
    status = pixelica2d_clear(&bridge->ctx, rgb_to_565(background_rgb));
    if (status != PIXELICA_OK) {
        return status_from_engine(status);
    }
    bridge->frame_started = true;
    return PE_OK;
}

int pe_draw_rect(pe_bridge *bridge, double x, double y, double width, double height,
                 double rotation, uint32_t rgb, int opacity)
{
    if (!bridge_valid(bridge) || rgb > 0xFFFFFFu || opacity < 0 || opacity > 255 ||
        !geometry_valid(x, y, width, height, rotation)) {
        return PE_ERR_INVALID_ARG;
    }
    if (!bridge->frame_started) {
        return PE_ERR_STATE;
    }
    bridge->solid_texel = rgb_to_565(rgb);
    return draw_sprite(bridge, &bridge->solid, x, y, width, height, rotation, opacity, NULL);
}

int pe_texture_create(pe_bridge *bridge, const uint8_t *rgba, size_t rgba_bytes,
                      int width, int height, uint32_t *out_id)
{
    size_t count, bytes, i;
    uint32_t index;
    pe_texture_slot *slot = NULL;
    uint16_t *texels;
    uint8_t *alpha;
    if (!bridge_valid(bridge) || rgba == NULL || out_id == NULL || width < 1 || height < 1 ||
        width > PE_MAX_TEXTURE_DIMENSION || height > PE_MAX_TEXTURE_DIMENSION) {
        return PE_ERR_INVALID_ARG;
    }
    count = (size_t)width * (size_t)height;
    if (rgba_bytes / 4u != count || rgba_bytes % 4u != 0u) {
        return PE_ERR_INVALID_ARG;
    }
    bytes = count * 3u; /* RGB565 texels + alpha plane */
    if (bridge->texture_count >= PE_MAX_TEXTURES || bytes > (size_t)PE_MAX_TEXTURE_BYTES ||
        bridge->texture_bytes > (size_t)PE_MAX_TEXTURE_BYTES - bytes) {
        return PE_ERR_CAPACITY;
    }
    for (index = 0u; index < PE_MAX_TEXTURES; ++index) {
        if (!bridge->slots[index].used) {
            slot = &bridge->slots[index];
            break;
        }
    }
    if (slot == NULL) {
        return PE_ERR_CAPACITY;
    }
    texels = (uint16_t *)malloc(count * sizeof(uint16_t));
    alpha = (uint8_t *)malloc(count);
    if (texels == NULL || alpha == NULL) {
        free(texels);
        free(alpha);
        return PE_ERR_NO_MEMORY;
    }
    for (i = 0u; i < count; ++i) {
        const uint8_t *px = rgba + i * 4u;
        texels[i] = PIXELICA_RGB565(px[0], px[1], px[2]);
        alpha[i] = px[3];
    }
    slot->texels = texels;
    slot->alpha = alpha;
    slot->bytes = bytes;
    slot->texture.pixels = texels;
    slot->texture.width = (uint16_t)width;
    slot->texture.height = (uint16_t)height;
    slot->generation = (uint16_t)(slot->generation == 0xFFFFu ? 1u : slot->generation + 1u);
    slot->used = true;
    bridge->texture_count += 1u;
    bridge->texture_bytes += bytes;
    *out_id = ((uint32_t)slot->generation << 16) | (index + 1u);
    return PE_OK;
}

int pe_texture_destroy(pe_bridge *bridge, uint32_t id)
{
    pe_texture_slot *slot;
    if (!bridge_valid(bridge)) {
        return PE_ERR_INVALID_ARG;
    }
    slot = slot_from_id(bridge, id);
    if (slot == NULL) {
        return PE_ERR_INVALID_ARG;
    }
    free(slot->texels);
    free(slot->alpha);
    bridge->texture_count -= 1u;
    bridge->texture_bytes -= slot->bytes;
    slot->texels = NULL;
    slot->alpha = NULL;
    slot->bytes = 0u;
    slot->texture.pixels = NULL;
    slot->used = false; /* generation is kept so the old id stays stale */
    return PE_OK;
}

int pe_texture_stats(const pe_bridge *bridge, uint32_t *count, size_t *bytes)
{
    if (!bridge_valid(bridge)) {
        return PE_ERR_INVALID_ARG;
    }
    if (count != NULL) {
        *count = bridge->texture_count;
    }
    if (bytes != NULL) {
        *bytes = bridge->texture_bytes;
    }
    return PE_OK;
}

int pe_draw_texture(pe_bridge *bridge, uint32_t id, double x, double y, double width,
                    double height, double rotation, int opacity)
{
    pe_texture_slot *slot;
    PIXELICA_DrawOptions options;
    if (!bridge_valid(bridge) || opacity < 0 || opacity > 255 ||
        !geometry_valid(x, y, width, height, rotation)) {
        return PE_ERR_INVALID_ARG;
    }
    slot = slot_from_id(bridge, id);
    if (slot == NULL) {
        return PE_ERR_INVALID_ARG;
    }
    if (!bridge->frame_started) {
        return PE_ERR_STATE;
    }
    options = pixelica_draw_options_default();
    options.alpha_mode = PIXELICA_ALPHA_BLEND;
    options.opacity = 255u; /* the sprite opacity carries the object opacity */
    options.texture_alpha = slot->alpha;
    options.texture_alpha_bytes = (size_t)slot->texture.width * (size_t)slot->texture.height;
    return draw_sprite(bridge, &slot->texture, x, y, width / (double)slot->texture.width,
                       height / (double)slot->texture.height, rotation, opacity, &options);
}

int pe_read_rgb(const pe_bridge *bridge, uint8_t *out, size_t capacity)
{
    size_t i;
    if (!bridge_valid(bridge) || out == NULL) {
        return PE_ERR_INVALID_ARG;
    }
    if (!bridge->frame_started) {
        return PE_ERR_STATE;
    }
    if (capacity / 3u < bridge->pixel_count) {
        return PE_ERR_NO_MEMORY;
    }
    for (i = 0u; i < bridge->pixel_count; ++i) {
        uint32_t p = bridge->pixels[i];
        uint32_t r = (p >> 11) & 0x1Fu;
        uint32_t g = (p >> 5) & 0x3Fu;
        uint32_t b = p & 0x1Fu;
        out[i * 3u + 0u] = (uint8_t)((r << 3) | (r >> 2));
        out[i * 3u + 1u] = (uint8_t)((g << 2) | (g >> 4));
        out[i * 3u + 2u] = (uint8_t)((b << 3) | (b >> 2));
    }
    return PE_OK;
}

/* Range-check in double first: narrowing an out-of-range double is UB. */
static bool in_range(double value, double lo, bool lo_inclusive, double hi)
{
    if (!isfinite(value) || value > hi) {
        return false;
    }
    return lo_inclusive ? value >= lo : value > lo;
}

int pe_body2d_step(PIXELICA_Body2D *body, double dt, double gravity, double jump_speed,
                   double max_fall_speed, int jump, const PIXELICA_AABB2D *obstacles, size_t count)
{
    if (body == NULL || (jump != 0 && jump != 1) ||
        !in_range(dt, 0.0, false, (double)PIXELICA_BODY2D_MAX_DT) ||
        !in_range(gravity, 0.0, true, (double)PIXELICA_BODY2D_MAX_ACCEL) ||
        !in_range(jump_speed, 0.0, true, (double)PIXELICA_BODY2D_MAX_ACCEL) ||
        !in_range(max_fall_speed, 0.0, false, (double)PIXELICA_BODY2D_MAX_ACCEL)) {
        return PE_ERR_INVALID_ARG;
    }
    /* A positive double that narrows to 0 is rejected by the engine. */
    return status_from_engine(pixelica_body2d_step(body, (float)dt, (float)gravity, (float)jump_speed,
                                                   (float)max_fall_speed, jump == 1, obstacles, count));
}

int pe_aabb2d_overlaps(const PIXELICA_AABB2D *a, const PIXELICA_AABB2D *b)
{
    return pixelica_aabb2d_overlaps(a, b) ? 1 : 0;
}

/*
 * Pixelica editor bridge - a tiny C ABI over Pixelica2D for the Python editor
 * (loaded with ctypes by tools/editor/pixelica_editor/runtime.py).
 *
 * The bridge owns one RGB565 render target, a 1x1 texel used for solid
 * rectangles and a bounded table of RGBA textures converted once to RGB565 +
 * an 8-bit alpha plane. Every draw goes straight to the real Pixelica2D
 * rasterizer (pixelica2d_draw_sprite / pixelica2d_draw_sprite_ex); there is no
 * other renderer.
 *
 * Rules
 *   - Every function validates all of its inputs before changing anything and
 *     returns a PE_* status. On failure, outputs and pixels are unchanged.
 *   - Sizes and colors are taken as signed/wide integers on purpose so that
 *     out-of-range values from the caller are rejected instead of silently
 *     truncated by the FFI.
 *   - Positions are top-left pivots; rotation is radians around that pivot,
 *     positive turns clockwise on screen (+Y is down).
 *   - Not thread-safe: use one bridge from one thread at a time.
 */
#ifndef PIXELICA_EDITOR_BRIDGE_H
#define PIXELICA_EDITOR_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/collision2d.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(PE_BRIDGE_BUILD)
#    define PE_API __declspec(dllexport)
#  else
#    define PE_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define PE_API __attribute__((visibility("default")))
#else
#  define PE_API
#endif

/* Bumped whenever a signature or semantic below changes. */
#define PE_ABI_VERSION 2

/* Render target limits (match the editor project schema). */
#define PE_MAX_DIMENSION 2048
#define PE_MAX_PIXELS 1048576
/* Texture limits: per-texture edge, live texture count, total bytes held. */
#define PE_MAX_TEXTURE_DIMENSION 4096
#define PE_MAX_TEXTURES 256
#define PE_MAX_TEXTURE_BYTES (96u * 1024u * 1024u)
/* Largest |coordinate| and |extent| accepted for a draw, in pixels. */
#define PE_MAX_COORD 1.0e7
#define PE_MAX_EXTENT 1.0e6
/* Largest |rotation| in radians (callers should normalize first). */
#define PE_MAX_ROTATION 1.0e4

enum {
    PE_OK = 0,
    PE_ERR_INVALID_ARG = 1, /* NULL, non-finite, out of range, stale texture id */
    PE_ERR_NO_MEMORY = 2,   /* allocation failed or output buffer too small */
    PE_ERR_CAPACITY = 3,    /* texture count or texture byte budget exhausted */
    PE_ERR_STATE = 4,       /* draw/read before pe_begin */
    PE_ERR_ENGINE = 5       /* Pixelica2D rejected the call (should not happen) */
};

typedef struct pe_bridge pe_bridge;

PE_API int pe_abi_version(void);
PE_API const char *pe_status_message(int status);

/* Create a width x height target (1..PE_MAX_DIMENSION, w*h <= PE_MAX_PIXELS).
 * *out is set only on success. */
PE_API int pe_create(int width, int height, pe_bridge **out);
/* Free the target and every texture. NULL is accepted. */
PE_API void pe_destroy(pe_bridge *bridge);

/* Clear the whole target to background_rgb (0xRRGGBB, converted to RGB565)
 * and start a frame. Draws and reads need a started frame. */
PE_API int pe_begin(pe_bridge *bridge, uint32_t background_rgb);

/* Solid rectangle as a Pixelica2D sprite. width/height: finite, non-zero
 * (negative mirrors around the pivot). rgb <= 0xFFFFFF, opacity 0..255. */
PE_API int pe_draw_rect(pe_bridge *bridge, double x, double y, double width, double height,
                        double rotation, uint32_t rgb, int opacity);

/* Upload straight-alpha RGBA8888 pixels (row-major, top row first,
 * rgba_bytes == width*height*4). *out_id is set only on success. */
PE_API int pe_texture_create(pe_bridge *bridge, const uint8_t *rgba, size_t rgba_bytes,
                             int width, int height, uint32_t *out_id);
/* Release a texture. Unknown/stale ids return PE_ERR_INVALID_ARG. */
PE_API int pe_texture_destroy(pe_bridge *bridge, uint32_t id);
/* Number of live textures and bytes they hold (either pointer may be NULL). */
PE_API int pe_texture_stats(const pe_bridge *bridge, uint32_t *count, size_t *bytes);

/* Draw a texture stretched to width x height (same rules as pe_draw_rect),
 * blended with its alpha plane multiplied by opacity. */
PE_API int pe_draw_texture(pe_bridge *bridge, uint32_t id, double x, double y, double width,
                           double height, double rotation, int opacity);

/* Copy the target as RGB24 (width*height*3 bytes, rows top-down) into out.
 * capacity smaller than that returns PE_ERR_NO_MEMORY and writes nothing. */
PE_API int pe_read_rgb(const pe_bridge *bridge, uint8_t *out, size_t capacity);

/* 2D collision (ABI 2). Thin wrappers over pixelica/collision2d.h; they need
 * no bridge instance. Structures are plain floats/uint32 (body: 32 bytes).
 *
 * Step a platformer body (see pixelica_body2d_step). The doubles must be
 * finite and in range (dt (0,0.25], gravity/jump_speed [0,10000],
 * max_fall_speed (0,10000]) and jump 0 or 1, else PE_ERR_INVALID_ARG. On
 * failure *body is unchanged. Starting penetration is PE_ERR_INVALID_ARG. */
PE_API int pe_body2d_step(PIXELICA_Body2D *body, double dt, double gravity, double jump_speed,
                          double max_fall_speed, int jump, const PIXELICA_AABB2D *obstacles,
                          size_t count);
/* 1 if the open interiors overlap, 0 if not (touching, NULL or invalid). */
PE_API int pe_aabb2d_overlaps(const PIXELICA_AABB2D *a, const PIXELICA_AABB2D *b);

#ifdef __cplusplus
}
#endif
#endif

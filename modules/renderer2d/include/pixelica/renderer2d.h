/*
 * Pixelica 2D - standalone software sprite / tilemap / text renderer.
 *
 * Part of the renderer2d component (Pixelica2D::core / Pixelica::renderer2d).
 * It depends only on the common headers below and never on pixelica.h,
 * renderer.h, scene.h or assets.h, so a 2D-only SDK carries no 3D code.
 *
 * Memory and state
 *   - Every object is caller-owned. The library never allocates, keeps no
 *     global mutable state and keeps no hidden draw queue: each draw call
 *     rasterizes immediately into the caller's RGB565 surface.
 *   - Pointers passed to a call (sprites, textures, list items, tiles, font
 *     rows, text) are only read during that call and are never retained.
 *   - There is no begin/end frame protocol. Draw, then hand the color buffer
 *     to the PAL (present_rgb565) yourself, once per frame.
 *
 * Errors
 *   - Every function validates ALL of its inputs before writing anything.
 *     On any non-PIXELICA_OK result, outputs, the context (camera, clip, stats)
 *     and every color/depth pixel are left exactly as they were.
 *   - PIXELICA_ERR_INVALID_ARG: NULL pointer, non-finite or out-of-range value,
 *     size/address overflow, or a forbidden memory overlap (see below).
 *   - PIXELICA_ERR_NO_MEMORY: only pixelica2d_draw_list, when the caller's order
 *     workspace is smaller than the item count.
 *
 * Memory overlap rules (checked as address ranges, PIXELICA_ERR_INVALID_ARG)
 *   - Color and depth buffers must be disjoint, and the context struct must
 *     not lie inside either of them.
 *   - Read-only inputs (texture structs and texels, list items, tile arrays,
 *     font rows, text bytes) must not overlap the color or depth buffer.
 *   - The draw_list order workspace must not overlap the items, textures,
 *     color, depth or the context.
 *   - Whether a pointer really addresses at least the documented number of
 *     elements, and is suitably aligned for its type, cannot be checked and
 *     is the caller's responsibility.
 *
 * Coordinates
 *   - Screen: pixel (0,0) is the top-left of the surface, +X right, +Y down.
 *     Pixel (x,y) is sampled at its center (x+0.5, y+0.5).
 *   - World: same axes as the screen. The camera maps a world point to the
 *     screen as  screen = (world - camera.position) * camera.zoom.
 *   - Sprites and tilemaps use world coordinates (camera applied, depth
 *     options available). fill_rect and draw_text use screen coordinates
 *     (camera and depth ignored), which suits HUD overlays.
 *   - All drawing is limited to the context clip rectangle.
 *
 * Pixel pipeline (sprites and tilemaps), per covered pixel:
 *   1. Map the pixel center back to source texel space in double precision
 *      and take the nearest texel with floor() (half-open source extent).
 *   2. Color key: if enabled and the texel equals color_key exactly, the
 *      pixel is discarded before any depth test or depth write.
 *   3. Depth test (optional): passes only if depth < stored depth (strict).
 *   4. Color: opacity 255 copies the texel exactly; otherwise each RGB565
 *      channel is blended as (src*a + dst*(255-a) + 127) / 255 in integers.
 *   5. Depth write (optional): stores the draw's depth.
 *   Opacity 0 writes nothing at all, not even depth.
 *
 * Statistics
 *   stats.draw_calls and stats.pixels_written saturate at UINT32_MAX and
 *   are reset by pixelica2d_clear. A pixel counts once each time it is written.
 *   Accepted draws count even when fully clipped, keyed or transparent.
 *
 * Coordinates are computed in double precision and every float-to-integer
 * conversion happens only after the value has been range-checked or clamped
 * to the clip rectangle, so any finite float input is safe to pass, however
 * large or small. Results for huge values are limited by double precision.
 */
#ifndef PIXELICA_RENDERER2D_H
#define PIXELICA_RENDERER2D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pixelica/status.h"
#include "pixelica/math.h"
#include "pixelica/image.h"
#include "pixelica/assets_common.h"
#include "pixelica/animation.h"
#include "pixelica/platform.h"
#include "pixelica/draw_options.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest surface width/height in pixels. */
#define PIXELICA2D_MAX_DIMENSION 4096u
/* Value pixelica2d_clear writes to the depth buffer ("infinitely far"). */
#define PIXELICA2D_DEPTH_CLEAR UINT16_MAX
/* Largest depth a sprite or tilemap may use when depth is enabled. */
#define PIXELICA2D_DEPTH_MAX ((uint16_t)(UINT16_MAX - 1u))
/* Tile id meaning "no tile in this cell". */
#define PIXELICA2D_EMPTY_TILE UINT16_MAX
/* Largest pixelica2d_draw_list item count. */
#define PIXELICA2D_MAX_ITEMS 65536u
/* Largest tilemap cell count (cols * rows). */
#define PIXELICA2D_MAX_TILES 1048576u
/* Largest pixelica2d_draw_text byte count. */
#define PIXELICA2D_MAX_TEXT_BYTES 65536u

/* Integer rectangle; width/height must be >= 0 where accepted. */
typedef struct PIXELICA2D_Rect { int32_t x, y, width, height; } PIXELICA2D_Rect;

/*
 * Caller-owned render target (RGB565 color plus optional 16-bit depth).
 *   pixels: required. Row y starts at pixels[y * stride]; the caller must
 *           provide (height - 1) * stride + width elements. Padding between
 *           width and stride is never read or written.
 *   width, height: 1..PIXELICA2D_MAX_DIMENSION. stride >= width.
 *   depth:  optional (NULL = no depth buffer; depth_stride is then ignored).
 *           Otherwise depth_stride >= width and (height - 1) * depth_stride
 *           + width elements, disjoint from the color buffer.
 */
typedef struct PIXELICA2D_Surface {
    uint16_t *pixels;
    uint16_t width, height;
    uint32_t stride;
    uint16_t *depth;
    uint32_t depth_stride;
} PIXELICA2D_Surface;

/* World-to-screen camera; zoom must be finite and > 0. */
typedef struct PIXELICA2D_Camera { PIXELICA_Vec2 position; float zoom; } PIXELICA2D_Camera;

typedef struct PIXELICA2D_Stats { uint32_t draw_calls, pixels_written; } PIXELICA2D_Stats;

/*
 * Rendering context. Plain public data, created by pixelica2d_init and owned by
 * the caller; it holds a copy of the surface descriptor, not the pixels.
 * Fields may be read freely. If you write them directly, every later call
 * revalidates them and rejects an inconsistent context with
 * PIXELICA_ERR_INVALID_ARG; prefer the setters.
 */
typedef struct PIXELICA2D_Context {
    PIXELICA2D_Surface surface;
    PIXELICA2D_Camera camera;
    PIXELICA2D_Rect clip;   /* always inside 0..width x 0..height; may be empty */
    PIXELICA2D_Stats stats;
} PIXELICA2D_Context;

/*
 * One textured quad.
 *   texture:  valid texture (pixels != NULL, 1..PIXELICA_MAX_TEXTURE_DIMENSION).
 *   source:   texel rectangle; x,y >= 0, width,height > 0, inside texture.
 *   position: world position of the pivot.
 *   scale:    finite, non-zero; a negative component mirrors the sprite
 *             around the pivot on that axis (flip).
 *   pivot:    source-local texel coordinates (0,0 = source top-left corner)
 *             of the point placed at `position`; may lie outside the source.
 *   rotation: radians around the pivot; positive turns clockwise on screen.
 *   depth:    0..PIXELICA2D_DEPTH_MAX when depth_test or depth_write is set
 *             (either flag requires a surface depth buffer).
 *   opacity:  0 (invisible) .. 255 (opaque).
 *   color_key_enabled / color_key: texels equal to color_key are skipped.
 *   depth_test: draw only where depth < stored depth.
 *   depth_write: store depth for every pixel that is drawn.
 * The world-space footprint is source.width * |scale.x| by
 * source.height * |scale.y| before rotation, then multiplied by the zoom.
 */
typedef struct PIXELICA2D_Sprite {
    const PIXELICA_Texture *texture;
    PIXELICA2D_Rect source;
    PIXELICA_Vec2 position, scale, pivot;
    float rotation;
    uint16_t depth;
    uint8_t opacity;
    bool color_key_enabled;
    uint16_t color_key;
    bool depth_test, depth_write;
} PIXELICA2D_Sprite;

/* pixelica2d_draw_list entry. sort_y must be finite. */
typedef struct PIXELICA2D_Item { PIXELICA2D_Sprite sprite; int32_t layer; float sort_y; } PIXELICA2D_Item;

/*
 * Grid tilemap drawn from a regular atlas.
 *   atlas: valid texture whose width/height are exact multiples of
 *          tile_width/tile_height (both >= 1). Tile id k is the k-th atlas
 *          cell in row-major order; PIXELICA2D_EMPTY_TILE marks an empty cell.
 *   tiles: caller-owned; cell (col,row) is tiles[row * stride_tiles + col].
 *          cols, rows >= 1, stride_tiles >= cols,
 *          cols * rows <= PIXELICA2D_MAX_TILES.
 *   position: world position of the map's top-left corner.
 *   scale: uniform, finite, > 0 (one tile is tile_width*scale world units).
 *   depth/opacity/color key/depth flags: as for PIXELICA2D_Sprite.
 */
typedef struct PIXELICA2D_Tilemap {
    const PIXELICA_Texture *atlas;
    const uint16_t *tiles;
    uint16_t cols, rows, tile_width, tile_height;
    uint32_t stride_tiles;
    PIXELICA_Vec2 position;
    float scale;
    uint16_t depth;
    uint8_t opacity;
    bool color_key_enabled;
    uint16_t color_key;
    bool depth_test, depth_write;
} PIXELICA2D_Tilemap;

/*
 * 1-bit bitmap font. Glyph g (character first_char + g) occupies rows
 * rows[g * height .. g * height + height - 1], one byte per row, bit 7 is
 * the leftmost pixel. width 1..8, height 1..32, glyph_count 1..256,
 * first_char + glyph_count <= 256.
 */
typedef struct PIXELICA2D_Font {
    const uint8_t *rows;
    uint16_t first_char, glyph_count;
    uint8_t width, height;
} PIXELICA2D_Font;

/*
 * Isometric (2:1 style) projection parameters. tile_width/tile_height:
 * on-screen size of one ground tile diamond, finite and > 0. height_scale:
 * screen pixels per unit of height, finite and >= 0. origin: screen
 * position of map point (0,0,0); finite.
 */
typedef struct PIXELICA2D_Iso {
    float tile_width, tile_height, height_scale;
    PIXELICA_Vec2 origin;
} PIXELICA2D_Iso;

/*
 * Initialize a context over a caller-owned surface (copied by value). The
 * camera becomes position (0,0) with zoom 1, the clip the full surface and
 * the stats zero. The pixels are NOT cleared. `out` must not lie inside
 * the color or depth buffer.
 */
PIXELICA_Status pixelica2d_init(PIXELICA2D_Context *out, const PIXELICA2D_Surface *surface);

/*
 * Fill the whole surface (width x height, ignoring the clip; padding is
 * never touched) with `color`, set the depth buffer (if any) to
 * PIXELICA2D_DEPTH_CLEAR and reset stats to zero.
 */
PIXELICA_Status pixelica2d_clear(PIXELICA2D_Context *ctx, uint16_t color);

/* Set the camera; NULL restores (0,0) zoom 1. Position finite, zoom > 0. */
PIXELICA_Status pixelica2d_set_camera(PIXELICA2D_Context *ctx, const PIXELICA2D_Camera *camera);

/*
 * Set the clip rectangle in screen pixels; NULL restores the full surface.
 * The rectangle is intersected with the surface (any int32 values are
 * fine; a zero or empty result is accepted and disables drawing). A
 * negative width or height is PIXELICA_ERR_INVALID_ARG.
 */
PIXELICA_Status pixelica2d_set_clip(PIXELICA2D_Context *ctx, const PIXELICA2D_Rect *clip);

/*
 * Default sprite for a valid texture: whole-texture source, position (0,0),
 * scale (1,1), pivot (0,0), rotation 0, depth 0, opacity 255, no color key,
 * no depth test or write.
 */
PIXELICA_Status pixelica2d_sprite_init(PIXELICA2D_Sprite *out, const PIXELICA_Texture *texture);

/*
 * Select cell `frame` (row-major) of a regular grid of width x height cells
 * covering sprite->texture. width/height must be > 0 and divide the
 * texture's width/height exactly, and frame < cell count. Only
 * sprite->source changes; on failure nothing changes.
 */
PIXELICA_Status pixelica2d_sprite_frame(PIXELICA2D_Sprite *sprite, uint32_t frame, uint16_t width, uint16_t height);

/*
 * Draw one sprite immediately (see PIXELICA2D_Sprite and the pixel pipeline
 * above). Counts one draw call, even if nothing is visible.
 */
PIXELICA_Status pixelica2d_draw_sprite(PIXELICA2D_Context *ctx, const PIXELICA2D_Sprite *sprite);

/*
 * Draw one sprite with opt-in sampling/transparency (see pixelica/draw_options.h
 * and docs/texture_options.md). options == NULL is exactly pixelica2d_draw_sprite.
 * With options the source rectangle only needs width/height > 0: x/y may be
 * negative and the rectangle may leave the texture. The absolute source texel
 * (source.x + floor(u), source.y + floor(v)) is repeated (floor modulo) or
 * clamped to the whole texture per axis; a texture_alpha plane (width*height,
 * tightly packed) is read at the same texel. Unlike the legacy call this
 * therefore accepts out-of-range source rectangles (clamped). OPAQUE ignores
 * options.opacity and the alpha plane. TEST/BLEND use
 * alpha = round(round(map*options.opacity/255)*sprite.opacity/255); TEST
 * discards alpha < cutoff and copies survivors opaque (depth_test/depth_write
 * honored), BLEND skips alpha 0, blends in RGB565, honors depth_test and never
 * writes depth. Invalid enums, inconsistent alpha pointer/size, or options/alpha
 * storage overlapping the color/depth buffer return PIXELICA_ERR_INVALID_ARG
 * with nothing changed.
 */
PIXELICA_Status pixelica2d_draw_sprite_ex(PIXELICA2D_Context *ctx, const PIXELICA2D_Sprite *sprite,
                                          const PIXELICA_DrawOptions *options);

/*
 * Fill a screen-space rectangle (camera and depth ignored) inside the clip
 * with `color` blended at `opacity` (same integer blend as sprites; 255 is
 * an exact copy, 0 writes nothing). width/height >= 0; any int32 position
 * is accepted without overflow. Counts one draw call.
 */
PIXELICA_Status pixelica2d_fill_rect(PIXELICA2D_Context *ctx, const PIXELICA2D_Rect *rect, uint16_t color, uint8_t opacity);

/*
 * Draw `count` sprites in stable painter order: layer ascending, then
 * sort_y ascending, then original index. Later items are drawn over earlier
 * ones (subject to each sprite's depth options).
 *   count:  0..PIXELICA2D_MAX_ITEMS. count == 0 is a no-op (items/order may then
 *           be NULL) that does not touch the stats.
 *   order:  caller workspace of order_count elements; only the first
 *           `count` are written (left holding the drawing order). If
 *           order_count < count the result is PIXELICA_ERR_NO_MEMORY.
 * Every item is validated before the first pixel is written, so one bad
 * item rejects the whole list with nothing drawn. Each item counts one
 * draw call.
 * Cost: the order is built with a stable insertion sort, O(count) for an
 * already (or nearly) sorted list but O(count^2) comparisons in the worst
 * case (reverse order). Keep large lists roughly pre-sorted, or sort them
 * yourself and use one layer per item. Pixel rasterization itself costs
 * the same as calling pixelica2d_draw_sprite per item.
 */
PIXELICA_Status pixelica2d_draw_list(PIXELICA2D_Context *ctx, const PIXELICA2D_Item *items, size_t count, size_t *order, size_t order_count);

/*
 * Draw a tilemap. Every cell's id is validated (even off-screen cells)
 * before any pixel is written: each id must be PIXELICA2D_EMPTY_TILE or less
 * than the atlas cell count. Rendering then only visits the screen pixels
 * that the map covers inside the clip, so a large map costs time
 * proportional to the visible area (plus the O(cells) validation pass).
 * Each non-empty cell inside the visible cell rectangle counts one draw
 * call (none when the map is entirely clipped).
 */
PIXELICA_Status pixelica2d_draw_tilemap(PIXELICA2D_Context *ctx, const PIXELICA2D_Tilemap *map);

/*
 * Draw `length` bytes of text (no terminator needed) with its first glyph's
 * top-left at screen pixel (x,y). '\n' returns to x and moves down by
 * font->height + 1. Every other byte advances by font->width + 1; bytes
 * outside the font's range advance without drawing. No UTF-8 decoding.
 * Each set glyph bit is one pixel of `color` blended at `opacity`;
 * camera and depth are ignored. length <= PIXELICA2D_MAX_TEXT_BYTES; text may
 * be NULL only when length is 0 (the font must still be valid). Counts one
 * draw call, including for empty text.
 */
PIXELICA_Status pixelica2d_draw_text(PIXELICA2D_Context *ctx, const PIXELICA2D_Font *font, const char *text, size_t length,
                          int32_t x, int32_t y, uint16_t color, uint8_t opacity);

/*
 * Project a map point (x,y ground plane, z height) to screen:
 *   sx = origin.x + (x - y) * tile_width / 2
 *   sy = origin.y + (x + y) * tile_height / 2 - z * height_scale
 * Computed in double; PIXELICA_ERR_INVALID_ARG if any input is invalid or the
 * result does not fit in a float (out unchanged).
 */
PIXELICA_Status pixelica2d_iso_project(const PIXELICA2D_Iso *iso, PIXELICA_Vec3 map, PIXELICA_Vec2 *out);

/*
 * Inverse of pixelica2d_iso_project for points at a known `height`: returns the
 * ground (x,y) that projects to `screen`. Same validation as project.
 */
PIXELICA_Status pixelica2d_iso_unproject(const PIXELICA2D_Iso *iso, PIXELICA_Vec2 screen, float height, PIXELICA_Vec2 *out);

#ifdef __cplusplus
}
#endif
#endif

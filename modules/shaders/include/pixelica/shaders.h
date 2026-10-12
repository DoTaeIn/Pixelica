/*
 * Pixelica - optional CPU postprocessing "shaders" for RGB565 framebuffers.
 *
 * Renderer-independent: this header needs only the common library
 * (pixelica/status.h, pixelica/platform.h). Effects run on any caller-owned
 * RGB565 surface (row 0 = top, row pitch `stride` pixels), and the presenter
 * wraps a PAL so the 2D renderer, the 3D core and hybrid setups can all be
 * postprocessed without the shader module knowing about any renderer.
 *
 * No heap, no globals. Every buffer is caller-owned and only borrowed for the
 * duration of a call, except where noted (presenter output/scratch/palette and
 * userdata stay borrowed while the presenter is in use). Single-threaded: a
 * surface, shader or presenter must not be used from two threads at once.
 *
 * Color math: RGB565 channels expand to RGB8 by bit replication
 * (r8 = r5 << 3 | r5 >> 2, g8 = g6 << 2 | g6 >> 4) and pack back by keeping the
 * high bits. "mix(a, b, t)" is per RGB8 channel a + (b - a) * t rounded to
 * nearest, then packed. Luma is (77 R + 150 G + 29 B + 128) / 256 on RGB8.
 *
 * Effects (PIXELICA_Shader fields not listed are ignored by that kind):
 *   TINT       mix(src, color, amount)
 *   GRAYSCALE  mix(src, (luma, luma, luma), amount)
 *   INVERT     mix(src, (255 - R, 255 - G, 255 - B), amount)
 *   POSTERIZE  each RGB8 channel rounded to the nearest of `levels` evenly
 *              spaced values 0 .. 255 (levels 2 .. 32)
 *   PALETTE    nearest of palette[0 .. palette_count) (1 .. 256 RGB565 entries)
 *              by squared RGB8 distance; the first entry wins ties; the entry
 *              is written unchanged
 *   SCANLINES  odd rows (y & 1) become mix(src, black, amount)
 *   VIGNETTE   mix(src, black, amount * clamp(nx*nx + ny*ny, 0, 1)) with
 *              nx = (2x - (w - 1)) / (w - 1) (0 when w == 1), ny likewise
 *   BLUR       box average over (2 radius + 1)^2 RGB8 samples (radius 1 .. 4),
 *              coordinates clamped to the edge, integer round to nearest;
 *              ignores amount
 *   OUTLINE    pixel becomes `color` when |luma(p) - luma(n)| > round(amount*255)
 *              for any of the four edge-clamped neighbors n, else unchanged
 *   CUSTOM     callback(pixel, userdata) per pixel, row-major order
 * TINT, GRAYSCALE, INVERT, SCANLINES, VIGNETTE and OUTLINE need a finite
 * amount in [0, 1]. BLUR, OUTLINE and CUSTOM read only an immutable tight
 * snapshot of the surface (in caller scratch), so results never depend on
 * pixels already written during the same pass.
 */
#ifndef PIXELICA_SHADERS_H
#define PIXELICA_SHADERS_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/platform.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_SHADER_MAX_DIMENSION 4096u /* max width and height */
#define PIXELICA_SHADER_MIN_LEVELS 2u
#define PIXELICA_SHADER_MAX_LEVELS 32u
#define PIXELICA_SHADER_MIN_RADIUS 1u
#define PIXELICA_SHADER_MAX_RADIUS 4u
#define PIXELICA_SHADER_MAX_PALETTE 256u

/*
 * Writable RGB565 surface. Valid when pixels != NULL, width and height are
 * 1 .. PIXELICA_SHADER_MAX_DIMENSION and stride >= width. Pixel (x, y) is
 * pixels[y * stride + x]; the range used is ((height - 1) * stride + width)
 * pixels. Row padding (x >= width) is never read or written.
 */
typedef struct PIXELICA_ShaderSurface {
    uint16_t *pixels;
    uint16_t width, height;
    uint32_t stride;
} PIXELICA_ShaderSurface;

/* Read-only image, same validity rules and layout as PIXELICA_ShaderSurface. */
typedef struct PIXELICA_ShaderImage {
    const uint16_t *pixels;
    uint16_t width, height;
    uint32_t stride;
} PIXELICA_ShaderImage;

typedef enum PIXELICA_ShaderKind {
    PIXELICA_SHADER_TINT,
    PIXELICA_SHADER_GRAYSCALE,
    PIXELICA_SHADER_INVERT,
    PIXELICA_SHADER_POSTERIZE,
    PIXELICA_SHADER_PALETTE,
    PIXELICA_SHADER_SCANLINES,
    PIXELICA_SHADER_VIGNETTE,
    PIXELICA_SHADER_BLUR,
    PIXELICA_SHADER_OUTLINE,
    PIXELICA_SHADER_CUSTOM
} PIXELICA_ShaderKind;

/*
 * Argument of a CUSTOM callback. `source` is the immutable tight snapshot of
 * the whole surface (stride == width), valid only during the callback; use
 * pixelica_shader_sample() for neighbor reads. `color` == source pixel (x, y).
 */
typedef struct PIXELICA_ShaderPixel {
    const PIXELICA_ShaderImage *source;
    uint16_t x, y, color;
} PIXELICA_ShaderPixel;

/* Returns the new RGB565 color of pixel (x, y). Must not call back into the
 * same apply/presenter, nor write the surface, scratch or snapshot. */
typedef uint16_t (*PIXELICA_PixelShader)(const PIXELICA_ShaderPixel *pixel, void *userdata);

/*
 * Effect description; start from pixelica_shader_default(). `palette` is
 * borrowed (RGB565 entries) and must stay alive while the shader is used.
 * `callback` and `userdata` are opaque: the memory userdata points to is the
 * caller's responsibility and is never range-checked.
 */
typedef struct PIXELICA_Shader {
    PIXELICA_ShaderKind kind;
    float amount;
    uint16_t color; /* RGB565 */
    uint8_t levels, radius;
    const uint16_t *palette;
    uint16_t palette_count;
    PIXELICA_PixelShader callback;
    void *userdata;
} PIXELICA_Shader;

/*
 * PAL wrapper that postprocesses every presented frame. Pass `&p->ops` (or a
 * copy of it) as the platform of the 2D/3D/hybrid renderer. ops.userdata is
 * the presenter itself, so the presenter must stay alive and must not move
 * while ops is in use. time_us / read_buttons / log forward to the upstream
 * callbacks with upstream.userdata (ops.log is NULL when upstream.log is).
 *
 * present_rgb565 validates the frame (pixels != NULL, 1 .. 4096 dimensions,
 * stride >= width), width * height <= output_capacity, the current `shader`
 * (it may be changed between frames, it is copied and revalidated on every
 * call), the scratch capacity it needs (width * height for BLUR / OUTLINE /
 * CUSTOM) and that the frame does not overlap the presenter, output or the
 * supplied scratch (the entire declared capacities must be disjoint, even
 * when the current effect does not need scratch). Any failure returns false without forwarding and without
 * changing output or scratch. Otherwise the frame is copied (never modified)
 * into output as a tight width x height image, the shader is applied there and
 * upstream.present_rgb565 is called exactly once with stride == width; its
 * result is returned.
 *
 * Fields are public for inspection; `shader` may be reassigned between
 * frames. output, scratch and the shader palette stay borrowed for the
 * presenter's lifetime and must not overlap each other or the presenter.
 */
typedef struct PIXELICA_ShaderPresenter {
    PIXELICA_PlatformOps upstream, ops;
    PIXELICA_Shader shader;
    uint16_t *output, *scratch;
    size_t output_capacity, scratch_capacity; /* in pixels */
} PIXELICA_ShaderPresenter;

/*
 * Defaults for `kind`: amount 1 (OUTLINE 0.2), color 0xFFFF white (OUTLINE
 * 0x0000 black), levels 4, radius 1, palette NULL / count 0, callback and
 * userdata NULL. PALETTE and CUSTOM still need palette / callback assigned
 * before use. Unknown kind or NULL out -> PIXELICA_ERR_INVALID_ARG, *out
 * untouched.
 */
PIXELICA_Status pixelica_shader_default(PIXELICA_ShaderKind kind, PIXELICA_Shader *out);

/*
 * Scratch pixels pixelica_shader_apply() needs: width * height for BLUR,
 * OUTLINE and CUSTOM when surface and shader are valid, otherwise 0 (also for
 * kinds that need none). Aliasing is not checked; apply is authoritative.
 */
size_t pixelica_shader_scratch_pixels(const PIXELICA_ShaderSurface *surface,
    const PIXELICA_Shader *shader);

/*
 * Applies `shader` in place to `surface`. Every relevant input is validated
 * before anything is written; on error neither the surface nor scratch is
 * modified.
 *   PIXELICA_ERR_INVALID_ARG: NULL surface/shader, invalid surface, unknown
 *     kind, out-of-range field used by the kind, a pixel range that would
 *     wrap the address space, the surface pixels overlapping the shader
 *     struct, the surface struct or (PALETTE) the palette, or the needed
 *     scratch being NULL or overlapping the surface pixels, the shader or
 *     surface struct.
 *   PIXELICA_ERR_CAPACITY: scratch_count < pixelica_shader_scratch_pixels().
 * Kinds without scratch ignore scratch / scratch_count (NULL / 0 allowed).
 * Only the first width * height scratch pixels are written; the caller is
 * responsible for scratch really holding scratch_count pixels.
 */
PIXELICA_Status pixelica_shader_apply(PIXELICA_ShaderSurface *surface, const PIXELICA_Shader *shader,
    uint16_t *scratch, size_t scratch_count);

/* Pixel at (x, y) with both coordinates clamped into the image; 0 for a NULL
 * or invalid image. */
uint16_t pixelica_shader_sample(const PIXELICA_ShaderImage *image, int32_t x, int32_t y);

/*
 * Initializes a presenter. Requires non-NULL out / upstream / shader,
 * upstream time_us, read_buttons and present_rgb565, a valid shader (PALETTE
 * palette and CUSTOM callback set), output != NULL with output_capacity > 0,
 * and scratch != NULL with scratch_capacity > 0 when the shader kind needs
 * scratch (scratch may be NULL / 0 otherwise). Byte sizes must not overflow or
 * wrap; out, output, any supplied scratch and (PALETTE) the palette must be
 * pairwise disjoint and out / output / scratch must not overlap *upstream or
 * *shader. Frame dimensions are only known (and checked) at present time.
 * Copies *upstream and *shader. On error returns PIXELICA_ERR_INVALID_ARG and
 * leaves *out untouched.
 */
PIXELICA_Status pixelica_shader_presenter_init(PIXELICA_ShaderPresenter *out,
    const PIXELICA_PlatformOps *upstream, const PIXELICA_Shader *shader, uint16_t *output,
    size_t output_capacity, uint16_t *scratch, size_t scratch_capacity);

#ifdef __cplusplus
}
#endif
#endif

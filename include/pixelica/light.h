/*
 * Pixelica - reusable pixel-lighting tools (common library).
 *
 * Three independent, opt-in tools for "modern pixel art" lighting. They are
 * renderer-independent: this header needs only the other common headers, so a
 * 2D-only, a 3D-only and a hybrid SDK all get them. Nothing here runs unless
 * you call it, and no existing draw path changes behavior.
 *
 *   1. SHAPE MASKS      pixelica_light_mask_build()  - an 8-bit soft shape
 *      (a window with panes, or an ellipse blob) plus a matching flat tint
 *      image (pixelica_light_tint_fill()). Together they are exactly a
 *      PIXELICA_Texture + a PIXELICA_DrawOptions.texture_alpha plane, so they
 *      feed the EXISTING alpha/additive paths of both renderers unchanged
 *      (3D: pixelica_draw_light / pixelica_draw_mesh_options,
 *       2D: pixelica2d_draw_sprite_ex).
 *   2. RECEIVER QUADS   pixelica_light_quad(), pixelica_light_quad_project()
 *      place such a shape on a receiver surface in world space (floor, wall,
 *      desk top), optionally by projecting an opening along one light
 *      direction. This is pure geometry.
 *   3. SELECTIVE BLOOM  pixelica_bloom_apply() adds a blurred glow to an
 *      RGB565 surface, selected by an EXPLICIT emissive mask and/or an
 *      explicit emissive image - never by thresholding the whole frame.
 *      Downsample -> bounded separable blur -> bilinear upsample -> additive
 *      composite, so non-emissive pixels keep their exact value and stay
 *      sharp.
 *
 * Memory and state
 *   - No heap, no global or static mutable state, no hidden queues. Every
 *     buffer is caller-owned and only borrowed for the duration of a call.
 *   - Every function validates ALL inputs before writing anything: on a
 *     non-PIXELICA_OK result every output buffer and every output struct is
 *     left exactly as it was.
 *   - Forbidden memory overlaps are checked as address ranges and reported as
 *     PIXELICA_ERR_INVALID_ARG. Whether a pointer really addresses the
 *     documented number of elements cannot be checked and stays the caller's
 *     responsibility.
 *   - Single-threaded: one surface/mask must not be used from two threads at
 *     once.
 *
 * What these tools do NOT do
 *   - They do not compute visibility or occlusion. A light patch is geometry
 *     the caller places; it is depth-TESTED against what is already drawn (so
 *     nearer geometry hides it) but nothing is traced, and an object standing
 *     between an opening and the receiver does not carve a hole into the
 *     patch. Contact shadows are likewise an artistic blob under an object,
 *     not a shadow of its silhouette.
 *   - Colour math is RGB565 end to end; see each function for the exact
 *     rounding.
 */
#ifndef PIXELICA_LIGHT_H
#define PIXELICA_LIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/draw_options.h"
#include "pixelica/framebuffer.h" /* PIXELICA_FrameView (bloom emissive input) */
#include "pixelica/image.h"       /* PIXELICA_Texture, PIXELICA_MAX_TEXTURE_DIMENSION */
#include "pixelica/math.h"        /* PIXELICA_Vec2, PIXELICA_Vec3 */
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest pane subdivision per axis of a RECT mask. */
#define PIXELICA_LIGHT_MAX_PANES 8u
/* Bloom limits (all inclusive). */
#define PIXELICA_BLOOM_MAX_DOWNSAMPLE 8u
#define PIXELICA_BLOOM_MAX_RADIUS 8u
#define PIXELICA_BLOOM_MAX_PASSES 3u

/*
 * Writable RGB565 surface (the bloom composite target). Valid when
 * pixels != NULL, 1 <= width, height <= PIXELICA_MAX_TEXTURE_DIMENSION and
 * stride >= width. Pixel (x, y) is pixels[y * stride + x], row 0 = TOP row;
 * the caller must provide (height - 1) * stride + width elements. Row padding
 * (x >= width) is never read or written.
 *
 * This is a plain CPU surface, so it works for a 2D render target
 * (PIXELICA2D_Surface.pixels), for a snapshot of a 3D frame
 * (pixelica_frame_view + pixelica_frame_copy) and for any other caller buffer.
 */
typedef struct PIXELICA_LightSurface {
    uint16_t *pixels;
    uint16_t width, height;
    size_t stride;
} PIXELICA_LightSurface;

/*
 * Read-only 8-bit coverage/alpha plane (0 = nothing, 255 = full). Same layout
 * rules as PIXELICA_LightSurface.
 *
 * A mask built by pixelica_light_mask_build() is TIGHT (stride == width),
 * which is what PIXELICA_DrawOptions.texture_alpha and
 * pixelica2d_draw_sprite_ex require of an alpha plane. Masks you build
 * yourself may use any stride >= width when only the bloom selection reads
 * them.
 */
typedef struct PIXELICA_LightMask {
    const uint8_t *alpha;
    uint16_t width, height;
    size_t stride;
} PIXELICA_LightMask;

typedef enum PIXELICA_LightShape {
    /* Rectangle with soft edges and optional window bars: a window-light
     * patch, a wall wash, a door spill. */
    PIXELICA_LIGHT_SHAPE_RECT = 0,
    /* Ellipse with a soft falloff: contact/blob shadows, lamp glows. */
    PIXELICA_LIGHT_SHAPE_ELLIPSE = 1
} PIXELICA_LightShape;

/*
 * Shape description; start from pixelica_light_mask_default() and change what
 * you need. A zero-initialized descriptor is NOT valid (panes must be >= 1).
 *
 * Geometry, for texel (x, y) of a width x height mask, with the texel centre
 * at u = (x + 0.5) / width, v = (y + 0.5) / height (both in (0, 1)):
 *
 *   edge(d) = 1                       if softness == 0 and d >= 0
 *           = 0                       if softness == 0 and d <  0
 *           = t * t * (3 - 2t), t = clamp(d / softness, 0, 1)   otherwise
 *
 *   ELLIPSE  r = sqrt((2u - 1)^2 + (2v - 1)^2),  c = edge(1 - r)
 *            so c == 1 inside r <= 1 - softness and 0 at and beyond r == 1.
 *   RECT     dx = 1 - |2u - 1|, dy = 1 - |2v - 1| (distance to the nearer
 *            edge as a fraction of the half extent), c = min(edge(dx), edge(dy)).
 *            Bars (only when bar > 0 and the axis has >= 2 panes): with
 *            p = u * panes_x and n = the nearest integer to p, an INTERNAL
 *            boundary (0 < n < panes_x) darkens c by
 *              clamp((|p - n| - bar / 2) / (panes_x / width), 0, 1)
 *            i.e. a bar of `bar` pane-pitch thickness with a one-texel ramp.
 *            The same applies to v with panes_y and height. The outer border
 *            is shaped by `softness` only, never by a bar.
 *
 *   c is then multiplied by (1 - fade_v * v) and the result is stored as
 *     alpha = floor(c * peak + 0.5)
 *
 * Fields (PIXELICA_ERR_INVALID_ARG outside these ranges):
 *   width, height  1 .. PIXELICA_MAX_TEXTURE_DIMENSION (the mask doubles as a
 *                  texture alpha plane, so it uses the texture limit)
 *   softness       finite, 0 .. 1; fraction of the half extent that fades out
 *   fade_v         finite, 0 .. 1; extra linear fade toward the last row
 *                  (0 = none; 1 = down to nothing), for light that gets
 *                  weaker with distance from the opening
 *   peak           alpha at full coverage (0 = invisible, 255 = full)
 *   panes_x/y      1 .. PIXELICA_LIGHT_MAX_PANES (RECT only; 1 = no bars)
 *   bar            finite, 0 .. 0.5 (RECT only); bar thickness as a fraction
 *                  of one pane pitch, 0 = no bars
 * ELLIPSE does not read panes_x, panes_y or bar, but they are range-checked
 * for every shape, so a stale value is reported instead of silently dropped.
 */
typedef struct PIXELICA_LightMaskDesc {
    PIXELICA_LightShape shape;
    uint16_t width, height;
    float softness;
    float fade_v;
    uint8_t peak;
    uint8_t panes_x, panes_y;
    float bar;
} PIXELICA_LightMaskDesc;

/*
 * Defaults for `shape`:
 *   RECT     48 x 32, softness 0.35, fade_v 0.35, peak 200, 2 x 3 panes,
 *            bar 0.08  (a window-light patch)
 *   ELLIPSE  32 x 32, softness 0.75, fade_v 0, peak 160, panes 1 x 1, bar 0
 *            (a soft contact shadow)
 * PIXELICA_ERR_INVALID_ARG (and *out untouched) for NULL out or an unknown
 * shape.
 */
PIXELICA_Status pixelica_light_mask_default(PIXELICA_LightShape shape,
                                            PIXELICA_LightMaskDesc *out);

/*
 * Rasterizes `desc` into `alpha` as a TIGHT width x height plane and fills
 * *out_mask with a view over it (stride == width). Only the first
 * width * height bytes are written.
 *
 * PIXELICA_ERR_INVALID_ARG (nothing written): NULL desc/alpha/out_mask, a
 * field outside its documented range, an unknown shape, a byte range that
 * would wrap the address space, or `desc` / `out_mask` overlapping the
 * written alpha range.
 * PIXELICA_ERR_CAPACITY: alpha_count < width * height.
 */
PIXELICA_Status pixelica_light_mask_build(const PIXELICA_LightMaskDesc *desc,
                                          uint8_t *alpha, size_t alpha_count,
                                          PIXELICA_LightMask *out_mask);

/*
 * Fills `pixels` with width * height copies of `tint` and describes it as a
 * PIXELICA_Texture. A light patch needs a colour texture the same size as its
 * alpha plane (the renderers sample both at the same texel), and that colour
 * is flat: the SHAPE lives in the mask. Use it for the warm tint of a light
 * patch and for the dark tint of a contact shadow.
 *
 * PIXELICA_ERR_INVALID_ARG (nothing written): NULL pixels/out_texture, width
 * or height outside 1 .. PIXELICA_MAX_TEXTURE_DIMENSION, a pixel range that
 * would wrap the address space, or `out_texture` overlapping the written
 * range.
 * PIXELICA_ERR_CAPACITY: pixel_count < width * height.
 */
PIXELICA_Status pixelica_light_tint_fill(uint16_t tint, uint16_t width, uint16_t height,
                                         uint16_t *pixels, size_t pixel_count,
                                         PIXELICA_Texture *out_texture);

/*
 * Where a shape sits in the world. The patch is the parallelogram
 *   origin + s * u_axis + t * v_axis,  s, t in [0, 1]
 * shifted by `bias` along the unit-length direction of `normal`.
 *
 *   origin  world position of mask texel (0, 0) - the patch corner, not its
 *           centre
 *   u_axis  world vector from `origin` to the mask's +U edge (full width)
 *   v_axis  world vector from `origin` to the mask's +V edge (full height)
 *   normal  the RECEIVER surface normal, pointing away from the surface (+Y
 *           for a floor). It decides the triangle winding (see below) and the
 *           direction of `bias`. Need not be unit length; must be non-zero
 *           and must not lie in the u/v plane.
 *   bias    world units to lift the patch off the receiver so the depth test
 *           does not fight the receiver's own pixels. A few thousandths of a
 *           world unit is usually enough; too much makes the patch float.
 */
typedef struct PIXELICA_LightPlacement {
    PIXELICA_Vec3 origin, u_axis, v_axis, normal;
    float bias;
} PIXELICA_LightPlacement;

/*
 * A ready-to-draw quad: 4 vertices, their mask UVs and 2 triangles.
 *
 * The index order is chosen so the quad is FRONT-facing (counter-clockwise,
 * PIXELICA_CULL_BACK) when seen from the +normal side of the receiver, which
 * is the side a viewer is on. Patches on a floor therefore need no cull-mode
 * change; set PIXELICA_CULL_NONE if you want them visible from behind too.
 */
typedef struct PIXELICA_LightQuad {
    PIXELICA_Vec3 vertices[4];
    PIXELICA_Vec2 uvs[4];
    uint16_t indices[6];
} PIXELICA_LightQuad;

/*
 * Builds the quad of `placement` directly (the shape already lies on the
 * receiver surface).
 *
 * PIXELICA_ERR_INVALID_ARG (and *out untouched): NULL argument, a non-finite
 * field, a zero-length normal, a degenerate parallelogram (u_axis parallel to
 * v_axis), a normal lying in the u/v plane (dot(cross(u, v), normal) == 0, so
 * there is no front side), or a corner that does not round to a finite float.
 */
PIXELICA_Status pixelica_light_quad(const PIXELICA_LightPlacement *placement,
                                    PIXELICA_LightQuad *out);

/*
 * Centred placement, which is how a blob under an object is described: the
 * quad spans 2 * radius_u by 2 * radius_v around `center`, lying in the plane
 * through `center` with normal `normal` and lifted by `bias` along it.
 *
 * +U follows the part of `u_axis` that lies IN the plane
 * (u_axis - normal * dot(u_axis, normal), both unit-length first), so a
 * slightly off-plane hint is fine; +V follows cross(normal, +U), so (U, V,
 * normal) is right-handed. UV (0, 0) is the corner at
 * center - radius_u * U - radius_v * V, as for every other quad.
 *
 * PIXELICA_ERR_INVALID_ARG (and *out untouched): a non-finite argument, a
 * zero-length normal, a radius that is not finite and > 0, a `u_axis` that is
 * zero or parallel to `normal` (nothing of it lies in the plane), or a corner
 * that does not round to a finite float.
 */
PIXELICA_Status pixelica_light_quad_centered(PIXELICA_Vec3 center, PIXELICA_Vec3 normal,
                                             PIXELICA_Vec3 u_axis,
                                             float radius_u, float radius_v, float bias,
                                             PIXELICA_LightQuad *out);

/*
 * Projects an opening (`source`: a window, a door) along ONE light direction
 * onto a receiver plane, which is what makes a window patch land on the floor
 * as a parallelogram instead of a rectangle.
 *
 * Each of the four corners c of the source rectangle (source.bias is applied
 * first, as in pixelica_light_quad) is moved to
 *   p = c + direction * t,   t = dot(plane_normal, plane_point - c)
 *                                / dot(plane_normal, direction)
 * and then lifted by `bias` along the unit-length direction of plane_normal.
 * The mask UVs of the source corners are kept, so the window's panes are
 * projected with it. The winding follows plane_normal as in
 * PIXELICA_LightQuad.
 *
 * `direction` is the direction the light TRAVELS (as in
 * PIXELICA_DirectionalLight.direction), so sunlight coming down through a
 * window has a negative Y component.
 *
 * This is a geometric projection, not a shadow computation: every corner must
 * reach the plane in front of the opening, and nothing between the opening
 * and the plane is considered.
 *
 * PIXELICA_ERR_INVALID_ARG (and *out untouched): NULL argument, a non-finite
 * field, a zero-length direction / plane_normal / source normal, a degenerate
 * source parallelogram, a direction parallel to the plane
 * (dot(plane_normal, direction) == 0), a corner whose t <= 0 (the plane is
 * not in front of that corner along `direction`), a projected quad that is
 * degenerate or edge-on to plane_normal, or a result that does not round to a
 * finite float.
 */
PIXELICA_Status pixelica_light_quad_project(const PIXELICA_LightPlacement *source,
                                            PIXELICA_Vec3 direction,
                                            PIXELICA_Vec3 plane_point,
                                            PIXELICA_Vec3 plane_normal,
                                            float bias,
                                            PIXELICA_LightQuad *out);

/*
 * Selective bloom: what glows is chosen EXPLICITLY, never by thresholding the
 * finished frame.
 *
 *   mask      optional 8-bit selection, the same width/height as the target.
 *             255 = this pixel emits fully, 0 = it does not emit at all.
 *             Build it as you draw the emissive geometry (the 3D core offers
 *             pixelica_emissive_mask_pixel() as a PIXELICA_MeshEffects hook
 *             that records exactly the fragments an emissive draw covered).
 *   emissive  optional separately rendered emissive image (its `color` plane;
 *             `depth` is ignored), the same width/height as the target. Use
 *             it when the glow comes from its own pass; it may be the target
 *             itself.
 * At least one of the two is required. The bloom source colour of a pixel is
 *   base   = emissive ? emissive colour : target colour
 *   weight = mask ? mask alpha / 255 : 1          (0 for a pixel gated out by
 *            `threshold`, see below)
 *   src    = base scaled by weight, per RGB8 channel, rounded
 *
 *   threshold  optional extra luma gate APPLIED INSIDE the selection, never
 *              instead of it: a selected pixel whose
 *              luma = (77 R + 150 G + 29 B + 128) / 256 on RGB8 is below
 *              `threshold` contributes nothing. 0 (the default) disables it.
 *   downsample 1 .. PIXELICA_BLOOM_MAX_DOWNSAMPLE. The source is box-averaged
 *              into a dw x dh image, dw = ceil(width / downsample),
 *              dh = ceil(height / downsample). Dimensions need not divide:
 *              an edge block simply covers fewer pixels and is averaged over
 *              the pixels it really has.
 *   radius     1 .. PIXELICA_BLOOM_MAX_RADIUS, in DOWNSAMPLED pixels. Each
 *              pass is a separable box blur (horizontal, then vertical) of
 *              2 * radius + 1 taps with coordinates clamped to the edge, so
 *              the cost is O(dw * dh * radius), not O(radius^2).
 *   passes     1 .. PIXELICA_BLOOM_MAX_PASSES. Repeating a box blur
 *              approximates a Gaussian; 2 is a good default.
 *   intensity  0 .. 255 strength of the additive composite. 0 leaves the
 *              target untouched and does no work beyond validation.
 *
 * Composite: the blurred image is sampled bilinearly at
 * ((x + 0.5) / downsample - 0.5, likewise y), clamped at the edges, giving an
 * RGB8 glow value per channel, which is added saturating to the target with
 * the renderer's additive rule, per native 5/6/5 channel with maximum m
 * (31 / 63 / 31):
 *   add = floor(glow * intensity / 255 * m / 255 + 0.5)
 *   dst = min(dst + add, m)
 * Because the composite only ever ADDS, a non-emissive pixel outside the glow
 * keeps its exact value: nothing is blurred over the frame.
 *
 * Precision and its limits. The downsampled planes keep 8 bits per channel
 * (they are not re-packed to RGB565 between passes), and the conversion above
 * rounds once, so the smallest glow that still changes a pixel is an RGB8
 * value of about 128/m: >= 5 for red/blue, >= 3 for green. Averaging is
 * physical, though: one bright texel spread over a downsample x downsample
 * block and then over 2 * radius + 1 taps loses roughly that factor of
 * brightness, so a single dim texel can still vanish. For small emitters use
 * a smaller `downsample`/`radius`, a brighter emissive colour, or a higher
 * `intensity`; docs/pixel_light.md lists measured numbers.
 */
typedef struct PIXELICA_BloomDesc {
    const PIXELICA_LightMask *mask;     /* NULL = no mask selection */
    const PIXELICA_FrameView *emissive; /* NULL = read the target itself */
    uint8_t downsample, radius, passes, intensity, threshold;
} PIXELICA_BloomDesc;

/*
 * Defaults: downsample 4, radius 3, passes 2, intensity 150, threshold 0,
 * no mask and no emissive image (so the caller MUST set at least one of them
 * before pixelica_bloom_apply accepts the descriptor).
 * PIXELICA_ERR_INVALID_ARG (and *out untouched) for NULL out.
 */
PIXELICA_Status pixelica_bloom_default(PIXELICA_BloomDesc *out);

/*
 * Scratch pixels pixelica_bloom_apply() needs:
 *   6 * ceil(width / downsample) * ceil(height / downsample)
 * (two ping-pong planes of the downsampled image, three 8-bit channels per
 * pixel). It returns 0 for any input pixelica_bloom_apply() would reject with
 * PIXELICA_ERR_INVALID_ARG except the scratch itself: the target, the
 * descriptor, the mask and the emissive image are validated here with exactly
 * the same rules, including the "no descriptor inside the target pixels" rule
 * below. Only `scratch` and `scratch_count` are not considered.
 */
size_t pixelica_bloom_scratch_pixels(const PIXELICA_LightSurface *target,
                                     const PIXELICA_BloomDesc *desc);

/*
 * Applies the bloom of `desc` to `target` in place (see PIXELICA_BloomDesc).
 * Only the first pixelica_bloom_scratch_pixels() scratch pixels are written.
 *
 * PIXELICA_ERR_INVALID_ARG (nothing written): NULL target/desc/scratch, an
 * invalid target, neither mask nor emissive image, a mask or emissive image
 * whose width/height differ from the target's or whose pointer/stride is
 * invalid, a field outside its documented range, a pixel range that would
 * wrap the address space, `scratch` overlapping the target pixels, the mask
 * bytes, the emissive pixels or any of the descriptors, or any DESCRIPTOR
 * object (target / desc / mask / emissive) lying inside the target pixel
 * range: the composite writes the target while still reading those structs.
 *
 * The read-only mask bytes and emissive PIXELS may overlap the target, which
 * is the intended way to bloom the frame you just drew: every source pixel is
 * consumed by the downsample pass (which writes only scratch) before the
 * composite writes the first target pixel.
 * PIXELICA_ERR_CAPACITY: scratch_count < pixelica_bloom_scratch_pixels().
 */
PIXELICA_Status pixelica_bloom_apply(PIXELICA_LightSurface *target,
                                     const PIXELICA_BloomDesc *desc,
                                     uint16_t *scratch, size_t scratch_count);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_LIGHT_H */

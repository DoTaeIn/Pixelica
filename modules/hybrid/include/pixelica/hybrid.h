/*
 * Pixelica - 2.5D hybrid: 2D sprites / HUD drawn into the 3D frame.
 *
 * This header is the ONLY place where the 2D and 3D engines meet. It needs
 * both Pixelica::core (3D) and Pixelica::renderer2d (2D); a 2D-only SDK does
 * not contain it.
 *
 * Frame protocol (exactly one PAL present per frame, owned by the 3D end):
 *
 *   pixelica_begin_frame(ctx, clear)          clears color + depth
 *   pixelica_draw_mesh(...)                   3D geometry writes color + depth
 *   pixelica2d_bind_3d(&canvas, ctx)          borrow the SAME buffers as a 2D view
 *   pixelica25d_draw_billboard(&canvas, ...)  depth-tested sprites at 3D positions
 *   pixelica2d_fill_rect / pixelica2d_draw_text    HUD in screen pixels (depth ignored)
 *   pixelica_end_frame(ctx)                   the only present
 *
 * Coordinate systems (do not mix them):
 *   3D world   right-handed, +X right, +Y UP, camera looks down -Z.
 *   2D screen  +X right, +Y DOWN, pixel (0,0) = top-left, row 0 = top.
 * pixelica25d_project converts a 3D point to the 2D screen space; a billboard's
 * position argument is 3D, everything else about the sprite is 2D.
 *
 * Statistics are separate: PIXELICA_FrameStats (pixelica_frame_stats) counts meshes and
 * 3D fragments only; PIXELICA2D_Context::stats counts 2D draws only. A billboard
 * never changes the 3D counters and a mesh never changes the 2D counters.
 */
#ifndef PIXELICA_HYBRID_H
#define PIXELICA_HYBRID_H
#include "pixelica/pixelica.h"
#include "pixelica/renderer2d.h"
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Result of projecting one world point (see pixelica25d_project).
 *
 *   screen              pixel position, +Y down. May lie outside the viewport.
 *   pixels_per_unit_x/y apparent size of one world unit along the camera's
 *                       screen X / Y axis at this depth, in pixels.
 *   depth               value comparable with the 3D depth buffer, 0..65534
 *                       (0 = near plane, never PIXELICA2D_DEPTH_CLEAR).
 *   drawable            false when the point is behind the camera or outside
 *                       the near/far planes. Every other field is then 0.
 */
typedef struct PIXELICA25D_Projection {
    PIXELICA_Vec2 screen;
    float pixels_per_unit_x, pixels_per_unit_y;
    uint16_t depth;
    bool drawable;
} PIXELICA25D_Projection;

/*
 * Creates a 2D view over the exact color and depth buffers of `ctx`.
 *
 * Requirements (else PIXELICA_ERR_INVALID_ARG and *out is left byte-for-byte
 * unchanged): out != NULL, ctx is a live context, a frame is active
 * (pixelica_begin_frame was called and pixelica_end_frame has not been), and `out` is
 * DISJOINT from everything the context owns: the PIXELICA_Context itself, its
 * color and depth buffers, its frame arena and its guard bytes (one
 * contiguous block, for heap and caller-owned contexts alike). Any overlap of
 * the sizeof(PIXELICA2D_Context) bytes at `out` with that block is rejected before
 * `out` is read or written; keep the view in your own storage (stack, static
 * or a separate allocation). The view has stride == depth_stride == the 3D
 * width, a default camera, a full-viewport clip and zeroed 2D stats.
 *
 * Bind never clears, presents or allocates. The view BORROWS the 3D buffers:
 * it must not be used after pixelica_end_frame, pixelica_destroy or the next
 * pixelica_begin_frame of that context. Do not retain it; bind again each frame.
 * The 3D context stays the owner and remains single-threaded.
 */
PIXELICA_Status pixelica2d_bind_3d(PIXELICA2D_Context *out, PIXELICA_Context *ctx);

/*
 * Projects `position` with a column-major view_projection matrix (clip =
 * view_projection * (x, y, z, 1)) onto a width x height viewport. The clip
 * transform is evaluated in double precision.
 *
 * Errors (PIXELICA_ERR_INVALID_ARG, *out unchanged): NULL pointers, non-finite
 * matrix or position, width/height outside 1..PIXELICA2D_MAX_DIMENSION, a
 * degenerate matrix (zero or non-finite x/y row norm), or a result that does
 * not fit a float (screen position, pixels-per-unit).
 *
 * If clip w < PIXELICA_CLIP_W_EPSILON (behind the camera) or clip z lies outside
 * [-w, w] (before near / beyond far) the call returns PIXELICA_OK with
 * drawable == false and all other fields zero. A point whose x/y fall outside
 * the viewport IS still drawable: a large billboard may intersect the screen
 * even when its anchor does not.
 *
 * Mapping: NDC (-1,-1) is the bottom-left corner, y flips to screen-down;
 * depth = round((ndc_z + 1) / 2 * 65534), the same quantization convention as
 * 3D meshes. The projection is evaluated in double precision whereas the 3D
 * mesh pipeline uses float intermediates, so a sprite and a mesh at the same
 * nominal distance may differ by one depth unit near a rounding boundary. Do
 * not rely on exact coplanarity or on equal-depth tie-breaking between a
 * billboard and a mesh: choose an explicit separation in view distance. (The
 * strict LESS test itself still keeps the first drawn of two EQUAL values.)
 * pixels_per_unit_x = |row0.xyz| * width / (2 w) and
 * pixels_per_unit_y = |row1.xyz| * height / (2 w), where row0/row1 are the
 * first two rows of the matrix. This is exact for the standard symmetric
 * perspective (pixelica_mat4_perspective) and orthographic (pixelica_mat4_orthographic)
 * projections with an orthonormal camera; oblique, sheared or off-axis custom
 * projections and non-uniform camera scales are not promised.
 */
PIXELICA_Status pixelica25d_project(const float view_projection[16], PIXELICA_Vec3 position,
                         uint16_t width, uint16_t height, PIXELICA25D_Projection *out);

/*
 * Draws `appearance` as a depth-tested billboard anchored at the 3D point
 * `position`, sized `world_size` world units, into a canvas bound with
 * pixelica2d_bind_3d (or any 2D context that has a depth buffer).
 *
 * The appearance supplies texture, source rectangle, pivot (source pixels,
 * the anchor that lands on the projected point), rotation, opacity, color key
 * and depth_write. The call overrides position (projected screen point),
 * depth (projected) and forces depth_test = true. The appearance's own
 * position, depth and depth_test are ignored and need not be valid.
 *
 * Scale: appearance.scale.x / .y must be finite and non-zero (the usual
 * pixelica2d_sprite_init default is +1). Only their SIGN is used: the magnitude is
 * world_size * pixels-per-unit / source size, and a negative component
 * mirrors the billboard around its pivot on that axis (x: left-right, y:
 * up-down), so a 2.5D character can face either way without a second
 * texture. The mirrored sprite keeps the same projected depth, color key,
 * opacity, rotation and pivot. `appearance` is read only.
 *
 * Validation happens before anything is drawn and before the "behind the
 * camera" early-out: a bad canvas (no depth buffer), NULL pointer, invalid
 * texture/source/pivot/rotation, a zero / NaN / infinite scale component,
 * non-finite or non-positive world_size, a bad matrix or an unrepresentable
 * size returns PIXELICA_ERR_INVALID_ARG even for a point that would not be
 * visible. On any error nothing is drawn and the canvas (pixels, depth,
 * stats) is unchanged.
 *
 * Behind / before near / beyond far: returns PIXELICA_OK, draws nothing, adds no
 * stat. A size that underflows to a zero pixel scale is treated the same way.
 *
 * The canvas camera is ignored (a billboard is placed in screen pixels, so a
 * scrolling 2D camera never moves it) and left untouched; the canvas clip is
 * honoured. An accepted billboard adds exactly one draw to the canvas stats.
 * Transparent texels (color key or opacity 0) write neither color nor depth.
 */
PIXELICA_Status pixelica25d_draw_billboard(PIXELICA2D_Context *canvas, const float view_projection[16],
                                const PIXELICA2D_Sprite *appearance, PIXELICA_Vec3 position, PIXELICA_Vec2 world_size);
#ifdef __cplusplus
}
#endif
#endif

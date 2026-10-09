/*
 * Pixelica - renderer data types (unlit: flat colored, per-triangle colored
 * or nearest-sampled textured triangles).
 *
 * Pipeline contract (implemented on the CPU in portable C11):
 *
 *   clip = view_projection * model * (vertex, 1)
 *
 * 1. Back-face culling (default PIXELICA_CULL_BACK). A triangle is FRONT-facing
 *    when its vertices appear counter-clockwise on screen (OpenGL default).
 *    Facing is decided from the homogeneous clip coordinates before
 *    clipping, so it is also correct for triangles crossing the camera.
 * 2. Clipping in homogeneous space against w >= PIXELICA_CLIP_W_EPSILON and the
 *    six frustum planes  -w <= x,y,z <= w  (Sutherland-Hodgman, bounded
 *    fixed-size stack polygon). Triangles behind the camera (w <= 0) are
 *    never projected. Intersections are always computed from the inside
 *    vertex toward the outside vertex, so an edge shared by two triangles
 *    is clipped to bit-identical points in both.
 * 3. Perspective divide and viewport transform: NDC (-1,-1) is the
 *    bottom-left corner of the target, (+1,+1) the top-right; buffer row 0
 *    is the TOP row. Pixel (px,py) is sampled at its center (px+.5, py+.5).
 * 4. Screen positions are snapped to fixed point with PIXELICA_SUBPIXEL_BITS
 *    fractional bits. Coverage uses integer (int64) edge functions and the
 *    TOP-LEFT fill rule: a pixel center exactly on an edge is covered only
 *    if that edge is a top edge (horizontal, interior below it) or a left
 *    edge. Triangles sharing an edge therefore never leave gaps and never
 *    cover a pixel twice. Zero-area (after snapping) triangles draw nothing.
 * 5. Depth: NDC z in [-1, 1] maps linearly to [0, PIXELICA_DEPTH_MAX_DRAWABLE]
 *    (0 = near plane, 65534 = far plane). The depth buffer is cleared to
 *    UINT16_MAX (PIXELICA_DEPTH_CLEAR), so anything inside the frustum passes
 *    against a cleared pixel. Test is strictly LESS: the nearer fragment
 *    wins, an equal-depth later fragment does not overwrite.
 *    Depth is interpolated linearly in screen space (NDC z is affine in
 *    screen space) and clamped to the triangle's vertex depth range.
 *
 * 6. Shading (see PIXELICA_Material): textured fragments take the nearest texel
 *    (opaque); otherwise the triangle's color. Texture coordinates are
 *    clipped linearly in clip space together with the position and are
 *    interpolated perspective-correctly (u/w, v/w and 1/w are affine in
 *    screen space). Flat-colored output is bit-identical to G0.
 *
 * Overflow contract: width/height <= PIXELICA_MAX_DIMENSION, so snapped
 * coordinates fit in 17 bits and every edge-function product fits easily in
 * int64. Post-clip coordinates are additionally clamped to the viewport.
 */
#ifndef PIXELICA_RENDERER_H
#define PIXELICA_RENDERER_H

#include <stdint.h>

#include "pixelica/image.h" /* PIXELICA_Texture, PIXELICA_RGB565, PIXELICA_MAX_TEXTURE_DIMENSION */
#include "pixelica/math.h"  /* PIXELICA_Vec2, PIXELICA_Vec3 */

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_MAX_DIMENSION 4096u
#define PIXELICA_SUBPIXEL_BITS 4
#define PIXELICA_DEPTH_CLEAR UINT16_MAX
#define PIXELICA_DEPTH_MAX_DRAWABLE ((uint16_t)(UINT16_MAX - 1u))
#define PIXELICA_CLIP_W_EPSILON 1e-5f

/*
 * Frame-arena bytes needed per mesh vertex (transformed clip-space vertex
 * scratch). Configure  frame_arena_bytes >=
 *   max_vertices_per_mesh * PIXELICA_ARENA_BYTES_PER_VERTEX.
 */
#define PIXELICA_ARENA_BYTES_PER_VERTEX 20u

/* PIXELICA_MAX_TEXTURE_DIMENSION, PIXELICA_RGB565 and PIXELICA_Texture live in image.h;
 * PIXELICA_Vec2 lives in math.h. Both are included above. */

/*
 * Indexed triangle list. Every 3 indices form one triangle; index_count must
 * be a multiple of 3 and every index must be < vertex_count. The mesh
 * (and the arrays it points to) is only read during pixelica_draw_mesh(); the
 * caller keeps ownership. Without a material the mesh is drawn unlit in the
 * single RGB565 `color`.
 */
typedef struct PIXELICA_Mesh {
    const PIXELICA_Vec3 *vertices;
    uint32_t vertex_count;
    const uint16_t *indices;
    uint32_t index_count;
    uint16_t color;
} PIXELICA_Mesh;

/*
 * Optional per-draw material (pixelica_draw_mesh_ex). Every pointer may be NULL.
 *
 *   uvs             one texture coordinate per mesh vertex (x = u, y = v).
 *                   When non-NULL every one of the mesh's vertex_count UVs
 *                   must be finite (checked before any pixel is written).
 *                   (u, v) = (0, 0) is the top-left corner of the texture,
 *                   (1, 1) the bottom-right. Sampling is nearest-neighbor:
 *                   texel (floor(u * width), floor(v * height)) with u and v
 *                   clamped to [0, 1] per fragment (1 maps to the last
 *                   texel).
 *   triangle_colors one RGB565 color per triangle (index_count / 3 entries).
 *   texture         requires uvs (for vertex_count > 0).
 *
 * Fragment color: the texel if texture != NULL (opaque, no blending);
 * otherwise triangle_colors[triangle] if non-NULL; otherwise mesh->color.
 * A NULL material is identical to pixelica_draw_mesh().
 */
typedef struct PIXELICA_Material {
    const PIXELICA_Vec2 *uvs;
    const uint16_t *triangle_colors;
    const PIXELICA_Texture *texture;
} PIXELICA_Material;

typedef enum PIXELICA_CullMode {
    PIXELICA_CULL_BACK = 0, /* default: skip clockwise (back-facing) triangles */
    PIXELICA_CULL_NONE      /* draw both windings */
} PIXELICA_CullMode;

/*
 * Per-frame counters, reset by pixelica_begin_frame(). Every counter saturates at
 * UINT32_MAX instead of wrapping.
 *
 * Coverage diagnostics (frame of W x H pixels):
 *   screen coverage    = pixels_covered / (W * H)
 *   geometric overdraw = fragments_covered / pixels_covered   (>= 1)
 *   shaded overdraw    = pixels_written / pixels_covered
 * pixels_covered counts a pixel the first time any fragment covers it in the
 * frame (detected as "depth still holds PIXELICA_DEPTH_CLEAR", which no drawn
 * fragment can produce), so it is the number of distinct covered pixels.
 */
typedef struct PIXELICA_FrameStats {
    uint32_t draw_calls;          /* accepted draw calls */
    uint32_t triangles_submitted; /* triangles in accepted meshes */
    uint32_t triangles_culled;    /* back-facing or edge-on */
    uint32_t triangles_rejected;  /* fully outside the frustum */
    uint32_t triangles_clipped;   /* needed polygon clipping */
    uint32_t triangles_rasterized;/* screen triangles sent to raster */
    uint32_t pixels_written;      /* fragments that passed depth test */
    uint32_t fragments_covered;   /* covered fragments before the depth test */
    uint32_t pixels_covered;      /* distinct frame pixels covered */
    uint32_t arena_high_water;    /* max frame-arena bytes used */
    PIXELICA_Status last_error;        /* most recent failure this frame */
} PIXELICA_FrameStats;

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_RENDERER_H */

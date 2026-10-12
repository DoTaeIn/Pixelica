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
 * 7. Draw options (pixelica_draw_mesh_options, draw_options.h; NULL = the
 *    defaults = the behavior above):
 *    - wrap_u/wrap_v REPEAT: per axis, texel floor(frac(t) * size) with
 *      frac(t) = t - floor(t) evaluated in double (negative t wraps, exact
 *      integers map to texel 0, any finite UV is safe). CLAMP is unchanged.
 *    - alpha: a = texture_alpha[same texel as the color] (255 if no plane or
 *      untextured); effective = (a * opacity + 127) / 255. OPAQUE ignores
 *      both. The depth test runs first (no sampling for hidden fragments).
 *      TEST: effective < alpha_cutoff is discarded (no color, no depth);
 *      survivors are opaque and write depth. BLEND: depth-tested (LESS)
 *      against what is already drawn but NEVER writes depth; effective 0
 *      is skipped, 255 copies, otherwise per 5/6/5 channel
 *      (src*a + dst*(255-a) + 127) / 255. No sorting is done: draw opaque
 *      and TEST geometry first, then BLEND geometry back-to-front.
 * 8. Mesh effects (pixelica_draw_mesh_effects, PIXELICA_MeshEffects below):
 *    world clip planes run before step 2 (trivial reject, then polygon
 *    clipping ahead of the frustum planes), bilinear filtering replaces the
 *    nearest fetch of step 6, and the pixel hook runs between alpha
 *    computation and the step 7 test/blend/write.
 *
 * Overflow contract: width/height <= PIXELICA_MAX_DIMENSION, so snapped
 * coordinates fit in 17 bits and every edge-function product fits easily in
 * int64. Post-clip coordinates are additionally clamped to the viewport.
 */
#ifndef PIXELICA_RENDERER_H
#define PIXELICA_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/draw_options.h" /* PIXELICA_DrawOptions (wrap, alpha) */
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
 *                   texel), or wrapped with PIXELICA_WRAP_REPEAT (step 7).
 *   triangle_colors one RGB565 color per triangle (index_count / 3 entries).
 *   texture         requires uvs (for vertex_count > 0).
 *
 * Fragment color: the texel if texture != NULL (opaque unless draw options
 * select TEST/BLEND);
 * otherwise triangle_colors[triangle] if non-NULL; otherwise mesh->color.
 * A NULL material is identical to pixelica_draw_mesh().
 */
typedef struct PIXELICA_Material {
    const PIXELICA_Vec2 *uvs;
    const uint16_t *triangle_colors;
    const PIXELICA_Texture *texture;
} PIXELICA_Material;

/*
 * Layered material (pixelica_draw_mesh_layers, docs/material_layers.md).
 * Fragment color, in this order:
 *   1. base: exactly the PIXELICA_Material color above (base.texture with
 *      the draw options' wrap, else triangle_colors, else mesh->color).
 *   2. layers[0..layer_count): texel t of layer k (own UVs, own wrap,
 *      nearest, RGB only) combined per native 5/6/5 channel with the
 *      previous color p (channel maximum m = 31/63/31):
 *        MULTIPLY  r = (p*t + m/2) / m        (integer, rounded)
 *        ADD       r = min(p + t, m)          (saturated)
 *        LERP      r = t
 *      then mixed by opacity a: (r*a + p*(255-a) + 127) / 255.
 *   3. vertex_colors (optional, one RGBA8 per vertex, NULL = opaque
 *      white = no change): r, g, b, a are clipped and interpolated
 *      perspective-correctly in double (no intermediate quantization),
 *      clamped to [0, 255] giving c. Each 5/6/5 channel of the result of
 *      step 2 becomes floor(channel * c / 255 + 0.5). 255 is an exact
 *      identity, 0 gives 0.
 *   4. alpha (TEST/BLEND only; OPAQUE ignores it): s = base texture alpha
 *      (draw options' texture_alpha, 255 without it; additional layers have
 *      none), s = floor(s * c_a / 255 + 0.5), then the usual
 *      (s * opacity + 127) / 255, cutoff and blend of step 7.
 * A zero-initialized PIXELICA_MaterialLayers is the flat mesh->color draw.
 */
#define PIXELICA_MAX_TEXTURE_LAYERS 3u

typedef struct PIXELICA_ColorRGBA8 {
    uint8_t r, g, b, a;
} PIXELICA_ColorRGBA8;

typedef enum PIXELICA_TextureCombine {
    PIXELICA_TEXTURE_MULTIPLY = 0,
    PIXELICA_TEXTURE_ADD = 1,
    PIXELICA_TEXTURE_LERP = 2
} PIXELICA_TextureCombine;

typedef struct PIXELICA_TextureLayer {
    const PIXELICA_Texture *texture; /* required, valid like base textures */
    const PIXELICA_Vec2 *uvs;        /* vertex_count finite UVs (required) */
    PIXELICA_WrapMode wrap_u, wrap_v;
    PIXELICA_TextureCombine combine;
    uint8_t opacity;                 /* 0 = layer has no effect, 255 = full */
} PIXELICA_TextureLayer;

typedef struct PIXELICA_MaterialLayers {
    PIXELICA_Material base;
    const PIXELICA_ColorRGBA8 *vertex_colors; /* NULL or vertex_count */
    const PIXELICA_TextureLayer *layers;  /* may be NULL if layer_count == 0 */
    size_t layer_count;                   /* 0..PIXELICA_MAX_TEXTURE_LAYERS */
} PIXELICA_MaterialLayers;

/*
 * Mesh effects (pixelica_draw_mesh_effects, docs/render_effects.md). A
 * zero-initialized PIXELICA_MeshEffects (NEAREST, no hook, no planes) and a
 * NULL pointer both draw exactly like pixelica_draw_mesh_layers.
 *
 * filter BILINEAR applies to the base texture, its texture_alpha plane and
 * every additional layer (NEAREST keeps the original sampling bit for bit).
 * Per axis with t the perspective-correct coordinate (double; non-finite or
 * no valid weight -> 0) and n the texture size:
 *   CLAMP : s = t*n - 0.5 (t first limited to [-1, 2], which never changes
 *           the result), i0 = floor(s), f = s - i0, taps i0 and i0 + 1 each
 *           clamped to [0, n-1].
 *   REPEAT: t' = t - floor(t), s = t'*n - 0.5, i0 = floor(s), f = s - i0,
 *           taps i0 and i0 + 1 each wrapped into [0, n-1] independently.
 * Each native 5/6/5 channel (and alpha8) of the four taps c00 (i0,j0),
 * c10 (i1,j0), c01 (i0,j1), c11 (i1,j1) is combined in double as
 *   ((c00*(1-fx) + c10*fx)*(1-fy) + (c01*(1-fx) + c11*fx)*fy)
 * and rounded with floor(v + 0.5). Layer combine, vertex RGBA and alpha
 * then follow PIXELICA_MaterialLayers unchanged.
 *
 * pixel: called once per covered fragment that passed the (early) depth
 * test, after material color, layers, vertex RGBA, texture alpha and
 * opacity, before the alpha test/blend/depth+color write. *out_color and
 * *out_alpha start as fragment->color/alpha (alpha is 255 in OPAQUE).
 * Returning false discards the fragment (no color, no depth). OPAQUE
 * ignores *out_alpha; TEST/BLEND use it for the cutoff/blend. x, y are
 * framebuffer pixels (row 0 = top), depth is the raw nonlinear uint16 depth
 * value that would be written. userdata is borrowed. The callback must not
 * call any API that mutates or draws into the same context (such calls are
 * refused); see docs/render_effects.md.
 *
 * clip_planes: a world-space point p is kept when
 *   dot(normal, p) + distance >= 0      (p = model * vertex)
 * Up to PIXELICA_MAX_WORLD_CLIP_PLANES planes, each with a finite non-zero
 * normal (need not be unit length) and finite distance. A non-zero count
 * requires an affine model matrix (m[3] = m[7] = m[11] = 0, m[15] = 1).
 * Triangles are polygon-clipped before projection (UVs and vertex RGBA
 * interpolated), not discarded per pixel.
 */
#define PIXELICA_MAX_WORLD_CLIP_PLANES 4u

typedef struct PIXELICA_WorldClipPlane {
    PIXELICA_Vec3 normal;
    float distance;
} PIXELICA_WorldClipPlane;

typedef struct PIXELICA_Fragment {
    uint16_t x, y;   /* framebuffer pixel */
    uint16_t depth;  /* raw depth buffer value (0 = near, nonlinear) */
    uint16_t color;  /* RGB565 after material/vertex color */
    uint8_t alpha;   /* after texture alpha * vertex alpha * opacity */
} PIXELICA_Fragment;

typedef bool (*PIXELICA_PixelFn)(void *userdata,
                                 const PIXELICA_Fragment *fragment,
                                 uint16_t *out_color, uint8_t *out_alpha);

typedef struct PIXELICA_MeshEffects {
    PIXELICA_FilterMode filter;
    PIXELICA_PixelFn pixel;   /* NULL = no hook */
    void *userdata;           /* passed to pixel, borrowed */
    const PIXELICA_WorldClipPlane *clip_planes; /* NULL if count == 0 */
    size_t clip_plane_count;  /* 0..PIXELICA_MAX_WORLD_CLIP_PLANES */
} PIXELICA_MeshEffects;

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
 * frame, tracked by a per-frame coverage bitmap (ceil(W*H/8) bytes of
 * context memory, cleared by pixelica_begin_frame). It is the number of
 * distinct geometrically covered pixels, including fragments that failed
 * the depth test, were alpha-discarded or were blended without depth.
 * pixels_written counts fragments whose color was written (passed depth
 * and, for TEST/BLEND, the alpha check); blended pixels count every time.
 */
typedef struct PIXELICA_FrameStats {
    uint32_t draw_calls;          /* accepted draw calls */
    uint32_t triangles_submitted; /* triangles in accepted meshes */
    uint32_t triangles_culled;    /* back-facing or edge-on */
    uint32_t triangles_rejected;  /* fully outside the frustum or one world clip plane */
    uint32_t triangles_clipped;   /* needed polygon clipping (frustum and/or world planes) */
    uint32_t triangles_rasterized;/* screen triangles sent to raster */
    uint32_t pixels_written;      /* fragments that wrote color */
    uint32_t fragments_covered;   /* covered fragments before the depth test */
    uint32_t pixels_covered;      /* distinct frame pixels covered (bitmap) */
    uint32_t arena_high_water;    /* max frame-arena bytes used */
    PIXELICA_Status last_error;        /* most recent failure this frame */
} PIXELICA_FrameStats;

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_RENDERER_H */

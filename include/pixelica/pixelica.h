/*
 * Pixelica - public engine API.
 *
 * Include this one header to get every public 3D-SDK type:
 * status.h, input.h, platform.h, math.h, image.h, renderer.h, assets_common.h,
 * assets.h, scene.h, animation.h.
 *
 * This is the 3D umbrella header (Pixelica::core / renderer3d). The common
 * headers (status, input, platform, math, animation, image, assets_common) can
 * be included individually without the 3D renderer; a 2D-only SDK installs no
 * pixelica.h.
 *
 * Memory ownership
 *   - pixelica_create() performs the ONLY heap allocation (one block holding the
 *     context, the RGB565 color buffer, the uint16 depth buffer, the
 *     frame arena and a ceil(W*H/8)-byte coverage bitmap for stats).
 *     pixelica_destroy() frees it. Nothing is allocated per frame.
 *   - pixelica_create_in_buffer() places the same layout in caller-owned memory
 *     (e.g. a static MCU buffer) and never allocates; size it with
 *     pixelica_memory_requirement(). Any alignment is accepted.
 *   - Buffers returned by pixelica_color_buffer()/pixelica_depth_buffer() are owned by
 *     the context and valid until pixelica_destroy(). Layout: width*height
 *     elements, row-major, row 0 = top, stride = width.
 *   - A failed create leaves *out_ctx == NULL and leaks nothing.
 *
 * Frame protocol
 *   pixelica_begin_frame -> any number (<= max_draw_commands) of pixelica_draw_mesh
 *   -> pixelica_end_frame. Draws rasterize immediately into the context buffers;
 *   mesh/matrix memory only needs to be valid during the call.
 *   pixelica_end_frame() calls PAL present_rgb565 exactly once (synchronously);
 *   there is no separate present call.
 *
 * Threading: a context is single-threaded; no global state is used.
 */
#ifndef PIXELICA_PIXELICA_H
#define PIXELICA_PIXELICA_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/framebuffer.h"

#include "pixelica/status.h"
#include "pixelica/input.h"
#include "pixelica/platform.h"
#include "pixelica/math.h"
#include "pixelica/image.h"
#include "pixelica/renderer.h"
#include "pixelica/assets_common.h"
#include "pixelica/assets.h"
#include "pixelica/scene.h"
#include "pixelica/animation.h"
#include "pixelica/camera.h"
#include "pixelica/debug.h"
#include "pixelica/chunks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Also declared (under the same guard) by scene.h. */
#ifndef PIXELICA_CONTEXT_TYPEDEF_DECLARED
#define PIXELICA_CONTEXT_TYPEDEF_DECLARED
typedef struct PIXELICA_Context PIXELICA_Context;
#endif

/*
 * Validation (else PIXELICA_ERR_INVALID_ARG):
 *   1 <= width, height <= PIXELICA_MAX_DIMENSION
 *   max_draw_commands >= 1, max_vertices_per_mesh >= 1
 *   frame_arena_bytes >= max_vertices_per_mesh * PIXELICA_ARENA_BYTES_PER_VERTEX
 */
typedef struct PIXELICA_Config {
    uint16_t width, height;
    uint16_t max_draw_commands;     /* accepted draws per frame */
    uint16_t max_vertices_per_mesh; /* larger meshes -> PIXELICA_ERR_CAPACITY */
    uint32_t frame_arena_bytes;     /* per-draw transform scratch */
} PIXELICA_Config;

/* Fixed simulation step: 60 Hz. The rate is defined once, by the common input
 * header (PIXELICA_INPUT_TICK_HZ), so input replay and pixelica_update() cannot drift
 * apart and the common input code needs no 3D header to check it. */
#define PIXELICA_UPDATE_HZ PIXELICA_INPUT_TICK_HZ
#define PIXELICA_MAX_UPDATES_PER_CALL 4u
/* Larger elapsed values are clamped to this before accounting. */
#define PIXELICA_MAX_ELAPSED_US 1000000u

typedef void (*PIXELICA_UpdateFn)(void *userdata, float dt, uint32_t buttons);

/* Bytes needed by pixelica_create_in_buffer() for `cfg` (includes alignment
 * slack). PIXELICA_ERR_INVALID_ARG for invalid config or size overflow. */
PIXELICA_Status pixelica_memory_requirement(const PIXELICA_Config *cfg, size_t *out_bytes);

/* `platform` may be NULL (headless). */
PIXELICA_Status pixelica_create(const PIXELICA_Config *cfg,
                      const PIXELICA_PlatformOps *platform,
                      PIXELICA_Context **out_ctx);

/* Caller-owned memory; PIXELICA_ERR_NO_MEMORY if `memory_bytes` is too small.
 * The memory must stay valid and untouched until pixelica_destroy(). */
PIXELICA_Status pixelica_create_in_buffer(const PIXELICA_Config *cfg,
                                const PIXELICA_PlatformOps *platform,
                                void *memory, size_t memory_bytes,
                                PIXELICA_Context **out_ctx);

/* Frees heap memory from pixelica_create(); for pixelica_create_in_buffer() it only
 * invalidates the context. NULL is a no-op. */
void pixelica_destroy(PIXELICA_Context *ctx);

/* Column-major projection*view used by subsequent draws (default identity),
 * so clip = matrix * model * vertex. A NULL or non-finite matrix puts the context in an invalid-camera state:
 * every pixelica_draw_mesh returns PIXELICA_ERR_INVALID_ARG until a valid matrix is
 * set. */
void pixelica_set_view_projection(PIXELICA_Context *ctx, const float matrix[16]);

/* Default PIXELICA_CULL_BACK. Invalid values are ignored. */
void pixelica_set_cull_mode(PIXELICA_Context *ctx, PIXELICA_CullMode mode);

/* Clears color to `clear_color`, depth to PIXELICA_DEPTH_CLEAR, the coverage
 * bitmap, resets the draw counter, frame arena and stats. Calling it again
 * restarts the frame. */
void pixelica_begin_frame(PIXELICA_Context *ctx, uint16_t clear_color);

/*
 * Validates everything before touching a pixel, so a failed draw leaves the
 * buffers unchanged:
 *   PIXELICA_ERR_INVALID_ARG: NULL ctx/mesh/model, not inside a frame, invalid
 *     camera, non-finite model matrix, NULL arrays with non-zero counts,
 *     index_count % 3 != 0, an index >= vertex_count, a non-finite vertex,
 *     or a transformed vertex that overflows to a non-finite value.
 *   PIXELICA_ERR_CAPACITY: vertex_count > max_vertices_per_mesh, or the frame
 *     already holds max_draw_commands accepted draws.
 * An empty mesh (index_count == 0) is accepted and draws nothing.
 */
PIXELICA_Status pixelica_draw_mesh(PIXELICA_Context *ctx, const PIXELICA_Mesh *mesh,
                         const float model[16]);

/*
 * pixelica_draw_mesh with an optional material (see PIXELICA_Material in renderer.h).
 * `material` == NULL is exactly pixelica_draw_mesh(). Additional validation,
 * also done before any pixel is written (PIXELICA_ERR_INVALID_ARG):
 *   texture != NULL with uvs == NULL (and vertex_count > 0), texture with
 *   NULL pixels or a width/height outside 1..PIXELICA_MAX_TEXTURE_DIMENSION, or
 *   any of the vertex_count UVs non-finite.
 * triangle_colors must hold index_count / 3 entries (not checkable).
 */
PIXELICA_Status pixelica_draw_mesh_ex(PIXELICA_Context *ctx, const PIXELICA_Mesh *mesh,
                            const float model[16],
                            const PIXELICA_Material *material);

/*
 * pixelica_draw_mesh_ex with texture wrap and alpha options (draw_options.h,
 * renderer.h step 7). `options` == NULL is exactly pixelica_draw_mesh_ex().
 * Also rejected before any pixel is written (PIXELICA_ERR_INVALID_ARG):
 *   wrap_u/wrap_v/alpha_mode outside their enums; texture_alpha == NULL with
 *   texture_alpha_bytes != 0; texture_alpha without a material texture or
 *   with texture_alpha_bytes < width*height; `options` or the declared
 *   alpha range overlapping context-owned memory (context, color, depth,
 *   arena, coverage). These checks apply in every mode, even OPAQUE, which
 *   otherwise ignores opacity and the alpha plane.
 */
PIXELICA_Status pixelica_draw_mesh_options(PIXELICA_Context *ctx,
                                 const PIXELICA_Mesh *mesh,
                                 const float model[16],
                                 const PIXELICA_Material *material,
                                 const PIXELICA_DrawOptions *options);

/*
 * pixelica_draw_mesh_options with a layered material (PIXELICA_MaterialLayers,
 * renderer.h; docs/material_layers.md). `material` == NULL is exactly
 * pixelica_draw_mesh_options(ctx, mesh, model, NULL, options); a material
 * without layers and vertex colors draws exactly like its `base`.
 * Rejected before any pixel, stat counter or arena byte is written
 * (PIXELICA_ERR_INVALID_ARG), in addition to the checks above on `base`:
 *   layer_count > PIXELICA_MAX_TEXTURE_LAYERS, layers == NULL with
 *   layer_count > 0; a layer with a NULL/invalid texture, NULL uvs (when
 *   vertex_count > 0), non-finite uvs, or wrap/combine outside their enums;
 *   the mesh/model, vertex/index arrays, material, its layer array, any texture struct/texels, uvs,
 *   triangle_colors or vertex_colors overlapping context-owned memory.
 */
PIXELICA_Status pixelica_draw_mesh_layers(PIXELICA_Context *ctx,
                                const PIXELICA_Mesh *mesh,
                                const float model[16],
                                const PIXELICA_MaterialLayers *material,
                                const PIXELICA_DrawOptions *options);

/*
 * pixelica_draw_mesh_layers with mesh effects (PIXELICA_MeshEffects in
 * renderer.h; docs/render_effects.md): bilinear filtering, a per-fragment
 * hook and up to PIXELICA_MAX_WORLD_CLIP_PLANES world clip planes.
 * `effects` == NULL is exactly pixelica_draw_mesh_layers(ctx, mesh, model,
 * material, options). With effects, the mesh/model/material arrays are
 * checked for context aliasing even when `material` is NULL, and these are
 * also rejected before any pixel, stat counter or arena byte is written
 * (PIXELICA_ERR_INVALID_ARG): `effects` or its plane array overlapping
 * context-owned memory; filter outside its enum; clip_plane_count >
 * PIXELICA_MAX_WORLD_CLIP_PLANES or > 0 with NULL clip_planes; a plane with
 * a non-finite or all-zero normal or a non-finite distance; a non-affine
 * model while planes are enabled. A vertex whose world position or plane
 * distance overflows float is rejected before any pixel is written.
 * While effects->pixel runs, same-context drawing/end-frame calls return
 * PIXELICA_ERR_INVALID_ARG and void mutators do nothing, without changing
 * state. The callback must not write borrowed buffers, draw through an
 * already-bound 2D view, or reuse live context storage in create_in_buffer.
 */
PIXELICA_Status pixelica_draw_mesh_effects(PIXELICA_Context *ctx,
                                 const PIXELICA_Mesh *mesh,
                                 const float model[16],
                                 const PIXELICA_MaterialLayers *material,
                                 const PIXELICA_DrawOptions *options,
                                 const PIXELICA_MeshEffects *effects);

/* Ends the frame and presents once via the PAL (if any).
 * PIXELICA_ERR_INVALID_ARG if no frame is active (present is not called);
 * PIXELICA_ERR_PLATFORM if present_rgb565 returned false. Either way the frame
 * is closed and the next pixelica_begin_frame works normally. */
PIXELICA_Status pixelica_end_frame(PIXELICA_Context *ctx);

/* NULL for a NULL context. */
const uint16_t *pixelica_color_buffer(const PIXELICA_Context *ctx);
const uint16_t *pixelica_depth_buffer(const PIXELICA_Context *ctx);

/* Borrowed offscreen/live buffer adapters (no allocation or copy). `out`
 * must not overlap context-owned memory. Invalid input leaves it unchanged.
 * Views/textures expire on destroy; their contents change on subsequent draws.
 * Sample another context or an independent snapshot to avoid read/write feedback.
 * See docs/framebuffer.md for snapshots, sampling and target setup. */
PIXELICA_Status pixelica_frame_view(const PIXELICA_Context *ctx,
                                  PIXELICA_FrameView *out);
PIXELICA_Status pixelica_frame_texture(const PIXELICA_Context *ctx,
                                     PIXELICA_Texture *out);

/* Stats of the current/most recent frame (zeroed for NULL ctx). */
PIXELICA_FrameStats pixelica_frame_stats(const PIXELICA_Context *ctx);

/*
 * Fixed-timestep driver.
 *  - Samples PAL read_buttons exactly once per call (0 when headless), even
 *    if no step runs, and passes that mask to every step of this call.
 *  - Accumulates time exactly in integer units of 1/60 microsecond, so
 *    there is no drift: N calls totalling T us run floor(T*60/1e6) steps
 *    as long as no time is dropped.
 *  - elapsed_us is clamped to PIXELICA_MAX_ELAPSED_US (no overflow for any
 *    uint64_t input). At most PIXELICA_MAX_UPDATES_PER_CALL steps run per call;
 *    whole steps beyond that are DROPPED (only the sub-step remainder is
 *    kept), preventing a spiral of death.
 *  - Each step calls update(userdata, 1.0f/60.0f, buttons).
 *  - NULL ctx or NULL update: nothing happens (no input read, no time
 *    accumulated).
 */
void pixelica_update(PIXELICA_Context *ctx, uint64_t elapsed_us,
                PIXELICA_UpdateFn update, void *userdata);

/* Debug aid: 1 if the guard words placed after the color buffer, depth
 * buffer, frame arena and coverage bitmap are intact, 0 if memory was
 * overrun (or NULL). */
int pixelica_memory_guards_intact(const PIXELICA_Context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_PIXELICA_H */

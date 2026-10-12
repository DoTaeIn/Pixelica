/*
 * Pixelica - 2.5D particle adapter: draws a PIXELICA_ParticleEmitter as
 * depth-tested billboards into a 3D frame.
 *
 * Part of the optional particles25d component (Pixelica::particles25d). It is
 * built on pixelica/hybrid.h, so it needs BOTH base SDKs: the 3D core
 * (Pixelica::core) and the 2D renderer (Pixelica::renderer2d), plus the hybrid
 * module and Pixelica::particles. A 2D-only or 3D-only SDK does not contain it.
 * The portable particle core never depends on this header.
 *
 * Frame protocol (the same one hybrid.h documents):
 *
 *   pixelica_begin_frame(ctx, clear);  pixelica_set_view_projection(ctx, vp);
 *   ... pixelica_draw_mesh(...) opaque 3D geometry first ...
 *   pixelica2d_bind_3d(&canvas, ctx);
 *   pixelica25d_particles_draw(&canvas, vp, &emitter, &options, scratch, n);
 *   ... HUD ...
 *   pixelica_end_frame(ctx);
 *
 * Coordinates and units
 *   - Particle positions are 3D world units (+Y up, camera looks down -Z) and
 *     are projected with the same column-major view_projection you gave the
 *     3D context. Size is the edge length in world units of a camera-facing
 *     square, scaled to pixels by pixelica25d_project at the particle's depth.
 *   - Rotation is in radians, in SCREEN space around the projected position,
 *     positive = clockwise on screen. The canvas camera is ignored (as for any
 *     billboard) and the canvas clip is honoured.
 *
 * Appearance
 *   - options->appearance == NULL (or options == NULL): every particle is a flat
 *     square from a 1x1 RGB565 texel with the particle's current color, centered
 *     on its projected position.
 *   - Otherwise the particle uses that sprite's texture, source rectangle, pivot
 *     (source texels; use the frame center for centered particles) and color
 *     key. As for pixelica25d_draw_billboard, only the SIGN of scale.x / scale.y
 *     is used (a negative sign mirrors), the source is stretched to the
 *     projected size x size, appearance.rotation is added to the particle
 *     rotation, and appearance.opacity multiplies the particle opacity. The
 *     appearance's position, depth, depth_test and depth_write are ignored.
 *   - The particle color is used ONLY for untextured squares. There is no tint.
 *
 * Depth and ordering
 *   - Billboards ALWAYS depth-test (hybrid.h forces it), so opaque meshes
 *     occlude particles behind them. The test is strict less-than against the
 *     16-bit depth buffer: a particle within one depth step of a surface can
 *     flicker; keep an explicit separation in view distance.
 *   - Depth is NOT written by default (depth_write == false), which is right
 *     for translucent particles. Set options->depth_write for opaque particles
 *     that must occlude later draws; it overrides appearance.depth_write.
 *   - Within one call particles are drawn FAR to NEAR by projected depth
 *     (quantized to the buffer's 16 bits; ties keep pool order), so translucent
 *     particles of one emitter blend correctly. Separate emitters, other
 *     billboards and sprites are NOT sorted against each other: draw them far
 *     to near yourself, one call per emitter.
 *   - Sorting needs caller-owned scratch of one item per live particle
 *     (emitter->count, at most PIXELICA_PARTICLES_MAX_CAPACITY items); the adapter
 *     never allocates. It costs O(n log n) comparisons.
 *
 * Material limits (what this adapter does NOT do)
 *   - Billboards only: no 3D orientation, no per-particle perspective skew, no
 *     lighting, no soft intersection with meshes (a hard depth edge), and no
 *     additive or multiply blend; only the renderer's normal alpha blend with
 *     8-bit opacity applied per RGB565 channel.
 *   - Nearest-texel sampling; a square that projects to less than one pixel can
 *     fall between pixel centers and draw nothing.
 *   - Cost is CPU fill rate: each particle rasterizes every covered pixel, so
 *     particles close to the camera are expensive and overdraw adds up.
 *     Particles behind the camera or outside the near/far planes are skipped.
 *
 * Errors
 *   - Everything is validated and projected before the first pixel is written.
 *     On a non-OK result the canvas color, depth and stats are unchanged.
 *   - PIXELICA_ERR_INVALID_ARG: NULL canvas / view_projection / emitter, a
 *     non-finite matrix, a canvas without a depth buffer, a bad emitter
 *     (capacity outside PIXELICA_PARTICLES_MIN/MAX_CAPACITY, count above capacity,
 *     NULL storage with a non-zero count), a live particle with a non-finite
 *     position or rotation or a size outside 0..PIXELICA_PARTICLES_MAX_VALUE, a
 *     particle whose projection is rejected by pixelica25d_project or whose
 *     projected size exceeds PIXELICA25D_PARTICLES_MAX_PROJECTED_SIZE pixels, an
 *     invalid appearance, or scratch that overlaps the emitter, pool, canvas,
 *     matrix, options, appearance or texture data. Inputs must be disjoint
 *     from canvas color/depth buffers and remain valid during the call.
 *   - PIXELICA_ERR_NO_MEMORY: scratch_count < emitter->count (or scratch NULL
 *     with live particles).
 *   - With no live particles the call returns PIXELICA_OK after validating the
 *     arguments above; scratch may then be NULL.
 *   - Particles with size 0 or a final opacity of 0 are skipped without a draw
 *     call; each drawn particle adds one draw to canvas->stats.
 */
#ifndef PIXELICA_PARTICLES25D_H
#define PIXELICA_PARTICLES25D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pixelica/hybrid.h"
#include "pixelica/particles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest accepted (world size * pixels per world unit) of one particle, pixels. */
#define PIXELICA25D_PARTICLES_MAX_PROJECTED_SIZE 1.0e9f

/* Per-call drawing options. A zero-initialized struct selects the defaults. */
typedef struct PIXELICA25D_ParticleOptions {
    const PIXELICA2D_Sprite *appearance; /* NULL = colored squares */
    bool depth_write;                    /* default false (translucent particles) */
} PIXELICA25D_ParticleOptions;

/* One caller-owned sorting slot; contents are overwritten by every call. */
typedef struct PIXELICA25D_ParticleSortItem {
    uint32_t index;
    uint16_t depth;
} PIXELICA25D_ParticleSortItem;

/*
 * Draws every live particle of `emitter` into `canvas` (bound with
 * pixelica2d_bind_3d, or any 2D context with a depth buffer), far to near.
 * `options` may be NULL (defaults). `scratch` must hold `scratch_count` items.
 */
PIXELICA_Status pixelica25d_particles_draw(PIXELICA2D_Context *canvas, const float view_projection[16],
    const PIXELICA_ParticleEmitter *emitter, const PIXELICA25D_ParticleOptions *options,
    PIXELICA25D_ParticleSortItem *scratch, size_t scratch_count);

#ifdef __cplusplus
}
#endif
#endif

/*
 * Pixelica - 2D particle adapter: draws a PIXELICA_ParticleEmitter with the
 * software 2D renderer.
 *
 * Part of the optional particles2d component (Pixelica::particles2d). It needs
 * Pixelica::particles (the portable core) and Pixelica::renderer2d, and exists
 * only in SDKs that contain both. It adds no state and no allocation: every
 * call rasterizes immediately into the caller's 2D surface through
 * pixelica2d_draw_sprite, and nothing passed in is retained.
 * Emitter, particle storage and appearance data must be disjoint from the
 * canvas struct and its color/depth buffers, and remain valid during the call.
 *
 * Coordinates and units
 *   - Particle X/Y are 2D world pixels (+Y down), mapped to the screen by the
 *     context camera exactly like a sprite position; Z is ignored. The camera
 *     zoom therefore scales particle sizes too. The clip rectangle is honoured.
 *   - Particle size is the edge length in world pixels of the square the
 *     particle is stretched to. Rotation is in radians, positive = clockwise
 *     on screen, around the particle position.
 *
 * Appearance
 *   - appearance == NULL: every particle is a flat square made from a 1x1
 *     RGB565 texel with the particle's current color, centered on its position.
 *   - appearance != NULL: the particle uses that sprite's texture, source
 *     rectangle, pivot (source texels; set it to the frame center for centered
 *     particles), color key, depth, depth_test and depth_write. Position,
 *     scale magnitude and opacity are replaced per particle: only the SIGN of
 *     scale.x / scale.y is used (a negative sign mirrors), the source is
 *     stretched to size x size, appearance.rotation is added to the particle
 *     rotation, and appearance.opacity multiplies the particle opacity.
 *   - The particle color is used ONLY for untextured squares. There is no tint
 *     API: a textured particle keeps its texels' colors.
 *
 * Material limits (what this adapter does NOT do)
 *   - Blending is the renderer's normal alpha blend only: no additive or
 *     multiply mode, and 8-bit opacity is applied per RGB565 channel, so soft
 *     fades show banding.
 *   - Sampling is nearest-texel, with no filtering. A square smaller than one
 *     pixel can fall between pixel centers and draw nothing.
 *   - Particles are drawn in pool order, which is NOT stable: expiry moves the
 *     last live particle into the freed slot. Overlapping translucent particles
 *     can therefore change stacking order from frame to frame. There is no
 *     sorting here (no scratch memory is taken); prefer opaque or sparse
 *     particles, or order emitters yourself. Use the 2.5D adapter for depth.
 *   - Cost is CPU fill rate: every covered pixel of every particle is
 *     rasterized, so large or many overlapping translucent particles (overdraw)
 *     dominate frame time. The pool size caps the particle count, not the cost.
 *
 * Errors
 *   - All inputs are validated before the first pixel is written. On
 *     PIXELICA_ERR_INVALID_ARG the surface, depth buffer and stats are unchanged:
 *     NULL ctx / emitter, an emitter whose capacity is outside
 *     PIXELICA_PARTICLES_MIN/MAX_CAPACITY or whose count exceeds it, NULL storage
 *     with a non-zero count, a live particle with a non-finite position or
 *     rotation or a size outside 0..PIXELICA_PARTICLES_MAX_VALUE, or an invalid
 *     appearance (texture, source, scale, pivot, rotation, depth value, or a
 *     depth flag on a surface without a depth buffer).
 *   - A malformed context (changed fields) is detected by the first draw; with
 *     nothing to draw it is not examined.
 *   - Particles with size 0 or a final opacity of 0 are skipped and cost no
 *     draw call; every other particle counts one draw call in ctx->stats.
 */
#ifndef PIXELICA_PARTICLES2D_H
#define PIXELICA_PARTICLES2D_H

#include "pixelica/particles.h"
#include "pixelica/renderer2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Draws every live particle of `emitter` into `ctx` in pool order (see above).
 * `appearance` is optional (NULL = colored squares) and is only read.
 */
PIXELICA_Status pixelica2d_particles_draw(PIXELICA2D_Context *ctx,
    const PIXELICA_ParticleEmitter *emitter, const PIXELICA2D_Sprite *appearance);

#ifdef __cplusplus
}
#endif
#endif

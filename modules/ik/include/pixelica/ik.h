/*
 * Pixelica - optional analytic two-bone IK (separate from the core library).
 *
 * Solves a root -> joint -> end chain with fixed bone lengths in 3D. It
 * returns joint/end POSITIONS only: it never touches a skeleton, keeps no
 * state, allocates nothing and reads no clock. The caller turns the positions
 * into bone rotations / local matrices (for a planar chain, atan2 of the
 * joint-root and end-joint directions is enough).
 *
 * root, target and pole must be expressed in the same coordinate space (world
 * or model); the result is in that space too. The pole is a POINT: it picks
 * the plane the chain bends in and the side the joint moves toward.
 *
 * Contract:
 *   - Every vector component must be finite, both lengths finite and > 0,
 *     both pointers non-NULL. Otherwise PIXELICA_ERR_INVALID_ARG.
 *   - The solve is analytic and O(1). Internally everything is double, so
 *     squares of float-range values and very short lengths neither overflow
 *     nor underflow.
 *   - d = |target - root| is clamped to [|L0 - L1|, L0 + L1]. `clamped` is
 *     true iff the input distance lies outside that closed range. A clamped
 *     solve is still PIXELICA_OK: there is no stretch and no angular limit. End is
 *     exactly `target` when reachable, else root + forward * clamped_d.
 *   - Bend direction: (pole - root) projected onto the plane perpendicular to
 *     `forward` (= unit target - root), normalized. If that projection is zero
 *     or smaller than 1e-12 of |pole - root| (pole on the chain axis, or
 *     pole == root), the fallback is the coordinate axis least parallel to
 *     `forward` (smallest |component|, ties X then Y then Z), projected and
 *     normalized. target == root has no direction and uses forward = +X.
 *   - Fully folded equal-length chain (d == 0): end == root and
 *     joint == root + bend * upper_length.
 *   - Otherwise (law of cosines, cancellation-stable form)
 *       a = 0.5 * (d + (L0 - L1) * (L0 + L1) / d)   clamped to [-L0, L0]
 *       h = sqrt(max(0, L0^2 - a^2))
 *       joint = root + forward * a + bend * h.
 *   - If joint or end does not fit in float, the result is
 *     PIXELICA_ERR_INVALID_ARG.
 *   - On any error `*out` is left unchanged. `out` may alias `chain`: all
 *     reads finish before the result is written.
 *   - Output rounds to float, so at huge coordinate / tiny length ratios the
 *     bone lengths hold only to float precision. Bitwise identical results on
 *     different platforms are not promised.
 */
#ifndef PIXELICA_IK_H
#define PIXELICA_IK_H

#include <stdbool.h>

#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PIXELICA_IKTwoBone {
    PIXELICA_Vec3 root;
    PIXELICA_Vec3 target;
    PIXELICA_Vec3 pole;
    float upper_length; /* root -> joint */
    float lower_length; /* joint -> end */
} PIXELICA_IKTwoBone;

typedef struct PIXELICA_IKTwoBoneResult {
    PIXELICA_Vec3 joint;
    PIXELICA_Vec3 end;
    bool clamped;
} PIXELICA_IKTwoBoneResult;

PIXELICA_Status pixelica_ik_two_bone(const PIXELICA_IKTwoBone *chain,
                           PIXELICA_IKTwoBoneResult *out);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_IK_H */

/*
 * Pixelica - OPTIONAL skeleton module: bone hierarchy posing and
 * positions-only linear blend skinning on the CPU.
 *
 * Built only with PIXELICA_BUILD_SKELETON=ON (target Pixelica::skeleton). The core
 * library and pixelica/pixelica.h never include this header.
 *
 * Nothing here allocates, reads a clock, keeps hidden/global state or needs a
 * PIXELICA_Context. Every array is caller-owned and only borrowed for the duration
 * of a call; no pointer is retained after return. Distinct calls with
 * disjoint outputs and workspaces share no mutable state.
 *
 * Matrices follow math.h: float[16], COLUMN-MAJOR, acting on COLUMN vectors
 * (element row r, column c is m[c * 4 + r]; translation in m[12..14]).
 * Every matrix passed to or produced by this module must be AFFINE: all 16
 * elements finite and the bottom row exactly (0, 0, 0, 1), i.e. m[3], m[7],
 * m[11] == 0 and m[15] == 1 (compared with ==, so -0.0 counts as 0). Output
 * matrices always carry a canonical +0/+1 bottom row.
 *
 * Coordinate spaces: a bone's WORLD matrix here means "bone -> model space";
 * the object's placement in the scene stays in the model matrix given to the
 * renderer. inverse_bind is the inverse of the bone's bind-pose WORLD matrix
 * (model -> bone space at bind time); it is not checked for invertibility or
 * for matching the bind pose. A palette matrix maps a bind-pose model-space
 * vertex to its posed model-space position.
 *
 * Errors: PIXELICA_ERR_INVALID_ARG for NULL required pointers, zero or oversized
 * counts, size computations that overflow, address ranges that wrap or
 * overlap in a forbidden way, non-finite / non-affine / out-of-range data, or
 * a result that does not fit a float. PIXELICA_ERR_NO_MEMORY only for a workspace
 * smaller than required. Checks run in this order: pointers and counts (and
 * extent size overflow) -> workspace size (NO_MEMORY) -> address ranges ->
 * data. On EVERY failure the logical outputs are byte-for-byte unchanged;
 * the workspace may have been modified (see each function).
 *
 * Results are computed with double intermediates and rounded to float once
 * per stored value; a chain of float matrices can differ from exact math by
 * roundoff and no ulp bound is promised.
 */
#ifndef PIXELICA_SKELETON_H
#define PIXELICA_SKELETON_H

#include <stddef.h>
#include <stdint.h>

#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bone slots per skinned vertex. */
#define PIXELICA_SKIN_INFLUENCES 4u
/* Largest accepted bone_count (every bone is addressable by a uint16_t). */
#define PIXELICA_SKELETON_MAX_BONES 65536u

/*
 * parent is -1 for a root, otherwise the index of an EARLIER bone:
 * 0 <= parent < this bone's index. This strict parent-before-child order
 * rules out cycles, self-parents and forward references; any number of
 * roots is allowed. inverse_bind is affine (see above).
 */
typedef struct PIXELICA_Bone {
    int32_t parent;
    float inverse_bind[16];
} PIXELICA_Bone;

/* bones points at bone_count records (1..PIXELICA_SKELETON_MAX_BONES). */
typedef struct PIXELICA_Skeleton {
    const PIXELICA_Bone *bones;
    uint32_t bone_count;
} PIXELICA_Skeleton;

/*
 * Influences of one vertex. Every weight must be finite and in [0, 1]
 * (all four slots are validated, zero slots included). A slot with weight
 * > 0 must name a bone < bone_count; the bone index of a zero-weight slot is
 * ignored. The weights must sum (in double) to a positive value within 1e-4
 * of 1; they are then normalized by that actual sum, so nearly normalized
 * data still keeps a point that every influence maps to the same place fixed.
 */
typedef struct PIXELICA_SkinWeight {
    uint16_t bones[PIXELICA_SKIN_INFLUENCES];
    float weights[PIXELICA_SKIN_INFLUENCES];
} PIXELICA_SkinWeight;

/*
 * Poses a skeleton. With N = skeleton->bone_count and bone i:
 *   world[i]   = local[i]                         (root, parent == -1)
 *   world[i]   = world[parent] * local[i]         (child)
 *   palette[i] = world[i] * bones[i].inverse_bind
 * where world[parent] is the float value stored for the parent.
 *
 * Extents (caller responsibility): skeleton->bones, local, world and palette
 * each hold N matrices/records; scratch holds scratch_matrices matrices and
 * must have scratch_matrices >= 2 * N (else PIXELICA_ERR_NO_MEMORY). Only the
 * first 2 * N scratch matrices are used: the first N receive world, the next
 * N palette; scratch contents are unspecified after any call.
 *
 * Requires: all pointers non-NULL; 1 <= N <= PIXELICA_SKELETON_MAX_BONES; every
 * parent valid (see PIXELICA_Bone); every local and inverse_bind matrix affine.
 * The whole input is validated before scratch is touched.
 *
 * Aliasing: world, palette and the used scratch range must not overlap each
 * other, local, the bones array or the *skeleton object (no in-place pose;
 * exact and partial overlaps are both rejected). Read-only inputs may alias
 * each other.
 *
 * All results are computed in scratch and checked against the float range
 * first; world and palette are written only after everything succeeded, and
 * are byte-for-byte unchanged on every failure.
 */
PIXELICA_Status pixelica_skeleton_pose(const PIXELICA_Skeleton *skeleton,
                            const float (*local)[16],
                            float (*world)[16], float (*palette)[16],
                            float (*scratch)[16], size_t scratch_matrices);

/*
 * Positions-only linear blend skinning (no normals; intended for the unlit
 * renderer path). For vertex v with normalized weights n_k = w_k / sum(w):
 *   out_positions[v] = sum over w_k > 0 of n_k * (palette[bone_k] * bind_v)
 * evaluated in double and range-checked before the single float rounding.
 *
 * Extents (caller responsibility): bind_positions, weights and out_positions
 * each hold vertex_count entries; palette holds bone_count matrices; scratch
 * holds scratch_vertices entries and must have scratch_vertices >=
 * vertex_count (else PIXELICA_ERR_NO_MEMORY). Only the first vertex_count scratch
 * entries are used; their contents are unspecified after any call.
 *
 * Requires: all pointers non-NULL; vertex_count >= 1; 1 <= bone_count <=
 * PIXELICA_SKELETON_MAX_BONES; every palette matrix affine (all bone_count of
 * them, even unreferenced ones); every bind position finite; every weight
 * record valid (see PIXELICA_SkinWeight).
 *
 * Aliasing: scratch must not overlap any other array. out_positions must not
 * overlap weights or palette; it may be EXACTLY bind_positions (same address,
 * in-place skinning), but any partial overlap with bind_positions is
 * rejected. Read-only inputs may alias each other.
 *
 * Results are computed into scratch; out_positions is written only after
 * every vertex succeeded and is byte-for-byte unchanged on every failure. The
 * vertex_count output positions are caller-owned and may be used directly
 * as PIXELICA_Mesh.vertices.
 */
PIXELICA_Status pixelica_skeleton_skin(const PIXELICA_Vec3 *bind_positions,
                            const PIXELICA_SkinWeight *weights, size_t vertex_count,
                            const float (*palette)[16], uint32_t bone_count,
                            PIXELICA_Vec3 *out_positions,
                            PIXELICA_Vec3 *scratch, size_t scratch_vertices);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_SKELETON_H */

/*
 * Pixelica - bounded scene list and axis-aligned box collision.
 *
 * No hierarchy and no ECS: a scene is a caller-owned fixed-capacity array of
 * nodes drawn in insertion order. Collision is a deterministic swept AABB
 * move against a caller array of static obstacle boxes. Nothing here
 * allocates.
 */
#ifndef PIXELICA_SCENE_H
#define PIXELICA_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/math.h"
#include "pixelica/renderer.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Also declared (under the same guard) by pixelica.h. */
#ifndef PIXELICA_CONTEXT_TYPEDEF_DECLARED
#define PIXELICA_CONTEXT_TYPEDEF_DECLARED
typedef struct PIXELICA_Context PIXELICA_Context;
#endif

/* Valid when every coordinate is finite and min <= max on each axis
 * (zero thickness allowed). */
typedef struct PIXELICA_AABB {
    PIXELICA_Vec3 min, max;
} PIXELICA_AABB;

/* 1 if `box` is valid (see above), else 0 (also for NULL). */
int pixelica_aabb_is_valid(const PIXELICA_AABB *box);

/*
 * True when the open interiors intersect on all three axes
 * (a.min < b.max && b.min < a.max per axis). Boxes that only touch along a
 * face, edge or corner do NOT overlap. NULL or NaN -> false.
 */
bool pixelica_aabb_overlaps(const PIXELICA_AABB *a, const PIXELICA_AABB *b);

/*
 * Moves `box` by up to `delta`, stopping at the first contact with any
 * obstacle. Sweeps the axes in the fixed order X, then Y, then Z; on each
 * axis the displacement is clamped so that the box ends exactly touching
 * (never penetrating) the nearest obstacle in the way, however large the
 * delta (no tunneling). Obstacles that merely touch the box sideways do not
 * block it, so the box slides along walls and floors.
 *
 * PIXELICA_ERR_INVALID_ARG (box and *out_applied unchanged/zeroed, nothing
 * moved): NULL box, NULL obstacles with count > 0, an invalid box or
 * obstacle, a non-finite delta, a box that initially overlaps an obstacle,
 * or a move whose result would overflow the float range.
 *
 * On success *out_applied (may be NULL) receives the displacement applied
 * per axis; each box coordinate is the float sum of its old value and that
 * displacement. The result never overlaps any obstacle.
 */
PIXELICA_Status pixelica_aabb_move(PIXELICA_AABB *box, PIXELICA_Vec3 delta,
                         const PIXELICA_AABB *obstacles, size_t count,
                         PIXELICA_Vec3 *out_applied);

/*
 * One scene object. `mesh` may be NULL for an invisible collider (its
 * material is then ignored). `bounds`
 * is a world-space box (used for collision when `solid`); it must be valid
 * even for non-solid nodes. `material` may be NULL (flat mesh color).
 */
typedef struct PIXELICA_SceneNode {
    const PIXELICA_Mesh *mesh;
    const PIXELICA_Material *material;
    float model[16];
    PIXELICA_AABB bounds;
    uint32_t id;
    bool solid;
} PIXELICA_SceneNode;

typedef struct PIXELICA_Scene {
    PIXELICA_SceneNode *nodes; /* caller-owned array of `capacity` nodes */
    size_t count, capacity;
} PIXELICA_Scene;

/* Empty scene over `nodes` (capacity 0 when nodes is NULL). NULL scene is a
 * no-op. */
void pixelica_scene_init(PIXELICA_Scene *scene, PIXELICA_SceneNode *nodes, size_t capacity);

/*
 * Copies `node` into the scene. PIXELICA_ERR_INVALID_ARG for NULL arguments, an
 * uninitialized scene, a non-finite model matrix, invalid bounds, a
 * structurally invalid mesh/material (same rules as pixelica_draw_mesh_ex,
 * including finite vertices) or an id already present;
 * PIXELICA_ERR_CAPACITY when the scene is full. Failure leaves the scene
 * unchanged. Mesh/material/texture memory must stay valid while the node is
 * in the scene.
 */
PIXELICA_Status pixelica_scene_add(PIXELICA_Scene *scene, const PIXELICA_SceneNode *node);

/* Node with `id`, or NULL. The pointer stays valid until the node array is
 * re-initialized. Callers may update model/bounds in place (keep them
 * finite/valid). */
PIXELICA_SceneNode *pixelica_scene_find(PIXELICA_Scene *scene, uint32_t id);

/* Copies the bounds of solid nodes (insertion order) into `out`, except the
 * node `skip` (e.g. the moving player; NULL skips nothing), and returns how
 * many were copied (at most out_capacity; 0 for NULL scene/out). */
size_t pixelica_scene_solid_bounds(const PIXELICA_Scene *scene,
                              const PIXELICA_SceneNode *skip, PIXELICA_AABB *out,
                              size_t out_capacity);

/*
 * Draws every node with a mesh, in insertion order, via pixelica_draw_mesh_ex
 * inside the current frame. Checked before anything is drawn:
 *   PIXELICA_ERR_INVALID_ARG  NULL/invalid scene or context, no active frame,
 *                        invalid camera;
 *   PIXELICA_ERR_CAPACITY     the drawable nodes exceed the frame's remaining
 *                        draw commands or a mesh exceeds
 *                        max_vertices_per_mesh.
 * Otherwise returns the first failing draw's status (earlier nodes remain
 * drawn); with nodes validated by pixelica_scene_add that can only happen if
 * their mesh data was modified afterwards or a transform overflows.
 */
PIXELICA_Status pixelica_scene_render(const PIXELICA_Scene *scene, PIXELICA_Context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_SCENE_H */

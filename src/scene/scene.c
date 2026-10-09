/*
 * Pixelica - bounded scene: a caller-owned node array drawn in insertion
 * order. Structural validation happens once in pixelica_scene_add; rendering
 * pre-checks the frame state and capacity so that the common failures
 * leave the frame untouched.
 */
#include <math.h>
#include <string.h>

#include "../core/context_internal.h"
#include "../renderer/draw_internal.h"

void pixelica_scene_init(PIXELICA_Scene *scene, PIXELICA_SceneNode *nodes, size_t capacity)
{
    if (scene == NULL) {
        return;
    }
    scene->nodes = nodes;
    scene->count = 0;
    scene->capacity = nodes != NULL ? capacity : 0u;
}

static int pixelica_scene_valid(const PIXELICA_Scene *scene)
{
    return scene != NULL && scene->count <= scene->capacity &&
           (scene->nodes != NULL || scene->capacity == 0);
}

static PIXELICA_Status pixelica_check_node(const PIXELICA_SceneNode *node)
{
    const PIXELICA_Mesh *mesh = node->mesh;
    PIXELICA_Status st;
    uint32_t i;

    if (!pixelica_mat4_is_finite(node->model) || !pixelica_aabb_is_valid(&node->bounds)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (mesh == NULL) {
        return PIXELICA_OK; /* invisible collider; material is ignored */
    }
    st = pixelica_check_mesh_structure(mesh, node->material);
    if (st == PIXELICA_OK) {
        st = pixelica_check_mesh_data(mesh, node->material);
    }
    if (st != PIXELICA_OK) {
        return st;
    }
    for (i = 0; i < mesh->vertex_count; ++i) {
        const PIXELICA_Vec3 v = mesh->vertices[i];
        if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_scene_add(PIXELICA_Scene *scene, const PIXELICA_SceneNode *node)
{
    size_t i;
    PIXELICA_Status st;

    if (!pixelica_scene_valid(scene) || node == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_check_node(node);
    if (st != PIXELICA_OK) {
        return st;
    }
    for (i = 0; i < scene->count; ++i) {
        if (scene->nodes[i].id == node->id) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    if (scene->count >= scene->capacity) {
        return PIXELICA_ERR_CAPACITY;
    }
    scene->nodes[scene->count] = *node;
    scene->count += 1u;
    return PIXELICA_OK;
}

PIXELICA_SceneNode *pixelica_scene_find(PIXELICA_Scene *scene, uint32_t id)
{
    size_t i;

    if (!pixelica_scene_valid(scene)) {
        return NULL;
    }
    for (i = 0; i < scene->count; ++i) {
        if (scene->nodes[i].id == id) {
            return &scene->nodes[i];
        }
    }
    return NULL;
}

size_t pixelica_scene_solid_bounds(const PIXELICA_Scene *scene,
                              const PIXELICA_SceneNode *skip, PIXELICA_AABB *out,
                              size_t out_capacity)
{
    size_t n = 0;
    size_t i;

    if (!pixelica_scene_valid(scene) || out == NULL) {
        return 0;
    }
    for (i = 0; i < scene->count && n < out_capacity; ++i) {
        const PIXELICA_SceneNode *node = &scene->nodes[i];
        if (node->solid && node != skip) {
            out[n++] = node->bounds;
        }
    }
    return n;
}

PIXELICA_Status pixelica_scene_render(const PIXELICA_Scene *scene, PIXELICA_Context *ctx)
{
    size_t drawable = 0;
    size_t i;

    if (!pixelica_context_valid(ctx)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!pixelica_scene_valid(scene) || !ctx->in_frame || !ctx->view_proj_valid) {
        ctx->stats.last_error = PIXELICA_ERR_INVALID_ARG;
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < scene->count; ++i) {
        const PIXELICA_Mesh *mesh = scene->nodes[i].mesh;
        if (mesh == NULL) {
            continue;
        }
        if (mesh->vertex_count > (uint32_t)ctx->cfg.max_vertices_per_mesh) {
            ctx->stats.last_error = PIXELICA_ERR_CAPACITY;
            return PIXELICA_ERR_CAPACITY;
        }
        ++drawable;
    }
    /* draw_count <= max_draw_commands always holds. */
    if (drawable >
        (size_t)((uint32_t)ctx->cfg.max_draw_commands - ctx->draw_count)) {
        ctx->stats.last_error = PIXELICA_ERR_CAPACITY;
        return PIXELICA_ERR_CAPACITY;
    }
    for (i = 0; i < scene->count; ++i) {
        const PIXELICA_SceneNode *node = &scene->nodes[i];
        PIXELICA_Status st;

        if (node->mesh == NULL) {
            continue;
        }
        st = pixelica_draw_mesh_ex(ctx, node->mesh, node->model, node->material);
        if (st != PIXELICA_OK) {
            return st;
        }
    }
    return PIXELICA_OK;
}

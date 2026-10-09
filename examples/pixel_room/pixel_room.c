/* Pixel Room: lifecycle, fixed-step simulation, rendering, snapshot/hash. */
#include "pixel_room_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Allocation                                                                */
/* ------------------------------------------------------------------------ */

static void *pr_default_alloc(void *user, size_t bytes)
{
    (void)user;
    return malloc(bytes);
}

static void pr_default_release(void *user, void *ptr)
{
    (void)user;
    free(ptr);
}

static size_t pr_padded(size_t n)
{
    return (n + 15u) & ~(size_t)15u;
}

/* Adds the 16-byte padded size of `add` to *total; false on overflow. */
static bool pr_add_padded(size_t *total, size_t add)
{
    size_t padded;
    if (add > SIZE_MAX - 15u) {
        return false;
    }
    padded = pr_padded(add);
    if (*total > SIZE_MAX - padded) {
        return false;
    }
    *total += padded;
    return true;
}

static bool pr_blob_ok(const PixelRoomBlob *blob)
{
    return blob->data != NULL && blob->size != 0u;
}

PIXELICA_Config pixel_room_context_config(uint16_t width, uint16_t height)
{
    PIXELICA_Config cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.max_draw_commands = (uint16_t)PIXEL_ROOM_MAX_DRAW_COMMANDS;
    cfg.max_vertices_per_mesh = (uint16_t)PIXEL_ROOM_MAX_MESH_VERTICES;
    cfg.frame_arena_bytes = PIXEL_ROOM_ARENA_BYTES;
    return cfg;
}

PIXELICA_Status pixel_room_create(PixelRoom **out_room, const PixelRoomParams *params)
{
    PixelRoomAllocator allocator;
    size_t mesh_need[PIXEL_ROOM_PROP_COUNT];
    size_t tex_need = 0u;
    size_t total = 0u;
    uint32_t i;
    PIXELICA_Status status;
    uint8_t *block;
    uint8_t *cursor;
    PixelRoom *room;

    if (out_room == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_room = NULL;
    if (params == NULL || params->width < 16u || params->height < 16u ||
        params->width > PIXELICA_MAX_DIMENSION || params->height > PIXELICA_MAX_DIMENSION) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    allocator.user = NULL;
    allocator.alloc = pr_default_alloc;
    allocator.release = pr_default_release;
    if (params->allocator != NULL) {
        if (params->allocator->alloc == NULL || params->allocator->release == NULL) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        allocator = *params->allocator;
    }

    /* Validate every asset and size the single allocation before allocating. */
    if (!pr_add_padded(&total, sizeof(PixelRoom))) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        const PixelRoomBlob *blob = &params->assets.props[i];
        if (!pr_blob_ok(blob)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        status = pixelica_mesh_asset_memory_required(blob->data, blob->size, &mesh_need[i]);
        if (status != PIXELICA_OK) {
            return status;
        }
        if (!pr_add_padded(&total, mesh_need[i])) {
            return PIXELICA_ERR_NO_MEMORY;
        }
    }
    if (!pr_blob_ok(&params->assets.checker)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    status = pixelica_texture_asset_memory_required(params->assets.checker.data,
                                               params->assets.checker.size, &tex_need);
    if (status != PIXELICA_OK) {
        return status;
    }
    if (!pr_add_padded(&total, tex_need)) {
        return PIXELICA_ERR_NO_MEMORY;
    }

    block = allocator.alloc(allocator.user, total);
    if (block == NULL) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    memset(block, 0, total);
    room = (PixelRoom *)(void *)block;
    room->allocator = allocator;
    room->block_bytes = total;
    room->width = params->width;
    room->height = params->height;

    cursor = block + pr_padded(sizeof(PixelRoom));
    status = PIXELICA_OK;
    for (i = 0; i < PIXEL_ROOM_PROP_COUNT && status == PIXELICA_OK; ++i) {
        const PixelRoomBlob *blob = &params->assets.props[i];
        status = pixelica_mesh_asset_load(&room->props[i].asset, blob->data, blob->size, cursor,
                                     mesh_need[i]);
        cursor += pr_padded(mesh_need[i]);
        if (status == PIXELICA_OK && room->props[i].asset.mesh.vertex_count > PIXEL_ROOM_MAX_MESH_VERTICES) {
            status = PIXELICA_ERR_CAPACITY;
        }
    }
    if (status == PIXELICA_OK) {
        status = pixelica_texture_asset_load(&room->checker, params->assets.checker.data,
                                        params->assets.checker.size, cursor, tex_need);
    }
    if (status == PIXELICA_OK) {
        status = pr_build_world(room);
    }
    if (status != PIXELICA_OK) {
        allocator.release(allocator.user, block);
        return status;
    }

    room->focus = pr_find_focus(room);
    pr_sync_scene(room);
    *out_room = room;
    return PIXELICA_OK;
}

void pixel_room_destroy(PixelRoom *room)
{
    PixelRoomAllocator allocator;
    if (room == NULL) {
        return;
    }
    allocator = room->allocator;
    allocator.release(allocator.user, room);
}

/* ------------------------------------------------------------------------ */
/* Simulation                                                                */
/* ------------------------------------------------------------------------ */

static float pr_wrap_angle(float angle)
{
    angle = fmodf(angle, PR_TWO_PI);
    if (angle < 0.0f) {
        angle += PR_TWO_PI;
    }
    if (angle >= PR_TWO_PI) {
        angle = 0.0f;
    }
    return angle;
}

static float pr_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int32_t pr_find_focus(const PixelRoom *room)
{
    const float px = 0.5f * (room->player.min.x + room->player.max.x);
    const float pz = 0.5f * (room->player.min.z + room->player.max.z);
    int32_t best = -1;
    int best_priority = -1;
    float best_distance = 0.0f;
    uint32_t i;

    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        const PRProp *prop = &room->props[i];
        float qx = pr_clampf(px, prop->bounds.min.x, prop->bounds.max.x);
        float qz = pr_clampf(pz, prop->bounds.min.z, prop->bounds.max.z);
        float dx = qx - px, dz = qz - pz;
        float d = sqrtf(dx * dx + dz * dz);
        int priority = (int)prop->priority;

        if (d > prop->reach) {
            continue;
        }
        if (d > 1e-4f) {
            float facing = (dx * room->face_x + dz * room->face_z) / d;
            if (facing < PIXEL_ROOM_FACING_COS) {
                continue;
            }
        }
        if (best < 0 || priority > best_priority ||
            (priority == best_priority && d < best_distance)) {
            best = (int32_t)i;
            best_priority = priority;
            best_distance = d;
        }
    }
    return best;
}

static void pr_move(PixelRoom *room, uint32_t buttons, float dt)
{
    float ix = 0.0f, iy = 0.0f, s, c, dx, dz, len;
    PIXELICA_AABB box;
    PIXELICA_Vec3 delta, applied;

    if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
        ix += 1.0f;
    }
    if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
        ix -= 1.0f;
    }
    if ((buttons & PIXELICA_BTN_UP) != 0u) {
        iy += 1.0f;
    }
    if ((buttons & PIXELICA_BTN_DOWN) != 0u) {
        iy -= 1.0f;
    }
    if (ix == 0.0f && iy == 0.0f) {
        return;
    }

    /* Camera-relative: right = (cos, -sin), forward (UP) = (-sin, -cos) in (x, z). */
    s = sinf(room->camera_yaw);
    c = cosf(room->camera_yaw);
    dx = c * ix - s * iy;
    dz = -s * ix - c * iy;
    len = sqrtf(dx * dx + dz * dz);
    if (!(len > 1e-6f)) {
        return;
    }
    dx /= len;
    dz /= len;
    room->face_x = dx;
    room->face_z = dz;

    delta.x = dx * PIXEL_ROOM_MOVE_SPEED * dt;
    delta.y = 0.0f;
    delta.z = dz * PIXEL_ROOM_MOVE_SPEED * dt;
    box = room->player;
    if (pixelica_aabb_move(&box, delta, room->obstacles, room->obstacle_count, &applied) == PIXELICA_OK) {
        /* Re-normalise the box size when that does not create a penetration, so
         * float rounding cannot make the cube grow or shrink over a long run. */
        PIXELICA_AABB clean = pr_player_box(0.5f * (box.min.x + box.max.x),
                                       0.5f * (box.min.z + box.max.z));
        room->player = pr_box_is_free(room, &clean) ? clean : box;
    }
}

static void pr_investigate(PixelRoom *room)
{
    if (room->focus >= 0) {
        uint32_t i = (uint32_t)room->focus;
        if (room->visits[i] < UINT32_MAX) {
            ++room->visits[i];
        }
        room->investigated_mask |= (uint32_t)1u << i;
        if (room->investigate_count < UINT32_MAX) {
            ++room->investigate_count;
        }
        room->event = PIXEL_ROOM_EVENT_INVESTIGATED;
        room->event_prop = room->focus;
    } else {
        if (room->miss_count < UINT32_MAX) {
            ++room->miss_count;
        }
        room->event = PIXEL_ROOM_EVENT_NOTHING;
        room->event_prop = -1;
    }
    room->event_ticks = PIXEL_ROOM_FLASH_TICKS;
}

void pixel_room_update(PixelRoom *room, float dt, uint32_t buttons)
{
    uint32_t pressed;

    if (room == NULL || !isfinite(dt) || !(dt > 0.0f)) {
        return;
    }
    if (dt > PIXEL_ROOM_MAX_DT) {
        dt = PIXEL_ROOM_MAX_DT;
    }
    buttons &= PIXEL_ROOM_INPUT_MASK;
    pressed = buttons & ~room->held;
    room->held = buttons;
    if (room->tick < UINT32_MAX) {
        ++room->tick;
    }
    if (room->event_ticks > 0u) {
        --room->event_ticks;
        if (room->event_ticks == 0u) {
            room->event = PIXEL_ROOM_EVENT_NONE;
            room->event_prop = -1;
        }
    }

    if ((buttons & PIXELICA_BTN_START) != 0u) {
        float turn = 0.0f;
        if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
            turn -= PIXEL_ROOM_CAMERA_RATE * dt;
        }
        if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
            turn += PIXEL_ROOM_CAMERA_RATE * dt;
        }
        if (turn != 0.0f) {
            room->camera_yaw = pr_wrap_angle(room->camera_yaw + turn);
        }
    } else {
        pr_move(room, buttons, dt);
    }

    room->focus = pr_find_focus(room);
    if ((pressed & PIXELICA_BTN_A) != 0u && (buttons & PIXELICA_BTN_START) == 0u) {
        pr_investigate(room);
    }
    pr_sync_scene(room);
}

void pixel_room_update_cb(void *room, float dt, uint32_t buttons)
{
    pixel_room_update((PixelRoom *)room, dt, buttons);
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

PIXELICA_Status pixel_room_render_with_cull(const PixelRoom *room, PIXELICA_Context *ctx,
                                       PIXELICA_CullMode cull)
{
    float view[16], view_projection[16], identity[16];
    PIXELICA_Status status;

    if (room == NULL || ctx == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    pr_camera_view(view, room->camera_yaw);
    pixelica_mat4_multiply(view_projection, room->projection, view);

    pixelica_begin_frame(ctx, PIXELICA_RGB565(22, 26, 40));
    pixelica_set_cull_mode(ctx, cull);
    pixelica_set_view_projection(ctx, view_projection);
    status = pixelica_scene_render(&room->scene, ctx);
    if (status != PIXELICA_OK) {
        return status;
    }

    /* HUD: vertices are already in NDC, so the identity matrix is the camera. */
    pixelica_mat4_identity(identity);
    pixelica_set_view_projection(ctx, identity);
    status = pixelica_draw_mesh_ex(ctx, &room->hud.mesh, identity, &room->hud.material);
    if (status != PIXELICA_OK) {
        return status;
    }
    return pixelica_end_frame(ctx);
}

PIXELICA_Status pixel_room_render(const PixelRoom *room, PIXELICA_Context *ctx)
{
    return pixel_room_render_with_cull(room, ctx, PIXELICA_CULL_BACK);
}

/* ------------------------------------------------------------------------ */
/* Snapshot, hash, helpers                                                   */
/* ------------------------------------------------------------------------ */

void pixel_room_snapshot(const PixelRoom *room, PixelRoomSnapshot *out)
{
    uint32_t i;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->focus = -1;
    out->event_prop = -1;
    if (room == NULL) {
        return;
    }
    out->tick = room->tick;
    out->player_x = 0.5f * (room->player.min.x + room->player.max.x);
    out->player_z = 0.5f * (room->player.min.z + room->player.max.z);
    out->facing_x = room->face_x;
    out->facing_z = room->face_z;
    out->camera_yaw = room->camera_yaw;
    out->focus = room->focus;
    out->investigated_mask = room->investigated_mask;
    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        out->visits[i] = room->visits[i];
    }
    out->investigate_count = room->investigate_count;
    out->miss_count = room->miss_count;
    out->last_event = room->event;
    out->event_prop = room->event_prop;
    out->event_ticks_left = room->event_ticks;
    out->held_buttons = room->held;
}

static uint64_t pr_hash_u32(uint64_t h, uint32_t value)
{
    int i;
    for (i = 0; i < 4; ++i) {
        h ^= (uint64_t)((value >> (8 * i)) & 0xFFu);
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t pr_hash_f32(uint64_t h, float value)
{
    uint32_t bits;
    if (value == 0.0f) {
        value = 0.0f; /* -0 hashes like +0 */
    }
    memcpy(&bits, &value, sizeof(bits));
    return pr_hash_u32(h, bits);
}

uint64_t pixel_room_snapshot_hash(const PixelRoomSnapshot *s)
{
    uint64_t h = 14695981039346656037ull;
    uint32_t i;
    if (s == NULL) {
        return h;
    }
    h = pr_hash_u32(h, s->tick);
    h = pr_hash_f32(h, s->player_x);
    h = pr_hash_f32(h, s->player_z);
    h = pr_hash_f32(h, s->facing_x);
    h = pr_hash_f32(h, s->facing_z);
    h = pr_hash_f32(h, s->camera_yaw);
    h = pr_hash_u32(h, (uint32_t)s->focus);
    h = pr_hash_u32(h, s->investigated_mask);
    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        h = pr_hash_u32(h, s->visits[i]);
    }
    h = pr_hash_u32(h, s->investigate_count);
    h = pr_hash_u32(h, s->miss_count);
    h = pr_hash_u32(h, (uint32_t)s->last_event);
    h = pr_hash_u32(h, (uint32_t)s->event_prop);
    h = pr_hash_u32(h, s->event_ticks_left);
    h = pr_hash_u32(h, s->held_buttons);
    return h;
}

uint64_t pixel_room_state_hash(const PixelRoom *room)
{
    PixelRoomSnapshot snapshot;
    pixel_room_snapshot(room, &snapshot);
    return pixel_room_snapshot_hash(&snapshot);
}

PIXELICA_Status pixel_room_teleport_player(PixelRoom *room, float x, float z)
{
    PIXELICA_AABB box;
    if (room == NULL || !isfinite(x) || !isfinite(z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    box = pr_player_box(x, z);
    if (!pr_box_in_room(&box) || !pr_box_is_free(room, &box)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    room->player = box;
    room->focus = pr_find_focus(room);
    pr_sync_scene(room);
    return PIXELICA_OK;
}

PIXELICA_Status pixel_room_set_camera_yaw(PixelRoom *room, float radians)
{
    if (room == NULL || !isfinite(radians)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    room->camera_yaw = pr_wrap_angle(radians);
    return PIXELICA_OK;
}

bool pixel_room_prop_bounds(const PixelRoom *room, uint32_t prop, PIXELICA_AABB *out)
{
    if (room == NULL || out == NULL || prop >= PIXEL_ROOM_PROP_COUNT) {
        return false;
    }
    *out = room->props[prop].bounds;
    return true;
}

bool pixel_room_prop_is_solid(const PixelRoom *room, uint32_t prop)
{
    return room != NULL && prop < PIXEL_ROOM_PROP_COUNT && room->props[prop].solid;
}

PIXELICA_AABB pixel_room_player_bounds(const PixelRoom *room)
{
    PIXELICA_AABB zero;
    memset(&zero, 0, sizeof(zero));
    return room != NULL ? room->player : zero;
}

size_t pixel_room_obstacle_count(const PixelRoom *room)
{
    return room != NULL ? room->obstacle_count : 0u;
}

bool pixel_room_obstacle(const PixelRoom *room, size_t index, PIXELICA_AABB *out)
{
    if (room == NULL || out == NULL || index >= room->obstacle_count) {
        return false;
    }
    *out = room->obstacles[index];
    return true;
}

const PIXELICA_Scene *pixel_room_scene(const PixelRoom *room)
{
    return room != NULL ? &room->scene : NULL;
}

size_t pixel_room_allocation_bytes(const PixelRoom *room)
{
    return room != NULL ? room->block_bytes : 0u;
}

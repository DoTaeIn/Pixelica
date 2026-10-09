/*
 * Pixel Room: portable game logic (no SDL, no ESP-IDF, no platform headers).
 *
 * One PixelRoom instance owns everything: decoded assets, generated geometry,
 * the scene, the collision world and the simulation state. There is no global
 * mutable state, so any number of independent rooms can coexist (the tests
 * create several and interleave them).
 *
 * Memory: pixel_room_create() performs exactly ONE allocation through the
 * allocator (the default is malloc/free) and pixel_room_destroy() releases it.
 * update, render, snapshot and hash never allocate.
 *
 * Simulation (fixed 60 Hz step, driven by pixelica_update()):
 *   arrows      move the cube, relative to the camera (UP = away from camera)
 *   START+LEFT  orbit the camera left, START+RIGHT orbit it right
 *               (arrows/A are ignored while START is held)
 *   A           investigate: acts on the focused prop on the press edge only
 *               (holding A does nothing more); a press with no prop in reach
 *               is counted as a miss
 *
 * Focus: the prop within its reach (distance from the cube centre to the
 * prop's footprint; PIXEL_ROOM_REACH, or PIXEL_ROOM_SMALL_REACH for the book
 * and cup) whose direction is within the facing cone. Props with a higher
 * priority (book, cup: they sit on the desk) win, then the nearest, then the
 * lowest index. So walking up to the desk first focuses the desk and stepping
 * closer focuses the small items. Visual feedback: a floating marker above the prop
 * (yellow = in reach, green = investigated, cyan = both, white flash right
 * after investigating) and a HUD strip (one pip per prop + a result flash).
 *
 * Determinism: the same sequence of (dt, buttons) calls on the same build
 * produces the same state hash. The hash is not claimed to be bit-identical
 * across different libm implementations (sinf/cosf are used for the camera).
 */
#ifndef PIXEL_ROOM_H
#define PIXEL_ROOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/pixelica.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Supported target profiles (any size >= 16 works; these are the two targets). */
#define PIXEL_ROOM_TINY_WIDTH 160u
#define PIXEL_ROOM_TINY_HEIGHT 120u
#define PIXEL_ROOM_STANDARD_WIDTH 256u
#define PIXEL_ROOM_STANDARD_HEIGHT 192u

/* Renderer limits the room needs (use pixel_room_context_config()). */
#define PIXEL_ROOM_MAX_DRAW_COMMANDS 64u
#define PIXEL_ROOM_MAX_MESH_VERTICES 4096u
#define PIXEL_ROOM_ARENA_BYTES (PIXEL_ROOM_MAX_MESH_VERTICES * PIXELICA_ARENA_BYTES_PER_VERTEX)

/* World (units: 1.0 is roughly one metre). Interior is [-4,4] x [-4,4]. */
#define PIXEL_ROOM_HALF_EXTENT 4.0f
#define PIXEL_ROOM_PLAYER_HALF 0.35f
#define PIXEL_ROOM_PLAYER_HEIGHT 0.7f
#define PIXEL_ROOM_MOVE_SPEED 3.0f      /* units per second */
#define PIXEL_ROOM_CAMERA_RATE 1.6f     /* radians per second */
#define PIXEL_ROOM_START_CAMERA_YAW 0.78539816f
#define PIXEL_ROOM_REACH 1.1f           /* investigation distance, inclusive */
#define PIXEL_ROOM_SMALL_REACH 0.8f     /* reach of the book and cup (sit on the desk) */
#define PIXEL_ROOM_FACING_COS 0.34f     /* cos of the half-angle of the facing cone */
#define PIXEL_ROOM_FLASH_TICKS 30u
#define PIXEL_ROOM_MAX_DT 1.0f          /* longer steps are clamped to this */
#define PIXEL_ROOM_INPUT_MASK                                                   \
    (PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT | PIXELICA_BTN_UP | PIXELICA_BTN_DOWN | PIXELICA_BTN_A | \
     PIXELICA_BTN_START)

typedef enum PixelRoomProp {
    PIXEL_ROOM_PROP_DESK = 0,
    PIXEL_ROOM_PROP_CHAIR,
    PIXEL_ROOM_PROP_BOOK,
    PIXEL_ROOM_PROP_CUP,
    PIXEL_ROOM_PROP_PLANT,
    PIXEL_ROOM_PROP_COUNT
} PixelRoomProp;

typedef enum PixelRoomEvent {
    PIXEL_ROOM_EVENT_NONE = 0,
    PIXEL_ROOM_EVENT_INVESTIGATED, /* A pressed with a prop in focus */
    PIXEL_ROOM_EVENT_NOTHING       /* A pressed with nothing in reach */
} PixelRoomEvent;

/* Raw asset file bytes (.p3mesh / .p3tex). Only read during pixel_room_create. */
typedef struct PixelRoomBlob {
    const void *data;
    size_t size;
} PixelRoomBlob;

typedef struct PixelRoomAssets {
    PixelRoomBlob props[PIXEL_ROOM_PROP_COUNT]; /* indexed by PixelRoomProp */
    PixelRoomBlob checker;                      /* floor texture */
} PixelRoomAssets;

/* alloc must return memory aligned for any object (like malloc) or NULL. */
typedef struct PixelRoomAllocator {
    void *user;
    void *(*alloc)(void *user, size_t bytes);
    void (*release)(void *user, void *ptr);
} PixelRoomAllocator;

typedef struct PixelRoomParams {
    uint16_t width, height;                /* target resolution, 16..PIXELICA_MAX_DIMENSION */
    PixelRoomAssets assets;
    const PixelRoomAllocator *allocator;   /* NULL = malloc/free */
} PixelRoomParams;

typedef struct PixelRoom PixelRoom;

/* Everything that defines the simulation, in a plain copyable struct. */
typedef struct PixelRoomSnapshot {
    uint32_t tick;                          /* accepted update steps (saturating) */
    float player_x, player_z;               /* centre of the cube */
    float facing_x, facing_z;               /* unit vector the cube looks along */
    float camera_yaw;                       /* radians in [0, 2*pi) */
    int32_t focus;                          /* prop index in reach, or -1 */
    uint32_t investigated_mask;             /* bit i set = prop i investigated */
    uint32_t visits[PIXEL_ROOM_PROP_COUNT]; /* investigations per prop */
    uint32_t investigate_count;             /* successful A presses */
    uint32_t miss_count;                    /* A presses with nothing in reach */
    PixelRoomEvent last_event;              /* NONE once the flash has expired */
    int32_t event_prop;                     /* prop of last_event or -1 */
    uint32_t event_ticks_left;
    uint32_t held_buttons;                  /* masked buttons of the last step */
} PixelRoomSnapshot;

/* Renderer configuration the room is designed for (same for both targets). */
PIXELICA_Config pixel_room_context_config(uint16_t width, uint16_t height);

/*
 * Creates a room. On failure *out_room is NULL and nothing is leaked; the
 * status is PIXELICA_ERR_INVALID_ARG (bad parameters/blob), PIXELICA_ERR_BAD_ASSET (a
 * loader rejected an asset), PIXELICA_ERR_CAPACITY (an asset has more vertices than
 * PIXEL_ROOM_MAX_MESH_VERTICES) or PIXELICA_ERR_NO_MEMORY. Props are scaled to a
 * nominal size and placed using their measured bounds, so the layout does not
 * depend on voxel counts.
 */
PIXELICA_Status pixel_room_create(PixelRoom **out_room, const PixelRoomParams *params);

/* NULL is a no-op. Frees the single allocation. */
void pixel_room_destroy(PixelRoom *room);

/* One fixed step. Ignored for NULL room or a non-finite/non-positive dt. */
void pixel_room_update(PixelRoom *room, float dt, uint32_t buttons);

/* Same as pixel_room_update with the PIXELICA_UpdateFn signature. */
void pixel_room_update_cb(void *room, float dt, uint32_t buttons);

/*
 * Draws the whole scene (begin_frame ... end_frame, which presents). Does not
 * modify the room. `ctx` should come from pixel_room_context_config().
 */
PIXELICA_Status pixel_room_render(const PixelRoom *room, PIXELICA_Context *ctx);

/* Same, forcing a cull mode (used by tests to audit triangle winding). */
PIXELICA_Status pixel_room_render_with_cull(const PixelRoom *room, PIXELICA_Context *ctx,
                                       PIXELICA_CullMode cull);

/* Zeroed snapshot (focus/event_prop = -1) for a NULL room. */
void pixel_room_snapshot(const PixelRoom *room, PixelRoomSnapshot *out);

/* FNV-1a 64 over every snapshot field (floats by bit pattern, -0 == +0). */
uint64_t pixel_room_snapshot_hash(const PixelRoomSnapshot *snapshot);
uint64_t pixel_room_state_hash(const PixelRoom *room);

/*
 * Photo/test helpers. Both validate first and leave the room untouched on
 * failure (PIXELICA_ERR_INVALID_ARG).
 *   teleport: the cube centre must be finite, the cube fully inside the room
 *             and not penetrating any obstacle.
 *   camera:   radians must be finite; the stored yaw is wrapped to [0, 2*pi).
 */
PIXELICA_Status pixel_room_teleport_player(PixelRoom *room, float x, float z);
PIXELICA_Status pixel_room_set_camera_yaw(PixelRoom *room, float radians);

/* Read-only views of the world, for tests and tools. */
const char *pixel_room_prop_name(uint32_t prop);
bool pixel_room_prop_bounds(const PixelRoom *room, uint32_t prop, PIXELICA_AABB *out);
bool pixel_room_prop_is_solid(const PixelRoom *room, uint32_t prop);
PIXELICA_AABB pixel_room_player_bounds(const PixelRoom *room);
size_t pixel_room_obstacle_count(const PixelRoom *room);
bool pixel_room_obstacle(const PixelRoom *room, size_t index, PIXELICA_AABB *out);
const PIXELICA_Scene *pixel_room_scene(const PixelRoom *room);
size_t pixel_room_allocation_bytes(const PixelRoom *room);

#ifdef __cplusplus
}
#endif

#endif /* PIXEL_ROOM_H */

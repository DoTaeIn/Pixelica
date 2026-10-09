/* Private layout of PixelRoom, shared by pixel_room.c and pixel_room_world.c. */
#ifndef PIXEL_ROOM_INTERNAL_H
#define PIXEL_ROOM_INTERNAL_H

#include "pixel_room.h"

#define PR_PI 3.14159265358979323846f
#define PR_TWO_PI 6.28318530717958647692f

/* Fixed pools for generated geometry (all meshes share them). */
#define PR_VERT_POOL 1536u
#define PR_IDX_POOL 3072u
#define PR_TRI_POOL (PR_IDX_POOL / 3u)
#define PR_UV_POOL 128u
#define PR_NODE_CAP 24u
#define PR_WALL_COUNT 4u
#define PR_OBSTACLE_CAP 16u
#define PR_MARKER_VARIANTS 4u
#define PR_HUD_QUADS 7u /* strip, one pip per prop, result flash */

/* Scene node ids. */
#define PR_ID_FLOOR 1u
#define PR_ID_SHELL 2u
#define PR_ID_DECOR 3u
#define PR_ID_DOOR 4u
#define PR_ID_WINDOW 5u
#define PR_ID_PROP_BASE 10u
#define PR_ID_PLAYER 20u
#define PR_ID_MARKER_BASE 30u

typedef struct PRMesh {
    PIXELICA_Mesh mesh;
    PIXELICA_Material material;
} PRMesh;

typedef struct PRProp {
    PIXELICA_MeshAsset asset;
    PIXELICA_AABB bounds;     /* world space */
    uint32_t node;       /* index into scene.nodes */
    uint32_t marker_node;
    float reach;
    uint8_t priority;
    bool solid;
} PRProp;

struct PixelRoom {
    PixelRoomAllocator allocator;
    size_t block_bytes;
    uint16_t width;
    uint16_t height;

    /* Simulation state: exactly what the snapshot reports. */
    uint32_t tick;
    PIXELICA_AABB player;
    float face_x, face_z;
    float camera_yaw;
    uint32_t held;
    int32_t focus;
    uint32_t investigated_mask;
    uint32_t visits[PIXEL_ROOM_PROP_COUNT];
    uint32_t investigate_count;
    uint32_t miss_count;
    PixelRoomEvent event;
    int32_t event_prop;
    uint32_t event_ticks;

    /* Decoded assets (storage lives in the same allocation). */
    PIXELICA_Texture checker;
    PRProp props[PIXEL_ROOM_PROP_COUNT];

    /* Generated geometry. */
    PRMesh floor, shell, decor, door, window, cube, hud;
    PRMesh marker[PR_MARKER_VARIANTS];
    uint16_t *hud_colors; /* PR_HUD_QUADS * 2 triangle colours inside tpool */
    uint32_t v_used, i_used, uv_used;
    PIXELICA_Vec3 vpool[PR_VERT_POOL];
    uint16_t ipool[PR_IDX_POOL];
    uint16_t tpool[PR_TRI_POOL];
    PIXELICA_Vec2 uvpool[PR_UV_POOL];

    /* World. */
    float projection[16];
    PIXELICA_AABB walls[PR_WALL_COUNT];
    PIXELICA_AABB obstacles[PR_OBSTACLE_CAP];
    size_t obstacle_count;
    PIXELICA_Scene scene;
    PIXELICA_SceneNode nodes[PR_NODE_CAP];
    uint32_t player_node;
};

/*
 * Builds geometry, places the (already loaded) props, fills the scene and
 * collision world and initialises the simulation state. Called once by create.
 */
PIXELICA_Status pr_build_world(PixelRoom *room);

/* Refreshes dynamic scene nodes and HUD colours from the simulation state. */
void pr_sync_scene(PixelRoom *room);

/* Builds the camera view matrix for an orbit yaw. */
void pr_camera_view(float out[16], float yaw);

/* True if the cube box penetrates no obstacle (touching is fine). */
bool pr_box_is_free(const PixelRoom *room, const PIXELICA_AABB *box);

/* True if the cube box lies fully inside the room interior. */
bool pr_box_in_room(const PIXELICA_AABB *box);

/* Cube box with its centre at (x, z). */
PIXELICA_AABB pr_player_box(float x, float z);

/* Prop that the next A press would investigate, or -1 (defined in pixel_room.c). */
int32_t pr_find_focus(const PixelRoom *room);

#endif /* PIXEL_ROOM_INTERNAL_H */

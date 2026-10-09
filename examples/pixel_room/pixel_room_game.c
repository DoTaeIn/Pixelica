/*
 * Pixel Room as a PIXELICA_Game for the shared drivers (SDL3 or headless).
 *
 * The same source is built twice with different compile definitions:
 *   pixel_room       PIXEL_ROOM_WIDTH=256 PIXEL_ROOM_HEIGHT=192 (STANDARD)
 *   pixel_room_tiny  PIXEL_ROOM_WIDTH=160 PIXEL_ROOM_HEIGHT=120 (TINY)
 * All game logic lives in pixel_room.c and is identical for both.
 *
 * Keys: arrows/WASD move, START (Enter) + left/right orbit the camera,
 * Space = A investigates, Escape quits.
 */
#include "../common/pixelica_game.h"
#include "pixel_room.h"
#include "room_embedded.h"

#ifndef PIXEL_ROOM_WIDTH
#define PIXEL_ROOM_WIDTH PIXEL_ROOM_STANDARD_WIDTH
#define PIXEL_ROOM_HEIGHT PIXEL_ROOM_STANDARD_HEIGHT
#endif

#if PIXEL_ROOM_WIDTH == PIXEL_ROOM_TINY_WIDTH && PIXEL_ROOM_HEIGHT == PIXEL_ROOM_TINY_HEIGHT
#define PIXEL_ROOM_TITLE "Pixelica Pixel Room (TINY 160x120)"
#elif PIXEL_ROOM_WIDTH == PIXEL_ROOM_STANDARD_WIDTH && PIXEL_ROOM_HEIGHT == PIXEL_ROOM_STANDARD_HEIGHT
#define PIXEL_ROOM_TITLE "Pixelica Pixel Room (STANDARD 256x192)"
#else
#define PIXEL_ROOM_TITLE "Pixelica Pixel Room"
#endif

static bool room_game_init(void **out_state, PIXELICA_Context *ctx)
{
    PixelRoomParams params;
    PixelRoom *room = NULL;
    PIXELICA_Status status;

    (void)ctx;
    memset(&params, 0, sizeof(params));
    params.width = (uint16_t)PIXEL_ROOM_WIDTH;
    params.height = (uint16_t)PIXEL_ROOM_HEIGHT;
    if (!pixel_room_embedded_assets(&params.assets)) {
        fprintf(stderr, "pixel_room: embedded assets are unavailable\n");
        return false;
    }
    status = pixel_room_create(&room, &params);
    if (status != PIXELICA_OK) {
        fprintf(stderr, "pixel_room: create failed (PIXELICA_Status %d)\n", (int)status);
        return false;
    }
    *out_state = room;
    return true;
}

static void room_game_update(void *state, float dt, uint32_t buttons)
{
    pixel_room_update((PixelRoom *)state, dt, buttons);
}

static PIXELICA_Status room_game_render(void *state, PIXELICA_Context *ctx)
{
    return pixel_room_render((const PixelRoom *)state, ctx);
}

static void room_game_shutdown(void *state)
{
    pixel_room_destroy((PixelRoom *)state);
}

const PIXELICA_Game pixelica_example_game = {
    PIXEL_ROOM_TITLE,
    {PIXEL_ROOM_WIDTH, PIXEL_ROOM_HEIGHT, PIXEL_ROOM_MAX_DRAW_COMMANDS,
     PIXEL_ROOM_MAX_MESH_VERTICES, PIXEL_ROOM_ARENA_BYTES},
    room_game_init,
    room_game_update,
    room_game_render,
    room_game_shutdown,
};

/*
 * Game logic of the Pixelica starter project. Portable C11: it uses only the
 * public Pixelica API, contains no platform code and no #ifdef per platform,
 * and allocates nothing after init. The same file is meant to be compiled for
 * a desktop driver (src/main_headless.c, src/main_sdl3.c) or, later, a
 * microcontroller driver.
 */
#ifndef PIXELICA_TEMPLATE_GAME_H
#define PIXELICA_TEMPLATE_GAME_H

#include <stdbool.h>
#include <stdint.h>

#include "pixelica/pixelica.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEMPLATE_RENDER_WIDTH 256u
#define TEMPLATE_RENDER_HEIGHT 192u

#define TEMPLATE_CHECKER_SIZE 8u

/*
 * All game state, including the floor texture's texels. The floor material
 * points into this struct, so initialize it in place with template_game_init()
 * and do not copy or move it afterwards.
 */
typedef struct TemplateGame {
    float yaw;
    float pitch;
    bool paused;
    uint32_t previous_buttons;
    float view_projection[16];
    uint16_t checker_texels[TEMPLATE_CHECKER_SIZE * TEMPLATE_CHECKER_SIZE];
    PIXELICA_Texture floor_texture;
    PIXELICA_Material floor_material;
} TemplateGame;

/* Engine configuration matching TEMPLATE_RENDER_WIDTH/HEIGHT and this game's meshes. */
PIXELICA_Config template_game_config(void);

/* false (and *game zeroed) if the projection cannot be built. */
bool template_game_init(TemplateGame *game);

/* PIXELICA_UpdateFn-compatible: pass to pixelica_update() with `game` as userdata. */
void template_game_update(void *game, float dt, uint32_t buttons);

/* pixelica_begin_frame ... pixelica_end_frame; end_frame presents through the platform. */
PIXELICA_Status template_game_render(const TemplateGame *game, PIXELICA_Context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_TEMPLATE_GAME_H */

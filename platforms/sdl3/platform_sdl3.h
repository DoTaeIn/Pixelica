/*
 * Private header of the SDL3 platform backend (not part of the public pixelica API).
 *
 * SDL3 is used only to show finished RGB565 pixels, to read the keyboard and
 * to read the clock. All 3D work happens in pixelica_core on the CPU.
 */
#ifndef PIXELICA_PLATFORM_SDL3_H
#define PIXELICA_PLATFORM_SDL3_H

#include <stdbool.h>
#include <stdint.h>

#include "pixelica/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PIXELICA_Sdl3Platform PIXELICA_Sdl3Platform;

typedef struct PIXELICA_Sdl3Config {
    const char *title;   /* NULL selects "Pixelica" */
    uint16_t width;      /* internal render width, must match the pixelica context */
    uint16_t height;     /* internal render height */
    bool hidden;         /* SDL dummy video driver + software renderer, no window shown */
} PIXELICA_Sdl3Config;

/*
 * Initializes SDL video, creates window/renderer/texture. Returns false and
 * releases everything already created on failure. Not reentrant.
 */
bool pixelica_sdl3_create(const PIXELICA_Sdl3Config *config, PIXELICA_Sdl3Platform **out_platform);

/* Safe on NULL. Must outlive any pixelica_context that uses its ops. */
void pixelica_sdl3_destroy(PIXELICA_Sdl3Platform *platform);

/* Fills the PAL table; userdata points at `platform`. */
PIXELICA_PlatformOps pixelica_sdl3_ops(PIXELICA_Sdl3Platform *platform);

/*
 * Processes pending window/keyboard events. Returns false once a quit was
 * requested (window close, SDL_EVENT_QUIT or Escape).
 */
bool pixelica_sdl3_pump_events(PIXELICA_Sdl3Platform *platform);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_PLATFORM_SDL3_H */

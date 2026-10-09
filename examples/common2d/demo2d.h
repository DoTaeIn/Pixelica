/*
 * Interface between a 2D example and the shared runner (common2d/main.c).
 *
 * A 2D demo has no PIXELICA_Context: the runner owns a static RGB565 target buffer
 * (at most 256x192), hands the demo a caller-owned PIXELICA2D_Surface over it, and
 * presents it through the PAL exactly once per rendered frame. The demo keeps
 * its own PIXELICA2D_Context (pixelica2d_init copies the surface by value) and never
 * allocates per frame.
 *
 * Lifecycle driven by the runner:
 *   init(&state, surface)   once; false means failure and nothing is kept
 *   update(state, buttons)  at a fixed 60 Hz; buttons are PIXELICA_BTN_* bits
 *   render(state)           once per presented frame; any status != PIXELICA_OK
 *                           stops the run with a non-zero exit code
 *   shutdown(state)         only after a successful init
 *
 * Exactly one demo defines `pixelica2d_example_demo` per executable.
 */
#ifndef PIXELICA2D_EXAMPLE_DEMO_H
#define PIXELICA2D_EXAMPLE_DEMO_H

#include <stdbool.h>
#include <stdint.h>

#include "pixelica/input.h"
#include "pixelica/renderer2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Size every example uses; also the capacity of the runner's static target. */
#define PIXELICA2D_DEMO_WIDTH 256u
#define PIXELICA2D_DEMO_HEIGHT 192u

typedef struct PIXELICA2D_Demo {
    const char *title;
    uint16_t width, height;
    bool (*init)(void **state, const PIXELICA2D_Surface *surface);
    void (*update)(void *state, uint32_t buttons);
    PIXELICA_Status (*render)(void *state);
    void (*shutdown)(void *state);
} PIXELICA2D_Demo;

extern const PIXELICA2D_Demo pixelica2d_example_demo;

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA2D_EXAMPLE_DEMO_H */

/*
 * hello_online - drawing with the 2D software renderer (pixelica/renderer2d.h).
 *
 * Only compiled when the renderer2d component is available
 * (HELLO_ONLINE_WITH_RENDERER2D). It reads a HolSession and draws a 256x192
 * RGB565 frame; it never calls the network.
 */
#ifndef HELLO_ONLINE_VIEW_H
#define HELLO_ONLINE_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "online_session.h"
#include "pixelica/renderer2d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOL_VIEW_WIDTH 256u
#define HOL_VIEW_HEIGHT 192u

typedef struct HolDraw {
    PIXELICA2D_Context ctx;
    PIXELICA2D_Font font;
} HolDraw;

/* `pixels` holds HOL_VIEW_WIDTH * HOL_VIEW_HEIGHT RGB565 values. */
bool hol_draw_init(HolDraw *draw, uint16_t *pixels);
/* `autopilot` only changes a hint line. PIXELICA_OK or the first renderer error. */
PIXELICA_Status hol_draw_frame(HolDraw *draw, const HolSession *session, bool autopilot);

#ifdef __cplusplus
}
#endif

#endif /* HELLO_ONLINE_VIEW_H */

/*
 * Pixelica - platform abstraction layer (PAL) contract.
 *
 * The core never includes SDL/ESP-IDF headers. A platform fills in a
 * PIXELICA_PlatformOps table and passes it to pixelica_create(); the core copies the
 * table by value, so the table itself need not outlive the call, but
 * `userdata` must stay valid until pixelica_destroy().
 *
 * Required callbacks (when a platform table is supplied):
 *   time_us, read_buttons, present_rgb565.  `log` may be NULL.
 * A NULL platform pointer is allowed for headless rendering: input then
 * reads as 0 and pixelica_end_frame() presents nothing.
 */
#ifndef PIXELICA_PLATFORM_H
#define PIXELICA_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "pixelica/input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PIXELICA_PlatformOps {
    void *userdata;

    /* Monotonic microsecond clock. 64-bit so it never wraps in practice.
     * The core itself does not call this in G0; the driver loop uses it to
     * compute the elapsed time passed to pixelica_update(). */
    uint64_t (*time_us)(void *userdata);

    /* Current logical button bitmask (PIXELICA_BTN_*). Called exactly once per
     * pixelica_update() call. */
    uint32_t (*read_buttons)(void *userdata);

    /* Synchronous presentation. Called exactly once per successful
     * pixelica_end_frame(). `pixels` is engine-owned RGB565 (row 0 = top),
     * `stride_pixels` is the row pitch in pixels (== width in G0).
     * The PAL must finish reading (or copy) the pixels before returning:
     * the engine may overwrite the buffer immediately afterwards. The PAL
     * must never free or retain the pointer. Return false on failure;
     * pixelica_end_frame() then returns PIXELICA_ERR_PLATFORM. */
    bool (*present_rgb565)(void *userdata,
                           const uint16_t *pixels,
                           uint16_t width,
                           uint16_t height,
                           uint32_t stride_pixels);

    /* Optional diagnostic sink, NUL-terminated message, may be NULL. */
    void (*log)(void *userdata, const char *message);
} PIXELICA_PlatformOps;

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_PLATFORM_H */

/*
 * The generated, embedded Pixel Room assets (assets/compiled/pixel_room_assets.h,
 * produced by tools/pixelica_pack's generate_demo_assets.py) wrapped for the room API.
 * No filesystem and no working-directory dependency at runtime.
 */
#ifndef PIXEL_ROOM_EMBEDDED_H
#define PIXEL_ROOM_EMBEDDED_H

#include <stdbool.h>

#include "pixel_room.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Points `out` at the embedded .p3mesh/.p3tex byte arrays (static storage). */
bool pixel_room_embedded_assets(PixelRoomAssets *out);

/* The embedded .p3cart bundle of the same resources (static storage). */
bool pixel_room_embedded_cart(PixelRoomBlob *out);

#ifdef __cplusplus
}
#endif

#endif /* PIXEL_ROOM_EMBEDDED_H */

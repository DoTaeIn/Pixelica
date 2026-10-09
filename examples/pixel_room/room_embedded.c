#include "room_embedded.h"

/* Found through the assets/compiled include directory set in examples/CMakeLists.txt. */
#include "pixel_room_assets.h"

bool pixel_room_embedded_assets(PixelRoomAssets *out)
{
    if (out == NULL) {
        return false;
    }
    out->props[PIXEL_ROOM_PROP_DESK].data = pixelica_asset_desk;
    out->props[PIXEL_ROOM_PROP_DESK].size = pixelica_asset_desk_size;
    out->props[PIXEL_ROOM_PROP_CHAIR].data = pixelica_asset_chair;
    out->props[PIXEL_ROOM_PROP_CHAIR].size = pixelica_asset_chair_size;
    out->props[PIXEL_ROOM_PROP_BOOK].data = pixelica_asset_book;
    out->props[PIXEL_ROOM_PROP_BOOK].size = pixelica_asset_book_size;
    out->props[PIXEL_ROOM_PROP_CUP].data = pixelica_asset_cup;
    out->props[PIXEL_ROOM_PROP_CUP].size = pixelica_asset_cup_size;
    out->props[PIXEL_ROOM_PROP_PLANT].data = pixelica_asset_plant;
    out->props[PIXEL_ROOM_PROP_PLANT].size = pixelica_asset_plant_size;
    out->checker.data = pixelica_asset_checker;
    out->checker.size = pixelica_asset_checker_size;
    return true;
}

bool pixel_room_embedded_cart(PixelRoomBlob *out)
{
    if (out == NULL) {
        return false;
    }
    out->data = pixelica_asset_room_cart;
    out->size = pixelica_asset_room_cart_size;
    return true;
}

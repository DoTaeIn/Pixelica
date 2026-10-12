#include <string.h>
#include "pixelica/input.h"
#include "pixelica/ui.h"
#include "pixelica/save.h"
#include "pixelica/i18n.h"
#include "pixelica/screens.h"
#include "pixelica/camera.h"
#include "pixelica/debug.h"
#include "pixelica/chunks.h"
#include "pixelica/collision2d.h"

static PIXELICA_Status load_chunk(void *userdata, PIXELICA_ChunkCoord coord)
{
    (void)userdata;
    (void)coord;
    return PIXELICA_OK;
}

static void unload_chunk(void *userdata, PIXELICA_ChunkCoord coord)
{
    (void)userdata;
    (void)coord;
}

int main(void)
{
    PIXELICA_Body2D body = {{{0.0f, 0.0f}, {1.0f, 1.0f}}, {0.0f, 0.0f}, 0u, 0u};
    const PIXELICA_AABB2D floor_box = {{-10.0f, 1.0f}, {10.0f, 2.0f}};
    if (pixelica_body2d_step(&body, 1.0f / 60.0f, 600.0f, 220.0f, 600.0f,
        false, &floor_box, 1u) != PIXELICA_OK || !body.grounded) return 40;
    PIXELICA_Chunks chunks = {0};
    PIXELICA_ChunkSlot slots[9];
    PIXELICA_ChunkCoord center = {0, 0, 0}, radius = {1, 1, 0};
    PIXELICA_ChunkRange range;
    size_t operations;
    if (pixelica_chunk_range(center, radius, &range) != PIXELICA_OK) return 30;
    if (pixelica_chunks_init(&chunks, slots, 9u, load_chunk, unload_chunk, NULL) != PIXELICA_OK) return 31;
    if (pixelica_chunks_set_range(&chunks, range) != PIXELICA_OK) return 32;
    if (pixelica_chunks_update(&chunks, 0u, &operations) != PIXELICA_OK) return 33;
    if (operations != 9u || !pixelica_chunks_contains(&chunks, center)) return 34;
    if (pixelica_chunks_clear(&chunks) != PIXELICA_OK || chunks.count != 0u) return 35;
    PIXELICA_Camera2D camera = {{0.0f, 0.0f}, 1.0f};
    PIXELICA_Vec2 viewport = {32.0f, 16.0f}, target = {16.0f, 8.0f};
    PIXELICA_DebugTiming timing;
    static PIXELICA_UI_DebugHud hud;
    if (pixelica_camera2d_follow(&camera, viewport, target, 0.0f, 0.016f, NULL) != PIXELICA_OK) return 20;
    if (camera.position.x != 0.0f || camera.position.y != 0.0f) return 21;
    if (pixelica_debug_timing_init(&timing, 0u) != PIXELICA_OK) return 22;
    if (pixelica_debug_timing_update(&timing, 16667u) != PIXELICA_OK) return 23;
    if (pixelica_ui_debug_hud_init(&hud, 0, 0) != PIXELICA_OK) return 24;
    if (pixelica_ui_debug_hud_update(&hud, &timing, NULL, 0u, 0) != PIXELICA_OK) return 25;
    static const PIXELICA_I18N_Entry entries[] = {{"play", "PLAY"}};
    static const PIXELICA_I18N_Locale locales[] = {{"en", entries, 1u}};
    PIXELICA_I18N_Catalog catalog;
    unsigned char save[32], loaded[4];
    size_t bytes, loaded_bytes;
    uint32_t schema;
    PIXELICA_ScreenManager screens = {0};
    PIXELICA_Screen screen = {0};
    if (pixelica_i18n_init(&catalog, locales, 1u, "en") != PIXELICA_OK) return 10;
    if (strcmp(pixelica_i18n_get(&catalog, "play"), "PLAY") != 0) return 11;
    if (pixelica_save_encode(1u, "OK", 3u, save, sizeof save, &bytes) != PIXELICA_OK) return 12;
    if (pixelica_save_decode(save, bytes, &schema, loaded, sizeof loaded, &loaded_bytes) != PIXELICA_OK) return 13;
    if (schema != 1u || loaded_bytes != 3u || memcmp(loaded, "OK", 3u) != 0) return 14;
    if (pixelica_screens_init(&screens, &screen) != PIXELICA_OK) return 15;
    if (pixelica_screens_update(&screens, 0.016f) != PIXELICA_OK) return 16;
    if (pixelica_screens_shutdown(&screens) != PIXELICA_OK) return 17;
    PIXELICA_UI_Widget widget;
    PIXELICA_UI_Context ui;
    PIXELICA_UI_Rect rect = {0, 0, 32, 16};
    PIXELICA_UI_Input input = pixelica_ui_input_buttons(PIXELICA_BTN_A);
    uint16_t pixels[32 * 16] = {0};
    PIXELICA_UI_Surface surface = {pixels, 32u, 16u, 32u};
    if (pixelica_ui_widget_default(PIXELICA_UI_CHECKBOX, 1u, rect,
        "ON", 2u, &widget) != PIXELICA_OK) return 1;
    if (pixelica_ui_init(&ui, &widget, 1u) != PIXELICA_OK) return 2;
    if (pixelica_ui_update(&ui, &input) != PIXELICA_OK) return 3;
    if (!widget.checked || !pixelica_ui_event(&ui, 1u).changed) return 4;
    if (pixelica_ui_render_rgb565(&ui, &surface) != PIXELICA_OK) return 5;
    return pixels[4u * 32u + 2u] != 0u ? 0 : 6; /* checkbox border */
}

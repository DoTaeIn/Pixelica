/*
 * Pixelica ESP32-P4 firmware entry point.
 *
 * Runs the selected example game (the same C files the desktop builds) through
 * the ESP32-P4 platform:
 *   - Headless run mode: no LCD, no buttons; renders a fixed number of frames
 *     with a fixed 16667 us step, logs the last frame's FNV-1a hash and the
 *     memory/present counters. Needs no board data.
 *   - Board run mode: LCD + buttons supplied by main/board_config.c, which is
 *     NOT implemented (returns ESP_ERR_NOT_SUPPORTED).
 *
 * Targets ESP-IDF v6.0 (esp32p4): hello_triangle and Pixel Room TINY compile;
 * NOT run on a board.
 */
#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "board_config.h"
#include "pixelica_esp32p4.h"
#include "pixelica_game.h"

static const char *TAG = "pixelica_app";

#define FIXED_STEP_US 16667u

static esp_err_t run_game(const PIXELICA_Game *game, PIXELICA_Esp32P4 *platform, bool fixed_step,
                          uint32_t frames /* 0 = until QUIT */)
{
    PIXELICA_Context *ctx = pixelica_esp32p4_context(platform);
    PIXELICA_PlatformOps ops = pixelica_esp32p4_ops(platform);
    void *state = NULL;

    if (!game->init(&state, ctx)) {
        ESP_LOGE(TAG, "game init failed");
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    uint32_t frame = 0u;
    uint64_t previous_us = ops.time_us(ops.userdata);

    while (frames == 0u || frame < frames) {
        if ((ops.read_buttons(ops.userdata) & PIXELICA_BTN_QUIT) != 0u) {
            break;
        }
        uint64_t elapsed_us = FIXED_STEP_US;
        if (!fixed_step) {
            const uint64_t now_us = ops.time_us(ops.userdata);
            elapsed_us = now_us >= previous_us ? now_us - previous_us : 0u;
            previous_us = now_us;
        }
        pixelica_update(ctx, elapsed_us, game->update, state);

        const PIXELICA_Status status = game->render(state, ctx);
        if (status != PIXELICA_OK) {
            ESP_LOGE(TAG, "render failed on frame %" PRIu32 " (PIXELICA_Status %d)", frame, (int)status);
            result = ESP_FAIL;
            break;
        }
        ++frame;
        if (fixed_step && (frame & 15u) == 0u) {
            vTaskDelay(1); /* headless never blocks on DMA: let the idle task and watchdog run */
        }
    }

    if (game->shutdown != NULL) {
        game->shutdown(state);
    }
    return result;
}

static void log_stats(const PIXELICA_Esp32P4 *platform)
{
    PIXELICA_Esp32P4Stats stats;
    if (pixelica_esp32p4_get_stats(platform, &stats) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "engine block: %u bytes (heap caps 0x%x); LCD staging: %u bytes",
             (unsigned)stats.engine_block_bytes, (unsigned)stats.engine_heap_caps,
             (unsigned)stats.lcd_staging_bytes);
    ESP_LOGI(TAG, "free internal: %u (min %u); free SPIRAM: %u (min %u)",
             (unsigned)stats.free_internal_bytes, (unsigned)stats.min_free_internal_bytes,
             (unsigned)stats.free_spiram_bytes, (unsigned)stats.min_free_spiram_bytes);
    ESP_LOGI(TAG, "presented=%" PRIu32 " failures=%" PRIu32 " present_us last=%" PRIu32 " max=%" PRIu32,
             stats.frames_presented, stats.present_failures, stats.present_us_last, stats.present_us_max);
}

void app_main(void)
{
    const PIXELICA_Game *game = &pixelica_example_game;
    PIXELICA_Esp32P4Config cfg;

    pixelica_esp32p4_config_defaults(&cfg);
    cfg.engine = game->config;
#if CONFIG_SPIRAM
    cfg.engine_heap_caps = 0u; /* default: PSRAM */
#else
    cfg.engine_heap_caps = (uint32_t)(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif

    bool fixed_step = true;
    uint32_t frames = 0u;
#if CONFIG_PIXELICA_FW_MODE_BOARD
    esp_err_t err = pixelica_board_configure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board configuration failed (%s); nothing was started", esp_err_to_name(err));
        return;
    }
    fixed_step = false;
#else
    frames = (uint32_t)CONFIG_PIXELICA_FW_HEADLESS_FRAMES;
#endif

    PIXELICA_Esp32P4 *platform = NULL;
    esp_err_t create_err = pixelica_esp32p4_create(&cfg, &platform);
    if (create_err != ESP_OK) {
        ESP_LOGE(TAG, "pixelica_esp32p4_create failed: %s", esp_err_to_name(create_err));
#if CONFIG_PIXELICA_FW_MODE_BOARD
        pixelica_board_release();
#endif
        return;
    }

    esp_err_t run_err = run_game(game, platform, fixed_step, frames);

    log_stats(platform);
#if !CONFIG_PIXELICA_FW_MODE_BOARD
    {
        PIXELICA_Esp32P4Stats stats;
        if (pixelica_esp32p4_get_stats(platform, &stats) == ESP_OK) {
            /* Same line format as the desktop headless driver. */
            ESP_LOGI(TAG, "headless: presented=%" PRIu32 " last_frame_fnv1a=%08" PRIx32,
                     stats.frames_presented, stats.headless_last_frame_fnv1a);
        }
    }
#endif

    if (pixelica_esp32p4_destroy(platform) != ESP_OK) {
        ESP_LOGE(TAG, "platform could not be destroyed safely (LCD DMA not proven idle); "
                      "its memory is intentionally leaked");
        return; /* do not tear down the board while the DMA may still run */
    }
#if CONFIG_PIXELICA_FW_MODE_BOARD
    pixelica_board_release();
#endif
    ESP_LOGI(TAG, "finished: %s", run_err == ESP_OK ? "ok" : "with errors");
}

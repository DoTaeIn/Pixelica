/*
 * Default board hook: not implemented, on purpose. Pixelica has no physical
 * board yet, so nothing here is guessed.
 */
#include "esp_log.h"

#include "board_config.h"

static const char *TAG = "pixelica_board";

esp_err_t pixelica_board_configure(PIXELICA_Esp32P4Config *cfg)
{
    (void)cfg;
    ESP_LOGE(TAG, "No board is configured. Implement pixelica_board_configure() in "
                  "firmware/esp32p4/main/board_config.c. Required board data (see docs/porting.md):");
    ESP_LOGE(TAG, "  - board / module, CPU clock, flash and PSRAM configuration (sdkconfig)");
    ESP_LOGE(TAG, "  - LCD controller model, interface (SPI or I80), bus pins, clock, color order");
    ESP_LOGE(TAG, "  - panel width/height (must equal the game's render size), x/y offset");
    ESP_LOGE(TAG, "  - an initialized esp_lcd panel + panel IO whose done callback is NOT already in use");
    ESP_LOGE(TAG, "  - button GPIOs, polarity and pulls for LEFT/RIGHT/UP/DOWN/A/START/QUIT (-1 = none)");
    ESP_LOGE(TAG, "Switch 'Run mode' to Headless in menuconfig to run without a board.");
    return ESP_ERR_NOT_SUPPORTED;
}

void pixelica_board_release(void)
{
}

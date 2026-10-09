/*
 * Board integration hook. The repository deliberately ships NO board data:
 * no LCD model, no pins, no clocks, no PSRAM size, no resolution.
 */
#ifndef PIXELICA_FW_BOARD_CONFIG_H
#define PIXELICA_FW_BOARD_CONFIG_H

#include "esp_err.h"
#include "pixelica_esp32p4.h"

/*
 * Called only in "Board" run mode, with `cfg` already holding the game's engine
 * configuration, all buttons unassigned and no display. Fill in:
 *   - cfg->display.panel / panel_io: an initialized SPI or I80 esp_lcd panel and
 *     panel IO whose size matches cfg->engine.width/height,
 *   - offset_x/offset_y, swap_bytes, staging_rows/alignment, transfer timeout,
 *   - cfg->buttons[].gpio_num / active_low / pull,
 *   - optionally cfg->engine_heap_caps.
 * Return ESP_OK, or an error to abort. The default implementation returns
 * ESP_ERR_NOT_SUPPORTED and logs what is missing.
 */
esp_err_t pixelica_board_configure(PIXELICA_Esp32P4Config *cfg);

/* Called after pixelica_esp32p4_destroy() succeeded, to tear down the bus/panel. */
void pixelica_board_release(void);

#endif /* PIXELICA_FW_BOARD_CONFIG_H */

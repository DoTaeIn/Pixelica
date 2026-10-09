/* Private to the ESP32-P4 platform component. */
#ifndef PIXELICA_ESP32P4_INTERNAL_H
#define PIXELICA_ESP32P4_INTERNAL_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lcd_transport.h"
#include "pixelica_esp32p4.h"

struct PIXELICA_Esp32P4 {
    /* Allocated from internal RAM: the done callback runs in an ISR and touches `lcd`. */
    PIXELICA_LcdTransport lcd;
    SemaphoreHandle_t done_sem;
    bool lcd_enabled;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t panel_io;
    bool (*quiesce)(void *user);
    void *quiesce_user;

    PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT];
    bool gpio_configured;

    void *engine_memory; /* heap_caps block holding the PIXELICA_Context; never given to the LCD */
    size_t engine_bytes;
    uint32_t engine_heap_caps;
    PIXELICA_Context *ctx;

    uint32_t frames_presented, present_failures;
    uint32_t present_us_last, present_us_max;
    uint32_t headless_hash;
};

extern const char *const PIXELICA_ESP_TAG;

/* input_gpio.c */
esp_err_t pixelica_esp_input_validate(const PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT]);
esp_err_t pixelica_esp_input_configure(const PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT]);
void pixelica_esp_input_release(const PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT]);
uint32_t pixelica_esp_input_read(const PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT]);

/* display_esp_lcd.c */
esp_err_t pixelica_esp_display_init(struct PIXELICA_Esp32P4 *platform, const PIXELICA_EspDisplayConfig *display,
                               uint16_t width, uint16_t height);
bool pixelica_esp_display_present(struct PIXELICA_Esp32P4 *platform, const uint16_t *pixels,
                             uint16_t width, uint16_t height, uint32_t stride_pixels);
esp_err_t pixelica_esp_display_recover(struct PIXELICA_Esp32P4 *platform);
/* ESP_ERR_INVALID_STATE (nothing freed) if the DMA cannot be proven idle. */
esp_err_t pixelica_esp_display_deinit(struct PIXELICA_Esp32P4 *platform);

#endif /* PIXELICA_ESP32P4_INTERNAL_H */

/*
 * Pixelica - ESP32-P4 platform (ESP-IDF v6.0).
 *
 * STATUS: compiles with ESP-IDF v6.0 for esp32p4 (hello_triangle and Pixel Room
 * TINY firmware); NOT run on any board. No LCD model, pin, clock, PSRAM size or
 * resolution is chosen here: the board integration supplies all of them.
 *
 * What this component does
 *   - Time:   esp_timer_get_time() (64-bit microseconds since boot).
 *   - Input:  user-assigned GPIOs mapped to the logical PIXELICA_BTN_* bits.
 *   - Output: a board-supplied, already initialized esp_lcd panel + panel IO
 *             of an SPI or I80 (asynchronous DMA) interface. The engine's
 *             pixels are copied into a permanent PAL-owned DMA staging buffer
 *             and only that buffer is queued; present_rgb565 returns after the
 *             "color transfer done" event. See lcd_transport.h for the exact
 *             lifetime contract (timeout/failure poison the transport and the
 *             staging buffer is neither reused nor freed while a transfer may
 *             be pending).
 *   - Memory: the engine block (colour + depth buffers + arena) is allocated
 *             once with heap_caps_malloc (PSRAM by default) and handed to
 *             pixelica_create_in_buffer(); it is freed by pixelica_esp32p4_destroy().
 *
 * Not supported: RGB and MIPI-DSI panels (continuous scan-out, different
 * ownership rules), audio, SD card.
 */
#ifndef PIXELICA_ESP32P4_H
#define PIXELICA_ESP32P4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "pixelica/pixelica.h"

#ifdef __cplusplus
extern "C" {
#endif

/* gpio_num value meaning "this logical button has no pin". */
#define PIXELICA_ESP_GPIO_UNASSIGNED (-1)

/* Order matches the PIXELICA_BTN_* bit positions. */
typedef enum PIXELICA_EspButton {
    PIXELICA_ESP_BUTTON_LEFT = 0,
    PIXELICA_ESP_BUTTON_RIGHT,
    PIXELICA_ESP_BUTTON_UP,
    PIXELICA_ESP_BUTTON_DOWN,
    PIXELICA_ESP_BUTTON_A,
    PIXELICA_ESP_BUTTON_START,
    PIXELICA_ESP_BUTTON_QUIT,
    PIXELICA_ESP_BUTTON_COUNT
} PIXELICA_EspButton;

typedef enum PIXELICA_EspPull {
    PIXELICA_ESP_PULL_NONE = 0,
    PIXELICA_ESP_PULL_UP,
    PIXELICA_ESP_PULL_DOWN
} PIXELICA_EspPull;

typedef struct PIXELICA_EspButtonPin {
    int gpio_num;     /* PIXELICA_ESP_GPIO_UNASSIGNED (-1) or a valid GPIO number */
    bool active_low;  /* pressed == level 0 */
    PIXELICA_EspPull pull; /* internal pull resistor */
} PIXELICA_EspButtonPin;

typedef struct PIXELICA_EspDisplayConfig {
    /* Both NULL = no LCD (headless: frames are hashed and discarded, used for
     * compile/smoke tests). Both non-NULL = SPI/I80 panel, already
     * initialized by the board code. One of the two alone is an error. */
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t panel_io;
    /* Panel-space position of the frame's top-left pixel (e.g. controller RAM offset). */
    uint16_t offset_x, offset_y;
    /* Rows copied+queued per transfer; 0 = whole frame. Smaller values need
     * less internal DMA RAM and more transfers. */
    uint16_t staging_rows;
    /* Staging alignment, power of two; 0 = PIXELICA_LCD_DEFAULT_ALIGNMENT (128 B,
     * conservative; confirm against the cache line size on the board). */
    uint16_t staging_alignment;
    /* Store RGB565 byte-swapped (most SPI/I80 controllers expect the high byte first). */
    bool swap_bytes;
    /* Completion timeout per transfer in ms; 0 = PIXELICA_LCD_DEFAULT_TIMEOUT_MS. */
    uint32_t transfer_timeout_ms;
    /* Optional: return true ONLY if the bus is provably idle and no more
     * done-events will arrive for abandoned transfers (lets a poisoned
     * transport recover without a late completion). NULL = not available. */
    bool (*quiesce)(void *user);
    void *quiesce_user;
} PIXELICA_EspDisplayConfig;

typedef struct PIXELICA_Esp32P4Config {
    /* Engine configuration; width/height are the rendered frame size and the
     * LCD region size. Must be valid for pixelica_memory_requirement(). */
    PIXELICA_Config engine;
    /* heap_caps flags for the engine block. 0 = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT.
     * Use MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT to force internal RAM. */
    uint32_t engine_heap_caps;
    PIXELICA_EspDisplayConfig display;
    PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT]; /* zero-init is NOT "unassigned": set gpio_num */
} PIXELICA_Esp32P4Config;

/* Fills `cfg` with all pins unassigned (-1), no display, engine zeroed. */
void pixelica_esp32p4_config_defaults(PIXELICA_Esp32P4Config *cfg);

typedef struct PIXELICA_Esp32P4 PIXELICA_Esp32P4;

/*
 * Validates the whole configuration first (nothing is touched on a validation
 * error), then configures GPIOs, creates the LCD transport, allocates the
 * engine block and the PIXELICA context. On any failure everything already done is
 * undone. ESP_ERR_INVALID_ARG: bad config; ESP_ERR_NO_MEM: allocation failed.
 */
esp_err_t pixelica_esp32p4_create(const PIXELICA_Esp32P4Config *cfg, PIXELICA_Esp32P4 **out_platform);

/* The context created by pixelica_esp32p4_create(); NULL after destroy started. */
PIXELICA_Context *pixelica_esp32p4_context(const PIXELICA_Esp32P4 *platform);

/* PAL table (userdata = platform). Valid as long as the platform exists. */
PIXELICA_PlatformOps pixelica_esp32p4_ops(PIXELICA_Esp32P4 *platform);

/* Tries to clear a poisoned display transport (see lcd_transport.h). */
esp_err_t pixelica_esp32p4_display_recover(PIXELICA_Esp32P4 *platform);

/*
 * Destroys the context, frees the engine block, releases GPIOs and the LCD
 * staging buffer. If the LCD DMA cannot be proven idle it returns
 * ESP_ERR_INVALID_STATE and DELIBERATELY KEEPS the platform object and the
 * staging buffer allocated (a leak is preferred to DMA into freed memory);
 * the handle stays valid and destroy may be retried later.
 */
esp_err_t pixelica_esp32p4_destroy(PIXELICA_Esp32P4 *platform);

typedef struct PIXELICA_Esp32P4Stats {
    /* Sizes decided at create time. */
    size_t engine_block_bytes;
    uint32_t engine_heap_caps;
    size_t lcd_staging_bytes;
    /* Heap state when sampled (heap_caps_get_*). Values exist only at run time on a board. */
    size_t free_internal_bytes, min_free_internal_bytes;
    size_t free_spiram_bytes, min_free_spiram_bytes;
    /* Presentation. */
    uint32_t frames_presented, present_failures;
    uint32_t present_us_last, present_us_max;
    uint32_t lcd_state; /* PIXELICA_LcdState; 0 when headless */
    uint32_t headless_last_frame_fnv1a; /* only meaningful without a display */
} PIXELICA_Esp32P4Stats;

esp_err_t pixelica_esp32p4_get_stats(const PIXELICA_Esp32P4 *platform, PIXELICA_Esp32P4Stats *out);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_ESP32P4_H */

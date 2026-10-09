/*
 * ESP32-P4 platform: PAL (time, buttons, present, log), engine memory in
 * PSRAM via heap_caps + pixelica_create_in_buffer, and lifecycle. Written against
 * the ESP-IDF v6.0 documentation. Compiles with ESP-IDF v6.0 (esp32p4); not run
 * on a board, so timing, PSRAM placement and DMA behavior are unverified.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "platform_esp32p4_internal.h"

const char *const PIXELICA_ESP_TAG = "pixelica";

/* The button order is shared with the logical input bits. */
_Static_assert(((uint32_t)1u << PIXELICA_ESP_BUTTON_LEFT) == PIXELICA_BTN_LEFT, "button order");
_Static_assert(((uint32_t)1u << PIXELICA_ESP_BUTTON_RIGHT) == PIXELICA_BTN_RIGHT, "button order");
_Static_assert(((uint32_t)1u << PIXELICA_ESP_BUTTON_UP) == PIXELICA_BTN_UP, "button order");
_Static_assert(((uint32_t)1u << PIXELICA_ESP_BUTTON_DOWN) == PIXELICA_BTN_DOWN, "button order");
_Static_assert(((uint32_t)1u << PIXELICA_ESP_BUTTON_A) == PIXELICA_BTN_A, "button order");
_Static_assert(((uint32_t)1u << PIXELICA_ESP_BUTTON_START) == PIXELICA_BTN_START, "button order");
_Static_assert(((uint32_t)1u << PIXELICA_ESP_BUTTON_QUIT) == PIXELICA_BTN_QUIT, "button order");

void pixelica_esp32p4_config_defaults(PIXELICA_Esp32P4Config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    for (int i = 0; i < (int)PIXELICA_ESP_BUTTON_COUNT; ++i) {
        cfg->buttons[i].gpio_num = PIXELICA_ESP_GPIO_UNASSIGNED;
    }
}

/* --- PAL ------------------------------------------------------------------ */

static uint64_t pal_time_us(void *userdata)
{
    (void)userdata;
    int64_t now = esp_timer_get_time(); /* monotonic, microseconds since boot */
    return now > 0 ? (uint64_t)now : 0u;
}

static uint32_t pal_read_buttons(void *userdata)
{
    const struct PIXELICA_Esp32P4 *p = userdata;
    return pixelica_esp_input_read(p->buttons);
}

/* No LCD: hash and discard, so a headless firmware still exercises the whole path. */
static bool headless_present(struct PIXELICA_Esp32P4 *p, const uint16_t *pixels, uint16_t width,
                             uint16_t height, uint32_t stride_pixels)
{
    if (pixels == NULL || width == 0u || height == 0u || stride_pixels < width) {
        return false;
    }
    uint32_t hash = 2166136261u;
    for (uint32_t y = 0u; y < height; ++y) {
        const uint16_t *row = pixels + (size_t)y * stride_pixels;
        for (uint32_t x = 0u; x < width; ++x) {
            hash = (hash ^ (uint32_t)(row[x] & 0xFFu)) * 16777619u;
            hash = (hash ^ (uint32_t)(row[x] >> 8)) * 16777619u;
        }
    }
    p->headless_hash = hash;
    return true;
}

static bool pal_present(void *userdata, const uint16_t *pixels, uint16_t width, uint16_t height,
                        uint32_t stride_pixels)
{
    struct PIXELICA_Esp32P4 *p = userdata;
    const int64_t started = esp_timer_get_time();
    const bool ok = p->lcd_enabled
        ? pixelica_esp_display_present(p, pixels, width, height, stride_pixels)
        : headless_present(p, pixels, width, height, stride_pixels);
    int64_t elapsed = esp_timer_get_time() - started;
    if (elapsed < 0) {
        elapsed = 0;
    }
    if (elapsed > (int64_t)UINT32_MAX) {
        elapsed = (int64_t)UINT32_MAX;
    }
    p->present_us_last = (uint32_t)elapsed;
    if (p->present_us_last > p->present_us_max) {
        p->present_us_max = p->present_us_last;
    }
    if (ok) {
        p->frames_presented += 1u;
    } else {
        p->present_failures += 1u;
    }
    return ok;
}

static void pal_log(void *userdata, const char *message)
{
    (void)userdata;
    ESP_LOGI(PIXELICA_ESP_TAG, "%s", message);
}

PIXELICA_PlatformOps pixelica_esp32p4_ops(PIXELICA_Esp32P4 *platform)
{
    PIXELICA_PlatformOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.userdata = platform;
    ops.time_us = pal_time_us;
    ops.read_buttons = pal_read_buttons;
    ops.present_rgb565 = pal_present;
    ops.log = pal_log;
    return ops;
}

/* --- lifecycle ------------------------------------------------------------- */

esp_err_t pixelica_esp32p4_create(const PIXELICA_Esp32P4Config *cfg, PIXELICA_Esp32P4 **out_platform)
{
    if (out_platform == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_platform = NULL;
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. Validate everything before touching hardware or memory. */
    size_t engine_bytes = 0u;
    if (pixelica_memory_requirement(&cfg->engine, &engine_bytes) != PIXELICA_OK) {
        ESP_LOGE(PIXELICA_ESP_TAG, "invalid engine configuration (pixelica_memory_requirement rejected it)");
        return ESP_ERR_INVALID_ARG;
    }
    if ((cfg->display.panel == NULL) != (cfg->display.panel_io == NULL)) {
        ESP_LOGE(PIXELICA_ESP_TAG, "display: panel and panel_io must both be set, or both NULL for headless");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = pixelica_esp_input_validate(cfg->buttons);
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t caps = cfg->engine_heap_caps != 0u
        ? cfg->engine_heap_caps
        : (uint32_t)(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    /* The platform object is touched by the LCD done interrupt: internal RAM. */
    struct PIXELICA_Esp32P4 *p = heap_caps_calloc(1u, sizeof(*p), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(p->buttons, cfg->buttons, sizeof(p->buttons));
    p->engine_heap_caps = caps;
    p->engine_bytes = engine_bytes;

    /* 2. Input. */
    err = pixelica_esp_input_configure(p->buttons);
    if (err != ESP_OK) {
        heap_caps_free(p);
        return err;
    }
    p->gpio_configured = true;

    /* 3. Display transport (permanent staging) if a panel was supplied. */
    if (cfg->display.panel != NULL) {
        err = pixelica_esp_display_init(p, &cfg->display, cfg->engine.width, cfg->engine.height);
        if (err != ESP_OK) {
            pixelica_esp_input_release(p->buttons);
            heap_caps_free(p);
            return err;
        }
    } else {
        ESP_LOGW(PIXELICA_ESP_TAG, "no display configured: frames are hashed and discarded (headless)");
    }

    /* 4. Engine memory (PSRAM by default) handed to pixelica_create_in_buffer. */
    p->engine_memory = heap_caps_malloc(engine_bytes, caps);
    if (p->engine_memory == NULL) {
        ESP_LOGE(PIXELICA_ESP_TAG, "engine block: %u bytes with heap caps 0x%x unavailable "
                 "(free internal %u, free SPIRAM %u)", (unsigned)engine_bytes, (unsigned)caps,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    {
        PIXELICA_PlatformOps ops = pixelica_esp32p4_ops(p);
        PIXELICA_Status status = pixelica_create_in_buffer(&cfg->engine, &ops, p->engine_memory,
                                                 engine_bytes, &p->ctx);
        if (status != PIXELICA_OK) {
            ESP_LOGE(PIXELICA_ESP_TAG, "pixelica_create_in_buffer failed (PIXELICA_Status %d)", (int)status);
            err = status == PIXELICA_ERR_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
            goto fail;
        }
    }
    *out_platform = p;
    return ESP_OK;

fail:
    heap_caps_free(p->engine_memory);
    p->engine_memory = NULL;
    (void)pixelica_esp_display_deinit(p); /* nothing was ever queued: succeeds */
    pixelica_esp_input_release(p->buttons);
    heap_caps_free(p);
    return err;
}

PIXELICA_Context *pixelica_esp32p4_context(const PIXELICA_Esp32P4 *platform)
{
    return platform != NULL ? platform->ctx : NULL;
}

esp_err_t pixelica_esp32p4_display_recover(PIXELICA_Esp32P4 *platform)
{
    if (platform == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return pixelica_esp_display_recover(platform);
}

esp_err_t pixelica_esp32p4_destroy(PIXELICA_Esp32P4 *platform)
{
    if (platform == NULL) {
        return ESP_OK;
    }
    /* The engine block is never visible to the LCD DMA (only staging is), so it
     * can be released even if the display cannot be drained. */
    if (platform->ctx != NULL) {
        pixelica_destroy(platform->ctx);
        platform->ctx = NULL;
    }
    if (platform->engine_memory != NULL) {
        heap_caps_free(platform->engine_memory);
        platform->engine_memory = NULL;
    }
    if (platform->gpio_configured) {
        pixelica_esp_input_release(platform->buttons);
        platform->gpio_configured = false;
    }
    esp_err_t err = pixelica_esp_display_deinit(platform);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE; /* platform object + staging intentionally kept */
    }
    heap_caps_free(platform);
    return ESP_OK;
}

esp_err_t pixelica_esp32p4_get_stats(const PIXELICA_Esp32P4 *platform, PIXELICA_Esp32P4Stats *out)
{
    if (platform == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->engine_block_bytes = platform->engine_bytes;
    out->engine_heap_caps = platform->engine_heap_caps;
    out->lcd_staging_bytes = platform->lcd.staging_bytes;
    out->free_internal_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->min_free_internal_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->free_spiram_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->min_free_spiram_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    out->frames_presented = platform->frames_presented;
    out->present_failures = platform->present_failures;
    out->present_us_last = platform->present_us_last;
    out->present_us_max = platform->present_us_max;
    out->lcd_state = platform->lcd_enabled ? (uint32_t)pixelica_lcd_transport_state(&platform->lcd) : 0u;
    out->headless_last_frame_fnv1a = platform->headless_hash;
    return ESP_OK;
}

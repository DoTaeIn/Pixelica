/*
 * esp_lcd glue for the lifetime-safe LCD transport (lcd_transport.h).
 *
 * Supported: SPI / I80 panels whose panel IO reports "color transfer done"
 * through esp_lcd_panel_io_register_event_callbacks(). The board supplies the
 * initialized panel and panel IO; this file never creates a bus or a panel.
 *
 * The completion callback gives a binary semaphore and bumps the transport's
 * completion counter. The return value of esp_lcd_panel_draw_bitmap() is only
 * "queued", never "finished".
 */
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "platform_esp32p4_internal.h"

/* Permanent staging lives in internal, DMA-capable, byte-addressable RAM. */
#define STAGING_CAPS (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static int port_draw(void *ctx, int x_start, int y_start, int x_end, int y_end, const void *pixels)
{
    struct PIXELICA_Esp32P4 *p = ctx;
    /* Queues an asynchronous transfer; completion arrives via the callback below. */
    return (int)esp_lcd_panel_draw_bitmap(p->panel, x_start, y_start, x_end, y_end, pixels);
}

static bool port_wait_done(void *ctx, uint32_t timeout_ms)
{
    struct PIXELICA_Esp32P4 *p = ctx;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (ticks == 0) {
        ticks = 1;
    }
    return xSemaphoreTake(p->done_sem, ticks) == pdTRUE;
}

static void *port_alloc_staging(void *ctx, size_t alignment, size_t bytes)
{
    (void)ctx;
    return heap_caps_aligned_alloc(alignment, bytes, STAGING_CAPS);
}

static void port_free_staging(void *ctx, void *staging)
{
    (void)ctx;
    heap_caps_free(staging);
}

static bool port_quiesce(void *ctx)
{
    struct PIXELICA_Esp32P4 *p = ctx;
    return p->quiesce != NULL && p->quiesce(p->quiesce_user);
}

/* Runs in the panel IO's interrupt context. */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    struct PIXELICA_Esp32P4 *p = user_ctx;
    BaseType_t high_task_woken = pdFALSE;
    pixelica_lcd_transport_isr_done(&p->lcd);
    xSemaphoreGiveFromISR(p->done_sem, &high_task_woken);
    return high_task_woken == pdTRUE;
}

esp_err_t pixelica_esp_display_init(struct PIXELICA_Esp32P4 *p, const PIXELICA_EspDisplayConfig *display,
                               uint16_t width, uint16_t height)
{
    p->panel = display->panel;
    p->panel_io = display->panel_io;
    p->quiesce = display->quiesce;
    p->quiesce_user = display->quiesce_user;

    p->done_sem = xSemaphoreCreateBinary();
    if (p->done_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    PIXELICA_LcdTransportConfig tc = {
        .port = {
            .ctx = p,
            .draw = port_draw,
            .wait_done = port_wait_done,
            .alloc_staging = port_alloc_staging,
            .free_staging = port_free_staging,
            .quiesce = display->quiesce != NULL ? port_quiesce : NULL,
        },
        .width = width,
        .height = height,
        .offset_x = display->offset_x,
        .offset_y = display->offset_y,
        .staging_rows = display->staging_rows,
        .alignment = display->staging_alignment,
        .swap_bytes = display->swap_bytes,
        .timeout_ms = display->transfer_timeout_ms,
    };
    PIXELICA_LcdStatus status = pixelica_lcd_transport_init(&p->lcd, &tc);
    if (status != PIXELICA_LCD_OK) {
        ESP_LOGE(PIXELICA_ESP_TAG, "LCD staging setup failed (PIXELICA_LcdStatus %d): need %ux%u RGB565 rows in "
                 "internal DMA RAM", (int)status, (unsigned)width, (unsigned)height);
        vSemaphoreDelete(p->done_sem);
        p->done_sem = NULL;
        return status == PIXELICA_LCD_ERR_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = on_color_trans_done,
    };
    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(p->panel_io, &callbacks, p);
    if (err != ESP_OK) {
        ESP_LOGE(PIXELICA_ESP_TAG, "registering the color-transfer-done callback failed: %s",
                 esp_err_to_name(err));
        (void)pixelica_lcd_transport_deinit(&p->lcd); /* nothing was ever queued: always succeeds */
        vSemaphoreDelete(p->done_sem);
        p->done_sem = NULL;
        return err;
    }
    p->lcd_enabled = true;
    return ESP_OK;
}

bool pixelica_esp_display_present(struct PIXELICA_Esp32P4 *p, const uint16_t *pixels, uint16_t width,
                             uint16_t height, uint32_t stride_pixels)
{
    PIXELICA_LcdStatus status = pixelica_lcd_transport_present(&p->lcd, pixels, width, height, stride_pixels);
    if (status == PIXELICA_LCD_OK) {
        return true;
    }
    ESP_LOGE(PIXELICA_ESP_TAG, "LCD present failed (PIXELICA_LcdStatus %d, transport state %d); "
             "staging is kept until the transfer is proven finished",
             (int)status, (int)pixelica_lcd_transport_state(&p->lcd));
    return false;
}

esp_err_t pixelica_esp_display_recover(struct PIXELICA_Esp32P4 *p)
{
    if (!p->lcd_enabled) {
        return ESP_OK;
    }
    return pixelica_lcd_transport_recover(&p->lcd) == PIXELICA_LCD_OK ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t pixelica_esp_display_deinit(struct PIXELICA_Esp32P4 *p)
{
    if (!p->lcd_enabled) {
        return ESP_OK;
    }
    if (pixelica_lcd_transport_state(&p->lcd) != PIXELICA_LCD_STATE_CLOSED &&
        pixelica_lcd_transport_deinit(&p->lcd) != PIXELICA_LCD_OK) {
        ESP_LOGE(PIXELICA_ESP_TAG, "LCD DMA could not be proven idle: the staging buffer and the platform "
                 "object are kept allocated (intentional leak)");
        return ESP_ERR_INVALID_STATE;
    }
    /* Drained (staging released). Detach the callback before the semaphore and the
     * platform object, which the callback uses, can go. If detaching fails keep both. */
    const esp_lcd_panel_io_callbacks_t none = {0};
    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(p->panel_io, &none, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(PIXELICA_ESP_TAG, "could not detach the color-transfer-done callback (%s): "
                 "the platform object is kept allocated", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    vSemaphoreDelete(p->done_sem);
    p->done_sem = NULL;
    p->lcd_enabled = false;
    return ESP_OK;
}

#include "lcd_transport.h"

#include <string.h>

static bool is_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool port_is_valid(const PIXELICA_LcdPort *port)
{
    return port->draw != NULL && port->wait_done != NULL &&
           port->alloc_staging != NULL && port->free_staging != NULL;
}

PIXELICA_LcdStatus pixelica_lcd_transport_init(PIXELICA_LcdTransport *t, const PIXELICA_LcdTransportConfig *config)
{
    if (t == NULL) {
        return PIXELICA_LCD_ERR_INVALID_ARG;
    }
    memset(t, 0, sizeof(*t));
    if (config == NULL || !port_is_valid(&config->port) || config->width == 0u ||
        config->height == 0u) {
        return PIXELICA_LCD_ERR_INVALID_ARG;
    }

    uint32_t alignment = config->alignment != 0u ? config->alignment : PIXELICA_LCD_DEFAULT_ALIGNMENT;
    if (alignment < 2u || !is_power_of_two(alignment)) {
        return PIXELICA_LCD_ERR_INVALID_ARG;
    }
    uint32_t rows = config->staging_rows;
    if (rows == 0u || rows > config->height) {
        rows = config->height;
    }

    /* Round the size up to a whole number of alignment units so a cache
     * write-back of the buffer never touches a neighbouring allocation. */
    uint64_t bytes = (uint64_t)rows * config->width * 2u;
    bytes = (bytes + alignment - 1u) / alignment * alignment;
    if (bytes > (uint64_t)SIZE_MAX / 2u) {
        return PIXELICA_LCD_ERR_INVALID_ARG;
    }

    void *staging = config->port.alloc_staging(config->port.ctx, alignment, (size_t)bytes);
    if (staging == NULL) {
        return PIXELICA_LCD_ERR_NO_MEMORY;
    }

    t->port = config->port;
    t->width = config->width;
    t->height = config->height;
    t->offset_x = config->offset_x;
    t->offset_y = config->offset_y;
    t->staging_rows = (uint16_t)rows;
    t->swap_bytes = config->swap_bytes;
    t->timeout_ms = config->timeout_ms != 0u ? config->timeout_ms : PIXELICA_LCD_DEFAULT_TIMEOUT_MS;
    t->staging = staging;
    t->staging_bytes = (size_t)bytes;
    t->state = PIXELICA_LCD_STATE_IDLE;
    t->submitted = 0u;
    t->completed = 0u;
    return PIXELICA_LCD_OK;
}

/* Waits until every submitted draw has a counted completion. */
static PIXELICA_LcdStatus wait_for_completion(PIXELICA_LcdTransport *t)
{
    for (;;) {
        uint32_t done = t->completed;
        if (done == t->submitted) {
            return PIXELICA_LCD_OK;
        }
        if ((int32_t)(done - t->submitted) > 0) {
            t->stat_protocol_errors += 1u;
            return PIXELICA_LCD_ERR_PROTOCOL;
        }
        if (!t->port.wait_done(t->port.ctx, t->timeout_ms)) {
            if (t->completed == t->submitted) { /* finished while timing out */
                return PIXELICA_LCD_OK;
            }
            t->stat_timeouts += 1u;
            return PIXELICA_LCD_ERR_TIMEOUT;
        }
        /* A notification can be stale or spurious: loop and re-check the counters. */
    }
}

static void copy_band(const PIXELICA_LcdTransport *t, const uint16_t *src, uint32_t stride_pixels,
                      uint32_t rows)
{
    uint16_t *dst = t->staging;
    size_t width = t->width;

    for (uint32_t row = 0u; row < rows; ++row) {
        const uint16_t *line = src + (size_t)row * stride_pixels;
        if (!t->swap_bytes) {
            memcpy(dst, line, width * sizeof(uint16_t));
        } else {
            for (size_t x = 0u; x < width; ++x) {
                uint16_t value = line[x];
                dst[x] = (uint16_t)((uint16_t)(value << 8) | (uint16_t)(value >> 8));
            }
        }
        dst += width;
    }
}

PIXELICA_LcdStatus pixelica_lcd_transport_present(PIXELICA_LcdTransport *t, const uint16_t *pixels,
                                        uint16_t width, uint16_t height, uint32_t stride_pixels)
{
    if (t == NULL) {
        return PIXELICA_LCD_ERR_INVALID_ARG;
    }
    if (t->state == PIXELICA_LCD_STATE_POISONED) {
        if (t->completed != t->submitted) {
            return PIXELICA_LCD_ERR_POISONED; /* staging untouched, nothing submitted */
        }
        /* The late completion of the abandoned draw has since been counted. */
        t->state = PIXELICA_LCD_STATE_IDLE;
        t->stat_recoveries += 1u;
    }
    if (t->state != PIXELICA_LCD_STATE_IDLE) {
        return PIXELICA_LCD_ERR_STATE;
    }
    if (pixels == NULL || width != t->width || height != t->height || stride_pixels < width) {
        return PIXELICA_LCD_ERR_INVALID_ARG;
    }
    /* IDLE means every earlier draw completed; a counter mismatch is a stale
     * or duplicate event, after which a completion can no longer be trusted. */
    if (t->completed != t->submitted) {
        t->state = PIXELICA_LCD_STATE_POISONED;
        t->stat_protocol_errors += 1u;
        return PIXELICA_LCD_ERR_PROTOCOL;
    }

    for (uint32_t row = 0u; row < t->height; row += t->staging_rows) {
        uint32_t rows = t->height - row;
        if (rows > t->staging_rows) {
            rows = t->staging_rows;
        }
        copy_band(t, pixels + (size_t)row * stride_pixels, stride_pixels, rows);

        /* Pessimistic: POISONED until this draw's completion is observed. */
        t->state = PIXELICA_LCD_STATE_POISONED;
        t->submitted += 1u;
        t->stat_bands += 1u;
        int rc = t->port.draw(t->port.ctx, (int)t->offset_x, (int)(t->offset_y + row),
                              (int)t->offset_x + (int)t->width,
                              (int)(t->offset_y + row + rows), t->staging);
        if (rc != 0) {
            t->stat_draw_failures += 1u; /* may have been partly queued: stay poisoned */
            return PIXELICA_LCD_ERR_DRAW;
        }
        PIXELICA_LcdStatus status = wait_for_completion(t);
        if (status != PIXELICA_LCD_OK) {
            return status;
        }
        t->state = PIXELICA_LCD_STATE_IDLE;
    }
    return PIXELICA_LCD_OK;
}

PIXELICA_LcdStatus pixelica_lcd_transport_recover(PIXELICA_LcdTransport *t)
{
    if (t == NULL) {
        return PIXELICA_LCD_ERR_INVALID_ARG;
    }
    if (t->state == PIXELICA_LCD_STATE_IDLE) {
        return PIXELICA_LCD_OK;
    }
    if (t->state != PIXELICA_LCD_STATE_POISONED) {
        return PIXELICA_LCD_ERR_STATE;
    }
    if (wait_for_completion(t) == PIXELICA_LCD_OK) {
        t->state = PIXELICA_LCD_STATE_IDLE;
        t->stat_recoveries += 1u;
        return PIXELICA_LCD_OK;
    }
    if (t->port.quiesce != NULL && t->port.quiesce(t->port.ctx)) {
        t->submitted = t->completed; /* abandoned draws are accounted as finished */
        t->state = PIXELICA_LCD_STATE_IDLE;
        t->stat_recoveries += 1u;
        return PIXELICA_LCD_OK;
    }
    return PIXELICA_LCD_ERR_BUSY;
}

PIXELICA_LcdStatus pixelica_lcd_transport_deinit(PIXELICA_LcdTransport *t)
{
    if (t == NULL) {
        return PIXELICA_LCD_ERR_INVALID_ARG;
    }
    if (t->state != PIXELICA_LCD_STATE_IDLE && t->state != PIXELICA_LCD_STATE_POISONED) {
        return PIXELICA_LCD_ERR_STATE;
    }
    if (pixelica_lcd_transport_recover(t) != PIXELICA_LCD_OK) {
        return PIXELICA_LCD_ERR_BUSY; /* keep staging: a transfer may still read it */
    }
    t->port.free_staging(t->port.ctx, t->staging);
    t->staging = NULL;
    t->staging_bytes = 0u;
    t->state = PIXELICA_LCD_STATE_CLOSED;
    return PIXELICA_LCD_OK;
}

PIXELICA_LcdState pixelica_lcd_transport_state(const PIXELICA_LcdTransport *t)
{
    return t != NULL ? t->state : PIXELICA_LCD_STATE_UNINIT;
}

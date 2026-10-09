/*
 * Pixelica - LCD transport with an explicit buffer-lifetime contract.
 *
 * Pure C11, no ESP-IDF includes: the ESP-IDF glue (display_esp_lcd.c) supplies
 * five small port callbacks, and a host test (host_test/test_lcd_transport.c)
 * supplies mocks. This is the one place that decides when DMA-visible memory
 * may be written, reused or freed.
 *
 * Why it exists
 *   PIXELICA_PlatformOps.present_rgb565 must return only when the engine may reuse
 *   its pixel buffer. esp_lcd_panel_draw_bitmap() on SPI/I80 panels only QUEUES
 *   an asynchronous DMA transfer; its return value is NOT completion. The
 *   engine pixels are therefore never handed to the LCD driver. They are copied
 *   (optionally byte-swapped) into a permanent, aligned, DMA-capable STAGING
 *   buffer owned by this transport, and only the staging buffer is queued.
 *
 * Lifetime contract
 *   - The staging buffer is allocated once in init and released only by a
 *     successful deinit. It is never allocated per frame.
 *   - A band (staging_rows rows) is copied into staging only while no
 *     transfer is outstanding. After draw() is called the staging buffer is
 *     read-only for the CPU until a completion for THAT draw has been
 *     observed.
 *   - Completion is observed only through the board's "color transfer done"
 *     event (esp_lcd_panel_io_register_event_callbacks ->
 *     on_color_trans_done), which must call pixelica_lcd_transport_isr_done() once
 *     per queued draw. The return value of draw() never counts as completion.
 *   - The transport is pessimistic: it enters POISONED before every draw()
 *     and returns to IDLE only after the matching completion was observed.
 *     Any timeout, draw() failure, or event/submission mismatch leaves it
 *     POISONED: staging is neither rewritten nor freed, and present() refuses
 *     to submit until the abandoned draw's completion has been counted (a
 *     late completion event; present() notices it without waiting).
 *   - pixelica_lcd_transport_recover() leaves POISONED only when a late completion
 *     arrives, or when the board's optional quiesce() proves the hardware is
 *     idle (and will deliver no further completion for the abandoned draws).
 *   - deinit frees staging only when IDLE (or after a successful recover). If
 *     the DMA cannot be proven idle it returns PIXELICA_LCD_ERR_BUSY and KEEPS
 *     ownership: the transport struct and staging must then stay allocated.
 *     Leaking is chosen over a DMA write into freed memory.
 *   - One draw is in flight at a time. present() is called from one task.
 *
 * Assumption to verify on real hardware (NOT verified): the panel IO delivers
 * exactly one on_color_trans_done per successful esp_lcd_panel_draw_bitmap()
 * call, after the last byte of that call's color data has been read. A
 * mismatch is detected only when more completions than draws are counted
 * (PIXELICA_LCD_ERR_PROTOCOL); a driver that never reports completion shows up as
 * PIXELICA_LCD_ERR_TIMEOUT.
 *
 * Not handled: RGB / MIPI-DSI panels. Their frame buffers are scanned out
 * continuously and have different ownership rules; no completion event is
 * assumed for them.
 */
#ifndef PIXELICA_ESP32P4_LCD_TRANSPORT_H
#define PIXELICA_ESP32P4_LCD_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define PIXELICA_LCD_INLINE static inline __attribute__((always_inline))
#else
#define PIXELICA_LCD_INLINE static inline
#endif

/* Conservative defaults (PAL tuning, not board data). */
#define PIXELICA_LCD_DEFAULT_ALIGNMENT 128u
#define PIXELICA_LCD_DEFAULT_TIMEOUT_MS 1000u

typedef enum PIXELICA_LcdStatus {
    PIXELICA_LCD_OK = 0,
    PIXELICA_LCD_ERR_INVALID_ARG,
    PIXELICA_LCD_ERR_NO_MEMORY, /* staging allocation failed */
    PIXELICA_LCD_ERR_STATE,     /* not initialized, or already closed */
    PIXELICA_LCD_ERR_POISONED,  /* earlier transfer may still be pending: nothing was submitted */
    PIXELICA_LCD_ERR_DRAW,      /* draw() failed; now POISONED */
    PIXELICA_LCD_ERR_TIMEOUT,   /* no completion in time; now POISONED */
    PIXELICA_LCD_ERR_PROTOCOL,  /* more completions than draws; now POISONED */
    PIXELICA_LCD_ERR_BUSY       /* recover/deinit could not prove DMA idle; ownership retained */
} PIXELICA_LcdStatus;

typedef enum PIXELICA_LcdState {
    PIXELICA_LCD_STATE_UNINIT = 0,
    PIXELICA_LCD_STATE_IDLE,     /* no transfer outstanding, staging owned by the CPU */
    PIXELICA_LCD_STATE_POISONED, /* a transfer may be reading staging */
    PIXELICA_LCD_STATE_CLOSED    /* deinit succeeded, staging released */
} PIXELICA_LcdState;

/* Callbacks supplied by the board glue / test. All run in task context. */
typedef struct PIXELICA_LcdPort {
    void *ctx;
    /* Queue ONE asynchronous transfer of the rectangle [x_start,x_end) x
     * [y_start,y_end) read from `pixels`. Returns 0 if accepted. Never counts
     * as completion. */
    int (*draw)(void *ctx, int x_start, int y_start, int x_end, int y_end, const void *pixels);
    /* Block up to timeout_ms for a completion notification (e.g. a semaphore
     * given by the done callback). true = a notification was consumed. May
     * return true spuriously; the transport re-checks its own counters. */
    bool (*wait_done)(void *ctx, uint32_t timeout_ms);
    /* Permanent DMA-capable allocation of `bytes` at `alignment`; NULL on failure. */
    void *(*alloc_staging)(void *ctx, size_t alignment, size_t bytes);
    void (*free_staging)(void *ctx, void *staging);
    /* Optional. Return true ONLY if the hardware is known to be idle (nothing
     * can still read staging) and no further completion will arrive for the
     * abandoned draws. NULL = recovery relies on a late completion only. */
    bool (*quiesce)(void *ctx);
} PIXELICA_LcdPort;

typedef struct PIXELICA_LcdTransportConfig {
    PIXELICA_LcdPort port;
    uint16_t width, height;       /* pixels per frame, 1.. */
    uint16_t offset_x, offset_y;  /* panel-space position of the frame's top-left pixel */
    uint16_t staging_rows;        /* rows per transfer; 0 = whole frame; clamped to height */
    uint16_t alignment;           /* staging alignment bytes, power of two >= 2; 0 = default */
    bool swap_bytes;              /* store each RGB565 value byte-swapped (big-endian wire order) */
    uint32_t timeout_ms;          /* per-transfer completion timeout; 0 = default */
} PIXELICA_LcdTransportConfig;

typedef struct PIXELICA_LcdTransport {
    PIXELICA_LcdPort port;
    uint16_t width, height, offset_x, offset_y, staging_rows;
    bool swap_bytes;
    uint32_t timeout_ms;
    uint16_t *staging;
    size_t staging_bytes;
    PIXELICA_LcdState state;
    uint32_t submitted;          /* draws issued (task side) */
    volatile uint32_t completed; /* completions counted (done callback side) */
    /* Diagnostics, written by the task side only. */
    uint32_t stat_bands, stat_timeouts, stat_draw_failures, stat_protocol_errors, stat_recoveries;
} PIXELICA_LcdTransport;

/* Allocates staging and enters IDLE. On failure `t` is left UNINIT. */
PIXELICA_LcdStatus pixelica_lcd_transport_init(PIXELICA_LcdTransport *t, const PIXELICA_LcdTransportConfig *config);

/*
 * Copies the frame band by band into staging and waits for each band's
 * completion before touching staging again. Returns only when no transfer is
 * outstanding (PIXELICA_LCD_OK) or the transport is POISONED. `width`/`height`
 * must equal the configured size; `stride_pixels` >= width.
 */
PIXELICA_LcdStatus pixelica_lcd_transport_present(PIXELICA_LcdTransport *t, const uint16_t *pixels,
                                        uint16_t width, uint16_t height, uint32_t stride_pixels);

/* Leaves POISONED if the outstanding draw is proven finished (see header comment). */
PIXELICA_LcdStatus pixelica_lcd_transport_recover(PIXELICA_LcdTransport *t);

/* Releases staging only when no DMA can be reading it; else PIXELICA_LCD_ERR_BUSY and nothing is freed. */
PIXELICA_LcdStatus pixelica_lcd_transport_deinit(PIXELICA_LcdTransport *t);

PIXELICA_LcdState pixelica_lcd_transport_state(const PIXELICA_LcdTransport *t);

/* Call exactly once per completed draw from the "color transfer done" event
 * (ISR-safe: one increment, no calls). */
PIXELICA_LCD_INLINE void pixelica_lcd_transport_isr_done(PIXELICA_LcdTransport *t)
{
    t->completed += 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_ESP32P4_LCD_TRANSPORT_H */

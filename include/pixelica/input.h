/*
 * Pixelica - logical input buttons, plus bounded input recording/replay.
 *
 * Games only ever see these logical bits. Each platform layer maps its own
 * physical keys/GPIOs onto them (SDL3: arrows/WASD -> directions,
 * Space -> A, Enter -> START, Escape/window close -> QUIT).
 */
#ifndef PIXELICA_INPUT_H
#define PIXELICA_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_BTN_LEFT  ((uint32_t)1u << 0)
#define PIXELICA_BTN_RIGHT ((uint32_t)1u << 1)
#define PIXELICA_BTN_UP    ((uint32_t)1u << 2)
#define PIXELICA_BTN_DOWN  ((uint32_t)1u << 3)
#define PIXELICA_BTN_A     ((uint32_t)1u << 4)
#define PIXELICA_BTN_START ((uint32_t)1u << 5)
/* Request to exit the application (Escape key, window close, ...). */
#define PIXELICA_BTN_QUIT  ((uint32_t)1u << 6)
/* Every defined button bit. */
#define PIXELICA_BTN_ALL   ((uint32_t)0x7Fu)

/* Edge helpers for a game that keeps the previous tick's mask. */
static inline uint32_t pixelica_buttons_pressed(uint32_t previous, uint32_t current)
{
    return current & ~previous;
}

static inline uint32_t pixelica_buttons_released(uint32_t previous, uint32_t current)
{
    return previous & ~current;
}

/*
 * Input recording and replay
 * --------------------------
 * Simulation is driven at a fixed tick rate (pixelica_update, 60 Hz), so game
 * logic is reproducible from the button mask seen by every tick,
 * independent of how many frames were rendered. A recording is a
 * run-length list of spans (mask, number of consecutive ticks) in caller
 * storage; nothing allocates.
 *
 * Canonical form (produced by the recorder, required by encode/decode):
 * every span has ticks >= 1, only PIXELICA_BTN_ALL bits, and differs in mask
 * from the previous span unless the previous span is full
 * (ticks == UINT32_MAX).
 */
#define PIXELICA_INPUT_TICK_HZ 60u

typedef struct PIXELICA_InputSpan {
    uint32_t buttons;
    uint32_t ticks;
} PIXELICA_InputSpan;

typedef struct PIXELICA_InputRecorder {
    PIXELICA_InputSpan *spans; /* caller storage */
    size_t capacity;
    size_t count;
    uint64_t total_ticks;
} PIXELICA_InputRecorder;

/* Empty recorder over `storage` (capacity 0 when storage is NULL). */
void pixelica_input_recorder_init(PIXELICA_InputRecorder *rec, PIXELICA_InputSpan *storage,
                             size_t capacity);

/* Appends one tick. Bits outside PIXELICA_BTN_ALL are dropped. PIXELICA_ERR_CAPACITY
 * (recording unchanged) when a new span is needed and storage is full;
 * PIXELICA_ERR_INVALID_ARG for NULL. */
PIXELICA_Status pixelica_input_record(PIXELICA_InputRecorder *rec, uint32_t buttons);

typedef struct PIXELICA_InputPlayer {
    const PIXELICA_InputSpan *spans;
    size_t count;
    size_t span;      /* current span index */
    uint32_t used;    /* ticks already played from spans[span] */
    uint64_t tick;    /* ticks played so far */
} PIXELICA_InputPlayer;

void pixelica_input_player_init(PIXELICA_InputPlayer *player, const PIXELICA_InputSpan *spans,
                           size_t count);

/* Mask for the next tick; false (and *out_buttons = 0) when finished.
 * Zero-tick spans are skipped. `out_buttons` may be NULL. */
bool pixelica_input_player_next(PIXELICA_InputPlayer *player, uint32_t *out_buttons);

/* Same signature as PIXELICA_UpdateFn (pixelica.h). */
typedef void (*PIXELICA_InputTickFn)(void *userdata, float dt, uint32_t buttons);

/* Calls tick(userdata, 1/60 s, mask) once per recorded tick, in order,
 * exactly like pixelica_update() steps do. Returns the number of ticks run
 * (0 for NULL arguments). */
uint64_t pixelica_input_replay(const PIXELICA_InputSpan *spans, size_t count,
                          PIXELICA_InputTickFn tick, void *userdata);

/*
 * Portable little-endian serialization ("P3IN" v1):
 *   magic "P3IN", version:u16 = 1, reserved:u16 = 0, span_count:u32,
 *   crc32:u32 (of the span bytes), then span_count * (buttons:u32,
 *   ticks:u32). Spans must be canonical. The size is exactly
 *   16 + 8 * span_count.
 */
#define PIXELICA_INPUT_HEADER_BYTES 16u
#define PIXELICA_INPUT_SPAN_BYTES 8u

/* Bytes needed to encode `span_count` spans (PIXELICA_ERR_CAPACITY if the
 * count does not fit the format or size_t). */
PIXELICA_Status pixelica_input_encoded_size(size_t span_count, size_t *out_bytes);

/* PIXELICA_ERR_INVALID_ARG for NULL or non-canonical spans, PIXELICA_ERR_NO_MEMORY
 * if out_size is too small. Nothing is written on failure
 * (*out_written = 0). */
PIXELICA_Status pixelica_input_encode(const PIXELICA_InputSpan *spans, size_t count,
                            void *out, size_t out_size, size_t *out_written);

/* Validates an encoding (PIXELICA_ERR_BAD_ASSET if malformed) and reports its
 * span count. */
PIXELICA_Status pixelica_input_decode_count(const void *data, size_t size,
                                  size_t *out_count);

/* Validates, then decodes into `out` (PIXELICA_ERR_NO_MEMORY if capacity is
 * smaller than the span count). On failure nothing is written to `out`
 * and *out_count = 0. Input may have any alignment. */
PIXELICA_Status pixelica_input_decode(const void *data, size_t size, PIXELICA_InputSpan *out,
                            size_t capacity, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_INPUT_H */

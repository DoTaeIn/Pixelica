/*
 * Pixelica - minimal debug utilities: frame timing and bounded text formatting.
 *
 * Renderer-, platform- and UI-independent. The caller owns every object and
 * measures time itself: pass each frame's duration in microseconds. Nothing
 * here reads a clock, allocates, or keeps global state. Single-threaded.
 * "Debug off" simply means the caller does not call these functions.
 *
 * On any failure every output (timing state, buffer, out_len, out_truncated)
 * is left unchanged. Errors:
 *   PIXELICA_ERR_INVALID_ARG  NULL/uninitialized object, out-of-range value,
 *                             bad name, zero capacity, overlapping memory
 */
#ifndef PIXELICA_DEBUG_H
#define PIXELICA_DEBUG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Averaging window used when init gets window_us == 0 (0.5 s). */
#define PIXELICA_DEBUG_DEFAULT_WINDOW_US 500000u
/* Largest accepted window and largest accepted single frame duration. */
#define PIXELICA_DEBUG_MAX_WINDOW_US 60000000u
#define PIXELICA_DEBUG_MAX_FRAME_US 10000000u
/* A window is also published after this many frames, whatever their length. */
#define PIXELICA_DEBUG_MAX_WINDOW_FRAMES 65535u
/* Metric name: 1..24 printable ASCII bytes (0x20..0x7E), no terminator needed. */
#define PIXELICA_DEBUG_NAME_MAX 24u
/* Buffer size that always holds either formatted line without truncation. */
#define PIXELICA_DEBUG_TEXT_BYTES 96u

/*
 * Caller-owned accumulator. Initialize with pixelica_debug_timing_init and
 * then only read the fields marked "read-only"; the rest is internal.
 * Zero-filled memory is "uninitialized" and rejected by every function.
 */
typedef struct PIXELICA_DebugTiming {
    /* read-only: last completed window; meaningful only if has_report */
    bool has_report;
    uint32_t fps_milli;     /* frames per second * 1000, saturated */
    uint32_t avg_frame_us;  /* mean frame duration of the window */
    uint32_t max_frame_us;  /* longest frame of the window */
    /* read-only: most recent frame passed to update */
    uint32_t last_frame_us;
    /* internal */
    uint32_t window_us, accum_us, frames, window_max_us, magic;
} PIXELICA_DebugTiming;

/*
 * window_us: averaging window, 1..PIXELICA_DEBUG_MAX_WINDOW_US, or 0 for
 * PIXELICA_DEBUG_DEFAULT_WINDOW_US. Resets any previous state.
 */
PIXELICA_Status pixelica_debug_timing_init(PIXELICA_DebugTiming *out, uint32_t window_us);

/*
 * Adds one displayed frame of frame_us microseconds (0..MAX_FRAME_US; 0 is
 * accepted for coarse clocks). When the window's total reaches window_us (or
 * MAX_WINDOW_FRAMES frames) the report is replaced and a new window starts.
 * A window whose total is 0 us publishes nothing, so FPS is never invented.
 * Longer frames (pauses, breakpoints) are rejected: clamp or skip them.
 */
PIXELICA_Status pixelica_debug_timing_update(PIXELICA_DebugTiming *timing, uint32_t frame_us);

/*
 * Formats "FPS 60.0 | 16.7 ms | max 20.1 ms" ("FPS -- | -- ms" before the
 * first report) as NUL-terminated ASCII. capacity includes the NUL and must
 * be >= 1. Longer text is cut at the buffer end: *out_truncated (may be NULL)
 * becomes true, status stays OK. *out_len (may be NULL) = bytes before NUL.
 */
PIXELICA_Status pixelica_debug_format_timing(const PIXELICA_DebugTiming *timing,
    char *out, size_t capacity, size_t *out_len, bool *out_truncated);

/*
 * Formats "<name>: <value>" with the same buffer/truncation rules.
 * name_bytes is 1..PIXELICA_DEBUG_NAME_MAX of printable ASCII.
 */
PIXELICA_Status pixelica_debug_format_metric(const char *name, size_t name_bytes,
    int32_t value, char *out, size_t capacity, size_t *out_len, bool *out_truncated);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_DEBUG_H */

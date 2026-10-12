/* Pixelica UI: always included in both SDKs, caller-owned and renderer-free. */
#ifndef PIXELICA_UI_H
#define PIXELICA_UI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pixelica/debug.h"
#include "pixelica/platform.h"
#include "pixelica/status.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_UI_MAX_WIDGETS 128u
#define PIXELICA_UI_MAX_TEXT_BYTES 4096u
#define PIXELICA_UI_MAX_DIMENSION 4096u
#define PIXELICA_UI_PREVIOUS ((uint32_t)1u << 0)
#define PIXELICA_UI_NEXT     ((uint32_t)1u << 1)
#define PIXELICA_UI_LEFT     ((uint32_t)1u << 2)
#define PIXELICA_UI_RIGHT    ((uint32_t)1u << 3)
#define PIXELICA_UI_ACCEPT   ((uint32_t)1u << 4)
#define PIXELICA_UI_CANCEL   ((uint32_t)1u << 5)
#define PIXELICA_UI_BUTTONS  ((uint32_t)0x3fu)

typedef struct PIXELICA_UI_Rect { int32_t x, y, width, height; } PIXELICA_UI_Rect;
typedef struct PIXELICA_UI_Surface {
    uint16_t *pixels;
    uint16_t width, height;
    uint32_t stride;
} PIXELICA_UI_Surface;

/* Fixed-cell Unicode bitmap callback. rows[0..height) use bit31 for the left
 * pixel. width=1..16, height=1..32, scale=1..4. False draws a missing-glyph box.
 * Do not modify UI state/buffers or retain rows. userdata is caller-owned. */
typedef bool (*PIXELICA_UI_GlyphFn)(void *userdata, uint32_t codepoint, uint32_t rows[32]);
typedef struct PIXELICA_UI_Font {
    uint8_t width, height, scale;
    PIXELICA_UI_GlyphFn glyph;
    void *userdata;
} PIXELICA_UI_Font;
typedef struct PIXELICA_UI_Theme {
    uint16_t panel, text, disabled_text, control, hover, active, accent, border;
    uint8_t padding;
    PIXELICA_UI_Font font;
} PIXELICA_UI_Theme;

typedef enum PIXELICA_UI_Kind {
    PIXELICA_UI_PANEL, PIXELICA_UI_LABEL, PIXELICA_UI_BUTTON,
    PIXELICA_UI_CHECKBOX, PIXELICA_UI_SLIDER, PIXELICA_UI_PROGRESS
} PIXELICA_UI_Kind;
typedef struct PIXELICA_UI_Widget {
    PIXELICA_UI_Kind kind;
    uint32_t id; /* nonzero, unique within a context */
    PIXELICA_UI_Rect rect;
    const char *text; /* strict UTF-8, borrowed, no terminator required */
    size_t text_bytes;
    bool visible, enabled;
    float value, min, max, step; /* SLIDER; PROGRESS uses value in [0,1] */
    bool checked; /* CHECKBOX */
} PIXELICA_UI_Widget;
typedef struct PIXELICA_UI_Input {
    int32_t pointer_x, pointer_y; /* framebuffer pixels; mouse/touch/custom */
    bool pointer_valid, pointer_down;
    uint32_t buttons; /* UI actions above; navigation/accept use rising edges */
} PIXELICA_UI_Input;
typedef struct PIXELICA_UI_Event {
    bool hovered, focused, pressed, clicked, changed;
} PIXELICA_UI_Event;

/* Initialize before use. Widgets, text and font userdata stay borrowed.
 * Widgets may be edited between calls; update/render validate the full list.
 * Events last until the next successful update/set_widgets. Single-threaded.
 * Edit theme and widget fields only; wants_pointer/buttons are read-only
 * input-consumption hints. Other context fields are internal storage. */
typedef struct PIXELICA_UI_Context {
    PIXELICA_UI_Widget *widgets;
    size_t count;
    PIXELICA_UI_Theme theme;
    PIXELICA_UI_Input previous;
    PIXELICA_UI_Event events[PIXELICA_UI_MAX_WIDGETS];
    uint32_t focus_id, active_id, magic;
    bool wants_pointer, wants_buttons;
} PIXELICA_UI_Context;
typedef struct PIXELICA_UI_Layout {
    PIXELICA_UI_Rect bounds;
    int32_t gap, cursor;
    bool horizontal;
} PIXELICA_UI_Layout;

PIXELICA_UI_Font pixelica_ui_font_default(void);
PIXELICA_UI_Theme pixelica_ui_theme_default(void);
PIXELICA_Status pixelica_ui_widget_default(PIXELICA_UI_Kind kind, uint32_t id,
    PIXELICA_UI_Rect rect, const char *text, size_t text_bytes, PIXELICA_UI_Widget *out);
PIXELICA_Status pixelica_ui_init(PIXELICA_UI_Context *out,
    PIXELICA_UI_Widget *widgets, size_t count);
/* Replaces menu, resets events/capture/focus, preserves previous input so
 * opening a menu while ACCEPT is held cannot activate the new menu. */
PIXELICA_Status pixelica_ui_set_widgets(PIXELICA_UI_Context *ui,
    PIXELICA_UI_Widget *widgets, size_t count);
/* Once per displayed frame. Invalid input preserves values/events/history.
 * clicked: button/checkbox only; changed: checkbox/slider values only.
 * Capture is cancelled by CANCEL, pointer loss or an ineligible widget. */
PIXELICA_Status pixelica_ui_update(PIXELICA_UI_Context *ui, const PIXELICA_UI_Input *input);
PIXELICA_UI_Event pixelica_ui_event(const PIXELICA_UI_Context *ui, uint32_t id);
/* UP/DOWN -> previous/next, LEFT/RIGHT, A|START -> accept; QUIT is not cancel.
 * Pointer starts invalid; assign framebuffer coordinates for mouse/touch. */
PIXELICA_UI_Input pixelica_ui_input_buttons(uint32_t logical_buttons);
PIXELICA_Status pixelica_ui_layout_init(PIXELICA_UI_Layout *out,
    PIXELICA_UI_Rect bounds, int32_t gap, bool horizontal);
PIXELICA_Status pixelica_ui_layout_next(PIXELICA_UI_Layout *layout,
    int32_t extent, PIXELICA_UI_Rect *out);

/* Draw in list order onto RGB565, clipped to the surface. Row padding and any
 * renderer depth/stats stay untouched. No heap, renderer, SDL or GPU needed.
 * Invalid input returns INVALID_ARG, outputs unchanged. Callback userdata and
 * actual backing allocation sizes are the caller's responsibility. */
PIXELICA_Status pixelica_ui_render_rgb565(const PIXELICA_UI_Context *ui,
    PIXELICA_UI_Surface *surface);

/* Debug HUD: draws pixelica/debug.h timing (and one optional caller metric) as
 * a small panel through the normal UI renderer. Caller-owned and renderer-free;
 * the struct holds its own one-widget context so no UI context is built per
 * frame. Keep it at a stable address (it points into itself; do not copy it).
 * Fields are internal except `ui.theme`, which may be edited (font/colors/
 * padding) before the next update. "Debug off" = do not call these. */
typedef struct PIXELICA_UI_DebugHud {
    PIXELICA_UI_Context ui;
    PIXELICA_UI_Widget panel;
    char text[2u * PIXELICA_DEBUG_TEXT_BYTES + 1u];
    int32_t x, y;
    uint32_t magic;
} PIXELICA_UI_DebugHud;

/* Panel's top-left corner at (x, y) in surface pixels (may be off-surface;
 * drawing is clipped). Starts empty: nothing is drawn until the first update. */
PIXELICA_Status pixelica_ui_debug_hud_init(PIXELICA_UI_DebugHud *out, int32_t x, int32_t y);
/* Reformats the text and resizes the panel to fit. metric_name NULL with
 * metric_name_bytes 0 omits the metric line; otherwise see
 * pixelica_debug_format_metric. Any failure (INVALID_ARG) keeps the previous
 * text. Text cut by the format buffers is drawn as truncated, never an error. */
PIXELICA_Status pixelica_ui_debug_hud_update(PIXELICA_UI_DebugHud *hud,
    const PIXELICA_DebugTiming *timing, const char *metric_name,
    size_t metric_name_bytes, int32_t metric_value);
/* Draws the panel clipped to the surface via pixelica_ui_render_rgb565: row
 * padding and any renderer depth stay untouched. INVALID_ARG leaves pixels
 * unchanged. */
PIXELICA_Status pixelica_ui_debug_hud_render_rgb565(const PIXELICA_UI_DebugHud *hud,
    PIXELICA_UI_Surface *surface);

/* For 3D/hybrid: wrap the PAL before creating the engine, then update UI before
 * end_frame. Copies the source to output before drawing, never casts away const.
 * Presenter, UI and output stay alive and at stable addresses until destroy.
 * Invalid present returns false without output mutation/upstream call.
 * Upstream failure is propagated. Time/buttons/log forward original userdata. */
typedef struct PIXELICA_UI_Presenter {
    PIXELICA_PlatformOps upstream, ops;
    const PIXELICA_UI_Context *ui;
    uint16_t *output;
    size_t output_capacity;
} PIXELICA_UI_Presenter;
PIXELICA_Status pixelica_ui_presenter_init(PIXELICA_UI_Presenter *out,
    const PIXELICA_PlatformOps *upstream, const PIXELICA_UI_Context *ui,
    uint16_t *output, size_t output_capacity);

#ifdef __cplusplus
}
#endif
#endif

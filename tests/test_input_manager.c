/*
 * Pixelica - optional input manager: NULL safety, multi-device merging,
 * shared actions, axis gates/deadzones/clamping, held/pressed/released edges,
 * unplug/focus-loss neutral snapshots, live rebinding, capacity limits,
 * transactional validation, aliasing, and the legacy seven-button bridge
 * (PAL read_buttons and "P3IN" v1 record/encode/decode/replay).
 *
 * Uses only the common headers plus pixelica/input_manager.h, so it builds and
 * runs with every renderer disabled.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/input.h"
#include "pixelica/input_manager.h"
#include "pixelica/platform.h"
#include "pixelica/status.h"

#include "test_util.h"

/* ---- helpers ------------------------------------------------------------ */

static PIXELICA_InputBinding mk_binding(uint8_t device, PIXELICA_InputControl kind,
                                   uint8_t control, uint8_t action,
                                   float scale, float deadzone)
{
    PIXELICA_InputBinding b;

    memset(&b, 0, sizeof b);
    b.device = device;
    b.control = control;
    b.action = action;
    b.kind = kind;
    b.scale = scale;
    b.deadzone = deadzone;
    return b;
}

static PIXELICA_InputDeviceState mk_state(bool connected, uint64_t buttons)
{
    PIXELICA_InputDeviceState s;
    size_t i;

    memset(&s, 0, sizeof s);
    s.connected = connected;
    s.buttons = buttons;
    for (i = 0; i < PIXELICA_INPUT_MAX_AXES; ++i) {
        s.axes[i] = 0.0f;
    }
    return s;
}

static PIXELICA_Status submit_buttons(PIXELICA_InputManager *m, uint8_t device,
                                 uint64_t buttons)
{
    PIXELICA_InputDeviceState s = mk_state(true, buttons);

    return pixelica_input_manager_submit(m, device, &s);
}

static PIXELICA_Status submit_axis(PIXELICA_InputManager *m, uint8_t device,
                              uint8_t axis, float value)
{
    PIXELICA_InputDeviceState s = mk_state(true, 0u);

    s.axes[axis] = value;
    return pixelica_input_manager_submit(m, device, &s);
}

static PIXELICA_Status submit_unplugged(PIXELICA_InputManager *m, uint8_t device)
{
    PIXELICA_InputDeviceState s = mk_state(false, ~(uint64_t)0u);

    /* Disconnected payloads are ignored, even when garbage. */
    s.axes[0] = pixelica_t_nan();
    s.axes[1] = pixelica_t_inf();
    s.axes[2] = -pixelica_t_inf();
    s.axes[7] = 5.0f;
    return pixelica_input_manager_submit(m, device, &s);
}

/* 1 when the action matches exactly (value within 1e-6). Used inside CHECK so
 * failures report the caller's line. */
static int action_is(const PIXELICA_InputManager *m, uint8_t action, float value,
                     bool held, bool pressed, bool released)
{
    PIXELICA_InputActionState s = pixelica_input_manager_action(m, action);

    return fabs((double)s.value - (double)value) <= 1e-6 && s.held == held &&
           s.pressed == pressed && s.released == released;
}

static int is_neutral(PIXELICA_InputActionState s)
{
    return fabs((double)s.value) <= 0.0 && !s.held && !s.pressed &&
           !s.released;
}

static int device_is_neutral(const PIXELICA_InputDeviceState *d)
{
    size_t i;

    if (d->connected || d->buttons != 0u) {
        return 0;
    }
    for (i = 0; i < PIXELICA_INPUT_MAX_AXES; ++i) {
        if (!(fabs((double)d->axes[i]) <= 0.0)) {
            return 0;
        }
    }
    return 1;
}

static PIXELICA_Status set1(PIXELICA_InputManager *m, PIXELICA_InputBinding b)
{
    return pixelica_input_manager_set_bindings(m, &b, 1u);
}

/* ---- tests -------------------------------------------------------------- */

static void test_null_and_init(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputDeviceState s = mk_state(true, 1u);
    PIXELICA_InputBinding b = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A,
                                    1.0f, 0.0f);
    size_t i;

    pixelica_input_manager_init(NULL);
    pixelica_input_manager_tick(NULL);
    CHECK(pixelica_input_manager_set_bindings(NULL, &b, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_manager_set_bindings(NULL, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_manager_submit(NULL, 0, &s) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_manager_submit(NULL, 0, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(is_neutral(pixelica_input_manager_action(NULL, 0)));
    CHECK(is_neutral(pixelica_input_manager_action(NULL, 255)));
    CHECK(pixelica_input_manager_buttons(NULL) == 0u);
    CHECK(pixelica_input_manager_read_buttons(NULL) == 0u);

    /* init fully resets garbage storage. */
    memset(&m, 0xA5, sizeof m);
    pixelica_input_manager_init(&m);
    CHECK(m.binding_count == 0u);
    for (i = 0; i < PIXELICA_INPUT_MAX_DEVICES; ++i) {
        CHECK(device_is_neutral(&m.devices[i]));
    }
    for (i = 0; i < PIXELICA_INPUT_MAX_ACTIONS; ++i) {
        CHECK(is_neutral(m.actions[i]));
    }
    CHECK(pixelica_input_manager_buttons(&m) == 0u);

    /* Bad submit arguments change nothing. */
    CHECK(pixelica_input_manager_submit(&m, 0, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_manager_submit(&m, 4, &s) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_manager_submit(&m, 255, &s) == PIXELICA_ERR_INVALID_ARG);
    CHECK(device_is_neutral(&m.devices[0]));

    /* Out-of-range queries are neutral even while real actions are live. */
    CHECK(set1(&m, b) == PIXELICA_OK);
    CHECK(pixelica_input_manager_submit(&m, 0, &s) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, true, false));
    CHECK(is_neutral(pixelica_input_manager_action(&m, 32)));
    CHECK(is_neutral(pixelica_input_manager_action(&m, 255)));

    /* Re-init clears bindings, devices and edges. */
    pixelica_input_manager_init(&m);
    CHECK(m.binding_count == 0u);
    CHECK(device_is_neutral(&m.devices[0]));
    CHECK(is_neutral(pixelica_input_manager_action(&m, PIXELICA_INPUT_A)));
    pixelica_input_manager_tick(&m);
    CHECK(is_neutral(pixelica_input_manager_action(&m, PIXELICA_INPUT_A)));
}

static void test_multi_device_merge(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[9];
    PIXELICA_InputDeviceState custom;

    /* Keyboard (0), gamepad (1) and a custom GPIO board (3) share actions. */
    b[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_LEFT, 1.0f, 0.0f);
    b[1] = mk_binding(0, PIXELICA_INPUT_BUTTON, 1, PIXELICA_INPUT_RIGHT, 1.0f, 0.0f);
    b[2] = mk_binding(0, PIXELICA_INPUT_BUTTON, 2, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[3] = mk_binding(1, PIXELICA_INPUT_AXIS_NEGATIVE, 0, PIXELICA_INPUT_LEFT, 1.0f, 0.2f);
    b[4] = mk_binding(1, PIXELICA_INPUT_AXIS_POSITIVE, 0, PIXELICA_INPUT_RIGHT, 1.0f, 0.2f);
    b[5] = mk_binding(1, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[6] = mk_binding(3, PIXELICA_INPUT_BUTTON, 63, 31, 1.0f, 0.0f);
    b[7] = mk_binding(3, PIXELICA_INPUT_AXIS, 7, 30, 1.0f, 0.0f);
    b[8] = mk_binding(3, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_START, 1.0f, 0.0f);

    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 9u) == PIXELICA_OK);
    CHECK(m.binding_count == 9u);

    /* Keyboard left only. */
    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_LEFT, 1.0f, true, true, false));
    CHECK(action_is(&m, PIXELICA_INPUT_RIGHT, 0.0f, false, false, false));
    CHECK(pixelica_input_manager_buttons(&m) == PIXELICA_BTN_LEFT);

    /* Hand over from keyboard to stick: still held, no edges. */
    CHECK(submit_buttons(&m, 0, 0u) == PIXELICA_OK);
    CHECK(submit_axis(&m, 1, 0, -1.0f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_LEFT, 1.0f, true, false, false));

    /* Both sources together clamp to 1. */
    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_LEFT, 1.0f, true, false, false));

    /* Stick right while keyboard left: separate actions, both held. */
    CHECK(submit_axis(&m, 1, 0, 0.6f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_LEFT, 1.0f, true, false, false));
    CHECK(action_is(&m, PIXELICA_INPUT_RIGHT, 0.5f, true, true, false));
    CHECK(pixelica_input_manager_buttons(&m) == (PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT));

    /* Custom device: button 63, axis 7, and a shared legacy action. */
    custom = mk_state(true, ((uint64_t)1u << 63) | 1u);
    custom.axes[7] = -0.75f;
    CHECK(pixelica_input_manager_submit(&m, 3, &custom) == PIXELICA_OK);
    CHECK(submit_buttons(&m, 0, 0u) == PIXELICA_OK);
    CHECK(submit_axis(&m, 1, 0, 0.0f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 31, 1.0f, true, true, false));
    CHECK(action_is(&m, 30, -0.75f, true, true, false));
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, true, false));
    CHECK(action_is(&m, PIXELICA_INPUT_LEFT, 0.0f, false, false, true));
    CHECK(action_is(&m, PIXELICA_INPUT_RIGHT, 0.0f, false, false, true));
    /* User actions 7..31 never leak into the legacy mask. */
    CHECK(pixelica_input_manager_buttons(&m) == PIXELICA_BTN_START);

    /* An unbound device does not affect anything. */
    CHECK(submit_buttons(&m, 2, ~(uint64_t)0u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(pixelica_input_manager_buttons(&m) == PIXELICA_BTN_START);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 0.0f, false, false, false));
}

static void test_shared_action_release(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[3];

    b[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 4, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[1] = mk_binding(0, PIXELICA_INPUT_BUTTON, 5, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[2] = mk_binding(2, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A, 1.0f, 0.0f);
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 3u) == PIXELICA_OK);

    CHECK(submit_buttons(&m, 0, 1u << 4) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, true, false));

    CHECK(submit_buttons(&m, 0, (1u << 4) | (1u << 5)) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, false, false));

    CHECK(submit_buttons(&m, 2, 1u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, false, false));

    /* Releasing all but the last never releases the action. */
    CHECK(submit_buttons(&m, 0, 1u << 5) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, false, false));
    CHECK(submit_buttons(&m, 0, 0u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, false, false));

    CHECK(submit_buttons(&m, 2, 0u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 0.0f, false, false, true));
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 0.0f, false, false, false));
}

static void test_axis_shaping(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[8];

    /* Same physical axis feeds several actions with different shaping. */
    b[0] = mk_binding(0, PIXELICA_INPUT_AXIS, 0, 7, 1.0f, 0.5f);
    b[1] = mk_binding(0, PIXELICA_INPUT_AXIS_POSITIVE, 0, 8, 1.0f, 0.5f);
    b[2] = mk_binding(0, PIXELICA_INPUT_AXIS_NEGATIVE, 0, 9, 1.0f, 0.5f);
    b[3] = mk_binding(0, PIXELICA_INPUT_AXIS, 0, 10, -1.0f, 0.0f);
    b[4] = mk_binding(0, PIXELICA_INPUT_AXIS, 0, 11, 0.5f, 0.0f);
    b[5] = mk_binding(0, PIXELICA_INPUT_AXIS, 1, 12, 1.0f, 0.0f);
    b[6] = mk_binding(0, PIXELICA_INPUT_AXIS, 2, 13, 1.0f, 0.2f);
    /* Negative gate with negative scale: a signed half-axis. */
    b[7] = mk_binding(0, PIXELICA_INPUT_AXIS_NEGATIVE, 0, 14, -1.0f, 0.0f);
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 8u) == PIXELICA_OK);

    CHECK(submit_axis(&m, 0, 0, 0.75f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 0.5f, true, true, false));  /* (0.75-0.5)/0.5 */
    CHECK(action_is(&m, 8, 0.5f, true, true, false));
    CHECK(action_is(&m, 9, 0.0f, false, false, false)); /* gated out */
    CHECK(action_is(&m, 10, -0.75f, true, true, false));
    CHECK(action_is(&m, 11, 0.375f, false, false, false));
    CHECK(action_is(&m, 14, 0.0f, false, false, false));

    CHECK(submit_axis(&m, 0, 0, -0.75f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, -0.5f, true, false, false)); /* fabs(value) held */
    CHECK(action_is(&m, 8, 0.0f, false, false, true));
    CHECK(action_is(&m, 9, 0.5f, true, true, false));   /* magnitude, positive */
    CHECK(action_is(&m, 10, 0.75f, true, false, false));
    CHECK(action_is(&m, 11, -0.375f, false, false, false));
    CHECK(action_is(&m, 14, -0.75f, true, true, false));

    /* Exactly at the deadzone is zero; full deflection reaches 1. */
    CHECK(submit_axis(&m, 0, 0, 0.5f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 0.0f, false, false, true));
    CHECK(action_is(&m, 9, 0.0f, false, false, true));
    CHECK(action_is(&m, 10, -0.5f, true, false, false));

    CHECK(submit_axis(&m, 0, 0, 1.0f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 1.0f, true, true, false));
    CHECK(action_is(&m, 8, 1.0f, true, true, false));
    CHECK(action_is(&m, 10, -1.0f, true, false, false));
    CHECK(action_is(&m, 11, 0.5f, true, true, false));

    CHECK(submit_axis(&m, 0, 0, -1.0f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, -1.0f, true, false, false));
    CHECK(action_is(&m, 9, 1.0f, true, true, false));
    CHECK(action_is(&m, 11, -0.5f, true, false, false));

    CHECK(submit_axis(&m, 0, 0, 0.25f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 0.0f, false, false, true));
    CHECK(action_is(&m, 8, 0.0f, false, false, false));

    /* Held threshold is inclusive at 0.5. */
    CHECK(submit_axis(&m, 0, 1, 0.49f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 12, 0.49f, false, false, false));
    CHECK(submit_axis(&m, 0, 1, 0.5f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 12, 0.5f, true, true, false));
    CHECK(submit_axis(&m, 0, 1, -0.5f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 12, -0.5f, true, false, false));
    CHECK(submit_axis(&m, 0, 1, -0.4999f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 12, -0.4999f, false, false, true));

    /* Deadzone rescale: (0.6-0.2)/0.8. */
    CHECK(submit_axis(&m, 0, 2, 0.6f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK_NEAR(pixelica_input_manager_action(&m, 13).value, 0.5, 1e-6);
    CHECK(submit_axis(&m, 0, 2, -0.1f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK_NEAR(pixelica_input_manager_action(&m, 13).value, 0.0, 0.0);
}

static void test_sum_clamp_opposing(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[6];
    PIXELICA_InputDeviceState s;

    /* Keyboard left/right and a stick drive one signed "move_x" action. */
    b[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, 7, -1.0f, 0.0f);
    b[1] = mk_binding(0, PIXELICA_INPUT_BUTTON, 1, 7, 1.0f, 0.0f);
    b[2] = mk_binding(1, PIXELICA_INPUT_AXIS, 0, 7, 1.0f, 0.0f);
    /* Positive and negative halves recombine into the signed axis. */
    b[3] = mk_binding(1, PIXELICA_INPUT_AXIS_POSITIVE, 1, 8, 1.0f, 0.0f);
    b[4] = mk_binding(1, PIXELICA_INPUT_AXIS_NEGATIVE, 1, 8, -1.0f, 0.0f);
    /* Deadzone is ignored for buttons. */
    b[5] = mk_binding(0, PIXELICA_INPUT_BUTTON, 2, 9, 0.5f, 0.9f);
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 6u) == PIXELICA_OK);

    CHECK(submit_buttons(&m, 0, 3u) == PIXELICA_OK); /* both directions */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 0.0f, false, false, false));

    CHECK(submit_buttons(&m, 0, 2u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 1.0f, true, true, false));

    CHECK(submit_axis(&m, 1, 0, 1.0f) == PIXELICA_OK); /* 1 + 1 clamps */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 1.0f, true, false, false));

    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);  /* -1 + 1 = 0 */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 0.0f, false, false, true));

    CHECK(submit_axis(&m, 1, 0, 0.25f) == PIXELICA_OK); /* -1 + 0.25 */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, -0.75f, true, true, false));

    s = mk_state(true, 0u);
    s.axes[0] = -1.0f;
    CHECK(pixelica_input_manager_submit(&m, 1, &s) == PIXELICA_OK); /* -1 - 1 clamps */
    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, -1.0f, true, false, false));

    s = mk_state(true, 0u);
    s.axes[1] = -0.75f;
    CHECK(pixelica_input_manager_submit(&m, 1, &s) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 8, -0.75f, true, true, false));
    s.axes[1] = 0.625f;
    CHECK(pixelica_input_manager_submit(&m, 1, &s) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 8, 0.625f, true, false, false));

    CHECK(submit_buttons(&m, 0, 4u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 9, 0.5f, true, true, false));
}

static void test_repeated_edges(void)
{
    PIXELICA_InputManager m;

    pixelica_input_manager_init(&m);
    CHECK(set1(&m, mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_START,
                              1.0f, 0.0f)) == PIXELICA_OK);

    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 0.0f, false, false, false));

    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    /* Queries never consume edges. */
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, true, false));
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, true, false));
    CHECK(pixelica_input_manager_buttons(&m) == PIXELICA_BTN_START);

    /* Snapshot persists without new submits; the edge lasts one tick. */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, false, false));
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, false, false));

    CHECK(submit_buttons(&m, 0, 0u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 0.0f, false, false, true));
    CHECK(action_is(&m, PIXELICA_INPUT_START, 0.0f, false, false, true));
    CHECK(pixelica_input_manager_buttons(&m) == 0u);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 0.0f, false, false, false));

    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, true, false));

    /* Latest snapshot wins: a tap entirely between ticks is not queued. */
    CHECK(submit_buttons(&m, 0, 0u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    CHECK(submit_buttons(&m, 0, 0u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 0.0f, false, false, false));
}

static void test_unplug_and_focus_loss(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[2];
    int t;

    b[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[1] = mk_binding(1, PIXELICA_INPUT_AXIS_POSITIVE, 0, PIXELICA_INPUT_RIGHT,
                      1.0f, 0.1f);
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 2u) == PIXELICA_OK);

    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    CHECK(submit_axis(&m, 1, 0, 1.0f) == PIXELICA_OK);
    for (t = 0; t < 3; ++t) {
        pixelica_input_manager_tick(&m);
        CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, t == 0, false));
        CHECK(action_is(&m, PIXELICA_INPUT_RIGHT, 1.0f, true, t == 0, false));
    }

    /* Focus loss: every source reports disconnected (garbage payload). */
    CHECK(submit_unplugged(&m, 0) == PIXELICA_OK);
    CHECK(submit_unplugged(&m, 1) == PIXELICA_OK);
    CHECK(device_is_neutral(&m.devices[0]));
    CHECK(device_is_neutral(&m.devices[1]));
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 0.0f, false, false, true));
    CHECK(action_is(&m, PIXELICA_INPUT_RIGHT, 0.0f, false, false, true));
    CHECK(pixelica_input_manager_buttons(&m) == 0u);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 0.0f, false, false, false));

    /* Focus regained. */
    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    CHECK(submit_axis(&m, 1, 0, 1.0f) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, true, false));
    CHECK(action_is(&m, PIXELICA_INPUT_RIGHT, 1.0f, true, true, false));

    /* Unplugging one device only affects its bindings. */
    CHECK(submit_unplugged(&m, 1) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, false, false));
    CHECK(action_is(&m, PIXELICA_INPUT_RIGHT, 0.0f, false, false, true));
    CHECK(pixelica_input_manager_buttons(&m) == PIXELICA_BTN_A);
}

static void test_rebinding_while_held(void)
{
    PIXELICA_InputManager m;

    pixelica_input_manager_init(&m);
    CHECK(set1(&m, mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A,
                              1.0f, 0.0f)) == PIXELICA_OK);
    CHECK(submit_buttons(&m, 0, 3u) == PIXELICA_OK); /* buttons 0 and 1 down */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, true, false));

    /* Rebinding leaves action states alone until the next tick... */
    CHECK(set1(&m, mk_binding(0, PIXELICA_INPUT_BUTTON, 1, PIXELICA_INPUT_A,
                              1.0f, 0.0f)) == PIXELICA_OK);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, true, false));
    /* ...then compares against the live held state: no spurious edges. */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, false, false));

    /* Rebinding to an idle control releases on the next tick. */
    CHECK(set1(&m, mk_binding(0, PIXELICA_INPUT_BUTTON, 2, PIXELICA_INPUT_A,
                              1.0f, 0.0f)) == PIXELICA_OK);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, false, false));
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 0.0f, false, false, true));

    /* Moving a held binding to another action releases/presses each. */
    CHECK(set1(&m, mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A,
                              1.0f, 0.0f)) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 1.0f, true, true, false));
    CHECK(set1(&m, mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_START,
                              1.0f, 0.0f)) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_A, 0.0f, false, false, true));
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, true, false));

    /* Clearing all bindings (NULL, 0) releases on the next tick. */
    CHECK(pixelica_input_manager_set_bindings(&m, NULL, 0u) == PIXELICA_OK);
    CHECK(m.binding_count == 0u);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, true, false));
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 0.0f, false, false, true));
}

static void test_capacity(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding many[PIXELICA_INPUT_MAX_BINDINGS + 1u];
    PIXELICA_InputBinding saved[PIXELICA_INPUT_MAX_BINDINGS];
    unsigned i;

    /* Every slot binds device 3 / button 63 / action 31 at 1/64 (exact),
     * so the value is 1 only when all 64 slots are processed. */
    for (i = 0; i < PIXELICA_INPUT_MAX_BINDINGS + 1u; ++i) {
        many[i] = mk_binding(3, PIXELICA_INPUT_BUTTON, 63, 31, 1.0f / 64.0f, 0.0f);
    }
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, many, PIXELICA_INPUT_MAX_BINDINGS) ==
          PIXELICA_OK);
    CHECK(m.binding_count == PIXELICA_INPUT_MAX_BINDINGS);
    CHECK(submit_buttons(&m, 3, (uint64_t)1u << 63) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK_NEAR(pixelica_input_manager_action(&m, 31).value, 1.0, 0.0);
    CHECK(action_is(&m, 31, 1.0f, true, true, false));

    /* Over capacity: CAPACITY and nothing changes. */
    memcpy(saved, m.bindings, sizeof saved);
    many[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, 0, 1.0f, 0.0f);
    CHECK(pixelica_input_manager_set_bindings(&m, many,
                                         PIXELICA_INPUT_MAX_BINDINGS + 1u) ==
          PIXELICA_ERR_CAPACITY);
    CHECK(pixelica_input_manager_set_bindings(&m, many, (size_t)-1) ==
          PIXELICA_ERR_CAPACITY);
    CHECK(pixelica_input_manager_set_bindings(&m, NULL, (size_t)-1) ==
          PIXELICA_ERR_CAPACITY);
    CHECK(m.binding_count == PIXELICA_INPUT_MAX_BINDINGS);
    CHECK(memcmp(saved, m.bindings, sizeof saved) == 0);

    /* Distinct full table, including the maximum device/control/action. */
    for (i = 0; i < PIXELICA_INPUT_MAX_BINDINGS; ++i) {
        uint8_t device = (uint8_t)(i % PIXELICA_INPUT_MAX_DEVICES);
        uint8_t action = (uint8_t)(i % PIXELICA_INPUT_MAX_ACTIONS);

        if ((i & 1u) != 0u) {
            many[i] = mk_binding(device, PIXELICA_INPUT_BUTTON,
                                 (uint8_t)(63u - i / 2u), action, 1.0f, 0.0f);
        } else {
            many[i] = mk_binding(device, (PIXELICA_InputControl)(1u + (i / 2u) % 3u),
                                 (uint8_t)(i % PIXELICA_INPUT_MAX_AXES), action,
                                 -1.0f, 0.999f);
        }
    }
    CHECK(pixelica_input_manager_set_bindings(&m, many, PIXELICA_INPUT_MAX_BINDINGS) ==
          PIXELICA_OK);
    CHECK(memcmp(many, m.bindings, sizeof saved) == 0);
    /* Action 31 now maps to device 3 buttons 48 and 32, both up. */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 31, 0.0f, false, false, true));

    /* count=0 clears with or without a pointer; count>0 needs one. */
    CHECK(pixelica_input_manager_set_bindings(&m, many, 0u) == PIXELICA_OK);
    CHECK(m.binding_count == 0u);
    CHECK(pixelica_input_manager_set_bindings(&m, NULL, 1u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(m.binding_count == 0u);
    pixelica_input_manager_tick(&m);
    CHECK(is_neutral(pixelica_input_manager_action(&m, 31)));
}

/* Variant `which` of an invalid binding; returns 0 when out of variants. */
static int bad_binding(int which, PIXELICA_InputBinding *out)
{
    PIXELICA_InputBinding b = mk_binding(1, PIXELICA_INPUT_BUTTON, 3, 7, 1.0f, 0.0f);

    switch (which) {
    case 0: b.device = 4; break;
    case 1: b.device = 255; break;
    case 2: b.action = 32; break;
    case 3: b.action = 255; break;
    case 4: b.control = 64; break;
    case 5: b.kind = PIXELICA_INPUT_AXIS; b.control = 8; break;
    case 6: b.kind = PIXELICA_INPUT_AXIS_POSITIVE; b.control = 8; break;
    case 7: b.kind = PIXELICA_INPUT_AXIS_NEGATIVE; b.control = 63; break;
    case 8: b.kind = (PIXELICA_InputControl)4; break;
    case 9: b.kind = (PIXELICA_InputControl)99; break;
    case 10: b.scale = pixelica_t_nan(); break;
    case 11: b.scale = pixelica_t_inf(); break;
    case 12: b.scale = -pixelica_t_inf(); break;
    case 13: b.scale = 1.0001f; break;
    case 14: b.scale = -1.0001f; break;
    case 15: b.deadzone = pixelica_t_nan(); break;
    case 16: b.deadzone = pixelica_t_inf(); break;
    case 17: b.deadzone = -pixelica_t_inf(); break;
    case 18: b.deadzone = 1.0f; break;
    case 19: b.deadzone = -0.0001f; break;
    case 20: b.kind = PIXELICA_INPUT_AXIS; b.control = 0; b.deadzone = 1.0f; break;
    default: return 0;
    }
    *out = b;
    return 1;
}

static void test_invalid_bindings_transactional(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputManager before;
    PIXELICA_InputBinding list[3];
    PIXELICA_InputBinding edge[6];
    int which;

    pixelica_input_manager_init(&m);
    list[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A, 1.0f, 0.0f);
    list[1] = mk_binding(1, PIXELICA_INPUT_AXIS, 0, PIXELICA_INPUT_LEFT, -1.0f, 0.2f);
    CHECK(pixelica_input_manager_set_bindings(&m, list, 2u) == PIXELICA_OK);
    CHECK(submit_buttons(&m, 0, 1u) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    memcpy(&before, &m, sizeof m);

    for (which = 0; bad_binding(which, &list[2]); ++which) {
        /* The bad entry is last: nothing before it may be written. */
        CHECK(pixelica_input_manager_set_bindings(&m, list, 3u) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(&m, &before, sizeof m) == 0);
        /* And first. */
        CHECK(pixelica_input_manager_set_bindings(&m, &list[2], 1u) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(&m, &before, sizeof m) == 0);
    }
    CHECK(which == 21);

    /* Boundary values that must be accepted. */
    edge[0] = mk_binding(3, PIXELICA_INPUT_BUTTON, 63, 31, 1.0f, 0.0f);
    edge[1] = mk_binding(3, PIXELICA_INPUT_AXIS, 7, 31, -1.0f, 0.999f);
    edge[2] = mk_binding(0, PIXELICA_INPUT_AXIS_POSITIVE, 7, 0, 0.0f, 0.0f);
    edge[3] = mk_binding(0, PIXELICA_INPUT_AXIS_NEGATIVE, 0, 0, -0.0f, 0.5f);
    edge[4] = mk_binding(2, PIXELICA_INPUT_BUTTON, 8, 30, 0.5f, 0.99f);
    edge[5] = mk_binding(1, PIXELICA_INPUT_BUTTON, 0, 7, 1.0f, 0.0f);
    CHECK(pixelica_input_manager_set_bindings(&m, edge, 6u) == PIXELICA_OK);
    CHECK(m.binding_count == 6u);
    CHECK(memcmp(m.bindings, edge, sizeof edge) == 0);
}

static void test_invalid_submit_transactional(void)
{
    static const uint8_t slots[2] = {0, 7};
    PIXELICA_InputManager m;
    PIXELICA_InputManager before;
    PIXELICA_InputDeviceState good;
    PIXELICA_InputDeviceState bad;
    float values[5];
    size_t i, k;

    values[0] = pixelica_t_nan();
    values[1] = pixelica_t_inf();
    values[2] = -pixelica_t_inf();
    values[3] = 1.0001f;
    values[4] = -1.5f;

    pixelica_input_manager_init(&m);
    CHECK(set1(&m, mk_binding(1, PIXELICA_INPUT_AXIS, 7, 7, 1.0f, 0.0f)) == PIXELICA_OK);
    good = mk_state(true, 0x5u);
    for (i = 0; i < PIXELICA_INPUT_MAX_AXES; ++i) {
        good.axes[i] = 0.75f;
    }
    CHECK(pixelica_input_manager_submit(&m, 1, &good) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    memcpy(&before, &m, sizeof m);

    for (k = 0; k < 2u; ++k) {
        for (i = 0; i < 5u; ++i) {
            bad = mk_state(true, ~(uint64_t)0u);
            bad.axes[slots[k]] = values[i];
            CHECK(pixelica_input_manager_submit(&m, 1, &bad) ==
                  PIXELICA_ERR_INVALID_ARG);
            CHECK(memcmp(&m, &before, sizeof m) == 0);
        }
    }
    CHECK(pixelica_input_manager_submit(&m, 4, &good) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&m, &before, sizeof m) == 0);

    /* The old source is still in effect. */
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 0.75f, true, false, false));

    /* Exact extremes are valid. */
    good.axes[0] = -1.0f;
    good.axes[7] = 1.0f;
    CHECK(pixelica_input_manager_submit(&m, 1, &good) == PIXELICA_OK);
    CHECK(m.devices[1].connected);
    CHECK(m.devices[1].buttons == 0x5u);
    CHECK_NEAR(m.devices[1].axes[0], -1.0, 0.0);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 7, 1.0f, true, false, false));

    /* Disconnected snapshots with invalid payloads are accepted as neutral. */
    CHECK(submit_unplugged(&m, 1) == PIXELICA_OK);
    CHECK(device_is_neutral(&m.devices[1]));
}

static void test_aliasing(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[3];
    PIXELICA_InputBinding saved[3];
    PIXELICA_InputDeviceState s;

    b[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[1] = mk_binding(1, PIXELICA_INPUT_AXIS, 2, 7, -0.5f, 0.25f);
    b[2] = mk_binding(2, PIXELICA_INPUT_AXIS_NEGATIVE, 3, 8, 1.0f, 0.0f);
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 3u) == PIXELICA_OK);
    memcpy(saved, m.bindings, sizeof saved);

    /* Exact self-copy. */
    CHECK(pixelica_input_manager_set_bindings(&m, m.bindings, 3u) == PIXELICA_OK);
    CHECK(m.binding_count == 3u);
    CHECK(memcmp(m.bindings, saved, sizeof saved) == 0);

    /* Invalid in-place data is rejected before any write. */
    m.bindings[2].deadzone = 2.0f;
    CHECK(pixelica_input_manager_set_bindings(&m, m.bindings, 3u) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(m.binding_count == 3u);
    CHECK(memcmp(m.bindings, saved, 2u * sizeof saved[0]) == 0);
    m.bindings[2].deadzone = 0.0f;

    /* Overlapping shift down by one. */
    CHECK(pixelica_input_manager_set_bindings(&m, m.bindings + 1, 2u) == PIXELICA_OK);
    CHECK(m.binding_count == 2u);
    CHECK(memcmp(&m.bindings[0], &saved[1], sizeof saved[1]) == 0);
    CHECK(memcmp(&m.bindings[1], &saved[2], sizeof saved[2]) == 0);

    /* Snapshot copied from the manager's own storage. */
    s = mk_state(true, 0x81u);
    s.axes[3] = -0.5f;
    CHECK(pixelica_input_manager_submit(&m, 0, &s) == PIXELICA_OK);
    CHECK(pixelica_input_manager_submit(&m, 0, &m.devices[0]) == PIXELICA_OK);
    CHECK(m.devices[0].connected && m.devices[0].buttons == 0x81u);
    CHECK_NEAR(m.devices[0].axes[3], -0.5, 0.0);
    CHECK(pixelica_input_manager_submit(&m, 2, &m.devices[0]) == PIXELICA_OK);
    CHECK(m.devices[2].connected && m.devices[2].buttons == 0x81u);
    CHECK_NEAR(m.devices[2].axes[3], -0.5, 0.0);
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 8, 0.5f, true, true, false));

    /* A disconnected slot copied over a live one makes it neutral. */
    CHECK(pixelica_input_manager_submit(&m, 2, &m.devices[3]) == PIXELICA_OK);
    CHECK(device_is_neutral(&m.devices[2]));
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, 8, 0.0f, false, false, true));
}

static void test_corrupted_storage_guard(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[2];

    b[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[1] = mk_binding(0, PIXELICA_INPUT_BUTTON, 1, PIXELICA_INPUT_START, 1.0f, 0.0f);
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 2u) == PIXELICA_OK);
    CHECK(submit_buttons(&m, 0, 3u) == PIXELICA_OK);

    /* Out-of-range direct writes must not index out of bounds. */
    m.binding_count = (size_t)100000u;
    m.bindings[0].device = 200;
    pixelica_input_manager_tick(&m);
    CHECK(action_is(&m, PIXELICA_INPUT_START, 1.0f, true, true, false));
    CHECK(pixelica_input_manager_buttons(&m) == PIXELICA_BTN_START);
}

static void test_legacy_bridge(void)
{
    /* Table, not constant expressions: avoids MSVC C4127 under /W4 /WX. */
    static const uint32_t ids[7] = {
        PIXELICA_INPUT_LEFT, PIXELICA_INPUT_RIGHT, PIXELICA_INPUT_UP, PIXELICA_INPUT_DOWN,
        PIXELICA_INPUT_A, PIXELICA_INPUT_START, PIXELICA_INPUT_QUIT};
    static const uint32_t bits[7] = {
        PIXELICA_BTN_LEFT, PIXELICA_BTN_RIGHT, PIXELICA_BTN_UP, PIXELICA_BTN_DOWN,
        PIXELICA_BTN_A, PIXELICA_BTN_START, PIXELICA_BTN_QUIT};
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[12];
    PIXELICA_PlatformOps pal;
    unsigned i;

    /* Action IDs 0..6 are PIXELICA_BTN_* bit positions. */
    for (i = 0; i < 7u; ++i) {
        CHECK(((uint32_t)1u << ids[i]) == bits[i]);
        CHECK(ids[i] == i);
    }

    for (i = 0; i < 7u; ++i) {
        b[i] = mk_binding(0, PIXELICA_INPUT_BUTTON, (uint8_t)i, (uint8_t)i,
                          1.0f, 0.0f);
    }
    b[7] = mk_binding(0, PIXELICA_INPUT_BUTTON, 7, 7, 1.0f, 0.0f);
    b[8] = mk_binding(1, PIXELICA_INPUT_AXIS_NEGATIVE, 0, PIXELICA_INPUT_LEFT, 1.0f, 0.0f);
    b[9] = mk_binding(1, PIXELICA_INPUT_AXIS_POSITIVE, 0, PIXELICA_INPUT_RIGHT, 1.0f, 0.0f);
    b[10] = mk_binding(1, PIXELICA_INPUT_AXIS_NEGATIVE, 1, PIXELICA_INPUT_UP, 1.0f, 0.0f);
    b[11] = mk_binding(1, PIXELICA_INPUT_AXIS_POSITIVE, 1, PIXELICA_INPUT_DOWN, 1.0f, 0.0f);
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 12u) == PIXELICA_OK);

    memset(&pal, 0, sizeof pal);
    pal.userdata = &m;
    pal.read_buttons = pixelica_input_manager_read_buttons;
    CHECK(pal.read_buttons(pal.userdata) == 0u);

    CHECK(submit_buttons(&m, 0, 0xFFu) == PIXELICA_OK);
    pixelica_input_manager_tick(&m);
    CHECK(pixelica_input_manager_buttons(&m) == PIXELICA_BTN_ALL);
    CHECK(pal.read_buttons(pal.userdata) == PIXELICA_BTN_ALL);
    CHECK(action_is(&m, 7, 1.0f, true, true, false));

    /* Analog sources reach the legacy mask past the 0.5 threshold. */
    CHECK(submit_buttons(&m, 0, 0u) == PIXELICA_OK);
    {
        PIXELICA_InputDeviceState s = mk_state(true, 0u);

        s.axes[0] = -0.6f;
        s.axes[1] = 0.4f;
        CHECK(pixelica_input_manager_submit(&m, 1, &s) == PIXELICA_OK);
    }
    pixelica_input_manager_tick(&m);
    CHECK(pal.read_buttons(pal.userdata) == PIXELICA_BTN_LEFT);
}

/* ---- "P3IN" v1 compatibility -------------------------------------------- */

#define P3IN_TICKS 24u

typedef struct ReplaySink {
    uint32_t masks[P3IN_TICKS];
    uint32_t count;
} ReplaySink;

static void replay_sink_tick(void *userdata, float dt, uint32_t buttons)
{
    ReplaySink *sink = (ReplaySink *)userdata;

    (void)dt;
    if (sink->count < P3IN_TICKS) {
        sink->masks[sink->count] = buttons;
    }
    sink->count++;
}

static void test_p3in_compat(void)
{
    PIXELICA_InputManager m;
    PIXELICA_InputBinding b[5];
    PIXELICA_InputSpan spans[P3IN_TICKS];
    PIXELICA_InputSpan decoded[P3IN_TICKS];
    PIXELICA_InputRecorder rec;
    PIXELICA_InputPlayer player;
    ReplaySink sink;
    uint32_t live[P3IN_TICKS];
    uint8_t bytes[PIXELICA_INPUT_HEADER_BYTES + PIXELICA_INPUT_SPAN_BYTES * P3IN_TICKS];
    size_t written = 0;
    size_t decoded_count = 0;
    uint32_t mask = 0u;
    uint32_t t;

    b[0] = mk_binding(0, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[1] = mk_binding(1, PIXELICA_INPUT_BUTTON, 0, PIXELICA_INPUT_A, 1.0f, 0.0f);
    b[2] = mk_binding(1, PIXELICA_INPUT_AXIS_POSITIVE, 0, PIXELICA_INPUT_RIGHT,
                      1.0f, 0.25f);
    b[3] = mk_binding(0, PIXELICA_INPUT_BUTTON, 9, 20, 1.0f, 0.0f); /* user action */
    b[4] = mk_binding(0, PIXELICA_INPUT_BUTTON, 1, PIXELICA_INPUT_START, 1.0f, 0.0f);
    pixelica_input_manager_init(&m);
    CHECK(pixelica_input_manager_set_bindings(&m, b, 5u) == PIXELICA_OK);
    pixelica_input_recorder_init(&rec, spans, P3IN_TICKS);

    /* Mixed digital/analog session on two devices, unplug mid-way. */
    for (t = 0; t < P3IN_TICKS; ++t) {
        PIXELICA_InputDeviceState pad = mk_state(t < 16u, (t / 3u) % 2u);

        pad.axes[0] = (float)(t % 8u) / 7.0f;
        CHECK(submit_buttons(&m, 0, ((t % 5u) == 0u ? 1u : 0u) |
                                    ((uint64_t)1u << 9) |
                                    (t == 10u ? 2u : 0u)) == PIXELICA_OK);
        CHECK(pixelica_input_manager_submit(&m, 1, &pad) == PIXELICA_OK);
        pixelica_input_manager_tick(&m);
        live[t] = pixelica_input_manager_buttons(&m);
        CHECK((live[t] & ~PIXELICA_BTN_ALL) == 0u);
        CHECK(pixelica_input_record(&rec, live[t]) == PIXELICA_OK);
    }
    CHECK(rec.total_ticks == P3IN_TICKS);
    CHECK(action_is(&m, 20, 1.0f, true, false, false)); /* not in P3IN */

    CHECK(pixelica_input_encode(rec.spans, rec.count, bytes, sizeof bytes,
                           &written) == PIXELICA_OK);
    CHECK(written == PIXELICA_INPUT_HEADER_BYTES +
                         PIXELICA_INPUT_SPAN_BYTES * rec.count);
    CHECK(memcmp(bytes, "P3IN", 4u) == 0);
    CHECK(pixelica_input_decode(bytes, written, decoded, P3IN_TICKS,
                           &decoded_count) == PIXELICA_OK);
    CHECK(decoded_count == rec.count);

    pixelica_input_player_init(&player, decoded, decoded_count);
    for (t = 0; t < P3IN_TICKS; ++t) {
        CHECK(pixelica_input_player_next(&player, &mask));
        CHECK(mask == live[t]);
    }
    CHECK(!pixelica_input_player_next(&player, &mask));

    memset(&sink, 0, sizeof sink);
    CHECK(pixelica_input_replay(decoded, decoded_count, replay_sink_tick, &sink) ==
          P3IN_TICKS);
    CHECK(sink.count == P3IN_TICKS);
    CHECK(memcmp(sink.masks, live, sizeof live) == 0);
}

int main(void)
{
    test_null_and_init();
    test_multi_device_merge();
    test_shared_action_release();
    test_axis_shaping();
    test_sum_clamp_opposing();
    test_repeated_edges();
    test_unplug_and_focus_loss();
    test_rebinding_while_held();
    test_capacity();
    test_invalid_bindings_transactional();
    test_invalid_submit_transactional();
    test_aliasing();
    test_corrupted_storage_guard();
    test_legacy_bridge();
    test_p3in_compat();
    return pixelica_t_finish("test_input_manager");
}

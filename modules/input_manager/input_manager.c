/*
 * Pixelica - optional input manager. See pixelica/input_manager.h for the contract.
 * Allocation-free and renderer-independent: sources submit device snapshots,
 * tick() folds every binding into per-action values in double precision and
 * derives held/pressed/released edges from the previous tick's held state.
 */
#include "pixelica/input_manager.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* An action is held when its magnitude reaches this threshold. */
#define PIXELICA_INPUT_HELD_THRESHOLD 0.5f

static void im_device_neutral(PIXELICA_InputDeviceState *device)
{
    size_t i;

    memset(device, 0, sizeof *device);
    device->connected = false;
    device->buttons = 0u;
    for (i = 0; i < PIXELICA_INPUT_MAX_AXES; ++i) {
        device->axes[i] = 0.0f;
    }
}

static void im_binding_clear(PIXELICA_InputBinding *binding)
{
    memset(binding, 0, sizeof *binding);
    binding->kind = PIXELICA_INPUT_BUTTON;
    binding->scale = 0.0f;
    binding->deadzone = 0.0f;
}

static PIXELICA_InputActionState im_action_neutral(void)
{
    PIXELICA_InputActionState s;

    memset(&s, 0, sizeof s);
    s.value = 0.0f;
    s.held = false;
    s.pressed = false;
    s.released = false;
    return s;
}

/* Also false for NaN and infinities. */
static bool im_in_range(float v, float lo, float hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

static bool im_binding_valid(const PIXELICA_InputBinding *b)
{
    unsigned limit = 0u;

    if ((unsigned)b->device >= PIXELICA_INPUT_MAX_DEVICES ||
        (unsigned)b->action >= PIXELICA_INPUT_MAX_ACTIONS) {
        return false;
    }
    switch (b->kind) {
    case PIXELICA_INPUT_BUTTON:
        limit = PIXELICA_INPUT_MAX_BUTTONS;
        break;
    case PIXELICA_INPUT_AXIS:
    case PIXELICA_INPUT_AXIS_POSITIVE:
    case PIXELICA_INPUT_AXIS_NEGATIVE:
        limit = PIXELICA_INPUT_MAX_AXES;
        break;
    default:
        return false;
    }
    if ((unsigned)b->control >= limit) {
        return false;
    }
    if (!im_in_range(b->scale, -1.0f, 1.0f)) {
        return false;
    }
    /* deadzone in [0,1): 1 itself would divide by zero when rescaling. */
    return isfinite(b->deadzone) && b->deadzone >= 0.0f && b->deadzone < 1.0f;
}

/* Raw component of one binding before scale, in [-1,1] (buttons: 0 or 1). */
static double im_binding_raw(const PIXELICA_InputDeviceState *device,
                             const PIXELICA_InputBinding *b)
{
    double x;
    double mag;
    double dz;

    if (!device->connected) {
        return 0.0;
    }
    if (b->kind == PIXELICA_INPUT_BUTTON) {
        /* Deadzone does not apply to digital controls. */
        return ((device->buttons >> b->control) & 1u) != 0u ? 1.0 : 0.0;
    }

    x = (double)device->axes[b->control];
    if (!isfinite(x)) {
        return 0.0; /* only reachable if storage was mutated directly */
    }
    if (x > 1.0) {
        x = 1.0;
    } else if (x < -1.0) {
        x = -1.0;
    }

    /* Direction gate first, so each half-axis gets the full deadzone range.
     * The negative half reports its magnitude as a positive value. */
    if (b->kind == PIXELICA_INPUT_AXIS_POSITIVE) {
        x = x > 0.0 ? x : 0.0;
    } else if (b->kind == PIXELICA_INPUT_AXIS_NEGATIVE) {
        x = x < 0.0 ? -x : 0.0;
    }

    mag = fabs(x);
    dz = (double)b->deadzone;
    if (mag <= dz) {
        return 0.0;
    }
    mag = (mag - dz) / (1.0 - dz);
    if (mag > 1.0) {
        mag = 1.0;
    }
    return x < 0.0 ? -mag : mag;
}

void pixelica_input_manager_init(PIXELICA_InputManager *manager)
{
    size_t i;

    if (manager == NULL) {
        return;
    }
    memset(manager, 0, sizeof *manager);
    for (i = 0; i < PIXELICA_INPUT_MAX_DEVICES; ++i) {
        im_device_neutral(&manager->devices[i]);
    }
    for (i = 0; i < PIXELICA_INPUT_MAX_BINDINGS; ++i) {
        im_binding_clear(&manager->bindings[i]);
    }
    manager->binding_count = 0;
    for (i = 0; i < PIXELICA_INPUT_MAX_ACTIONS; ++i) {
        manager->actions[i] = im_action_neutral();
    }
}

PIXELICA_Status pixelica_input_manager_set_bindings(PIXELICA_InputManager *manager,
    const PIXELICA_InputBinding *bindings, size_t count)
{
    size_t i;

    if (manager == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (count > PIXELICA_INPUT_MAX_BINDINGS) {
        return PIXELICA_ERR_CAPACITY;
    }
    if (count > 0u && bindings == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* Validate everything before the first write (transactional). */
    for (i = 0; i < count; ++i) {
        if (!im_binding_valid(&bindings[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    /* `bindings` may alias (part of) manager->bindings. */
    if (count > 0u) {
        memmove(manager->bindings, bindings, count * sizeof *bindings);
    }
    for (i = count; i < PIXELICA_INPUT_MAX_BINDINGS; ++i) {
        im_binding_clear(&manager->bindings[i]);
    }
    manager->binding_count = count;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_input_manager_submit(PIXELICA_InputManager *manager, uint8_t device,
    const PIXELICA_InputDeviceState *state)
{
    PIXELICA_InputDeviceState snapshot;
    size_t i;

    if (manager == NULL || state == NULL ||
        (unsigned)device >= PIXELICA_INPUT_MAX_DEVICES) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!state->connected) {
        im_device_neutral(&manager->devices[device]);
        return PIXELICA_OK;
    }
    /* Copy first: `state` may alias a slot in manager->devices. */
    snapshot = *state;
    for (i = 0; i < PIXELICA_INPUT_MAX_AXES; ++i) {
        if (!im_in_range(snapshot.axes[i], -1.0f, 1.0f)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    snapshot.connected = true;
    manager->devices[device] = snapshot;
    return PIXELICA_OK;
}

void pixelica_input_manager_tick(PIXELICA_InputManager *manager)
{
    double sums[PIXELICA_INPUT_MAX_ACTIONS];
    size_t count;
    size_t i;

    if (manager == NULL) {
        return;
    }
    for (i = 0; i < PIXELICA_INPUT_MAX_ACTIONS; ++i) {
        sums[i] = 0.0;
    }

    count = manager->binding_count;
    if (count > PIXELICA_INPUT_MAX_BINDINGS) {
        count = PIXELICA_INPUT_MAX_BINDINGS; /* guard against corrupted storage */
    }
    for (i = 0; i < count; ++i) {
        const PIXELICA_InputBinding *b = &manager->bindings[i];

        /* Stored bindings were validated; re-check so direct writes to the
         * storage can never index out of bounds. */
        if (!im_binding_valid(b)) {
            continue;
        }
        sums[b->action] +=
            im_binding_raw(&manager->devices[b->device], b) * (double)b->scale;
    }

    for (i = 0; i < PIXELICA_INPUT_MAX_ACTIONS; ++i) {
        PIXELICA_InputActionState *a = &manager->actions[i];
        double v = sums[i];
        bool was_held = a->held;
        bool held;

        if (v > 1.0) {
            v = 1.0;
        } else if (v < -1.0) {
            v = -1.0;
        }
        a->value = (float)v;
        held = fabsf(a->value) >= PIXELICA_INPUT_HELD_THRESHOLD;
        a->held = held;
        a->pressed = held && !was_held;
        a->released = !held && was_held;
    }
}

PIXELICA_InputActionState pixelica_input_manager_action(const PIXELICA_InputManager *manager,
    uint8_t action)
{
    if (manager == NULL || (unsigned)action >= PIXELICA_INPUT_MAX_ACTIONS) {
        return im_action_neutral();
    }
    return manager->actions[action];
}

uint32_t pixelica_input_manager_buttons(const PIXELICA_InputManager *manager)
{
    uint32_t mask = 0u;
    unsigned i;

    if (manager == NULL) {
        return 0u;
    }
    /* Action IDs 0..6 are the PIXELICA_BTN_* bit positions. */
    for (i = 0; i <= (unsigned)PIXELICA_INPUT_QUIT; ++i) {
        if (manager->actions[i].held) {
            mask |= (uint32_t)1u << i;
        }
    }
    return mask & PIXELICA_BTN_ALL;
}

uint32_t pixelica_input_manager_read_buttons(void *userdata)
{
    return pixelica_input_manager_buttons((const PIXELICA_InputManager *)userdata);
}

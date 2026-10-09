/* Optional, allocation-free input mapping shared by all renderer profiles. */
#ifndef PIXELICA_INPUT_MANAGER_H
#define PIXELICA_INPUT_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pixelica/input.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_INPUT_MAX_DEVICES 4u
#define PIXELICA_INPUT_MAX_BUTTONS 64u
#define PIXELICA_INPUT_MAX_AXES 8u
#define PIXELICA_INPUT_MAX_ACTIONS 32u
#define PIXELICA_INPUT_MAX_BINDINGS 64u

/* IDs 0..6 match PIXELICA_BTN_* bit positions. User actions may use IDs 7..31. */
typedef enum PIXELICA_InputActionId {
    PIXELICA_INPUT_LEFT = 0, PIXELICA_INPUT_RIGHT, PIXELICA_INPUT_UP, PIXELICA_INPUT_DOWN,
    PIXELICA_INPUT_A, PIXELICA_INPUT_START, PIXELICA_INPUT_QUIT
} PIXELICA_InputActionId;

typedef enum PIXELICA_InputControl {
    PIXELICA_INPUT_BUTTON = 0,
    PIXELICA_INPUT_AXIS,
    PIXELICA_INPUT_AXIS_POSITIVE,
    PIXELICA_INPUT_AXIS_NEGATIVE
} PIXELICA_InputControl;

typedef struct PIXELICA_InputDeviceState {
    bool connected;
    uint64_t buttons;
    float axes[PIXELICA_INPUT_MAX_AXES]; /* finite, [-1,1]; triggers may use [0,1] */
} PIXELICA_InputDeviceState;

typedef struct PIXELICA_InputBinding {
    uint8_t device;
    uint8_t control;
    uint8_t action;
    PIXELICA_InputControl kind;
    float scale;    /* finite [-1,1] */
    float deadzone; /* finite [0,1); axis components are rescaled after it */
} PIXELICA_InputBinding;

typedef struct PIXELICA_InputActionState {
    float value; /* sum of contributions clamped to [-1,1] */
    bool held;  /* fabs(value) >= 0.5 */
    bool pressed;
    bool released;
} PIXELICA_InputActionState;

/* Caller-owned, initialize before use. Fields are implementation storage.
 * Single-threaded: drivers must hand off IRQ/thread data before submit(). */
typedef struct PIXELICA_InputManager {
    PIXELICA_InputDeviceState devices[PIXELICA_INPUT_MAX_DEVICES];
    PIXELICA_InputBinding bindings[PIXELICA_INPUT_MAX_BINDINGS];
    size_t binding_count;
    PIXELICA_InputActionState actions[PIXELICA_INPUT_MAX_ACTIONS];
} PIXELICA_InputManager;

/* Safe on NULL; init also resets devices, bindings and all edges. */
void pixelica_input_manager_init(PIXELICA_InputManager *manager);
/* Transactional copy. count=0 clears bindings (NULL allowed). Rebinding does
 * not modify action states until the next tick; live held state is compared. */
PIXELICA_Status pixelica_input_manager_set_bindings(PIXELICA_InputManager *manager,
    const PIXELICA_InputBinding *bindings, size_t count);
/* Latest snapshot wins. Disconnected snapshots are neutral regardless of
 * payload. Invalid connected snapshots leave the old source untouched. */
PIXELICA_Status pixelica_input_manager_submit(PIXELICA_InputManager *manager, uint8_t device,
    const PIXELICA_InputDeviceState *state);
/* Call exactly once per simulation tick, after sampling sources. Unsubmitted
 * devices retain their last snapshot; submit connected=false on unplug/focus
 * loss. This is a snapshot API: taps entirely between samples are not queued. */
void pixelica_input_manager_tick(PIXELICA_InputManager *manager);
/* Invalid arguments produce neutral values. Queries do not consume edges. */
PIXELICA_InputActionState pixelica_input_manager_action(const PIXELICA_InputManager *manager,
    uint8_t action);
/* Legacy seven-button bridge. Extra actions/analog data are not in P3IN v1. */
uint32_t pixelica_input_manager_buttons(const PIXELICA_InputManager *manager);
uint32_t pixelica_input_manager_read_buttons(void *userdata);

#ifdef __cplusplus
}
#endif
#endif

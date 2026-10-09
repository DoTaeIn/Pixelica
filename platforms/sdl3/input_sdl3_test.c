/*
 * Headless test of the SDL3 input adapter (input_sdl3.h).
 *
 * Needs no window, display, keyboard injection or physical gamepad: devices are
 * SDL virtual gamepads (SDL_AttachVirtualJoystick). Real controllers are kept
 * out of SDL's device list with hints. The physical keyboard path is not
 * exercised; the key map is validated through set_key() and the default table.
 * Checks never use assert(), so they stay active in Release builds.
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input_sdl3.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(actual, expected, tol)                                    \
    do {                                                                     \
        double actual_ = (double)(actual);                                   \
        double expected_ = (double)(expected);                               \
        ++g_checks;                                                          \
        if (!(actual_ >= expected_ - (tol) && actual_ <= expected_ + (tol))) { \
            ++g_failures;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s ~= %s (%.9g vs %.9g)\n", \
                    __FILE__, __LINE__, #actual, #expected, actual_,         \
                    expected_);                                              \
        }                                                                    \
    } while (0)

#define BIT(b) (UINT64_C(1) << (b))

/* Button bits 0..14 (SOUTH..DPAD_RIGHT) plus TOUCHPAD (20): 16 buttons; all six axes. */
#define FULL_BUTTON_MASK (0x7FFFu | (1u << SDL_GAMEPAD_BUTTON_TOUCHPAD))
#define FULL_AXIS_MASK 0x3Fu

/* ------------------------------------------------------------------------ */
/* Virtual gamepad helpers                                                  */
/* ------------------------------------------------------------------------ */

typedef struct TestPad {
    SDL_JoystickID id;
    SDL_Joystick *joystick; /* the test's own handle, used to drive the device */
    Uint32 button_mask;
    Uint32 axis_mask;
} TestPad;

static int count_below(Uint32 mask, int bit)
{
    int n = 0;
    for (int i = 0; i < bit; ++i) {
        if ((mask & (1u << i)) != 0u) {
            ++n;
        }
    }
    return n;
}

/* Registers a virtual gamepad. The virtual driver numbers the SDL joystick
 * buttons/axes in the order of the set bits of the masks. */
static bool pad_attach(TestPad *pad, Uint32 button_mask, Uint32 axis_mask)
{
    static Uint16 next_product_id = 1u;
    SDL_VirtualJoystickDesc desc;

    memset(pad, 0, sizeof(*pad));
    SDL_INIT_INTERFACE(&desc);
    desc.type = (Uint16)SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = (Uint16)count_below(axis_mask, 32);
    desc.nbuttons = (Uint16)count_below(button_mask, 32);
    desc.button_mask = button_mask;
    desc.axis_mask = axis_mask;
    /* SDL's CRC-free fallback can reuse mappings: isolate virtual fixtures. */
    desc.vendor_id = 0xFFFFu;
    desc.product_id = next_product_id++;
    desc.name = "Pixelica test pad";
    pad->button_mask = button_mask;
    pad->axis_mask = axis_mask;

    pad->id = SDL_AttachVirtualJoystick(&desc);
    if (pad->id == 0) {
        fprintf(stderr, "SDL_AttachVirtualJoystick failed: %s\n", SDL_GetError());
        return false;
    }
    pad->joystick = SDL_OpenJoystick(pad->id);
    if (pad->joystick == NULL) {
        fprintf(stderr, "SDL_OpenJoystick failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

/* Unplugs the device and releases the test's handle. */
static void pad_detach(TestPad *pad)
{
    if (pad->id != 0) {
        CHECK(SDL_DetachVirtualJoystick(pad->id));
    }
    if (pad->joystick != NULL) {
        SDL_CloseJoystick(pad->joystick);
    }
    memset(pad, 0, sizeof(*pad));
}

static void pad_button(const TestPad *pad, SDL_GamepadButton button, bool down)
{
    CHECK((pad->button_mask & (1u << (int)button)) != 0u);
    CHECK(SDL_SetJoystickVirtualButton(pad->joystick, count_below(pad->button_mask, (int)button), down));
}

static void pad_axis(const TestPad *pad, SDL_GamepadAxis axis, int value)
{
    CHECK((pad->axis_mask & (1u << (int)axis)) != 0u);
    CHECK(SDL_SetJoystickVirtualAxis(pad->joystick, count_below(pad->axis_mask, (int)axis), (Sint16)value));
}

/* ------------------------------------------------------------------------ */
/* Adapter / manager helpers                                                */
/* ------------------------------------------------------------------------ */

static void poll_ok(PIXELICA_InputSdl3 *adapter, bool accept_input)
{
    PIXELICA_Status status = pixelica_input_sdl3_poll(adapter, accept_input);
    CHECK(status == PIXELICA_OK);
}

static void adapter_start(PIXELICA_InputSdl3 *adapter, PIXELICA_InputManager *manager, uint8_t pad_count)
{
    PIXELICA_Status status;
    pixelica_input_manager_init(manager);
    memset(adapter, 0, sizeof(*adapter));
    status = pixelica_input_sdl3_init(adapter, manager, pad_count);
    CHECK(status == PIXELICA_OK);
}

static const PIXELICA_InputDeviceState *source_state(const PIXELICA_InputManager *manager, unsigned device)
{
    return &manager->devices[device];
}

/* Binds "source <device>, keyboard-style button <control>" to action A. */
static void bind_button_to_a(PIXELICA_InputManager *manager, uint8_t device, uint8_t control)
{
    PIXELICA_InputBinding binding;
    memset(&binding, 0, sizeof(binding));
    binding.device = device;
    binding.control = control;
    binding.action = (uint8_t)PIXELICA_INPUT_A;
    binding.kind = PIXELICA_INPUT_BUTTON;
    binding.scale = 1.0f;
    binding.deadzone = 0.0f;
    CHECK(pixelica_input_manager_set_bindings(manager, &binding, 1u) == PIXELICA_OK);
}

static PIXELICA_InputActionState action_a(const PIXELICA_InputManager *manager)
{
    return pixelica_input_manager_action(manager, (uint8_t)PIXELICA_INPUT_A);
}

static void submit_custom(PIXELICA_InputManager *manager, uint8_t device)
{
    PIXELICA_InputDeviceState state;
    memset(&state, 0, sizeof(state));
    state.connected = true;
    state.buttons = 0x5u;
    state.axes[0] = 0.25f;
    CHECK(pixelica_input_manager_submit(manager, device, &state) == PIXELICA_OK);
}

static void check_custom_intact(const PIXELICA_InputManager *manager, unsigned device)
{
    const PIXELICA_InputDeviceState *state = source_state(manager, device);
    CHECK(state->connected);
    CHECK(state->buttons == 0x5u);
    CHECK_NEAR(state->axes[0], 0.25, 1e-6);
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                    */
/* ------------------------------------------------------------------------ */

/* Runs before the test takes its own SDL reference, so SDL_WasInit shows the
 * adapter's reference counting. */
static void test_invalid_args_and_lifecycle(void)
{
    static const SDL_Scancode defaults[12] = {
        SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
        SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_A,
        SDL_SCANCODE_D, SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_KP_ENTER};
    PIXELICA_InputManager manager;
    PIXELICA_InputManager other_manager;
    PIXELICA_InputSdl3 adapter;
    PIXELICA_InputSdl3 second;

    memset(&adapter, 0, sizeof(adapter));
    memset(&second, 0, sizeof(second));
    pixelica_input_manager_init(&manager);
    pixelica_input_manager_init(&other_manager);
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) == 0);

    /* Invalid init arguments change nothing and never touch SDL. */
    CHECK(pixelica_input_sdl3_init(NULL, &manager, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_init(&adapter, NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_init(&adapter, &manager, 4u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_init(&adapter, &manager, 255u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(!adapter.initialized);
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) == 0);

    /* Use before init / NULL. */
    CHECK(pixelica_input_sdl3_poll(NULL, true) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_poll(&adapter, true) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_set_key(NULL, 0u, SDL_SCANCODE_A) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 0u, SDL_SCANCODE_A) == PIXELICA_ERR_INVALID_ARG);
    pixelica_input_sdl3_shutdown(NULL);
    pixelica_input_sdl3_shutdown(&adapter);
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) == 0);

    /* Owned sources are neutralised by init; source 3 belongs to someone else. */
    submit_custom(&manager, 0u);
    submit_custom(&manager, 1u);
    submit_custom(&manager, 2u);
    submit_custom(&manager, 3u);
    CHECK(pixelica_input_sdl3_init(&adapter, &manager, 2u) == PIXELICA_OK);
    CHECK(adapter.initialized);
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
    CHECK(!source_state(&manager, 0u)->connected);
    CHECK(!source_state(&manager, 1u)->connected);
    CHECK(!source_state(&manager, 2u)->connected);
    check_custom_intact(&manager, 3u);

    /* Double init is rejected without leaking a reference or touching state. */
    CHECK(pixelica_input_sdl3_init(&adapter, &other_manager, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(adapter.manager == &manager);
    CHECK(adapter.pad_count == 2u);

    /* Default key map. */
    for (size_t i = 0u; i < 12u; ++i) {
        CHECK(adapter.keys[i] == defaults[i]);
    }
    for (size_t i = 12u; i < PIXELICA_INPUT_MAX_BUTTONS; ++i) {
        CHECK(adapter.keys[i] == SDL_SCANCODE_UNKNOWN);
    }

    /* Key remapping validation: rejected calls leave the map unchanged. */
    CHECK(pixelica_input_sdl3_set_key(&adapter, 12u, SDL_SCANCODE_Z) == PIXELICA_OK);
    CHECK(adapter.keys[12] == SDL_SCANCODE_Z);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 63u, (SDL_Scancode)(SDL_SCANCODE_COUNT - 1)) == PIXELICA_OK);
    CHECK(adapter.keys[63] == (SDL_Scancode)(SDL_SCANCODE_COUNT - 1));
    CHECK(pixelica_input_sdl3_set_key(&adapter, 0u, SDL_SCANCODE_Z) == PIXELICA_OK); /* duplicates allowed */
    CHECK(adapter.keys[0] == SDL_SCANCODE_Z && adapter.keys[12] == SDL_SCANCODE_Z);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 0u, SDL_SCANCODE_UNKNOWN) == PIXELICA_OK); /* unmap */
    CHECK(adapter.keys[0] == SDL_SCANCODE_UNKNOWN);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 12u, SDL_SCANCODE_X) == PIXELICA_OK);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 64u, SDL_SCANCODE_A) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 255u, SDL_SCANCODE_A) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 12u, SDL_SCANCODE_COUNT) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 12u, (SDL_Scancode)(SDL_SCANCODE_COUNT + 10)) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_sdl3_set_key(&adapter, 12u, (SDL_Scancode)(-1)) == PIXELICA_ERR_INVALID_ARG);
    CHECK(adapter.keys[12] == SDL_SCANCODE_X);

    /* A second adapter on another manager shares SDL's subsystem by refcount. */
    CHECK(pixelica_input_sdl3_init(&second, &other_manager, 0u) == PIXELICA_OK);
    pixelica_input_sdl3_shutdown(&adapter);
    CHECK(!adapter.initialized);
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
    check_custom_intact(&manager, 3u);
    pixelica_input_sdl3_shutdown(&adapter); /* already shut down: must not drop second's reference */
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
    pixelica_input_sdl3_shutdown(&second);
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) == 0);
}

static void test_full_mask_snapshot(void)
{
    PIXELICA_InputManager manager;
    PIXELICA_InputSdl3 adapter;
    TestPad pad;

    adapter_start(&adapter, &manager, 2u);
    CHECK(pad_attach(&pad, FULL_BUTTON_MASK, FULL_AXIS_MASK));

    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 0u)->connected); /* keyboard */
    CHECK(source_state(&manager, 0u)->buttons == 0u); /* no video: no key can be down */
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == 0u);
    CHECK(!source_state(&manager, 2u)->connected); /* no second pad */
    CHECK(SDL_GetGamepadFromID(pad.id) != NULL);

    /* Buttons: raw bit N = SDL_GamepadButton N. */
    pad_button(&pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pad_button(&pad, SDL_GAMEPAD_BUTTON_START, true);
    pad_button(&pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    pad_button(&pad, SDL_GAMEPAD_BUTTON_TOUCHPAD, true);
    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons ==
          (BIT(SDL_GAMEPAD_BUTTON_SOUTH) | BIT(SDL_GAMEPAD_BUTTON_START) |
           BIT(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) | BIT(SDL_GAMEPAD_BUTTON_TOUCHPAD)));
    pad_button(&pad, SDL_GAMEPAD_BUTTON_START, false);
    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 1u)->buttons ==
          (BIT(SDL_GAMEPAD_BUTTON_SOUTH) | BIT(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) |
           BIT(SDL_GAMEPAD_BUTTON_TOUCHPAD)));

    /* Axes: raw index N = SDL_GamepadAxis N; sticks -32768 -> -1, 32767 -> 1. */
    pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_RIGHTX, 16384);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_RIGHTY, -16384);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    poll_ok(&adapter, true);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_LEFTX], -1.0, 1e-6);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_LEFTY], 1.0, 1e-6);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_RIGHTX], 16384.0 / 32767.0, 1e-5);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_RIGHTY], -0.5, 1e-6);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER], 0.0, 1e-6);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER], 1.0, 1e-6);
    CHECK_NEAR(source_state(&manager, 1u)->axes[6], 0.0, 0.0); /* axes 6 and 7 stay unused */
    CHECK_NEAR(source_state(&manager, 1u)->axes[7], 0.0, 0.0);
    /* The buttons are unchanged by an axis-only update. */
    CHECK(source_state(&manager, 1u)->buttons ==
          (BIT(SDL_GAMEPAD_BUTTON_SOUTH) | BIT(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) |
           BIT(SDL_GAMEPAD_BUTTON_TOUCHPAD)));

    /* Half-pulled trigger lands in [0,1]; no value leaves its range. */
    pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    poll_ok(&adapter, true);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER], 0.5, 0.01);
    for (unsigned axis = 0u; axis < PIXELICA_INPUT_MAX_AXES; ++axis) {
        float value = source_state(&manager, 1u)->axes[axis];
        CHECK(value >= -1.0f && value <= 1.0f);
    }

    /* Release everything. */
    pad_button(&pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    pad_button(&pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    pad_button(&pad, SDL_GAMEPAD_BUTTON_TOUCHPAD, false);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTX, 0);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTY, 0);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_RIGHTY, 0);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 1u)->buttons == 0u);
    for (unsigned axis = 0u; axis < PIXELICA_INPUT_MAX_AXES; ++axis) {
        CHECK_NEAR(source_state(&manager, 1u)->axes[axis], 0.0, 1e-6);
    }

    /* Shutdown neutralises the sources and closes the gamepad handle. */
    pixelica_input_sdl3_shutdown(&adapter);
    CHECK(!source_state(&manager, 0u)->connected);
    CHECK(!source_state(&manager, 1u)->connected);
    CHECK(SDL_GetGamepadFromID(pad.id) == NULL);
    pad_detach(&pad);
}

/* A pad that reports only some buttons/axes: absent controls read as zero and
 * the present ones keep their SDL gamepad indexes. */
static void test_reduced_mask_snapshot(void)
{
    PIXELICA_InputManager manager;
    PIXELICA_InputSdl3 adapter;
    TestPad pad;
    const Uint32 buttons = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_START);
    const Uint32 axes = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    adapter_start(&adapter, &manager, 1u);
    CHECK(pad_attach(&pad, buttons, axes));
    /* SDL_OpenGamepad initializes trigger zero points; drive it after opening. */
    poll_ok(&adapter, true);
    pad_button(&pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pad_button(&pad, SDL_GAMEPAD_BUTTON_START, true);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
    pad_axis(&pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    poll_ok(&adapter, true);

    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == (BIT(SDL_GAMEPAD_BUTTON_SOUTH) | BIT(SDL_GAMEPAD_BUTTON_START)));
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_LEFTX], 1.0, 1e-6);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_LEFTY], 0.0, 0.0);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_RIGHTX], 0.0, 0.0);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_RIGHTY], 0.0, 0.0);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER], 0.0, 0.0);
    CHECK_NEAR(source_state(&manager, 1u)->axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER], 1.0, 1e-6);

    pixelica_input_sdl3_shutdown(&adapter);
    pad_detach(&pad);
}

/* Stable slots, hot-unplug, "no free slot" and reconnect. */
static void test_slots_and_hotplug(void)
{
    PIXELICA_InputManager manager;
    PIXELICA_InputSdl3 adapter;
    TestPad a;
    TestPad b;
    TestPad c;
    TestPad d;

    adapter_start(&adapter, &manager, 2u);

    /* Two pads present at the first poll take slots 1 and 2 in SDL order. */
    CHECK(pad_attach(&a, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    CHECK(pad_attach(&b, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&a, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pad_button(&b, SDL_GAMEPAD_BUTTON_START, true);
    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == BIT(SDL_GAMEPAD_BUTTON_SOUTH));
    CHECK(source_state(&manager, 2u)->connected);
    CHECK(source_state(&manager, 2u)->buttons == BIT(SDL_GAMEPAD_BUTTON_START));
    CHECK(SDL_GetGamepadFromID(a.id) != NULL);
    CHECK(SDL_GetGamepadFromID(b.id) != NULL);

    /* A third pad has no slot: it stays closed and disturbs nothing. */
    CHECK(pad_attach(&c, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&c, SDL_GAMEPAD_BUTTON_EAST, true);
    poll_ok(&adapter, true);
    CHECK(SDL_GetGamepadFromID(c.id) == NULL);
    CHECK(source_state(&manager, 1u)->buttons == BIT(SDL_GAMEPAD_BUTTON_SOUTH));
    CHECK(source_state(&manager, 2u)->buttons == BIT(SDL_GAMEPAD_BUTTON_START));

    /* Unplug A: slot 1 goes neutral, B keeps slot 2 (no shifting), and C is not
     * put into the freed slot during the same poll. */
    pad_detach(&a);
    poll_ok(&adapter, true);
    CHECK(!source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == 0u);
    CHECK(source_state(&manager, 2u)->connected);
    CHECK(source_state(&manager, 2u)->buttons == BIT(SDL_GAMEPAD_BUTTON_START));
    CHECK(SDL_GetGamepadFromID(c.id) == NULL);

    /* The next poll gives the free slot to C. */
    poll_ok(&adapter, true);
    CHECK(SDL_GetGamepadFromID(c.id) != NULL);
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == BIT(SDL_GAMEPAD_BUTTON_EAST));
    CHECK(source_state(&manager, 2u)->buttons == BIT(SDL_GAMEPAD_BUTTON_START));

    /* Unplug everything, then reconnect a new device: lowest free slot. */
    pad_detach(&b);
    pad_detach(&c);
    poll_ok(&adapter, true);
    CHECK(!source_state(&manager, 1u)->connected);
    CHECK(!source_state(&manager, 2u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == 0u);
    CHECK(source_state(&manager, 2u)->buttons == 0u);

    CHECK(pad_attach(&d, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&d, SDL_GAMEPAD_BUTTON_NORTH, true);
    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == BIT(SDL_GAMEPAD_BUTTON_NORTH));
    CHECK(!source_state(&manager, 2u)->connected);

    pixelica_input_sdl3_shutdown(&adapter);
    CHECK(SDL_GetGamepadFromID(d.id) == NULL);
    pad_detach(&d);
}

/* A device replaced inside one poll must not hand its held state to the
 * replacement: the manager sees one neutral sample in between. */
static void test_replacement_is_neutral_for_one_tick(void)
{
    PIXELICA_InputManager manager;
    PIXELICA_InputSdl3 adapter;
    TestPad x;
    TestPad y;
    PIXELICA_InputActionState state;

    adapter_start(&adapter, &manager, 1u);
    bind_button_to_a(&manager, 1u, (uint8_t)SDL_GAMEPAD_BUTTON_SOUTH);

    CHECK(pad_attach(&x, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&x, SDL_GAMEPAD_BUTTON_SOUTH, true);
    poll_ok(&adapter, true);
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(state.held && state.pressed);

    /* X leaves and Y, also holding SOUTH, arrives before the next poll. */
    pad_detach(&x);
    CHECK(pad_attach(&y, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&y, SDL_GAMEPAD_BUTTON_SOUTH, true);

    poll_ok(&adapter, true);
    CHECK(!source_state(&manager, 1u)->connected);
    CHECK(SDL_GetGamepadFromID(y.id) == NULL);
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(!state.held && state.released);

    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == BIT(SDL_GAMEPAD_BUTTON_SOUTH));
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(state.held && state.pressed && !state.released);

    pixelica_input_sdl3_shutdown(&adapter);
    pad_detach(&y);
}

/* accept_input=false: neutral for every owned source, hot-plug still tracked. */
static void test_accept_input(void)
{
    PIXELICA_InputManager manager;
    PIXELICA_InputSdl3 adapter;
    TestPad p;
    TestPad q;
    PIXELICA_InputActionState state;

    adapter_start(&adapter, &manager, 1u);
    bind_button_to_a(&manager, 1u, (uint8_t)SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(pad_attach(&p, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&p, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pad_axis(&p, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);

    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 0u)->connected);
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == BIT(SDL_GAMEPAD_BUTTON_SOUTH));
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(state.held && state.pressed);

    /* Focus lost while the button is still down. */
    poll_ok(&adapter, false);
    CHECK(!source_state(&manager, 0u)->connected);
    CHECK(source_state(&manager, 0u)->buttons == 0u);
    CHECK(!source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == 0u);
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(!state.held && state.released);
    poll_ok(&adapter, false);
    CHECK(!source_state(&manager, 1u)->connected);

    /* Swap the device while unfocused: it is tracked but not reported. */
    pad_detach(&p);
    CHECK(pad_attach(&q, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&q, SDL_GAMEPAD_BUTTON_SOUTH, true);
    poll_ok(&adapter, false); /* slot of p is freed */
    poll_ok(&adapter, false); /* q takes it, still neutral */
    CHECK(SDL_GetGamepadFromID(q.id) != NULL);
    CHECK(!source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == 0u);

    /* Focus regained: normal sampling resumes with the current device state. */
    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 0u)->connected);
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(source_state(&manager, 1u)->buttons == BIT(SDL_GAMEPAD_BUTTON_SOUTH));
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(state.held && state.pressed);

    pixelica_input_sdl3_shutdown(&adapter);
    pad_detach(&q);
}

/* The adapter never ticks the manager: edges appear only after the caller's tick. */
static void test_manager_tick_edges(void)
{
    PIXELICA_InputManager manager;
    PIXELICA_InputSdl3 adapter;
    TestPad pad;
    PIXELICA_InputActionState state;

    adapter_start(&adapter, &manager, 1u);
    bind_button_to_a(&manager, 1u, (uint8_t)SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(pad_attach(&pad, FULL_BUTTON_MASK, FULL_AXIS_MASK));

    poll_ok(&adapter, true);
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(!state.held && !state.pressed && !state.released);

    pad_button(&pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    poll_ok(&adapter, true);
    poll_ok(&adapter, true); /* several polls per tick: still one press */
    state = action_a(&manager);
    CHECK(!state.held && !state.pressed); /* poll alone changes no action state */
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(state.held && state.pressed && !state.released);

    poll_ok(&adapter, true);
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(state.held && !state.pressed && !state.released);

    pad_button(&pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    poll_ok(&adapter, true);
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(!state.held && !state.pressed && state.released);

    poll_ok(&adapter, true);
    pixelica_input_manager_tick(&manager);
    state = action_a(&manager);
    CHECK(!state.held && !state.pressed && !state.released);

    pixelica_input_sdl3_shutdown(&adapter);
    pad_detach(&pad);
}

/* Sources above the pad count belong to custom hardware and are never written. */
static void test_reserved_sources_untouched(void)
{
    PIXELICA_InputManager manager;
    PIXELICA_InputSdl3 adapter;
    TestPad pad;
    TestPad extra;

    /* One pad slot: sources 2 and 3 are custom. */
    pixelica_input_manager_init(&manager);
    submit_custom(&manager, 2u);
    submit_custom(&manager, 3u);
    memset(&adapter, 0, sizeof(adapter));
    CHECK(pixelica_input_sdl3_init(&adapter, &manager, 1u) == PIXELICA_OK);
    check_custom_intact(&manager, 2u);
    check_custom_intact(&manager, 3u);

    CHECK(pad_attach(&pad, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    CHECK(pad_attach(&extra, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 1u)->connected);
    CHECK(SDL_GetGamepadFromID(extra.id) == NULL); /* the second pad must not spill into source 2 */
    check_custom_intact(&manager, 2u);
    check_custom_intact(&manager, 3u);

    poll_ok(&adapter, false);
    check_custom_intact(&manager, 2u);
    check_custom_intact(&manager, 3u);

    pad_detach(&pad);
    poll_ok(&adapter, true);
    poll_ok(&adapter, true);
    check_custom_intact(&manager, 2u);
    check_custom_intact(&manager, 3u);

    CHECK(pixelica_input_sdl3_set_key(&adapter, 1u, SDL_SCANCODE_B) == PIXELICA_OK);
    pixelica_input_sdl3_shutdown(&adapter);
    check_custom_intact(&manager, 2u);
    check_custom_intact(&manager, 3u);
    pad_detach(&extra);

    /* No pad slots: sources 1..3 are custom, only the keyboard (0) is owned. */
    pixelica_input_manager_init(&manager);
    submit_custom(&manager, 1u);
    submit_custom(&manager, 2u);
    memset(&adapter, 0, sizeof(adapter));
    CHECK(pixelica_input_sdl3_init(&adapter, &manager, 0u) == PIXELICA_OK);
    CHECK(pad_attach(&pad, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    poll_ok(&adapter, true);
    poll_ok(&adapter, false);
    poll_ok(&adapter, true);
    CHECK(source_state(&manager, 0u)->connected);
    CHECK(SDL_GetGamepadFromID(pad.id) == NULL); /* no slot, so the adapter never opens a pad */
    check_custom_intact(&manager, 1u);
    check_custom_intact(&manager, 2u);
    pixelica_input_sdl3_shutdown(&adapter);
    check_custom_intact(&manager, 1u);
    check_custom_intact(&manager, 2u);
    pad_detach(&pad);
}

/* poll() pumps SDL but leaves the application's queue alone. */
static void test_events_not_consumed(void)
{
    PIXELICA_InputManager manager;
    PIXELICA_InputSdl3 adapter;
    TestPad pad;
    SDL_Event event;
    bool found = false;

    adapter_start(&adapter, &manager, 1u);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    event.user.code = 4242;
    CHECK(SDL_PushEvent(&event));

    CHECK(pad_attach(&pad, FULL_BUTTON_MASK, FULL_AXIS_MASK));
    pad_button(&pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    poll_ok(&adapter, true);
    poll_ok(&adapter, true);
    poll_ok(&adapter, false);
    CHECK(SDL_HasEvent(SDL_EVENT_USER));

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_USER && event.user.code == 4242) {
            found = true;
        }
    }
    CHECK(found);

    pixelica_input_sdl3_shutdown(&adapter);
    pad_detach(&pad);
}

/* ------------------------------------------------------------------------ */

/* Keep real controllers out of SDL's list (so slots are deterministic) and let
 * virtual pads update without window focus. Virtual devices are never filtered
 * by the ignore list. */
static void configure_sdl(void)
{
    static const char *const off_hints[] = {
        SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_JOYSTICK_DIRECTINPUT,
        SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_JOYSTICK_MFI};

    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xFFFF/0xFFFF",
                            SDL_HINT_OVERRIDE);
    for (size_t i = 0u; i < sizeof(off_hints) / sizeof(off_hints[0]); ++i) {
        SDL_SetHintWithPriority(off_hints[i], "0", SDL_HINT_OVERRIDE);
    }
}

int main(void)
{
    int baseline = 0;
    SDL_JoystickID *listed;

    SDL_SetMainReady();
    configure_sdl();

    test_invalid_args_and_lifecycle();

    /* The test keeps its own reference so the virtual pads outlive the
     * adapters' init/shutdown pairs. */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_InitSubSystem(GAMEPAD) failed: %s\n", SDL_GetError());
        return 1;
    }
    listed = SDL_GetGamepads(&baseline);
    SDL_free(listed);
    if (baseline != 0) {
        fprintf(stderr, "FAIL: %d non-virtual gamepad(s) are visible to SDL; the test needs none\n", baseline);
        ++g_failures;
    } else {
        test_full_mask_snapshot();
        test_reduced_mask_snapshot();
        test_slots_and_hotplug();
        test_replacement_is_neutral_for_one_tick();
        test_accept_input();
        test_manager_tick_edges();
        test_reserved_sources_untouched();
        test_events_not_consumed();
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) == 0); /* every adapter reference was released */
    SDL_Quit();

    printf("pixelica_input_sdl3_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

/*
 * Logical buttons <-> user-assigned GPIOs. No pin is chosen here; every pin
 * comes from the board configuration (-1 = unassigned). No debouncing: the
 * game reads edges at the fixed update rate and the board may add RC/hardware
 * debouncing or a filter in front of this.
 */
#include "driver/gpio.h"
#include "esp_log.h"

#include "platform_esp32p4_internal.h"

esp_err_t pixelica_esp_input_validate(const PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT])
{
    for (int i = 0; i < (int)PIXELICA_ESP_BUTTON_COUNT; ++i) {
        const int pin = buttons[i].gpio_num;
        if (pin == PIXELICA_ESP_GPIO_UNASSIGNED) {
            continue;
        }
        if (pin < 0 || !GPIO_IS_VALID_GPIO(pin)) {
            ESP_LOGE(PIXELICA_ESP_TAG, "button %d: GPIO %d is not a valid GPIO of this chip", i, pin);
            return ESP_ERR_INVALID_ARG;
        }
        if (buttons[i].pull != PIXELICA_ESP_PULL_NONE && buttons[i].pull != PIXELICA_ESP_PULL_UP &&
            buttons[i].pull != PIXELICA_ESP_PULL_DOWN) {
            ESP_LOGE(PIXELICA_ESP_TAG, "button %d: invalid pull setting", i);
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; ++j) {
            if (buttons[j].gpio_num == pin) {
                ESP_LOGE(PIXELICA_ESP_TAG, "buttons %d and %d both use GPIO %d", j, i, pin);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

esp_err_t pixelica_esp_input_configure(const PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT])
{
    for (int i = 0; i < (int)PIXELICA_ESP_BUTTON_COUNT; ++i) {
        const int pin = buttons[i].gpio_num;
        if (pin == PIXELICA_ESP_GPIO_UNASSIGNED) {
            continue;
        }
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = buttons[i].pull == PIXELICA_ESP_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = buttons[i].pull == PIXELICA_ESP_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            ESP_LOGE(PIXELICA_ESP_TAG, "gpio_config(GPIO %d) failed: %s", pin, esp_err_to_name(err));
            for (int k = 0; k < i; ++k) { /* undo the pins configured so far */
                if (buttons[k].gpio_num != PIXELICA_ESP_GPIO_UNASSIGNED) {
                    gpio_reset_pin((gpio_num_t)buttons[k].gpio_num);
                }
            }
            return err;
        }
    }
    return ESP_OK;
}

void pixelica_esp_input_release(const PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT])
{
    for (int i = 0; i < (int)PIXELICA_ESP_BUTTON_COUNT; ++i) {
        if (buttons[i].gpio_num != PIXELICA_ESP_GPIO_UNASSIGNED) {
            gpio_reset_pin((gpio_num_t)buttons[i].gpio_num);
        }
    }
}

uint32_t pixelica_esp_input_read(const PIXELICA_EspButtonPin buttons[PIXELICA_ESP_BUTTON_COUNT])
{
    uint32_t mask = 0u;
    for (int i = 0; i < (int)PIXELICA_ESP_BUTTON_COUNT; ++i) {
        if (buttons[i].gpio_num == PIXELICA_ESP_GPIO_UNASSIGNED) {
            continue;
        }
        const int level = gpio_get_level((gpio_num_t)buttons[i].gpio_num);
        const bool pressed = buttons[i].active_low ? (level == 0) : (level != 0);
        if (pressed) {
            mask |= (uint32_t)1u << i; /* PIXELICA_ESP_BUTTON_* order == PIXELICA_BTN_* bit order */
        }
    }
    return mask;
}

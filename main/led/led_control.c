#include "led_control.h"

#include "esp_err.h"
#include "led_strip.h"

static led_strip_handle_t led = NULL;

void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = 21,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led));
    led_strip_clear(led);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!led) return;
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
}

void led_clear(void)
{
    if (!led) return;
    led_strip_clear(led);
    led_strip_refresh(led);
}

void led_blink_missing_card(void)
{
    static bool led_on = true;

    if (led_on) {
        led_set(255, 255, 255);
    } else {
        led_clear();
    }

    led_on = !led_on;
}

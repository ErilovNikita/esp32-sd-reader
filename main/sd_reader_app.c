#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_control.h"
#include "sd_card_logic.h"

static const char *TAG = "SD_READER";

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "Starting SD Card Reader");

    led_init();
    card_detect_init();

    led_set(0, 0, 255);

    bool last_present = card_present();

    if (last_present) {
        mount_card();
    } else {
        printf("\n[SD] Card not inserted\n");
        led_blink_missing_card();
    }

    while (true) {
        bool now_present = card_present();

        if (now_present != last_present) {
            vTaskDelay(pdMS_TO_TICKS(200));

            now_present = card_present();

            if (now_present != last_present) {
                last_present = now_present;

                if (now_present) {
                    mount_card();
                } else {
                    unmount_card();
                }
            }
        }

        if (!last_present) {
            led_blink_missing_card();
            vTaskDelay(pdMS_TO_TICKS(700));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
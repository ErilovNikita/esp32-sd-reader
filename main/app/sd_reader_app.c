#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led_control.h"
#include "sd_card_logic.h"
#include "web_server.h"

static const char *TAG = "SD_READER";

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_WARN);
    esp_log_level_set("WEB", ESP_LOG_WARN);

    init_nvs();

    led_init();
    card_detect_init();
    ESP_ERROR_CHECK(web_server_start());

    led_set(0, 0, 255);

    bool last_present = card_present();

    if (last_present) {
        mount_card();
    } else {
        sd_card_mark_missing();
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

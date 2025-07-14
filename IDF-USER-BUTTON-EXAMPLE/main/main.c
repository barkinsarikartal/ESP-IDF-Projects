#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define BOOT_BUTTON_GPIO GPIO_NUM_0
static const char *TAG = "USER_BUTTON";

void app_main(void) {
    // GPIO ayarı: BOOT tuşu giriş ve pull-up aktif
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    int last_state = 1;
    while (1) {
        int current_state = gpio_get_level(BOOT_BUTTON_GPIO);

        // Butona basma tespiti (HIGH -> LOW geçişi)
        if (last_state == 1 && current_state == 0) {
            ESP_LOGI(TAG, "Hello world");
        }

        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms debounce süresi
    }
}
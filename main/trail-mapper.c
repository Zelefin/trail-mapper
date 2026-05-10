#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "trail-mapper";

void app_main(void)
{
    uint32_t counter = 0;

    while (true) {
        ESP_LOGI(TAG, "Hello, World! heartbeat=%" PRIu32, counter++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#include "ro/app.hpp"
#include "esp_log.h"

static const char* TAG = "ro_main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "RO Controller starting");
    ro::app::start();
}

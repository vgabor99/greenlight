#include "driver/gpio.h"
#include "esp_log.h"
#include "board/board.h"
#include "board/usb_sense.h"

static const char *TAG = "greenlight";

void usb_sense_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << VBUS_SENSE_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb sense: D7 setup failed (%s)", esp_err_to_name(err));
    }
}

bool usb_sense_read(void)
{
    return gpio_get_level(VBUS_SENSE_PIN);
}

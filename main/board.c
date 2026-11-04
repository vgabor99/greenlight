#include "driver/gpio.h"
#include "board.h"

/* XIAO ESP32-C6 antenna switch: GPIO3 low enables it, GPIO14 low selects the onboard ceramic antenna. */
void rf_switch_init(void)
{
    gpio_config_t cfg = {.pin_bit_mask = (1ULL << 3) | (1ULL << 14), .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(3, 0);
    gpio_set_level(14, 0);
}

#include "driver/gpio.h"
#include "board.h"

void board_init_rf_switch(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << RF_SWITCH_ENABLE_PIN) | (1ULL << RF_ANTENNA_SELECT_PIN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(RF_SWITCH_ENABLE_PIN, 0);
    gpio_set_level(RF_ANTENNA_SELECT_PIN, 0);
}

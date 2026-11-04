#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "battery.h"
#include "board.h"
#include "inputs.h"
#include "sleep.h"

static const char *TAG = "greenlight";

#ifndef SIM_SLEEP
/* Each input wakes the chip on its own active level, so an input that is still active wakes it at once. The
 * internal pulls are set on the LP IO side, which the sleep code holds. */
static void enable_wake_pins(void)
{
    const uint64_t pir_mask = 1ULL << PIR_PIN, pair_button_mask = 1ULL << PAIR_BUTTON_PIN;
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(pir_mask, PIR_ACTIVE_LEVEL ? ESP_EXT1_WAKEUP_ANY_HIGH
                                                                               : ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(pair_button_mask, ESP_EXT1_WAKEUP_ANY_LOW));
    if (PIR_ACTIVE_LEVEL) {
        rtc_gpio_pullup_dis(PIR_PIN);  /* the external pull-down sets the idle level */
    } else {
        rtc_gpio_pullup_en(PIR_PIN);
    }
    rtc_gpio_pulldown_dis(PIR_PIN);
    rtc_gpio_pullup_en(PAIR_BUTTON_PIN);
    rtc_gpio_pulldown_dis(PAIR_BUTTON_PIN);
}
#endif

/* Built with -DSLEEP_TIMER_S=N, a timer also wakes it after N s, for bench tests (the USB port, and so a reset over
 * it, is gone while it sleeps). Built with -DSIM_SLEEP=1, the sleep is simulated instead: wait for either input,
 * then restart. USB stays up, so every wake logs and the console and a reset over USB keep working
 * (tools/console.py, paced_write_test.py). */
void sleep_until_input(void)
{
#ifdef SIM_SLEEP
    ESP_LOGI(TAG, "simulated deep sleep");
    while (!inputs_any_active()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "wake: %s", inputs_pair_button_pressed() ? "pair button" : "PIR");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
#else
    enable_wake_pins();
#ifdef SLEEP_TIMER_S
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_TIMER_S * 1000000ULL));
    ESP_LOGI(TAG, "deep sleep (timer wake after %d s)", SLEEP_TIMER_S);
#else
    ESP_LOGI(TAG, "deep sleep");
#endif
    vTaskDelay(pdMS_TO_TICKS(100));  /* let the log out before USB goes away */
    esp_deep_sleep_start();
#endif
}

/* Simulated like SLEEP; the final firmware replaces this with esp_deep_sleep_start() with VBUS as the only wake
 * source. The boot then finds the battery recovered. */
void sleep_until_usb_power(void)
{
    ESP_LOGI(TAG, "low battery: waiting for USB power");
    while (!battery_on_usb_power()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "wake: USB power");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

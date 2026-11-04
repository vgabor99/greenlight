#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "battery.h"
#include "epd.h"
#include "inputs.h"
#include "sleep.h"

static const char *TAG = "greenlight";

/* Deep sleep (DESIGN.md 3.6): the PIR (D0) and the pair button (D1) wake the chip, each on its own active level,
 * so an input that is still active wakes it at once. The wake is a reset: only NVS and RTC memory carry over.
 * The internal pulls are set on the LP IO side, which the sleep code holds. Built with -DSLEEP_TIMER_S=N, a timer
 * also wakes it after N s, for bench tests (the USB port, and so a reset over it, is gone while it sleeps).
 * Built with -DSIM_SLEEP=1, the sleep is simulated instead: wait for either input, then restart. USB stays up, so
 * every wake logs and the console and a reset over USB keep working (tools/console.py, paced_write_test.py). */
state_t st_sleep(void)
{
#ifdef SIM_SLEEP
    ESP_LOGI(TAG, "simulated deep sleep");
    while (!active()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "wake: %s", pair_active ? "pair button" : "PIR");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
#else
    const uint64_t pir = 1ULL << PIR_PIN, pair = 1ULL << PAIR_BUTTON_PIN;
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(pir, PIR_ACTIVE_LEVEL ? ESP_EXT1_WAKEUP_ANY_HIGH
                                                                          : ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(pair, ESP_EXT1_WAKEUP_ANY_LOW));
    if (PIR_ACTIVE_LEVEL) {
        rtc_gpio_pullup_dis(PIR_PIN);  /* the external pull-down sets the idle level */
    } else {
        rtc_gpio_pullup_en(PIR_PIN);
    }
    rtc_gpio_pulldown_dis(PIR_PIN);
    rtc_gpio_pullup_en(PAIR_BUTTON_PIN);
    rtc_gpio_pulldown_dis(PAIR_BUTTON_PIN);
#ifdef SLEEP_TIMER_S
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(SLEEP_TIMER_S * 1000000ULL));
    ESP_LOGI(TAG, "deep sleep (timer wake after %d s)", SLEEP_TIMER_S);
#else
    ESP_LOGI(TAG, "deep sleep");
#endif
    vTaskDelay(pdMS_TO_TICKS(100));  /* let the log out before USB goes away */
    esp_deep_sleep_start();
#endif
    return ST_BOOT;
}

/* Low battery (DESIGN.md 6.6): the battery screen is on the panel and the radio is off. The PIR and the pair
 * button are ignored: only USB power wakes the device, and the boot then finds the battery recovered. Simulated
 * like SLEEP; the final firmware replaces this with esp_deep_sleep_start() with VBUS as the only wake source. */
state_t st_low_battery(void)
{
    ESP_LOGI(TAG, "low battery: waiting for USB power");
    while (!usb_powered()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "wake: USB power");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ST_BOOT;
}

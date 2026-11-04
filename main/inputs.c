#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "epd.h"
#include "inputs.h"

static const char *TAG = "greenlight";

#define OCC_HOLD_MS     5000   /* occupancy: the PIR was seen within this long (a PIR wake counts as seen at boot) */

/* ---- Inputs: PIR (D0) and pair button (D1), level based, polled and debounced by a task ---- */
#define INPUT_POLL_MS        10
#define INPUT_DEBOUNCE_POLLS 5  /* a level must be stable for this many polls (50 ms) */

volatile bool pir_active;
volatile bool pair_active;
static volatile TickType_t last_activity;  /* last time any input was active */
static volatile TickType_t pair_since;     /* when the pair button went down (valid while pair_active) */
static volatile TickType_t last_pir;       /* last time the PIR was active */

bool active(void)
{
    return pir_active || pair_active;
}

uint32_t ms_since(TickType_t t)
{
    return pdTICKS_TO_MS(xTaskGetTickCount() - t);
}

/* 0 while an input is active, else the time since the last activity: the RUN_MS countdown. */
uint32_t idle_ms(void)
{
    return active() ? 0 : ms_since(last_activity);
}

/* Reported occupancy (DESIGN.md 6.4): a short PIR pulse, the one that woke the device included, is long over by
 * the time the wake's reports go out, so it holds for OCC_HOLD_MS. */
bool occupied(void)
{
    return ms_since(last_pir) < OCC_HOLD_MS;
}

uint32_t pair_held_ms(void)
{
    return pair_active ? ms_since(pair_since) : 0;
}

void touch_activity(void)
{
    last_activity = xTaskGetTickCount();
}

static void input_task(void *arg)
{
    int pir_n = 0, pair_n = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
        TickType_t now = xTaskGetTickCount();
        bool pir = gpio_get_level(PIR_PIN) == PIR_ACTIVE_LEVEL;
        bool pair = gpio_get_level(PAIR_BUTTON_PIN) == PAIR_BUTTON_ACTIVE_LEVEL;
        if (pir != pir_active) {
            if (++pir_n >= INPUT_DEBOUNCE_POLLS) {
                pir_active = pir;
                pir_n = 0;
            }
        } else {
            pir_n = 0;
        }
        if (pair != pair_active) {
            if (++pair_n >= INPUT_DEBOUNCE_POLLS) {
                pair_active = pair;
                pair_n = 0;
                if (pair) {
                    pair_since = now;
                }
            }
        } else {
            pair_n = 0;
        }
        if (pir_active || pair_active) {
            last_activity = now;
        }
        if (pir_active) {
            last_pir = now;
        }
    }
}

void inputs_init(void)
{
    /* Deep sleep left both pins held on the LP IO mux (st_sleep); gpio_config() routes them back to digital. */
    rtc_gpio_hold_dis(PIR_PIN);
    rtc_gpio_hold_dis(PAIR_BUTTON_PIN);
    gpio_config_t pair_cfg = {
        .pin_bit_mask = 1ULL << PAIR_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  /* button between the pin and GND */
    };
    ESP_ERROR_CHECK(gpio_config(&pair_cfg));
    /* Active-high PIR: no internal pull, the external pull-down sets the idle level. Active-low button: pull-up. */
    gpio_config_t pir_cfg = {
        .pin_bit_mask = 1ULL << PIR_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = PIR_ACTIVE_LEVEL ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pir_cfg));
    /* Start with the real levels: a button still held from the wake-up counts at once. */
    pir_active = gpio_get_level(PIR_PIN) == PIR_ACTIVE_LEVEL;
    pair_active = gpio_get_level(PAIR_BUTTON_PIN) == PAIR_BUTTON_ACTIVE_LEVEL;
    TickType_t now = xTaskGetTickCount();
    pair_since = last_activity = now;
    /* Ticks start at 0 on every wake: a PIR wake counts as seen at boot, without a PIR the hold starts out expired
     * (unsigned, so "-2 holds" is long ago). */
    bool pir_wake = esp_sleep_get_ext1_wakeup_status() & (1ULL << PIR_PIN);
    last_pir = pir_active ? now : pir_wake ? 0 : -2 * pdMS_TO_TICKS(OCC_HOLD_MS);
    ESP_LOGI(TAG, "inputs at boot: PIR(D0)=%d pair(D1)=%d", gpio_get_level(PIR_PIN), gpio_get_level(PAIR_BUTTON_PIN));
    xTaskCreate(input_task, "inputs", 2048, NULL, 5, NULL);
}

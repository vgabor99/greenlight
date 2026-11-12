#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "board/board.h"
#include "board/inputs.h"
#include "util/elapsed_time.h"

static const char *TAG = "greenlight";

#define OCCUPANCY_HOLD_MS    5000  /* occupancy: the PIR was seen within this long (a PIR wake counts as seen at boot) */
#define INPUT_POLL_MS        10
#define INPUT_DEBOUNCE_POLLS 5     /* a level must be stable for this many polls (50 ms) */

/* Debounced input states and when they last changed, written by the polling task */
static volatile bool       pir_active;
static volatile bool       pair_button_pressed;
static volatile TickType_t last_activity_tick;       /* last time any input was active */
static volatile TickType_t pair_button_pressed_tick; /* when the pair button went down (valid while pressed) */
static volatile TickType_t last_pir_tick;            /* last time the PIR was active */

bool inputs_any_active(void)
{
    return pir_active || pair_button_pressed;
}

bool inputs_pair_button_pressed(void)
{
    return pair_button_pressed;
}

uint32_t inputs_idle_ms(void)
{
    return inputs_any_active() ? 0 : elapsed_ms(last_activity_tick);
}

/* A short PIR pulse, the one that woke the device included, is long over by the time the wake's reports go out,
 * so it holds for OCCUPANCY_HOLD_MS. */
bool inputs_occupied(void)
{
    return elapsed_ms(last_pir_tick) < OCCUPANCY_HOLD_MS;
}

uint32_t inputs_pair_button_held_ms(void)
{
    return pair_button_pressed ? elapsed_ms(pair_button_pressed_tick) : 0;
}

static bool read_pir(void)
{
    return gpio_get_level(PIR_PIN) == PIR_ACTIVE_LEVEL;
}

static bool read_pair_button(void)
{
    return gpio_get_level(PAIR_BUTTON_PIN) == PAIR_BUTTON_ACTIVE_LEVEL;
}

/* The state follows the level once the level has differed from it for INPUT_DEBOUNCE_POLLS polls in a row.
 * True when the state changed. */
static bool debounce(bool level, volatile bool *state, int *differing_polls)
{
    if (level == *state) {
        *differing_polls = 0;
        return false;
    }
    if (++*differing_polls < INPUT_DEBOUNCE_POLLS) {
        return false;
    }
    *state = level;
    *differing_polls = 0;
    return true;
}

static void input_task(void *arg)
{
    int pir_differing_polls = 0, pair_button_differing_polls = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
        TickType_t now = xTaskGetTickCount();
        bool       pir = read_pir();
        bool       pair_button = read_pair_button();
        debounce(pir, &pir_active, &pir_differing_polls);
        if (debounce(pair_button, &pair_button_pressed, &pair_button_differing_polls) && pair_button_pressed) {
            pair_button_pressed_tick = now;
        }
        if (pir_active || pair_button_pressed) {
            last_activity_tick = now;
        }
        if (pir_active) {
            last_pir_tick = now;
        }
    }
}

static void configure_pins(void)
{
    /* Deep sleep left both pins held on the LP IO mux (sleep.c); gpio_config() routes them back to digital. */
    rtc_gpio_hold_dis(PIR_PIN);
    rtc_gpio_hold_dis(PAIR_BUTTON_PIN);
    gpio_config_t pair_button_config = {
        .pin_bit_mask = 1ULL << PAIR_BUTTON_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,  /* button between the pin and GND */
    };
    ESP_ERROR_CHECK(gpio_config(&pair_button_config));
    /* Active-high PIR: no internal pull, the external pull-down sets the idle level. Active-low button: pull-up. */
    gpio_config_t pir_config = {
        .pin_bit_mask = 1ULL << PIR_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = PIR_ACTIVE_LEVEL ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pir_config));
}

void inputs_init(void)
{
    configure_pins();
    /* Start with the real levels: a button still held from the wake-up counts at once. */
    pir_active          = read_pir();
    pair_button_pressed = read_pair_button();
    TickType_t now = xTaskGetTickCount();
    pair_button_pressed_tick = last_activity_tick = now;
    /* Ticks start at 0 on every wake: a PIR wake counts as seen at boot, without a PIR the hold starts out expired
     * (unsigned, so "-2 holds" is long ago). */
    bool woken_by_pir = esp_sleep_get_ext1_wakeup_status() & (1ULL << PIR_PIN);
    last_pir_tick = pir_active ? now : woken_by_pir ? 0 : -2 * pdMS_TO_TICKS(OCCUPANCY_HOLD_MS);
    ESP_LOGI(TAG, "inputs at boot: PIR(D0)=%d pair(D1)=%d", gpio_get_level(PIR_PIN), gpio_get_level(PAIR_BUTTON_PIN));
    xTaskCreate(input_task, "inputs", 2048, NULL, 5, NULL);
}

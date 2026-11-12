#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app/keep_awake.h"

/* Tick count when the last hold ends. Ticks start at 0 on every boot, so 0 is "expired". */
static TickType_t awake_until_tick;

/* The Zigbee task (HA's writes) and the state machine both extend the deadline: the max needs a lock. */
static portMUX_TYPE awake_until_lock = portMUX_INITIALIZER_UNLOCKED;

/* Ticks from now until `tick`, negative once it has passed (signed, so a tick count wrap does not matter). */
static int32_t ticks_until(TickType_t tick)
{
    return (int32_t)(tick - xTaskGetTickCount());
}

void keep_awake_for(uint32_t ms)
{
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    taskENTER_CRITICAL(&awake_until_lock);
    if ((int32_t)(until - awake_until_tick) > 0) {
        awake_until_tick = until;
    }
    taskEXIT_CRITICAL(&awake_until_lock);
}

bool keep_awake_expired(void)
{
    taskENTER_CRITICAL(&awake_until_lock);
    TickType_t until = awake_until_tick;
    taskEXIT_CRITICAL(&awake_until_lock);
    return ticks_until(until) <= 0;
}

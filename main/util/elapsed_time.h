#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Milliseconds since the tick count `since`. */
static inline uint32_t elapsed_ms(TickType_t since)
{
    return pdTICKS_TO_MS(xTaskGetTickCount() - since);
}

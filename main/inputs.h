#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

extern volatile bool pir_active;
extern volatile bool pair_active;

bool     active(void);
uint32_t ms_since(TickType_t t);
uint32_t idle_ms(void);
bool     occupied(void);
uint32_t pair_held_ms(void);
void     touch_activity(void);
void     inputs_init(void);

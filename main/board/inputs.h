#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The PIR (D0) and the pair button (D1): level based, polled and debounced by a task. */

/* Configure the pins and start the polling task. */
void inputs_init(void);

bool inputs_any_active(void);
bool inputs_pair_button_pressed(void);

/* 0 while an input is active, else the time since one last was (since boot if none was). */
uint32_t inputs_idle_ms(void);

/* How long the pair button has been held down, 0 while it is up. */
uint32_t inputs_pair_button_held_ms(void);

/* Reported occupancy (docs/zigbee.md#when-the-device-reports): the PIR was active within the occupancy hold time. */
bool inputs_occupied(void);

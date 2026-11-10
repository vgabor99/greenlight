#pragma once

#include <stdint.h>

/* Battery measurement on D2 (docs/hardware.md#power-and-battery): VBAT through a 1:2 divider into the ADC. */

#include <stdbool.h>

/* Measure (calibrated, averaged) and return VBAT in mV (0 if the ADC failed); with log, also log the pin voltage
 * and the VBAT it means. Sets the ADC up on the first call (from battery_init(), before the console task starts);
 * safe to call from several tasks after that. */
uint16_t battery_sense_read_mv(bool log);

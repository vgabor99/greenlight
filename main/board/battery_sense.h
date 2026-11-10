#pragma once

#include <stdint.h>

/* Battery measurement on D2 (docs/hardware.md#power-and-battery): VBAT through a 1:2 divider into the ADC. */

/* Measure (calibrated, averaged), log the pin voltage and the VBAT it means, and return VBAT in mV (0 if the ADC
 * failed). Sets the ADC up on the first call. */
uint16_t battery_sense_read_mv(void);

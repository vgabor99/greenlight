#pragma once

#include <stdbool.h>

/* USB sense on the MTDO pad, GPIO7 (docs/hardware.md#power-and-battery): the 5V pin through a divider. Also
 * LOW_BATTERY's wake pin (board/sleep.c). */

/* The pin as a plain input without pulls: the divider sets the level. */
void usb_sense_init(void);

/* True while the 5V pin carries USB power. */
bool usb_sense_read(void);

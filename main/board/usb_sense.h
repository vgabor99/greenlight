#pragma once

#include <stdbool.h>

/* USB sense on D7 (docs/hardware.md#power-and-battery): the 5V pin through a divider. For now only read and
 * logged; USB power for the battery logic is still the console's simulation (board/battery.c). */

/* D7 as a plain input: no pull-up (GPIO17 comes out of reset with UART0 RX's pull-up), no pull-down. */
void usb_sense_init(void);

/* True while the 5V pin carries USB power. */
bool usb_sense_read(void);

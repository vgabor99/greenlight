#pragma once

/* The USB-Serial-JTAG console (tools/console.py): the battery simulation.
 * "bat <mV>" sets the simulated battery voltage, "usb 0|1" the simulated USB power, "bat" or "usb" alone print
 * them. Applied at once; the state loops pick them up. */
void console_init(void);

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Battery (DESIGN.md 6.6): simulated until the hardware is there. */
#define BATTERY_LOW_ENTER_MV 3500  /* below this the wake ends in LOW_BATTERY */
#define BATTERY_LOW_EXIT_MV  3700  /* LOW_BATTERY is left only at or above this, or on USB power */

/* Load the simulated values and the low-battery latch. */
void battery_init(void);

uint16_t battery_voltage_mv(void);
bool     battery_on_usb_power(void);

/* Low: not on USB power and below the threshold (the exit threshold while latched). */
bool battery_is_low(void);

/* The LOW_BATTERY latch, kept through the sleep. */
bool battery_low_latched(void);
void battery_set_low_latched(bool latched);

/* The console's battery simulation: set the values (kept through the sleep, a power cycle and a reflash, like a
 * real battery's charge), then announce them, which logs them and flags them for a report. */
void battery_simulate_voltage(uint16_t mv);
void battery_simulate_usb_power(bool on);
void battery_announce_simulation(void);

/* True once after each announced change of the simulation. */
bool battery_take_simulation_change(void);

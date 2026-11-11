#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Battery (docs/behaviour.md#low-battery): the measured voltage and the sensed USB power, each with a console
 * override. */
#define BATTERY_LOW_ENTER_MV 3500  /* below this the wake ends in LOW_BATTERY */
#define BATTERY_LOW_EXIT_MV  3700  /* LOW_BATTERY is left only at or above this, or on USB power */

/* Measure the voltage; load the console's values and the low-battery latch. */
void battery_init(void);

/* Measure the voltage again (the mid-wake check); battery_voltage_mv() returns the last measurement. */
void     battery_measure(void);
uint16_t battery_voltage_mv(void);
bool     battery_on_usb_power(void);

/* Low: not on USB power and below the threshold (the exit threshold while latched). */
bool battery_is_low(void);

/* The LOW_BATTERY latch, kept through the sleep. */
bool battery_low_latched(void);
void battery_set_low_latched(bool latched);

/* The console's battery simulation: set the values (kept through the sleep, a power cycle and a reflash, like a
 * real battery's charge; voltage 0 = the measured one), then announce them, which logs them and flags them for a
 * report. */
void battery_simulate_voltage(uint16_t mv);
#define BATTERY_USB_SENSED 0  /* the USB sense pin decides */
#define BATTERY_USB_ON     1
#define BATTERY_USB_OFF    2
void battery_simulate_usb_power(uint8_t usb);
void battery_announce_simulation(void);

/* True once after each announced change of the simulation. */
bool battery_take_simulation_change(void);

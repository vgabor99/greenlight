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
 * real battery's charge; voltage 0 = the measured one), then announce them, which logs them and counts the change. */
void battery_simulate_voltage(uint16_t mv);
typedef enum {  /* stored in NVS by value (STORE_KEY_SIMULATED_USB): keep the values */
    BATTERY_USB_SENSED = 0,  /* the USB sense pin decides */
    BATTERY_USB_ON     = 1,
    BATTERY_USB_OFF    = 2,
} battery_usb_override_t;
void battery_simulate_usb_power(battery_usb_override_t usb);
void battery_announce_simulation(void);

/* Announced changes of the simulation since boot: whoever shows or reports the battery acts when this moves. */
uint32_t battery_simulation_changes(void);

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ---- Battery (DESIGN.md 6.6) ---- */
#define BAT_ENTER_MV   3500  /* below this the wake ends in LOW_BATTERY */
#define BAT_EXIT_MV    3700  /* LOW_BATTERY is left only at or above this, or on USB power */
#define BAT_CHECK_MS   1000  /* mid-wake check period */
#define SIM_MV_DEFAULT 4100  /* simulated voltage until the console sets one */

extern volatile uint16_t sim_mv;
extern volatile bool     sim_usb;
extern volatile bool     bat_latched;
extern volatile bool     bat_changed;

void     battery_load(void);
uint16_t battery_mv(void);
bool     usb_powered(void);
bool     battery_low(void);
void     bat_latch(bool on);

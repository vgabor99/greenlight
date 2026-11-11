#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "screen/render.h"
#include "state_machine.h"

/* The screen for the device state (docs/display.md), drawn from the display model, on the proper or the debug screen. */

/* Power the panel up and clear it, at boot (in parallel with the Zigbee start-up). False if the panel failed. */
bool display_power_on(void);

/* End of the wake: clear the panel, draw the screen of final_state on it and power it off. Safe to cut power after. */
void display_power_off(device_state_t final_state);

/* The state to show; the next display_wait_ms() redraws. */
void display_show_state(device_state_t state);

/* This wake failed: from now on the proper screen shows `screen` (RENDER_UNPAIRED, RENDER_JOIN_FAILED,
 * RENDER_ERROR or RENDER_BATTERY) whatever the state. */
void            display_set_failure(render_screen_t screen);
render_screen_t display_failure(void);  /* RENDER_BLANK while the wake has not failed */

/* This wake's number, for the debug screen. */
void display_set_wake_number(uint32_t wake_number);

/* Draw the screen now (~0.5 s). Blocks the state loop; the input task keeps polling the inputs meanwhile. */
void display_redraw(void);

/* The state loops wait with this, so whatever arrives is drawn while it comes in. Writes that arrive during a
 * refresh are drawn together by the next one. */
void display_wait_ms(uint32_t ms);

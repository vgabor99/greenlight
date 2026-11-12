#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keeps the panel showing the application state (docs/display.md): takes snapshots of it (app/app_state.h), has the
 * UI draw them (screen/render.h) and sends the frames to the panel (panel/panel.h). */

/* Power the panel up and clear it, at boot (in parallel with the Zigbee start-up). False if the panel failed. */
bool display_power_on(void);

/* End of the wake: clear the panel, draw the state on it and power it off (only power it off if display_power_on()
 * failed). Safe to cut power after. */
void display_power_off(void);

/* Draw the state now (~0.5 s). Blocks the state loop; the input task keeps polling the inputs meanwhile. */
void display_redraw(void);

/* The state loops wait with this, so whatever changes is drawn while it comes in. Changes during a refresh are
 * drawn together by the next one. */
void display_wait_ms(uint32_t ms);

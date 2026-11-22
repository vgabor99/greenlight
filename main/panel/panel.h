#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "panel/epaper.h"

/* The output device (docs/display.md#refresh-policy): the e-paper panel as the application uses it. Partial
 * refreshes, a full clear every so many of them against ghosting, and nothing at all when the frame is already on
 * the panel. Every clear and refresh logs "panel: ..." with its duration and the BUSY level before it (BUSY hang
 * hunt). Panel failures are returned, never recovered from (docs/behaviour.md#failure-modes). */

/* Frames are 1-bit, PANEL_FRAME_BYTES bytes: rows of PANEL_WIDTH / 8 bytes, MSB first, 1 = white, 0 = black. */
#define PANEL_WIDTH       EPAPER_WIDTH
#define PANEL_HEIGHT      EPAPER_HEIGHT
#define PANEL_FRAME_BYTES EPAPER_FRAME_BYTES

/* Set the panel up (also after panel_power_off(): it releases the pins). After a deep sleep that panel_power_off()
 * ended, load the frame the panel shows into the controller (no update, ~60 ms); otherwise clear it to white
 * (~0.5 s). */
esp_err_t panel_power_on(void);

/* Drive the whole panel to white (~0.5 s). `reason` goes to the log. */
esp_err_t panel_clear(const char *reason);

/* Show the frame with a partial refresh (~0.5 s). `label` goes to the log. */
esp_err_t panel_refresh(const uint8_t *frame, const char *label);

/* Put the panel to sleep and drive its pins low; safe to cut power after. The image stays, and RTC memory keeps it
 * for the next panel_power_on() unless a clear or refresh failed. */
void panel_power_off(void);

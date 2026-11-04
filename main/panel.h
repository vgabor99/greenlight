#pragma once

#include <stdint.h>
#include "esp_err.h"

/* The panel as the application uses it (DESIGN.md 8): partial refreshes, a full clear every so many of them
 * against ghosting, and nothing at all when the frame is already on the panel. Every clear and refresh logs
 * "panel: ..." with its duration and the BUSY level before it (BUSY hang hunt). */

/* Drive the whole panel to white (~0.5 s). `reason` goes to the log. */
esp_err_t panel_clear(const char *reason);

/* Show the frame (FRAMEBUFFER_SIZE bytes) with a partial refresh (~0.5 s). `label` goes to the log. */
esp_err_t panel_refresh(const uint8_t *frame, const char *label);

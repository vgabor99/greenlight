#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Driver for the Waveshare 4.2" e-paper module (400x300, SSD1683 controller). Frames are 1-bit,
 * EPAPER_ROW_BYTES * EPAPER_HEIGHT bytes, rows MSB first, 1 = white, 0 = black. */
#define EPAPER_WIDTH     400
#define EPAPER_HEIGHT    300
#define EPAPER_ROW_BYTES   (EPAPER_WIDTH / 8)
#define EPAPER_FRAME_BYTES (EPAPER_ROW_BYTES * EPAPER_HEIGHT)

/* Set up the pins and SPI, reset the controller (hardware and software) and configure it. Also wakes the panel
 * after epaper_power_off(). */
esp_err_t epaper_init(void);

/* Software full refresh to white: one white pass (~0.5 s); resets ghosting. */
esp_err_t epaper_clear(void);

/* Show the frame with a partial refresh: ~0.45 s, no flash, ghosting accumulates. */
esp_err_t epaper_refresh_partial(const uint8_t *frame);

/* The BUSY line: 1 while the controller works (for the log). */
int epaper_busy_level(void);

/* Controller deep sleep, release SPI, drive all pins low and hold them; safe to cut power after. */
void epaper_power_off(void);

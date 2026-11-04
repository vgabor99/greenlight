#pragma once

#include <stdint.h>
#include "esp_err.h"

#define EPD_WIDTH  400
#define EPD_HEIGHT 300
#define EPD_STRIDE (EPD_WIDTH / 8)

esp_err_t epd_init(void);
/* Push a 1-bit framebuffer (EPD_STRIDE * EPD_HEIGHT bytes, 1 = white, 0 = black) and refresh. */
esp_err_t epd_clear(void);                         /* software full refresh to white: one white pass (~0.5 s); resets ghosting */
esp_err_t epd_display_partial(const uint8_t *fb);  /* ~0.45 s, no flash, ghosting accumulates */
void epd_sleep(void);     /* controller deep sleep; epd_init() (hardware reset) wakes it */
void epd_poweroff(void);  /* sleep + release SPI + drive all pins low and hold them; safe to cut power after */

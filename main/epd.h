#pragma once

#include <stdint.h>
#include "esp_err.h"

#define EPD_WIDTH  400
#define EPD_HEIGHT 300
#define EPD_STRIDE (EPD_WIDTH / 8)

/* Pin assignment: GPIO numbers on a Seeed XIAO ESP32-C6 (D-label in comment) */
#define EPD_PIN_SCK  19  /* D8 */
#define EPD_PIN_MOSI 18  /* D10 */
#define EPD_PIN_CS   20  /* D9 */
#define EPD_PIN_DC   22  /* D4 */
#define EPD_PIN_RST  23  /* D5 */
#define EPD_PIN_BUSY 21  /* D3 */

/* "Somebody is around" input: Panasonic EKMB PIR output (active high, floats when idle; external 100k
 * pull-down to GND, which keeps the output current under the sensor's 100 uA). Set PIR_ACTIVE_LEVEL to 0
 * for a push button to GND instead: the pin then gets the internal pull-up. Deep-sleep wake pin. */
#define PIR_PIN          0   /* D0 */
#define PIR_ACTIVE_LEVEL 1

/* Pair button between this pin and GND (internal pull-up, active low); deep-sleep wake pin (GPIO0-7) */
#define PAIR_BUTTON_PIN          1  /* D1 */
#define PAIR_BUTTON_ACTIVE_LEVEL 0

esp_err_t epd_init(void);
/* Push a 1-bit framebuffer (EPD_STRIDE * EPD_HEIGHT bytes, 1 = white, 0 = black) and refresh. */
esp_err_t epd_clear(void);                         /* software full refresh to white: one white pass (~0.5 s); resets ghosting */
esp_err_t epd_display_partial(const uint8_t *fb);  /* ~0.45 s, no flash, ghosting accumulates */
void epd_sleep(void);     /* controller deep sleep; epd_init() (hardware reset) wakes it */
void epd_poweroff(void);  /* sleep + release SPI + drive all pins low and hold them; safe to cut power after */

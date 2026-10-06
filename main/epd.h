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
#define EPD_PIN_DC   1   /* D1 */
#define EPD_PIN_RST  2   /* D2 */
#define EPD_PIN_BUSY 21  /* D3 */

esp_err_t epd_init(void);
/* Push a 1-bit framebuffer (EPD_STRIDE * EPD_HEIGHT bytes, 1 = white, 0 = black) and refresh.
 * Takes ~6 s: it repeats a quick refresh several times to reach full black (see epd.c). */
esp_err_t epd_display(const uint8_t *fb);
void epd_sleep(void);

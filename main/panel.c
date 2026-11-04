#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "epd.h"
#include "frame_dump.h"
#include "inputs.h"
#include "panel.h"

static const char *TAG = "greenlight";

#define CLEAR_EVERY 50  /* partial refreshes between full clears, against ghosting */

int               partials;   /* partial refreshes since the last full clear */

/* Panel diagnostics (BUSY hang hunt): every clear and refresh logs its duration, the BUSY level before it
 * starts and, for a refresh, how many pixels are black and how many change. */
static uint8_t prev_fb[EPD_STRIDE * EPD_HEIGHT];  /* last frame sent; all white after a clear */

esp_err_t panel_clear(const char *why)
{
    int        busy = gpio_get_level(EPD_PIN_BUSY);
    TickType_t t0   = xTaskGetTickCount();
    esp_err_t  err  = epd_clear();
    memset(prev_fb, 0xFF, sizeof(prev_fb));
    ESP_LOGI(TAG, "panel: clear (%s) %lu ms, busy before %d%s", why, (unsigned long)ms_since(t0), busy,
             err == ESP_OK ? "" : ", FAILED");
    return err;
}

/* Partial refresh of fb (~0.5 s), with a full clear every CLEAR_EVERY partials. Nothing when fb is already on
 * the panel: most writes from HA do not change the proper screen. */
esp_err_t panel_refresh(const uint8_t *fb, const char *label)
{
    if (memcmp(fb, prev_fb, sizeof(prev_fb)) == 0) {
        return ESP_OK;
    }
#ifdef SCREEN_DUMP
    dump_frame(fb, label);
#endif
    if (++partials >= CLEAR_EVERY) {
        partials = 0;
        if (panel_clear("periodic") != ESP_OK) {
            ESP_LOGE(TAG, "e-paper clear failed");
        }
    }
    int black = 0, changed = 0;
    for (size_t i = 0; i < sizeof(prev_fb); i++) {
        black += __builtin_popcount((uint8_t)~fb[i]);
        changed += __builtin_popcount(fb[i] ^ prev_fb[i]);
    }
    memcpy(prev_fb, fb, sizeof(prev_fb));
    int        busy = gpio_get_level(EPD_PIN_BUSY);
    TickType_t t0   = xTaskGetTickCount();
    esp_err_t  err  = epd_display_partial(fb);
    ESP_LOGI(TAG, "panel: refresh #%d (%s) %lu ms, busy before %d, black %d, changed %d%s", partials, label,
             (unsigned long)ms_since(t0), busy, black, changed, err == ESP_OK ? "" : ", FAILED");
    return err;
}

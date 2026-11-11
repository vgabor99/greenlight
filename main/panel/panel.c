#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "panel/epaper.h"
#include "panel/frame_dump.h"
#include "panel/panel.h"
#include "util/elapsed_time.h"

static const char *TAG = "greenlight";

#define PARTIALS_BETWEEN_CLEARS 50  /* partial refreshes between full clears, against ghosting */

static uint8_t frame_on_panel[PANEL_FRAME_BYTES];  /* last frame sent; all white after a clear */
static int     partials_since_clear;

esp_err_t panel_power_on(void)
{
    esp_err_t err = epaper_init();
    return err == ESP_OK ? panel_clear("boot") : err;
}

esp_err_t panel_clear(const char *reason)
{
    int        busy_before = epaper_busy_level();
    TickType_t start = xTaskGetTickCount();
    esp_err_t  err = epaper_clear();
    memset(frame_on_panel, 0xFF, sizeof(frame_on_panel));
    partials_since_clear = 0;
    ESP_LOGI(TAG, "panel: clear (%s) %lu ms, busy before %d%s", reason, (unsigned long)elapsed_ms(start), busy_before,
             err == ESP_OK ? "" : ", FAILED");
    return err;
}

/* How many pixels of frame are black, and how many differ from the frame on the panel. */
static void count_pixels(const uint8_t *frame, int *black, int *changed)
{
    *black = *changed = 0;
    for (size_t i = 0; i < sizeof(frame_on_panel); i++) {
        *black += __builtin_popcount((uint8_t)~frame[i]);
        *changed += __builtin_popcount(frame[i] ^ frame_on_panel[i]);
    }
}

/* Most writes from HA do not change the proper screen: those frames are already on the panel. */
esp_err_t panel_refresh(const uint8_t *frame, const char *label)
{
    if (memcmp(frame, frame_on_panel, sizeof(frame_on_panel)) == 0) {
        return ESP_OK;
    }
#ifdef SCREEN_DUMP
    frame_dump_print(frame, label);
#endif
    if (++partials_since_clear >= PARTIALS_BETWEEN_CLEARS && panel_clear("periodic") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper clear failed");
    }
    int black, changed;
    count_pixels(frame, &black, &changed);
    memcpy(frame_on_panel, frame, sizeof(frame_on_panel));
    int        busy_before = epaper_busy_level();
    TickType_t start = xTaskGetTickCount();
    esp_err_t  err = epaper_refresh_partial(frame);
    ESP_LOGI(TAG, "panel: refresh #%d (%s) %lu ms, busy before %d, black %d, changed %d%s", partials_since_clear,
             label, (unsigned long)elapsed_ms(start), busy_before, black, changed, err == ESP_OK ? "" : ", FAILED");
    return err;
}

void panel_power_off(void)
{
    TickType_t start = xTaskGetTickCount();
    epaper_power_off();
    ESP_LOGI(TAG, "panel: poweroff %lu ms", (unsigned long)elapsed_ms(start));
    ESP_LOGI(TAG, "panel asleep, pins low: safe to remove power now");
}

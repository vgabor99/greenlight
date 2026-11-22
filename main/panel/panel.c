#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "panel/epaper.h"
#include "panel/frame_dump.h"
#include "panel/panel.h"
#include "util/elapsed_time.h"

static const char *TAG = "greenlight";

#define PARTIALS_BETWEEN_CLEARS 50           /* partial refreshes between full clears, against ghosting */
#define SLEEP_FRAME_MAGIC       0x5EEF4A3Eu  /* on_panel holds the frame the panel went to sleep with */

/* The frame on the panel and the partial refreshes since the last clear. They live in RTC memory, which keeps them
 * through deep sleep, so a wake can load the frame into the controller instead of clearing the panel
 * (docs/behaviour.md#panel-on-each-wake). `magic` and `crc` are set only when the panel went to sleep showing it. */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t crc;
    int      partials_since_clear;
    uint8_t  frame[PANEL_FRAME_BYTES];  /* last frame sent; all white after a clear */
} on_panel;

static bool frame_known;  /* on_panel.frame is what the panel shows: no clear or refresh failed since power-on */

static uint32_t on_panel_crc(void)
{
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)&on_panel.partials_since_clear,
                                    sizeof(on_panel.partials_since_clear));
    return esp_rom_crc32_le(crc, on_panel.frame, sizeof(on_panel.frame));
}

/* Only a wake from deep sleep (or the SIM_SLEEP restart) after a clean panel_power_off() continues from the frame
 * on the panel. After a power-on, a flash or a crash the frame is unknown. */
static bool sleep_frame_valid(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    return (reason == ESP_RST_DEEPSLEEP || reason == ESP_RST_SW) && on_panel.magic == SLEEP_FRAME_MAGIC &&
           on_panel.crc == on_panel_crc();
}

esp_err_t panel_power_on(void)
{
    bool resume = sleep_frame_valid();
    on_panel.magic = 0;  /* valid again only after a clean panel_power_off() */
    esp_err_t err = epaper_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!resume) {
        return panel_clear("boot");
    }
    int        busy_before = epaper_busy_level();
    TickType_t start = xTaskGetTickCount();
    epaper_load_frame(on_panel.frame);
    frame_known = true;
    ESP_LOGI(TAG, "panel: load (sleep frame) %lu ms, busy before %d, partials %d", (unsigned long)elapsed_ms(start),
             busy_before, on_panel.partials_since_clear);
    return ESP_OK;
}

esp_err_t panel_clear(const char *reason)
{
    int        busy_before = epaper_busy_level();
    TickType_t start = xTaskGetTickCount();
    esp_err_t  err = epaper_clear();
    memset(on_panel.frame, 0xFF, sizeof(on_panel.frame));
    on_panel.partials_since_clear = 0;
    frame_known = err == ESP_OK;
    ESP_LOGI(TAG, "panel: clear (%s) %lu ms, busy before %d%s", reason, (unsigned long)elapsed_ms(start), busy_before,
             err == ESP_OK ? "" : ", FAILED");
    return err;
}

/* How many pixels of frame are black, and how many differ from the frame on the panel. */
static void count_pixels(const uint8_t *frame, int *black, int *changed)
{
    *black = *changed = 0;
    for (size_t i = 0; i < sizeof(on_panel.frame); i++) {
        *black += __builtin_popcount((uint8_t)~frame[i]);
        *changed += __builtin_popcount(frame[i] ^ on_panel.frame[i]);
    }
}

/* Most writes from HA do not change the proper screen: those frames are already on the panel. */
esp_err_t panel_refresh(const uint8_t *frame, const char *label)
{
    if (memcmp(frame, on_panel.frame, sizeof(on_panel.frame)) == 0) {
        return ESP_OK;
    }
#ifdef SCREEN_DUMP
    frame_dump_print(frame, label);
#endif
    if (++on_panel.partials_since_clear >= PARTIALS_BETWEEN_CLEARS && panel_clear("periodic") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper clear failed");
    }
    int black, changed;
    count_pixels(frame, &black, &changed);
    memcpy(on_panel.frame, frame, sizeof(on_panel.frame));
    int        busy_before = epaper_busy_level();
    TickType_t start = xTaskGetTickCount();
    esp_err_t  err = epaper_refresh_partial(frame);
    if (err != ESP_OK) {
        frame_known = false;
    }
    ESP_LOGI(TAG, "panel: refresh #%d (%s) %lu ms, busy before %d, black %d, changed %d%s",
             on_panel.partials_since_clear, label, (unsigned long)elapsed_ms(start), busy_before, black, changed,
             err == ESP_OK ? "" : ", FAILED");
    return err;
}

/* The frame is kept for the next wake only if the panel shows it: after a failed clear or refresh, that wake clears. */
void panel_power_off(void)
{
    TickType_t start = xTaskGetTickCount();
    epaper_power_off();
    if (frame_known) {
        on_panel.crc   = on_panel_crc();
        on_panel.magic = SLEEP_FRAME_MAGIC;
    }
    ESP_LOGI(TAG, "panel: poweroff %lu ms", (unsigned long)elapsed_ms(start));
    ESP_LOGI(TAG, "panel asleep, pins low: safe to remove power now");
}

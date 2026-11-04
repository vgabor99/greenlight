#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "battery.h"
#include "display.h"
#include "display_model.h"
#include "panel.h"
#include "render.h"
#include "view_freshness.h"

static const char *TAG = "greenlight";

volatile bool     view_dirty;
static state_t           view_state;
render_screen_t   failed;   /* RENDER_UNPAIRED, RENDER_JOIN_FAILED, RENDER_ERROR or RENDER_BATTERY once
                                      * this wake failed, else RENDER_BLANK */

void show_state(state_t st)
{
    view_state = st;
    view_dirty = true;
}

/* What the proper screen shows (DESIGN.md 8): the house status, unless the device is pairing, joining or
 * syncing, or this wake failed. */
static render_screen_t screen_for(state_t st)
{
    if (failed != RENDER_BLANK) {
        return failed;
    }
    switch (st) {
    case ST_UNPAIRED:       return RENDER_UNPAIRED;
    case ST_PAIRING:        return RENDER_SEARCHING;
    case ST_ERASE_NETWORK:  return RENDER_SEARCHING;  /* pairing follows the restart */
    case ST_INTERVIEW:      return RENDER_JOINING;
    case ST_BOOT:           return shown.view.id ? RENDER_HOUSE : RENDER_BLANK;
    case ST_SLEEP:          return shown.view.id ? RENDER_HOUSE : RENDER_SYNCING;  /* a stale view beats none */
    default:                return shown.view.id && !syncing && !oos ? RENDER_HOUSE : RENDER_SYNCING;
    }
}

/* What the last redraw showed of the staleness: the flag, and on the debug screen the age as drawn. */
static bool drawn_stale;
static char drawn_age[RENDER_AGE_LEN];

/* The staleness on the screen is out of date (checked under view_lock). */
static bool live_changed(void)
{
    if (screen_stale() != drawn_stale) {
        return true;
    }
    if (RENDER_DEBUG_SCREEN) {
        char age[RENDER_AGE_LEN];
        render_age(age, screen_age_ms());
        return strcmp(age, drawn_age) != 0;
    }
    return false;
}

/* Draw the screen now (~0.5 s). Blocks the state loop; input_task keeps polling the inputs meanwhile. */
void redraw(void)
{
    static render_view_t v;
    xSemaphoreTake(view_lock, portMAX_DELAY);
    view_dirty  = false;
    v.stale     = screen_stale();
    v.age_ms    = screen_age_ms();
    v.valid_ms  = SCREEN_VALID_MS;
    if (v.stale != drawn_stale) {
        ESP_LOGI(TAG, "screen %s (age %lld ms)", v.stale ? "stale" : "fresh", (long long)v.age_ms);
    }
    drawn_stale = v.stale;
    render_age(drawn_age, v.age_ms);
    v.screen    = screen_for(view_state);
    v.state     = state_names[view_state];
    v.wake      = wake_n;
    v.uptime_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    v.ds_hash   = ds.hash;
    v.ds_count  = ds.count;
    v.shown     = shown;
    v.oos       = oos;
    v.battery_mv = battery_mv();
    v.usb       = usb_powered();
    v.bat_low   = bat_latched || battery_low();
    v.rx_count  = rx_count;
    for (int i = 0; i < rx_count; i++) {
        v.rx[i] = rx_log[(rx_head - 1 - i + RENDER_RX_N) % RENDER_RX_N];
    }
    xSemaphoreGive(view_lock);
    if (panel_refresh(render_status(&v), v.state) != ESP_OK) {
        ESP_LOGE(TAG, "e-paper update failed");
    }
}

/* The state loops wait with this, so whatever arrives is drawn while it comes in. Writes that arrive during a
 * refresh are drawn together by the next one. */
void wait_ms(uint32_t ms)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    bool dirty = view_dirty || live_changed();
    xSemaphoreGive(view_lock);
    if (dirty) {
        redraw();
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

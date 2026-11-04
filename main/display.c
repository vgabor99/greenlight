#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "battery.h"
#include "debug_screen.h"
#include "display.h"
#include "display_model.h"
#include "panel.h"
#include "render.h"
#include "view_freshness.h"

static const char *TAG = "greenlight";

static device_state_t  shown_state;
static render_screen_t failure_screen = RENDER_BLANK;
static uint32_t        wake_number;

/* What the last redraw showed of the staleness: the flag, and on the debug screen the age as drawn. */
static bool drawn_stale;
static char drawn_age[DEBUG_SCREEN_AGE_LEN];

void display_show_state(device_state_t state)
{
    shown_state = state;
    display_model_mark_changed();
}

void display_set_failure(render_screen_t screen)
{
    failure_screen = screen;
}

render_screen_t display_failure(void)
{
    return failure_screen;
}

void display_set_wake_number(uint32_t number)
{
    wake_number = number;
}

/* What the proper screen shows (DESIGN.md 8): the house status, unless the device is pairing, joining or
 * syncing, or this wake failed. */
static render_screen_t choose_screen(device_state_t state, const render_content_t *content, bool syncing)
{
    if (failure_screen != RENDER_BLANK) {
        return failure_screen;
    }
    bool view_shown = content->shown.view.id != 0;
    switch (state) {
    case STATE_UNPAIRED:       return RENDER_UNPAIRED;
    case STATE_PAIRING:        return RENDER_SEARCHING;
    case STATE_ERASE_NETWORK:  return RENDER_SEARCHING;  /* pairing follows the restart */
    case STATE_INTERVIEW:      return RENDER_JOINING;
    case STATE_BOOT:           return view_shown ? RENDER_HOUSE : RENDER_BLANK;
    case STATE_SLEEP:          return view_shown ? RENDER_HOUSE : RENDER_SYNCING;  /* a stale view beats none */
    default:                   return view_shown && !syncing && !content->out_of_sync ? RENDER_HOUSE : RENDER_SYNCING;
    }
}

/* The staleness on the screen is out of date. */
static bool drawn_staleness_outdated(void)
{
    if (view_freshness_is_stale() != drawn_stale) {
        return true;
    }
    if (RENDER_DEBUG_SCREEN) {
        char age[DEBUG_SCREEN_AGE_LEN];
        debug_screen_format_age(age, view_freshness_age_ms());
        return strcmp(age, drawn_age) != 0;
    }
    return false;
}

void display_redraw(void)
{
    static render_content_t content;
    bool                 syncing;
    display_model_snapshot(&content, &syncing);
    if (content.stale != drawn_stale) {
        ESP_LOGI(TAG, "screen %s (age %lld ms)", content.stale ? "stale" : "fresh", (long long)content.age_ms);
    }
    drawn_stale = content.stale;
    debug_screen_format_age(drawn_age, content.age_ms);
    content.screen       = choose_screen(shown_state, &content, syncing);
    content.state_name   = device_state_name(shown_state);
    content.wake_number  = wake_number;
    content.uptime_ms    = pdTICKS_TO_MS(xTaskGetTickCount());
    content.battery_mv   = battery_voltage_mv();
    content.on_usb_power = battery_on_usb_power();
    content.battery_low  = battery_low_latched() || battery_is_low();
    if (panel_refresh(render_frame(&content), content.state_name) != ESP_OK) {
        ESP_LOGE(TAG, "e-paper update failed");
    }
}

void display_wait_ms(uint32_t ms)
{
    if (display_model_has_changed() || drawn_staleness_outdated()) {
        display_redraw();
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

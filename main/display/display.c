#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app/app_state.h"
#include "display/display.h"
#include "panel/panel.h"
#include "screen/render.h"

static const char *TAG = "greenlight";

/* The state the panel was last drawn from, and the latest one (static: they are large). */
static app_state_t drawn, latest;

static void draw(const app_state_t *state)
{
    if (state->stale != drawn.stale) {
        ESP_LOGI(TAG, "screen %s (age %lld ms)", state->stale ? "stale" : "fresh", (long long)state->age_ms);
    }
    if (panel_refresh(render_frame(state), device_state_name(state->state)) != ESP_OK) {
        ESP_LOGE(TAG, "e-paper update failed");
    }
    drawn = *state;
}

void display_redraw(void)
{
    app_state_snapshot(&latest);
    draw(&latest);
}

void display_wait_ms(uint32_t ms)
{
    app_state_snapshot(&latest);
    if (render_is_outdated(&drawn, &latest)) {
        draw(&latest);
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

bool display_power_on(void)
{
    return panel_power_on() == ESP_OK;
}

/* Cleanest state for power-off: full white reset, then the screen to sleep with (SLEEP on the debug screen). */
void display_power_off(void)
{
    if (panel_clear("shutdown") != ESP_OK) {
        ESP_LOGE(TAG, "e-paper final clear failed");
    }
    display_redraw();
    ESP_LOGI(TAG, "final refresh done");
    panel_power_off();
}

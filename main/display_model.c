#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "display.h"
#include "display_model.h"
#include "persistent_store.h"
#include "view_freshness.h"

static const char *TAG = "greenlight";

zb_dataset_t   ds;
render_shown_t shown;
bool           oos;  /* the last view received was for another dataset (not stored) */
volatile bool  view_seen;  /* a view was applied in this wake (VIEW_WAIT_MS) */
SemaphoreHandle_t view_lock;  /* guards ds, shown, oos, syncing and rx_log: the Zigbee task writes them */
bool              syncing;  /* dataset entries arrived and their commit has not */
render_rx_t       rx_log[RENDER_RX_N];  /* ring; rx_head is the next slot */
int               rx_head, rx_count;

void state_load(void)
{
    blob_load("ds", &ds, sizeof(ds));
    blob_load("view", &shown, sizeof(shown));
    nvs_erase_key(nvs, "snap");  /* rev 5 snapshot, no longer used */
}

/* Zigbee callbacks (called in the Zigbee task): update the view, the state loop redraws. */
void on_view(const zb_view_t *v, zb_view_result_t result)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    oos = result == ZB_VIEW_OOS;
    if (result == ZB_VIEW_OK || result == ZB_VIEW_TIME) {
        view_seen = true;
        screen_valid_until = esp_rtc_get_time_us() + SCREEN_VALID_MS * 1000ULL;
    }
    if (result == ZB_VIEW_OK) {
        render_build_rows(&shown, &ds, v);
    } else if (result == ZB_VIEW_TIME) {
        shown.view.time = v->time;
    }
    view_dirty = true;
    xSemaphoreGive(view_lock);
    if (result == ZB_VIEW_OK || result == ZB_VIEW_TIME) {
        blob_save("view", &shown, sizeof(shown));
    }
    if (result == ZB_VIEW_OK) {
        ESP_LOGI(TAG, "view %06lx: %u active lines%s", (unsigned long)v->id, shown.active, shown.active ? "" : " (All OK)");
        for (int i = 0; i < shown.nrows; i++) {
            ESP_LOGI(TAG, "  %d: %s", i + 1, shown.row[i]);
        }
    }
}

void on_dataset(const zb_dataset_t *d)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    ds = *d;
    view_dirty = true;
    xSemaphoreGive(view_lock);
    blob_save("ds", &ds, sizeof(ds));
    ESP_LOGI(TAG, "dataset %06lx stored: %u lines", (unsigned long)d->hash, d->count);
}

void on_rx(const zb_rx_t *ev)
{
    xSemaphoreTake(view_lock, portMAX_DELAY);
    if (ev->kind == ZB_RX_ENTRY && ev->entry.tag >= 0) {
        syncing = true;
    } else if (ev->kind == ZB_RX_DS_COMMIT) {
        syncing = false;
    }
    rx_log[rx_head] = (render_rx_t){.ms = pdTICKS_TO_MS(xTaskGetTickCount()), .ev = *ev};
    rx_head = (rx_head + 1) % RENDER_RX_N;
    if (rx_count < RENDER_RX_N) {
        rx_count++;
    }
    view_dirty = true;
    xSemaphoreGive(view_lock);
}

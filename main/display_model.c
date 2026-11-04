#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display_model.h"
#include "persistent_store.h"
#include "view_freshness.h"

static const char *TAG = "greenlight";

static SemaphoreHandle_t lock;
static volatile bool     changed;
static zb_dataset_t      dataset;
static render_shown_t    shown;
static bool              out_of_sync;    /* the last view received was for another dataset (not stored) */
static bool              syncing;        /* dataset entries arrived and their commit has not */
static volatile bool     view_received;  /* a view was applied in this wake (VIEW_WAIT_MS) */
static render_rx_t       received_writes[RENDER_RX_N];  /* ring; next_write_slot is the next slot */
static int               next_write_slot, received_write_count;

void display_model_init(void)
{
    lock = xSemaphoreCreateMutex();
}

void display_model_load(void)
{
    persistent_store_load_blob(STORE_KEY_DATASET, &dataset, sizeof(dataset));
    persistent_store_load_blob(STORE_KEY_SHOWN_VIEW, &shown, sizeof(shown));
    persistent_store_erase(STORE_KEY_RETIRED_SNAPSHOT);
}

/* Nothing is known about the panel any more. */
void display_model_erase_stored(void)
{
    persistent_store_erase(STORE_KEY_DATASET);
    persistent_store_erase(STORE_KEY_SHOWN_VIEW);
}

uint32_t display_model_dataset_hash(void)
{
    return dataset.hash;
}

uint32_t display_model_shown_view_id(void)
{
    return shown.view.id;
}

bool display_model_view_received(void)
{
    return view_received;
}

void display_model_mark_changed(void)
{
    changed = true;
}

bool display_model_has_changed(void)
{
    return changed;
}

void display_model_snapshot(render_view_t *content, bool *is_syncing)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    changed           = false;
    content->stale    = view_freshness_is_stale();
    content->age_ms   = view_freshness_age_ms();
    content->valid_ms = VIEW_VALID_MS;
    content->ds_hash  = dataset.hash;
    content->ds_count = dataset.count;
    content->shown    = shown;
    content->oos      = out_of_sync;
    content->rx_count = received_write_count;
    for (int i = 0; i < received_write_count; i++) {  /* newest first */
        content->rx[i] = received_writes[(next_write_slot - 1 - i + RENDER_RX_N) % RENDER_RX_N];
    }
    *is_syncing = syncing;
    xSemaphoreGive(lock);
}

static void log_shown_view(void)
{
    ESP_LOGI(TAG, "view %06lx: %u active lines%s", (unsigned long)shown.view.id, shown.active,
             shown.active ? "" : " (All OK)");
    for (int i = 0; i < shown.nrows; i++) {
        ESP_LOGI(TAG, "  %d: %s", i + 1, shown.row[i]);
    }
}

void display_model_on_view(const zb_view_t *view, zb_view_result_t result)
{
    bool accepted = result == ZB_VIEW_OK || result == ZB_VIEW_TIME;
    xSemaphoreTake(lock, portMAX_DELAY);
    out_of_sync = result == ZB_VIEW_OOS;
    if (accepted) {
        view_received = true;
        view_freshness_renew();
    }
    if (result == ZB_VIEW_OK) {
        render_build_rows(&shown, &dataset, view);
    } else if (result == ZB_VIEW_TIME) {
        shown.view.time = view->time;
    }
    changed = true;
    xSemaphoreGive(lock);
    if (accepted) {
        persistent_store_save_blob(STORE_KEY_SHOWN_VIEW, &shown, sizeof(shown));
    }
    if (result == ZB_VIEW_OK) {
        log_shown_view();
    }
}

void display_model_on_dataset(const zb_dataset_t *new_dataset)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    dataset = *new_dataset;
    changed = true;
    xSemaphoreGive(lock);
    persistent_store_save_blob(STORE_KEY_DATASET, &dataset, sizeof(dataset));
    ESP_LOGI(TAG, "dataset %06lx stored: %u lines", (unsigned long)new_dataset->hash, new_dataset->count);
}

void display_model_on_received_write(const zb_rx_t *write)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (write->kind == ZB_RX_ENTRY && write->entry.tag >= 0) {
        syncing = true;
    } else if (write->kind == ZB_RX_DS_COMMIT) {
        syncing = false;
    }
    received_writes[next_write_slot] = (render_rx_t){.ms = pdTICKS_TO_MS(xTaskGetTickCount()), .ev = *write};
    next_write_slot = (next_write_slot + 1) % RENDER_RX_N;
    if (received_write_count < RENDER_RX_N) {
        received_write_count++;
    }
    changed = true;
    xSemaphoreGive(lock);
}

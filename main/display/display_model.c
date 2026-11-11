#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display/display_model.h"
#include "display/view_freshness.h"
#include "persistent_store.h"

static const char *TAG = "greenlight";

/* The dataset is stored in NVS as is (STORE_KEY_DATASET): its layout must not change. */
_Static_assert(sizeof(dataset_t) == 2088, "the stored dataset's layout");

static SemaphoreHandle_t lock;
static volatile bool     changed;
static dataset_t      dataset;
static shown_view_t    shown;
static bool              out_of_sync;    /* the last view received was for another dataset (not stored) */
static bool              syncing;        /* dataset entries arrived and their commit has not */
static volatile bool     view_received;  /* a view was applied in this wake (VIEW_WAIT_MS) */
static render_received_write_t       received_writes[RENDER_RECEIVED_WRITES];  /* ring; next_write_slot is the next slot */
static int               next_write_slot, received_write_count;

void display_model_init(void)
{
    lock = xSemaphoreCreateMutex();
}

void display_model_load(void)
{
    persistent_store_load_blob(STORE_KEY_DATASET, &dataset, sizeof(dataset));
    persistent_store_load_blob(STORE_KEY_SHOWN_VIEW, &shown, sizeof(shown));
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

void display_model_snapshot(render_content_t *content, bool *is_syncing)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    changed                       = false;
    content->stale                = view_freshness_is_stale();
    content->age_ms               = view_freshness_age_ms();
    content->valid_ms             = VIEW_VALID_MS;
    content->dataset_hash         = dataset.hash;
    content->dataset_lines        = dataset.count;
    content->shown                = shown;
    content->out_of_sync          = out_of_sync;
    content->received_write_count = received_write_count;
    for (int i = 0; i < received_write_count; i++) {  /* newest first */
        int slot = (next_write_slot - 1 - i + RENDER_RECEIVED_WRITES) % RENDER_RECEIVED_WRITES;
        content->received_writes[i] = received_writes[slot];
    }
    *is_syncing = syncing;
    xSemaphoreGive(lock);
}

static void log_shown_view(void)
{
    ESP_LOGI(TAG, "view %06lx: %u active lines%s", (unsigned long)shown.view.id, shown.active_lines,
             shown.active_lines ? "" : " (All OK)");
    for (int i = 0; i < shown.row_count; i++) {
        ESP_LOGI(TAG, "  %d: %s", i + 1, shown.rows[i]);
    }
}

void display_model_on_view(const view_t *view, zigbee_view_result_t result)
{
    bool accepted = result == ZIGBEE_VIEW_NEW || result == ZIGBEE_VIEW_TIME_ONLY;
    xSemaphoreTake(lock, portMAX_DELAY);
    out_of_sync = result == ZIGBEE_VIEW_OUT_OF_SYNC;
    if (accepted) {
        view_received = true;
        view_freshness_renew();
    }
    if (result == ZIGBEE_VIEW_NEW) {
        view_layout_build(&shown, &dataset, view);
    } else if (result == ZIGBEE_VIEW_TIME_ONLY) {
        shown.view.time = view->time;
    }
    changed = true;
    xSemaphoreGive(lock);
    if (accepted) {
        persistent_store_save_blob(STORE_KEY_SHOWN_VIEW, &shown, sizeof(shown));
    }
    if (result == ZIGBEE_VIEW_NEW) {
        log_shown_view();
    }
}

void display_model_on_dataset(const dataset_t *new_dataset)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    dataset = *new_dataset;
    changed = true;
    xSemaphoreGive(lock);
    persistent_store_save_blob(STORE_KEY_DATASET, &dataset, sizeof(dataset));
    ESP_LOGI(TAG, "dataset %06lx stored: %u lines", (unsigned long)new_dataset->hash, new_dataset->count);
}

void display_model_on_received_write(const zigbee_received_write_t *write)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (write->kind == ZIGBEE_WRITE_DATASET_ENTRY && write->entry.dataset_hash >= 0) {
        syncing = true;
    } else if (write->kind == ZIGBEE_WRITE_DATASET_COMMIT) {
        syncing = false;
    }
    received_writes[next_write_slot] = (render_received_write_t){.uptime_ms = pdTICKS_TO_MS(xTaskGetTickCount()), .write = *write};
    next_write_slot = (next_write_slot + 1) % RENDER_RECEIVED_WRITES;
    if (received_write_count < RENDER_RECEIVED_WRITES) {
        received_write_count++;
    }
    changed = true;
    xSemaphoreGive(lock);
}

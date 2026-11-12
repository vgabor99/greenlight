#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app/app_state.h"
#include "app/view_freshness.h"
#include "board/battery.h"
#include "persistent_store.h"

static const char *TAG = "greenlight";

/* The dataset is stored in NVS as is (STORE_KEY_DATASET): its layout must not change. */
_Static_assert(sizeof(dataset_t) == 2088, "the stored dataset's layout");
/* The shown view is stored as far as its lines go (STORE_KEY_SHOWN_VIEW): the header and line_count lines, so a
 * write costs flash in proportion to what is shown (a time-only view rewrites it every few seconds while awake). */
#define SHOWN_VIEW_HEADER_BYTES offsetof(shown_view_t, lines)
_Static_assert(SHOWN_VIEW_HEADER_BYTES == 17 && sizeof(((shown_view_t *)0)->lines[0]) == 65, "the stored view's layout");

static size_t shown_view_stored_size(const shown_view_t *view)
{
    return SHOWN_VIEW_HEADER_BYTES + view->line_count * sizeof(view->lines[0]);
}

static SemaphoreHandle_t          lock;
static device_state_t             device_state;
static volatile wake_failure_t    wake_failure;
static uint32_t                   wake_number;
static uint32_t                   changes;
static dataset_t                  dataset;
static shown_view_t               shown;
static bool                       out_of_sync;    /* the last view received was for another dataset (not stored) */
static bool                       syncing;        /* dataset entries arrived and their commit has not */
static volatile bool              view_received;  /* a view was applied in this wake (VIEW_WAIT_MS) */
static received_write_t received_writes[APP_STATE_RECEIVED_WRITES];  /* ring; next_write_slot is the next slot */
static int                        next_write_slot, received_write_count;

void app_state_init(void)
{
    lock = xSemaphoreCreateMutex();
}

void app_state_load(void)
{
    persistent_store_load_blob(STORE_KEY_DATASET, &dataset, sizeof(dataset));
    /* A blob whose size does not fit its line count (another layout, such as the old laid-out rows) is no view. */
    size_t stored_size = persistent_store_load_blob_prefix(STORE_KEY_SHOWN_VIEW, &shown, sizeof(shown));
    if (stored_size != shown_view_stored_size(&shown) || shown.line_count > DATASET_MAX_LINES) {
        memset(&shown, 0, sizeof(shown));
    }
}

/* Nothing is known about the panel any more. */
void app_state_erase_stored(void)
{
    persistent_store_erase(STORE_KEY_DATASET);
    persistent_store_erase(STORE_KEY_SHOWN_VIEW);
}

void app_state_set_device_state(device_state_t state)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    device_state = state;
    changes++;
    xSemaphoreGive(lock);
}

void app_state_set_wake_number(uint32_t number)
{
    wake_number = number;
}

void app_state_set_wake_failure(wake_failure_t failure)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    wake_failure = failure;
    changes++;
    xSemaphoreGive(lock);
}

wake_failure_t app_state_wake_failure(void)
{
    return wake_failure;
}

uint32_t app_state_dataset_hash(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t hash = dataset.hash;
    xSemaphoreGive(lock);
    return hash;
}

uint32_t app_state_shown_view_id(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t id = shown.view.id;
    xSemaphoreGive(lock);
    return id;
}

bool app_state_view_received(void)
{
    return view_received;
}

void app_state_snapshot(app_state_t *state)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    state->state                = device_state;
    state->failure              = wake_failure;
    state->changes              = changes;
    state->dataset_hash         = dataset.hash;
    state->dataset_lines        = dataset.count;
    state->shown                = shown;
    state->syncing              = syncing;
    state->out_of_sync          = out_of_sync;
    state->received_write_count = received_write_count;
    for (int i = 0; i < received_write_count; i++) {  /* newest first */
        int slot = (next_write_slot - 1 - i + APP_STATE_RECEIVED_WRITES) % APP_STATE_RECEIVED_WRITES;
        state->received_writes[i] = received_writes[slot];
    }
    xSemaphoreGive(lock);
    state->stale                      = view_freshness_is_stale();
    state->age_ms                     = view_freshness_age_ms();
    state->valid_ms                   = VIEW_VALID_MS;
    state->wake_number                = wake_number;
    state->uptime_ms                  = pdTICKS_TO_MS(xTaskGetTickCount());
    state->battery_mv                 = battery_voltage_mv();
    state->on_usb_power               = battery_on_usb_power();
    state->battery_low                = battery_low_latched() || battery_is_low();
    state->battery_simulation_changes = battery_simulation_changes();
}

/* Show the view: take the text of its active lines from the dataset. */
static void show_view(const view_t *view)
{
    memset(&shown, 0, sizeof(shown));
    shown.view = *view;
    for (int line = 0; line < dataset.count; line++) {
        if (data_model_line_active(view, line)) {
            memcpy(shown.lines[shown.line_count++], dataset.text[line], sizeof(shown.lines[0]));
        }
    }
}

static void log_shown_view(void)
{
    ESP_LOGI(TAG, "view %06lx: %u active lines%s", (unsigned long)shown.view.id, shown.line_count,
             shown.line_count ? "" : " (All OK)");
    for (int i = 0; i < shown.line_count; i++) {
        ESP_LOGI(TAG, "  %d: %s", i + 1, shown.lines[i]);
    }
}

void app_state_apply_view(const view_t *view, view_result_t result)
{
    bool accepted = result == VIEW_NEW || result == VIEW_TIME_ONLY;
    xSemaphoreTake(lock, portMAX_DELAY);
    out_of_sync = result == VIEW_OUT_OF_SYNC;
    if (accepted) {
        view_received = true;
        view_freshness_renew();
    }
    if (result == VIEW_NEW) {
        show_view(view);
    } else if (result == VIEW_TIME_ONLY) {
        shown.view.time = view->time;
    }
    changes++;
    xSemaphoreGive(lock);
    if (accepted) {
        persistent_store_save_blob(STORE_KEY_SHOWN_VIEW, &shown, shown_view_stored_size(&shown));
    }
    if (result == VIEW_NEW) {
        log_shown_view();
    }
}

void app_state_store_dataset(const dataset_t *new_dataset)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    dataset = *new_dataset;
    changes++;
    xSemaphoreGive(lock);
    persistent_store_save_blob(STORE_KEY_DATASET, &dataset, sizeof(dataset));
    ESP_LOGI(TAG, "dataset %06lx stored: %u lines", (unsigned long)new_dataset->hash, new_dataset->count);
}

void app_state_note_received(const received_write_t *write)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (write->kind == RECEIVED_DATASET_ENTRY && write->entry.dataset_hash >= 0) {
        syncing = true;
    } else if (write->kind == RECEIVED_DATASET_COMMIT) {
        syncing = false;
    }
    received_writes[next_write_slot]           = *write;
    received_writes[next_write_slot].uptime_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    next_write_slot = (next_write_slot + 1) % APP_STATE_RECEIVED_WRITES;
    if (received_write_count < APP_STATE_RECEIVED_WRITES) {
        received_write_count++;
    }
    changes++;
    xSemaphoreGive(lock);
}

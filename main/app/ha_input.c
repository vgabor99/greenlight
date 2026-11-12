#include <string.h>
#include "esp_log.h"
#include "app/app_state.h"
#include "app/ha_input.h"
#include "app/keep_awake.h"

static const char *TAG = "greenlight";

/* How long HA's writes keep the device awake (docs/behaviour.md#timeouts) */
#define DATASET_SYNC_HOLD_MS 20000  /* after a ds_entry or ds_commit: the next part, or the view after it */
#define RESYNC_HOLD_MS       20000  /* after an out-of-sync view: HA sees dataset_hash and starts the sync */

void ha_input_on_view(const view_t *view, uint8_t received_mask)
{
    received_write_t write = {.kind = RECEIVED_VIEW, .view = {.view = *view, .received_mask = received_mask}};
    write.view.result      = received_mask != ZIGBEE_VIEW_ALL_ATTRIBUTES
                                 ? VIEW_INCOMPLETE
                                 : data_model_check_view(view, app_state_dataset_hash(), app_state_shown_view_id());
    switch (write.view.result) {
    case VIEW_INCOMPLETE:
        ESP_LOGW(TAG, "view %06lx incomplete: attributes 0x%x", (unsigned long)view->id, received_mask);
        app_state_note_received(&write);
        break;
    case VIEW_OUT_OF_SYNC:
        ESP_LOGW(TAG, "view %06lx out of sync: for dataset %06lx, stored %06lx", (unsigned long)view->id,
                 (unsigned long)view->dataset_hash, (unsigned long)app_state_dataset_hash());
        app_state_apply_view(view, write.view.result);
        app_state_note_received(&write);
        keep_awake_for(RESYNC_HOLD_MS);                       /* stay awake for the resync */
        zigbee_report_dataset_hash(app_state_dataset_hash());  /* tells HA which dataset the device has */
        break;
    case VIEW_NEW:
    case VIEW_TIME_ONLY:
        ESP_LOGI(TAG, "view %06lx %s: bits %08lx time %lu", (unsigned long)view->id,
                 write.view.result == VIEW_NEW ? "accepted" : "time only", (unsigned long)view->bits,
                 (unsigned long)view->time);
        app_state_apply_view(view, write.view.result);  /* stored before it is acknowledged */
        app_state_note_received(&write);
        zigbee_report_shown_view_id(view->id);
        break;
    }
}

void ha_input_on_dataset_entry(const zigbee_dataset_entry_t *entry)
{
    received_write_t write = {
        .kind  = RECEIVED_DATASET_ENTRY,
        .entry = {.dataset_hash = entry->dataset_hash, .index = entry->index, .part = entry->part, .more = entry->more},
    };
    strlcpy(write.entry.text, entry->text, sizeof(write.entry.text));
    app_state_note_received(&write);
    keep_awake_for(DATASET_SYNC_HOLD_MS);
}

void ha_input_on_dataset_commit(const zigbee_dataset_commit_t *commit)
{
    received_write_t write = {.kind = RECEIVED_DATASET_COMMIT, .commit = {.hash = commit->hash, .count = commit->count}};
    if (!commit->well_formed) {
        ESP_LOGW(TAG, "ds_commit %06lx: bad count %u", (unsigned long)commit->hash, commit->count);
        write.commit.result = COMMIT_BAD_COUNT;
    } else if (commit->hash == app_state_dataset_hash()) {
        ESP_LOGI(TAG, "ds_commit %06lx: already stored", (unsigned long)commit->hash);
        write.commit.result = COMMIT_ALREADY_STORED;
        zigbee_report_dataset_hash(commit->hash);  /* re-acknowledge */
    } else if (!commit->dataset) {
        ESP_LOGW(TAG, "ds_commit %06lx rejected: entry %02d missing a part or for another dataset",
                 (unsigned long)commit->hash, commit->first_missing);
        write.commit.result        = COMMIT_REJECTED;
        write.commit.first_missing = commit->first_missing;
    } else {
        write.commit.result = COMMIT_STORED;
        app_state_store_dataset(commit->dataset);  /* stored before it is acknowledged */
        ESP_LOGI(TAG, "ds_commit %06lx accepted: %u entries", (unsigned long)commit->hash, commit->count);
        zigbee_report_dataset_hash(commit->hash);
    }
    app_state_note_received(&write);
    keep_awake_for(DATASET_SYNC_HOLD_MS);
}

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app/device_state.h"
#include "data_model.h"
#include "zigbee/zigbee.h"

/* The application state (docs/firmware.md#tasks-and-data-flow): the device state, the stored dataset, the view on
 * the panel and how current it is, and the writes HA sent in this wake. The state machine and the Zigbee task change
 * it; the display draws snapshots of it. A mutex guards it. The dataset and the shown view are kept in NVS through
 * the sleep. */

#define APP_STATE_RECEIVED_WRITES 8  /* received writes kept for the debug screen */

/* The view on the panel with the text of each of its active lines, taken from the dataset it was for, so a new
 * dataset never changes it (docs/data-model.md#view). Stored in NVS as is (STORE_KEY_SHOWN_VIEW): keep the layout. */
typedef struct {
    view_t  view;        /* view.id 0 = nothing shown */
    uint8_t line_count;  /* active lines; 0 = All OK */
    char    lines[DATASET_MAX_LINES][LINE_MAX_BYTES + 1];  /* in dataset order, "II|name|state" */
} shown_view_t;

typedef struct {
    uint32_t                uptime_ms;  /* when it arrived */
    zigbee_received_write_t write;
} app_state_received_write_t;

/* Everything a screen may show, at one moment (app_state_snapshot()). */
typedef struct {
    device_state_t             state;
    wake_failure_t             failure;
    uint32_t                   changes;        /* counts the changes of the state, the failure and the model */
    uint32_t                   dataset_hash;   /* the stored dataset, 0 = none */
    uint8_t                    dataset_lines;
    shown_view_t               shown;          /* the view on the panel */
    bool                       syncing;        /* dataset entries arrived and their commit has not */
    bool                       out_of_sync;    /* the last view received was for another dataset */
    bool                       stale;          /* the shown view is out of date (docs/data-model.md#staleness-flag) */
    int64_t                    age_ms;         /* age of the last received view, -1 = unknown */
    uint32_t                   valid_ms;       /* how long a received view stays fresh */
    uint32_t                   wake_number;
    uint32_t                   uptime_ms;
    uint16_t                   battery_mv;
    bool                       on_usb_power;
    bool                       battery_low;    /* latched or below the threshold */
    uint32_t                   battery_simulation_changes;
    int                        received_write_count;
    app_state_received_write_t received_writes[APP_STATE_RECEIVED_WRITES];  /* newest first */
} app_state_t;

void app_state_init(void);  /* before any other call */
void app_state_load(void);  /* the stored dataset and view */
void app_state_erase_stored(void);

void app_state_set_device_state(device_state_t state);
void app_state_set_wake_number(uint32_t wake_number);

/* This wake failed: from now on the screen shows it whatever the state. WAKE_FAILURE_NONE while the wake goes well. */
void           app_state_set_wake_failure(wake_failure_t failure);
wake_failure_t app_state_wake_failure(void);

uint32_t app_state_dataset_hash(void);   /* 0 = no dataset */
uint32_t app_state_shown_view_id(void);  /* 0 = nothing shown */
bool     app_state_view_received(void);  /* a view was applied in this wake */

void app_state_snapshot(app_state_t *state);

/* Zigbee callbacks (zigbee_callbacks_t), called in the Zigbee task */
void app_state_on_view(const view_t *view, zigbee_view_result_t result);
void app_state_on_dataset(const dataset_t *dataset);
void app_state_on_received_write(const zigbee_received_write_t *write);

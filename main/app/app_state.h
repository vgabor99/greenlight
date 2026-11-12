#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app/device_state.h"
#include "data_model.h"

/* The application state (docs/firmware.md#tasks-and-data-flow): the device state, the stored dataset, the view on
 * the panel and how current it is, and the writes HA sent in this wake. The state machine and the Zigbee task change
 * it; the display draws snapshots of it. A mutex guards it. The dataset and the shown view are kept in NVS through
 * the sleep. */

#define APP_STATE_RECEIVED_WRITES 8  /* received writes kept for the debug screen */

/* The view on the panel with the text of each of its active lines, taken from the dataset it was for, so a new
 * dataset never changes it (docs/data-model.md#view). Stored in NVS up to its last line (app_state.c): keep the layout. */
typedef struct {
    view_t  view;        /* view.id 0 = nothing shown */
    uint8_t line_count;  /* active lines; 0 = All OK */
    char    lines[DATASET_MAX_LINES][LINE_MAX_BYTES + 1];  /* in dataset order, "II|name|state" */
} shown_view_t;

/* One write HA sent and what came of it (app/ha_input.c), for the debug screen. */
typedef enum { RECEIVED_VIEW, RECEIVED_DATASET_ENTRY, RECEIVED_DATASET_COMMIT } received_kind_t;

typedef enum {
    COMMIT_STORED,
    COMMIT_ALREADY_STORED,  /* the stored dataset already has this hash */
    COMMIT_BAD_COUNT,       /* count > DATASET_MAX_LINES or hash 0 */
    COMMIT_REJECTED,        /* a line is missing a part, or has one for another dataset */
} commit_result_t;

typedef struct {
    uint32_t        uptime_ms;  /* when it arrived */
    received_kind_t kind;
    union {
        struct {
            view_t        view;
            view_result_t result;
            uint8_t       received_mask;  /* VIEW_INCOMPLETE: which fields arrived (bit 0 = bits) */
        } view;
        struct {
            int32_t dataset_hash;  /* the dataset it is for; -1 = not a valid entry */
            uint8_t index;
            uint8_t part;          /* a long line comes in parts */
            bool    more;          /* another part of this line follows */
            char    text[LINE_MAX_BYTES + 1];
        } entry;
        struct {
            uint32_t        hash;
            uint8_t         count;
            commit_result_t result;
            uint8_t         first_missing;  /* REJECTED: the first line missing a part or wrongly tagged */
        } commit;
    };
} received_write_t;

/* Everything a screen may show, at one moment (app_state_snapshot()). */
typedef struct {
    device_state_t             state;
    failure_t                  failure;
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
    received_write_t received_writes[APP_STATE_RECEIVED_WRITES];  /* newest first */
} app_state_t;

void app_state_init(void);  /* before any other call */
void app_state_load(void);  /* the stored dataset and view */
void app_state_erase_stored(void);

void app_state_set_device_state(device_state_t state);
void app_state_set_wake_number(uint32_t wake_number);

/* This wake failed: from now on the screen shows it whatever the state. FAILURE_NONE while the wake goes well. */
void           app_state_set_failure(failure_t failure);
failure_t      app_state_failure(void);

uint32_t app_state_dataset_hash(void);   /* 0 = no dataset */
uint32_t app_state_shown_view_id(void);  /* 0 = nothing shown */
bool     app_state_view_received(void);  /* a view was applied in this wake */

void app_state_snapshot(app_state_t *state);

/* HA's input, once app/ha_input.c has checked it */
void app_state_apply_view(const view_t *view, view_result_t result);  /* NEW shows it, TIME_ONLY renews it */
void app_state_store_dataset(const dataset_t *dataset);
void app_state_note_received(const received_write_t *write);           /* for the debug screen; tracks syncing */

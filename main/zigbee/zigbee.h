#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "data_model.h"

/* Zigbee side of the device (docs/zigbee.md): the transport between HA and the device. It decodes HA's writes and
 * sends the reports it is asked to; it keeps no application state and decides nothing about the data
 * (data_model.h). The stack runs in its own task; every function here is safe to call from the application task. */

#define ZIGBEE_LINE_MAX_PARTS 3   /* a line comes in up to this many ds_entry writes */
#define ZIGBEE_PART_MAX_BYTES 26  /* longest text of one ds_entry write */

#define ZIGBEE_VIEW_ALL_ATTRIBUTES 0x0F  /* on_view's received_mask when all four view_* attributes arrived */

/* One ds_entry write as it arrived: part `part` of line `index` of the dataset `dataset_hash`. */
typedef struct {
    int32_t dataset_hash;  /* -1 = not a valid "HHHHHH|II|text" entry */
    uint8_t index;
    uint8_t part;          /* 0 .. ZIGBEE_LINE_MAX_PARTS-1 */
    bool    more;          /* another part of this line follows */
    char    text[ZIGBEE_PART_MAX_BYTES + 1];
} zigbee_dataset_entry_t;

/* One ds_commit, with the dataset that the entries received for it make up. */
typedef struct {
    uint32_t         hash;
    uint8_t          count;
    bool             well_formed;    /* hash not 0 and count at most DATASET_MAX_LINES */
    const dataset_t *dataset;        /* every line arrived, all its parts for this hash; else NULL */
    uint8_t          first_missing;  /* no dataset: the first line missing a part or with one for another hash */
} zigbee_dataset_commit_t;

/* What HA writes to the display input (docs/zigbee.md#input-ha-to-device), decoded. Deciding what it means, and
 * acknowledging it with the reports below, is the receiver's business. All called in the Zigbee task. */
typedef struct {
    /* The view_* attributes of one write; received_mask says which arrived (bit 0 = view_bits). */
    void (*on_view)(const view_t *view, uint8_t received_mask);
    void (*on_dataset_entry)(const zigbee_dataset_entry_t *entry);
    void (*on_dataset_commit)(const zigbee_dataset_commit_t *commit);
} zigbee_callbacks_t;

/* The reported attributes' first values. */
typedef struct {
    uint32_t wake_count;     /* this wake's number (0 .. 2^24-1, different from the previous wake's) */
    uint32_t shown_view_id;  /* 0 = none */
    uint32_t dataset_hash;   /* 0 = none */
    uint16_t battery_mv;     /* at boot, also for the Power Configuration attributes */
} zigbee_initial_values_t;

/* Start the Zigbee stack task. */
void zigbee_start(const zigbee_callbacks_t *callbacks, const zigbee_initial_values_t *initial);

/* Wait for the stack to report whether a network was restored from storage. Returns false on timeout. */
bool zigbee_wait_started(uint32_t timeout_ms, bool *factory_new);

/* True while the device is on a network (restored, or joined by pairing). */
bool zigbee_is_joined(void);

/* Network steering (pairing). zigbee_start_pairing() keeps retrying until zigbee_stop_pairing(). */
void zigbee_start_pairing(void);
void zigbee_stop_pairing(void);

/* Leave the network, erase all Zigbee data and restart the chip. Does not return. */
void zigbee_erase_network(void);

/* Reports to the coordinator (docs/zigbee.md#state-device-to-ha). Callable from any task, the receiver's callbacks
 * included. */
void zigbee_report_wake_count(uint32_t wake_count);    /* 0xFC8A wake_count: HA's push trigger */
void zigbee_report_shown_view_id(uint32_t view_id);    /* 0xFC8A shown_view_id */
void zigbee_report_dataset_hash(uint32_t dataset_hash);  /* 0xFC8A dataset_hash */
void zigbee_report_awake(bool awake);    /* 0xFC8A awake */
void zigbee_report_occupancy(bool occupied);
/* Power Configuration (charge from mv, alarm bit 0 = low) and 0xFC8A battery_mv */
void zigbee_report_battery(uint16_t mv, bool low);
/* The low-battery farewell: the battery reports with low set, batteryAlarmState asking Z2M for a default response,
 * resent every second until it is confirmed or timeout_ms is over. True when confirmed. */
bool zigbee_report_battery_low(uint16_t mv, uint32_t timeout_ms);
/* SHUTDOWN: occupancy 0 and awake false, each asking Z2M for a default response, resent every second until both
 * are confirmed or timeout_ms is over. True when both were confirmed. */
bool zigbee_report_goodbye(uint32_t timeout_ms);

/* Diagnostic: the parent and the neighbour table to the log. */
void zigbee_log_neighbors(void);

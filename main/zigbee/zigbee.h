#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "data_model.h"

/* Zigbee side of the device (docs/zigbee.md): the transport of the data model (data_model.h), the reports and the
 * display input. The stack runs in its own task; every function here is safe to call from the application task. */

#define ZIGBEE_LINE_MAX_PARTS 3   /* a line comes in up to this many ds_entry writes */
#define ZIGBEE_PART_MAX_BYTES 26  /* longest text of one ds_entry write */

typedef enum {
    ZIGBEE_VIEW_NEW,          /* new content: draw it */
    ZIGBEE_VIEW_TIME_ONLY,    /* same id as shown: only the time is new */
    ZIGBEE_VIEW_OUT_OF_SYNC,  /* for another dataset than the stored one: nothing changes */
    ZIGBEE_VIEW_INCOMPLETE,   /* not all four attributes arrived together (HA error, or Z2M split the write) */
} zigbee_view_result_t;

typedef enum {
    ZIGBEE_COMMIT_STORED,
    ZIGBEE_COMMIT_ALREADY_STORED,  /* the stored dataset already has this hash */
    ZIGBEE_COMMIT_BAD_COUNT,       /* count > DATASET_MAX_LINES or hash 0 */
    ZIGBEE_COMMIT_REJECTED,        /* an entry is missing or tagged with another hash */
} zigbee_commit_result_t;

/* One received write to the display input (docs/zigbee.md#input-ha-to-device), as it arrived; for the debug screen. */
typedef enum { ZIGBEE_WRITE_VIEW, ZIGBEE_WRITE_DATASET_ENTRY, ZIGBEE_WRITE_DATASET_COMMIT } zigbee_write_kind_t;

typedef struct {
    zigbee_write_kind_t kind;
    union {
        struct {
            view_t               view;
            zigbee_view_result_t result;
            uint8_t              received_mask;  /* INCOMPLETE: which attributes arrived (bit 0 = view_bits) */
        } view;
        struct {
            int32_t dataset_hash;  /* the dataset it is for; -1 = not a valid "HHHHHH|II|text" entry */
            uint8_t index;
            uint8_t part;          /* 0 .. ZIGBEE_LINE_MAX_PARTS-1 */
            bool    more;          /* another part of this line follows */
            char    text[ZIGBEE_PART_MAX_BYTES + 1];
        } entry;
        struct {
            uint32_t               hash;
            uint8_t                count;
            zigbee_commit_result_t result;
            uint8_t                first_missing;  /* REJECTED: the first index missing or wrongly tagged */
        } commit;
    };
} zigbee_received_write_t;

/* All called in the Zigbee task. */
typedef struct {
    /* A view was received and checked (every result but INCOMPLETE). For NEW and TIME_ONLY, the device reports
     * shown_view_id after this returns, so the view must be stored by then. */
    void (*on_view)(const view_t *view, zigbee_view_result_t result);
    /* A valid dataset commit: store the dataset. dataset_hash is reported after this returns. */
    void (*on_dataset)(const dataset_t *dataset);
    /* HA is working with the device (dataset write, out-of-sync view): stay awake. */
    void (*on_activity)(void);
    /* Any write to the display input and its outcome, for the debug screen. */
    void (*on_received_write)(const zigbee_received_write_t *write);
} zigbee_callbacks_t;

/* Start the Zigbee stack task. dataset_hash and shown_view_id are the stored dataset and view (0 = none);
 * wake_count is this wake's number (0 .. 2^24-1, different from the previous wake's); battery_mv the battery
 * voltage at boot, for the Power Configuration attributes' first values. */
void zigbee_start(const zigbee_callbacks_t *callbacks, uint32_t dataset_hash, uint32_t shown_view_id,
                  uint32_t wake_count, uint16_t battery_mv);

/* Wait for the stack to report whether a network was restored from storage. Returns false on timeout. */
bool zigbee_wait_started(uint32_t timeout_ms, bool *factory_new);

/* True while the device is on a network (restored, or joined by pairing). */
bool zigbee_is_joined(void);

/* Network steering (pairing). zigbee_start_pairing() keeps retrying until zigbee_stop_pairing(). */
void zigbee_start_pairing(void);
void zigbee_stop_pairing(void);

/* Leave the network, erase all Zigbee data and restart the chip. Does not return. */
void zigbee_erase_network(void);

/* Reports to the coordinator (docs/zigbee.md#state-device-to-ha) */
void zigbee_report_wake_count(void);     /* 0xFC8A wake_count: HA's push trigger */
void zigbee_report_shown_view_id(void);  /* 0xFC8A shown_view_id */
void zigbee_report_dataset_hash(void);   /* 0xFC8A dataset_hash */
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

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Zigbee side of the device (DESIGN.md section 6): the data model, the reports and the display input.
 * The stack runs in its own task; every function here is safe to call from the application task. */

#define ZB_MAX_LINES  6
#define ZB_LINE_CHARS 28  /* longest line text accepted (the rest of a slot is the 7-character hash tag) */

typedef struct {
    uint32_t hash;                           /* 24-bit hash chosen by HA */
    uint8_t  count;                          /* number of lines, 0 = "All OK" */
    char     line[ZB_MAX_LINES][ZB_LINE_CHARS + 1];
    char     footer[ZB_LINE_CHARS + 1];
} zb_snapshot_t;

typedef struct {
    /* A valid commit arrived (all lines tagged with the hash). Called in the Zigbee task. */
    void (*snapshot)(const zb_snapshot_t *snap);
    /* The footer text was written. Called in the Zigbee task. */
    void (*footer)(const char *footer);
} zb_callbacks_t;

/* Start the Zigbee stack task. shown_hash is the hash of the snapshot on the panel (0 = unknown); wake_count is
 * this wake's number (0 .. 2^24-1, different from the previous wake's), reported by zb_report_wake(). */
void zb_start(const zb_callbacks_t *cb, uint32_t shown_hash, uint32_t wake_count);

/* Wait for the stack to report whether a network was restored from storage. Returns false on timeout. */
bool zb_wait_started(uint32_t timeout_ms, bool *factory_new);

/* True while the device is on a network (restored, or joined by pairing). */
bool zb_joined(void);

/* Network steering (pairing). zb_pairing_start() keeps retrying until zb_pairing_stop(). */
void zb_pairing_start(void);
void zb_pairing_stop(void);

/* Leave the network, erase all Zigbee data and restart the chip. Does not return. */
void zb_erase_network(void);

/* Reports to the coordinator (DESIGN.md 6.4). */
void zb_report_battery(void);               /* the three Power Configuration attributes, fixed 100 % in the MVP */
void zb_report_occupancy(bool occupied);
void zb_report_shown_hash(uint32_t hash);
void zb_report_awake(bool awake);
void zb_log_neighbors(void);                /* diagnostic: parent and neighbour table to the log */
void zb_report_wake(void);                  /* the wake counter (endpoint 2): the last report of every wake */

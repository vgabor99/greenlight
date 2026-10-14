#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Zigbee side of the device (DESIGN.md section 6): the data model, the reports and the display input.
 * The stack runs in its own task; every function here is safe to call from the application task. */

#define ZB_MAX_LINES  6
#define ZB_LINE_CHARS 28  /* longest line text accepted (the rest of a slot is the 7-character hash tag) */
#define ZB_SLOT_CHARS 40  /* longest slot string accepted: "HHHHHH|" + ZB_LINE_CHARS fits in this */

typedef struct {
    uint32_t hash;                           /* 24-bit hash chosen by HA */
    uint8_t  count;                          /* number of lines, 0 = "All OK" */
    char     line[ZB_MAX_LINES][ZB_LINE_CHARS + 1];
    char     footer[ZB_LINE_CHARS + 1];
} zb_snapshot_t;

/* One received write to the display input (DESIGN.md 6.5), as it arrived; for the debug screen. */
typedef enum { ZB_RX_LINE, ZB_RX_FOOTER, ZB_RX_COMMIT } zb_rx_kind_t;
typedef enum { ZB_COMMIT_OK, ZB_COMMIT_SAME, ZB_COMMIT_BAD_COUNT, ZB_COMMIT_REJECTED } zb_commit_result_t;

typedef struct {
    zb_rx_kind_t       kind;
    uint8_t            line;                     /* LINE: slot 1..6; COMMIT rejected: the first bad line */
    char               text[ZB_SLOT_CHARS + 1];  /* LINE, FOOTER: the string as written, tag included */
    uint32_t           hash;                     /* COMMIT */
    uint8_t            count;                    /* COMMIT */
    zb_commit_result_t result;                   /* COMMIT */
    int32_t            tag;                      /* COMMIT rejected: tag of the bad line, -1 = no valid tag */
} zb_rx_t;

typedef struct {
    /* A valid commit arrived (all lines tagged with the hash). Called in the Zigbee task. */
    void (*snapshot)(const zb_snapshot_t *snap);
    /* The footer text was written. Called in the Zigbee task. */
    void (*footer)(const char *footer);
    /* Any write to the display input, and the outcome of each commit. Called in the Zigbee task. */
    void (*rx)(const zb_rx_t *ev);
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
